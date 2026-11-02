// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "ElementFilter.h"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace sfz {
namespace oscairn {

namespace {
constexpr double kReferenceRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kControlInterval = 16; // samples between coefficient updates

struct Knot {
    int index;
    float cents; // relative to 440 Hz
};
// Cutoff measured on reference renders every 8 indices (white noise, resonance 0), in cents
// from 440 Hz, linear in cents between knots. Below 144 the renders' cutoffs (under 30 Hz)
// are not measurable; the table continues at 11.54 cents per index, the knots' mean slope.
constexpr Knot kCutoffKnots[] = {
    { 144, -3952.f }, { 152, -3817.f }, { 160, -3666.f }, { 168, -3734.f }, { 176, -3669.f }, { 184, -3578.f },
    { 192, -3486.f }, { 200, -3541.f }, { 208, -3360.f }, { 216, -3226.f }, { 224, -3117.f }, { 232, -3077.f },
    { 240, -2901.f }, { 248, -2828.f }, { 256, -2749.f }, { 264, -2703.f }, { 272, -2647.f }, { 280, -2529.f },
    { 288, -2502.f }, { 296, -2442.f }, { 304, -2390.f }, { 312, -2215.f }, { 320, -2065.f }, { 328, -1978.f },
    { 336, -1809.f }, { 344, -1724.f }, { 352, -1627.f }, { 360, -1549.f }, { 368, -1508.f }, { 376, -1391.f },
    { 384, -1356.f }, { 392, -1325.f }, { 400, -1247.f }, { 408, -1199.f }, { 416, -1029.f }, { 424, -839.f },
    { 432, -753.f }, { 440, -638.f }, { 448, -519.f }, { 456, -446.f }, { 464, -337.f }, { 472, -282.f },
    { 480, -225.f }, { 488, -162.f }, { 496, -81.f }, { 504, -57.f }, { 512, 23.f }, { 520, 214.f },
    { 528, 347.f }, { 536, 482.f }, { 544, 598.f }, { 552, 696.f }, { 560, 774.f }, { 568, 846.f },
    { 576, 925.f }, { 584, 984.f }, { 592, 1057.f }, { 600, 1116.f }, { 608, 1171.f }, { 616, 1252.f },
    { 624, 1437.f }, { 632, 1570.f }, { 640, 1701.f }, { 648, 1812.f }, { 656, 1895.f }, { 664, 1989.f },
    { 672, 2059.f }, { 680, 2147.f }, { 688, 2209.f }, { 696, 2263.f }, { 704, 2316.f }, { 712, 2370.f },
    { 720, 2502.f }, { 728, 2663.f }, { 736, 2802.f }, { 744, 2918.f }, { 752, 3026.f }, { 760, 3122.f },
    { 768, 3214.f }, { 776, 3291.f }, { 784, 3363.f }, { 792, 3429.f }, { 800, 3490.f }, { 808, 3544.f },
    { 816, 3598.f }, { 824, 3745.f }, { 832, 3906.f }, { 840, 4047.f }, { 848, 4168.f }, { 856, 4279.f },
    { 864, 4375.f }, { 872, 4467.f }, { 880, 4545.f }, { 888, 4621.f }, { 896, 4688.f }, { 904, 4753.f },
    { 912, 4812.f },
};
constexpr float kBelowSlope = 11.54f;
// Above index 912 the measured cutoff stays at 7290 Hz (and the frequency coefficient there).
constexpr double kTopHz = 7290.0;

float cutoffCents(int32_t index) noexcept
{
    const int32_t i = std::max<int32_t>(0, std::min<int32_t>(1023, index));
    const Knot* first = std::begin(kCutoffKnots);
    const Knot* last = std::end(kCutoffKnots) - 1;
    if (i <= first->index)
        return first->cents - kBelowSlope * static_cast<float>(first->index - i);
    if (i >= last->index)
        return 1200.0f * std::log2(static_cast<float>(kTopHz) / 440.0f);
    const Knot* hi = std::upper_bound(first, last + 1, i, [](int32_t v, const Knot& k) { return v < k.index; });
    const Knot* lo = hi - 1;
    const float f = static_cast<float>(i - lo->index) / static_cast<float>(hi->index - lo->index);
    return lo->cents + f * (hi->cents - lo->cents);
}
// Damping limit at resonance 0, measured: 2 up to index 816, then falling by 0.0104 per
// index to 1 (about index 912); expressed through the knots so that cutoff shifts in cents
// move along it the same way.
float dampingLimit(double hz) noexcept
{
    const float cents = 1200.0f * static_cast<float>(std::log2(hz / 440.0));
    const Knot* k = std::begin(kCutoffKnots);
    const Knot* end = std::end(kCutoffKnots);
    while (k != end && k->index < 816)
        ++k;
    if (cents <= k->cents)
        return 2.0f;
    float index = static_cast<float>((end - 1)->index);
    for (; k + 1 != end; ++k) {
        if (cents <= (k + 1)->cents) {
            index = static_cast<float>(k->index) + static_cast<float>((k + 1)->index - k->index) * (cents - k->cents) / ((k + 1)->cents - k->cents);
            break;
        }
    }
    return std::max(1.0f, 2.0f - 0.0104f * (index - 816.0f));
}
} // namespace

bool filterTypeSupported(int32_t type) noexcept
{
    switch (type) {
    case 0: // two low-pass sections
    case 4: // low-pass section, then a high-pass section with damping 1
    case 6: // two high-pass sections
    case 7: // one high-pass section
        return true;
    default:
        return false;
    }
}

float filterCutoffHz(int32_t index, float cents) noexcept
{
    return 440.0f * std::exp2((cutoffCents(index) + cents) / 1200.0f);
}

float filterDamping(int32_t resonance) noexcept
{
    const int32_t r = std::max<int32_t>(0, std::min<int32_t>(127, resonance));
    return 2.0f * std::exp2(-static_cast<float>(r) / 16.0f);
}

float filterGain(int32_t gain) noexcept
{
    // Passband level measured at these values (dB), linear in dB between them; about
    // 0.375 dB per step below 64; 0 is silent
    static constexpr float values[5] = { 64.f, 128.f, 192.f, 230.f, 255.f };
    static constexpr float db[5] = { -68.34f, -44.38f, -20.18f, -5.45f, 3.25f };
    const int32_t g = std::max<int32_t>(0, std::min<int32_t>(255, gain));
    if (g == 0)
        return 0.0f;
    float level = db[0] - 0.375f * (values[0] - static_cast<float>(g));
    for (int k = 0; k < 4; ++k) {
        if (static_cast<float>(g) >= values[k] && static_cast<float>(g) <= values[k + 1])
            level = db[k] + (db[k + 1] - db[k]) * (static_cast<float>(g) - values[k]) / (values[k + 1] - values[k]);
    }
    return std::pow(10.0f, level / 20.0f);
}

void ElementFilter::setSampleRate(double sampleRate) noexcept
{
    sampleRate_ = sampleRate;
    const double r = sampleRate / kReferenceRate;
    const double whole = std::round(r);
    allpass_ = std::abs(r - whole) > 1e-9 || whole < 1.0;
    // A pure delay for whole multiples; otherwise the allpass carries 1 to 3 host samples
    // (more would need a higher order to stay accurate) and the rest is a delay line
    delay_ = allpass_ ? std::max(0, static_cast<int>(std::floor(r)) - 2) : static_cast<int>(whole);
    delay_ = std::min(delay_, kMaxDelay - 1);
    // shortcut: rates above 16 x 44.1 kHz keep a 15-sample delay line and drift from the
    // 44.1 kHz response; raise kMaxDelay if such rates are ever used
    const double theta = 2.0 * kPi * 18000.0 / kReferenceRate;
    const double phi = theta / r;
    const double alpha = (theta - delay_ * phi - 2.0 * phi) / 2.0;
    pin2_[0] = static_cast<float>(std::sin(phi + alpha));
    pin2_[1] = static_cast<float>(std::sin(2.0 * phi + alpha));
    pin2_[2] = static_cast<float>(-std::sin(alpha));
}

void ElementFilter::design(Section& s, float hz, float damping) const noexcept
{
    const double h = std::min<double>(hz, kTopHz);
    s.f = static_cast<float>(2.0 * std::sin(kPi * h / kReferenceRate));
    s.d = std::min(damping, dampingLimit(h));
    s.c1 = s.c2 = s.g = 0.0f;
    if (allpass_) {
        // The allpass (c2 + c1 z^-1 + z^-2) / (1 + c1 z^-1 + c2 z^-2) lags by
        // 2 phi + 2 arg(1 + c1 e^-j phi + c2 e^-j2phi); pin z^-M times it to one 44.1 kHz
        // sample at the cutoff and at 18 kHz
        const double r = sampleRate_ / kReferenceRate;
        const double theta = 2.0 * kPi * h / kReferenceRate;
        const double phi = theta / r;
        const double alpha = (theta - delay_ * phi - 2.0 * phi) / 2.0;
        const double a0 = std::sin(phi + alpha), a1 = std::sin(2.0 * phi + alpha), b = -std::sin(alpha);
        const double det = a0 * pin2_[1] - a1 * pin2_[0];
        s.c1 = static_cast<float>((b * pin2_[1] - a1 * pin2_[2]) / det);
        s.c2 = static_cast<float>((a0 * pin2_[2] - b * pin2_[0]) / det);
        if (delay_ == 0)
            s.g = s.c2; // the allpass's direct path closes the loop within the sample
    }
    const float f = s.f, d = s.d, g = s.g;
    s.k = 1.0f / (1.0f - g);
    const float k = s.k;
    s.cs = k * f + k * k * f * g + d * g * k + d;
    s.inv = 1.0f / (1.0f + k * k * f * f * g + d * g * k * f);
}

// The part of the reference-rate delay's output known before this sample's input
float ElementFilter::delayedValue(const Section& s, Delay& line) const noexcept
{
    if (delay_ == 0)
        return line.s1; // the direct path g * input is solved for
    const float u = line.line[(line.pos + kMaxDelay - delay_) % kMaxDelay];
    if (!allpass_)
        return u;
    const float y = s.c2 * u + line.s1;
    line.s1 = s.c1 * (u - y) + line.s2;
    line.s2 = u - s.c2 * y;
    return y;
}

void ElementFilter::push(const Section& s, Delay& line, float value) const noexcept
{
    if (delay_ == 0) {
        const float y = s.c2 * value + line.s1;
        line.s1 = s.c1 * (value - y) + line.s2;
        line.s2 = value - s.c2 * y;
        return;
    }
    line.line[line.pos] = value;
    line.pos = (line.pos + 1) % kMaxDelay;
}

float ElementFilter::run(Section& s, int channel, float x) const noexcept
{
    // Two-integrator state-variable section: low = low' + f band'; high = x - low - d band';
    // band = f high + band', with every prime the reference-rate delay of its signal
    Delay& lowLine = s.low[channel];
    Delay& bandLine = s.band[channel];
    const float low1 = delayedValue(s, lowLine);
    const float band1 = delayedValue(s, bandLine);
    const float high = (x - s.k * low1 - s.cs * band1) * s.inv;
    const float band = s.k * (s.f * high + band1);
    const float low = s.k * (low1 + s.f * band1) + s.k * s.f * s.g * band;
    push(s, lowLine, low);
    push(s, bandLine, band);
    return s.highPass ? high : low;
}

void ElementFilter::start(const ElementFilterParameters& parameters, int note) noexcept
{
    p_ = parameters;
    keyCents_ = static_cast<float>(p_.keyFollow - 64) * 3.125f * static_cast<float>(note - 60);
    gain_ = filterGain(p_.gain);
    for (auto& s : sections_)
        s = Section {};
    damping_ = filterDamping(p_.resonance);
    const bool highPass = p_.type == 6 || p_.type == 7;
    sections_[0].highPass = highPass;
    sections_[1].highPass = highPass || p_.type == 4;
    if (p_.type == 4)
        design(sections_[1], filterCutoffHz(p_.hpfCutoff), 1.0f);
    update(0.0f);
}

void ElementFilter::update(float cents) noexcept
{
    const float hz = filterCutoffHz(p_.cutoff, keyCents_ + cents);
    design(sections_[0], hz, damping_);
    if (p_.type != 4) {
        Section& second = sections_[1];
        second.f = sections_[0].f;
        second.d = sections_[0].d;
        second.c1 = sections_[0].c1;
        second.c2 = sections_[0].c2;
        second.g = sections_[0].g;
        second.k = sections_[0].k;
        second.cs = sections_[0].cs;
        second.inv = sections_[0].inv;
    }
}

void ElementFilter::process(absl::Span<float> left, absl::Span<float> right, absl::Span<const float> cents) noexcept
{
    if (!filterTypeSupported(p_.type))
        return;
    const bool stereo = !right.empty();
    const size_t n = stereo ? std::min(left.size(), right.size()) : left.size();
    const int stages = (p_.type == 0 || p_.type == 6 || p_.type == 4) ? 2 : 1;
    for (size_t i = 0; i < n; ++i) {
        if (!cents.empty() && i % kControlInterval == 0 && i < cents.size())
            update(cents[i]);
        for (int ch = 0; ch < (stereo ? 2 : 1); ++ch) {
            float& sample = ch == 0 ? left[i] : right[i];
            float x = sample;
            for (int s = 0; s < stages; ++s)
                x = run(sections_[s], ch, x);
            sample = gain_ * x;
        }
    }
}

} // namespace oscairn
} // namespace sfz

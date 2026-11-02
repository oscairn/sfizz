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

float cutoffCents(float index) noexcept
{
    const float i = std::max(0.0f, std::min(1023.0f, index));
    const Knot* first = std::begin(kCutoffKnots);
    const Knot* last = std::end(kCutoffKnots) - 1;
    if (i <= static_cast<float>(first->index))
        return first->cents - kBelowSlope * (static_cast<float>(first->index) - i);
    if (i >= static_cast<float>(last->index))
        return 1200.0f * std::log2(static_cast<float>(kTopHz) / 440.0f);
    const Knot* hi = std::upper_bound(first, last + 1, i, [](float v, const Knot& k) { return v < static_cast<float>(k.index); });
    const Knot* lo = hi - 1;
    const float f = (i - static_cast<float>(lo->index)) / static_cast<float>(hi->index - lo->index);
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
    case 1: // low-pass sections with the resonance and with damping 1
    case 2: // low-pass section and a one-pole low-pass
    case 3: // one-pole low-pass and a low-pass section 4097 cents higher
    case 4: // low-pass section, then a high-pass section with damping 1
    case 5: // one-pole low-pass, then a high-pass section with damping 1
    case 6: // two high-pass sections
    case 7: // one high-pass section
    case 8: // two band-pass sections
    case 10: // low-pass and high-pass sections with damping 1 spread by the resonance
    case 11: // one band-pass section
    case 12: // two notch sections
    case 13: // one notch section
    case 14: // low-pass sections at the cutoff and at the distance, summed
    case 15: // high-pass sections, summed
    case 16: // band-pass sections, summed
    case 17: // notch sections at the cutoff and at the distance, in series
    case 19: // low-pass minus band-pass of one section
    case 21: // the filter gain only (measured flat)
        return true;
    default:
        return false;
    }
}

float filterCutoffHz(int32_t index, float cents) noexcept
{
    return 440.0f * std::exp2((cutoffCents(static_cast<float>(index)) + cents) / 1200.0f);
}

float cutoffVelocityCents(int32_t cutoff, int32_t sensitivity, int velocity) noexcept
{
    // Measured (fl3 cv_*): the cutoff index moves by 0.115 * sensitivity * (velocity - 115),
    // +63 counting as 64, negative sensitivities mirroring the velocity (128 - v); fitted rms
    // about 4 index steps
    if (sensitivity == 0)
        return 0.0f;
    const int32_t s = sensitivity >= 63 ? 64 : std::max<int32_t>(-64, sensitivity);
    const float v = static_cast<float>(s > 0 ? velocity : 128 - velocity);
    const float shift = 0.115f * static_cast<float>(s > 0 ? s : -s) * (v - 115.0f);
    const float base = static_cast<float>(cutoff);
    return cutoffCents(base + shift) - cutoffCents(base);
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

void ElementFilter::pin(Section& s, double h) const noexcept
{
    s.c1 = s.c2 = s.g = 0.0f;
    if (!allpass_)
        return;
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

void ElementFilter::design(Section& s, float hz, float damping) const noexcept
{
    const double h = std::min<double>(hz, kTopHz);
    s.f = static_cast<float>(2.0 * std::sin(kPi * h / kReferenceRate));
    s.d = std::min(damping, dampingLimit(h));
    pin(s, h);
    const float f = s.f, d = s.d, g = s.g;
    s.k = 1.0f / (1.0f - g);
    const float k = s.k;
    s.cs = k * f + k * k * f * g + d * g * k + d;
    s.inv = 1.0f / (1.0f + k * k * f * f * g + d * g * k * f);
}

void ElementFilter::designOnePole(Section& s, float hz) const noexcept
{
    // y = f x + (1 - f) y', f = min(1, 2 sin(pi fc / 44100)); with the reference-rate delay
    // y' = g y + known: y = (f x + (1 - f) known) / (1 - (1 - f) g)
    const double h = std::min<double>(hz, kTopHz);
    s.f = std::min(1.0f, static_cast<float>(2.0 * std::sin(kPi * h / kReferenceRate)));
    pin(s, h);
    s.inv = 1.0f / (1.0f - (1.0f - s.f) * s.g);
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
    Delay& lowLine = s.low[channel];
    if (s.output == Output::OnePole) {
        const float y1 = delayedValue(s, lowLine);
        const float y = (s.f * x + (1.0f - s.f) * y1) * s.inv;
        push(s, lowLine, y);
        return y;
    }
    // Two-integrator state-variable section: low = low' + f band'; high = x - low - d band';
    // band = f high + band', with every prime the reference-rate delay of its signal
    Delay& bandLine = s.band[channel];
    const float low1 = delayedValue(s, lowLine);
    const float band1 = delayedValue(s, bandLine);
    const float high = (x - s.k * low1 - s.cs * band1) * s.inv;
    const float band = s.k * (s.f * high + band1);
    const float low = s.k * (low1 + s.f * band1) + s.k * s.f * s.g * band;
    push(s, lowLine, low);
    push(s, bandLine, band);
    switch (s.output) {
    case Output::High: return high;
    case Output::Band: return band;
    case Output::Notch: return high + low;
    case Output::LowMinusBand: return low - band;
    default: return low;
    }
}

void ElementFilter::start(const ElementFilterParameters& parameters, int note, int velocity) noexcept
{
    p_ = parameters;
    keyCents_ = static_cast<float>(p_.keyFollow - 64) * 3.125f * static_cast<float>(note - 60)
        + cutoffVelocityCents(p_.cutoff, p_.velocitySensitivity, velocity);
    gain_ = filterGain(p_.gain);
    for (auto& s : sections_)
        s = Section {};
    damping_ = filterDamping(p_.resonance);
    using O = Output;
    struct Layout {
        O first, second;
        int stages;
        bool parallel;
    };
    const Layout layout = [this]() -> Layout {
        switch (p_.type) {
        case 0: return { O::Low, O::Low, 2, false };
        case 1: return { O::Low, O::Low, 2, false };
        case 2: return { O::Low, O::OnePole, 2, false };
        case 3: return { O::OnePole, O::Low, 2, false };
        case 4: return { O::Low, O::High, 2, false };
        case 5: return { O::OnePole, O::High, 2, false };
        case 6: return { O::High, O::High, 2, false };
        case 7: return { O::High, O::High, 1, false };
        case 8: return { O::Band, O::Band, 2, false };
        case 10: return { O::Low, O::High, 2, false };
        case 11: return { O::Band, O::Band, 1, false };
        case 12: return { O::Notch, O::Notch, 2, false };
        case 13: return { O::Notch, O::Notch, 1, false };
        case 14: return { O::Low, O::Low, 2, true };
        case 15: return { O::High, O::High, 2, true };
        case 16: return { O::Band, O::Band, 2, true };
        case 17: return { O::Notch, O::Notch, 2, false };
        case 19: return { O::LowMinusBand, O::Low, 1, false };
        default: return { O::Low, O::Low, 0, false };
        }
    }();
    sections_[0].output = layout.first;
    sections_[1].output = layout.second;
    stages_ = layout.stages;
    parallel_ = layout.parallel;
    if (p_.type == 4 || p_.type == 5)
        design(sections_[1], filterCutoffHz(p_.hpfCutoff), 1.0f);
    update(0.0f);
}

void ElementFilter::update(float cents) noexcept
{
    cents_ = cents;
    const float shift = keyCents_ + cents;
    const float hz = filterCutoffHz(p_.cutoff, shift);
    Section& first = sections_[0];
    Section& second = sections_[1];
    switch (p_.type) {
    case 1: // the second section has damping 1
        design(first, hz, damping_);
        design(second, hz, 1.0f);
        return;
    case 2:
        design(first, hz, damping_);
        designOnePole(second, hz);
        return;
    case 3: { // measured: the section sits 4097 cents above the cutoff
        designOnePole(first, hz);
        design(second, hz * 10.66f, damping_);
        return;
    }
    case 4:
        design(first, hz, damping_);
        return;
    case 5:
        designOnePole(first, hz);
        return;
    case 10: { // measured: the sections move apart by 38.2 cents per resonance step above 7.5
        const float spread = std::exp2(std::max(0.0f, 38.2f * (static_cast<float>(p_.resonance) - 7.5f)) / 1200.0f);
        design(first, hz * spread, 1.0f);
        design(second, hz / spread, 1.0f);
        return;
    }
    case 14:
    case 15:
    case 16:
    case 17: { // measured: the second section's index moves by 7 per distance step
        design(first, hz, damping_);
        design(second, filterCutoffHz(p_.cutoff + 7 * p_.distance, shift), damping_);
        return;
    }
    default:
        design(first, hz, damping_);
        // the second section, when used, is the same
        second.f = first.f;
        second.d = first.d;
        second.c1 = first.c1;
        second.c2 = first.c2;
        second.g = first.g;
        second.k = first.k;
        second.cs = first.cs;
        second.inv = first.inv;
    }
}

#if defined(__GNUC__) // vector extensions; other compilers take the per-sample path
namespace {
// One sample of a section whose reference-rate delay is the allpass alone (delay_ 0: 48 and
// 96 kHz), with the delays' states in locals. run()'s equations with the allpass's output
// substituted, so that each state depends on the previous ones through four multiply-adds
// instead of about nine (the loop is bound by that latency); the output is picked by
// 0/1 weights and the one-pole is the same equations with its own coefficients, which
// leaves no branches in the loop.
struct DirectCoefficients {
    // high = inv x - ci band' - ki low'; band = kf high + k band';
    // low = kl low' + kx x + kf band' + kfg band; out = wh high + wb band + wl low
    float inv, ci, ki, kf, k, kl, kx, kfg;
    float c1, c2, c1m, c2m; // allpass of value v: s1 = s2' - c1 s1' + c1m v; s2 = c2m v - c2 s1'
    float wh, wb, wl;
};
// The two channels as the lanes of a vector: the states of both sections then fit in
// registers with the coefficients
typedef float Pair __attribute__((vector_size(8)));
struct DirectState {
    Pair low1, low2, band1, band2;
};
inline Pair directRun(const DirectCoefficients& c, DirectState& s, Pair x) noexcept
{
    const Pair low1 = s.low1, band1 = s.band1;
    const Pair high = c.inv * x - c.ci * band1 - c.ki * low1;
    const Pair band = c.kf * high + c.k * band1;
    const Pair low = (c.kl * low1 + c.kx * x + c.kf * band1) + c.kfg * band;
    // the newest value last, to keep it off the longer path
    s.low1 = (s.low2 - c.c1 * low1) + c.c1m * low;
    s.low2 = c.c2m * low - c.c2 * low1;
    s.band1 = (s.band2 - c.c1 * band1) + c.c1m * band;
    s.band2 = c.c2m * band - c.c2 * band1;
    return c.wh * high + c.wb * band + c.wl * low;
}
} // namespace

template <int Channels, int Mode>
void ElementFilter::runDirect(float* const* channels, size_t n) noexcept
{
    // Mode: 0 no section, 1 one, 2 two in series, 3 two in parallel
    auto coefficients = [](const Section& s) {
        DirectCoefficients c;
        c.c1 = s.c1;
        c.c2 = s.c2;
        c.c1m = s.c1 * (1.0f - s.c2);
        c.c2m = 1.0f - s.c2 * s.c2;
        c.wh = c.wb = c.wl = 0.0f;
        if (s.output == Output::OnePole) { // y = (f x + (1 - f) y') inv
            c.inv = c.ci = c.ki = c.kf = c.k = c.kfg = 0.0f;
            c.kl = (1.0f - s.f) * s.inv;
            c.kx = s.f * s.inv;
            c.wl = 1.0f;
            return c;
        }
        c.inv = s.inv;
        c.ci = s.cs * s.inv;
        c.ki = s.k * s.inv;
        c.kf = s.k * s.f;
        c.k = c.kl = s.k;
        c.kx = 0.0f;
        c.kfg = s.k * s.f * s.g;
        switch (s.output) {
        case Output::High: c.wh = 1.0f; break;
        case Output::Band: c.wb = 1.0f; break;
        case Output::Notch: c.wh = c.wl = 1.0f; break;
        case Output::LowMinusBand: c.wl = 1.0f, c.wb = -1.0f; break;
        default: c.wl = 1.0f;
        }
        return c;
    };
    const DirectCoefficients c0 = coefficients(sections_[0]), c1 = coefficients(sections_[1]);
    auto load = [](const Section& s) {
        return DirectState { Pair { s.low[0].s1, s.low[1].s1 }, Pair { s.low[0].s2, s.low[1].s2 },
            Pair { s.band[0].s1, s.band[1].s1 }, Pair { s.band[0].s2, s.band[1].s2 } };
    };
    auto store = [](Section& s, const DirectState& st) {
        for (int ch = 0; ch < 2; ++ch) {
            s.low[ch].s1 = st.low1[ch], s.low[ch].s2 = st.low2[ch];
            s.band[ch].s1 = st.band1[ch], s.band[ch].s2 = st.band2[ch];
        }
    };
    DirectState a = load(sections_[0]), b = load(sections_[1]);
    const float gain = gain_;
    float* left = channels[0];
    float* right = channels[1];
    for (size_t i = 0; i < n; ++i) {
        Pair x = { left[i], Channels == 2 ? right[i] : 0.0f };
        if (Mode == 1)
            x = directRun(c0, a, x);
        else if (Mode == 2)
            x = directRun(c1, b, directRun(c0, a, x));
        else if (Mode == 3)
            x = directRun(c0, a, x) + directRun(c1, b, x);
        x = gain * x;
        left[i] = x[0];
        if (Channels == 2)
            right[i] = x[1];
    }
    store(sections_[0], a);
    store(sections_[1], b);
}

template <int Channels>
void ElementFilter::runDirect(float* const* channels, size_t n) noexcept
{
    if (stages_ == 0)
        runDirect<Channels, 0>(channels, n);
    else if (parallel_)
        runDirect<Channels, 3>(channels, n);
    else if (stages_ == 1)
        runDirect<Channels, 1>(channels, n);
    else
        runDirect<Channels, 2>(channels, n);
}
#endif

void ElementFilter::process(absl::Span<float> left, absl::Span<float> right, absl::Span<const float> cents) noexcept
{
    if (!filterTypeSupported(p_.type))
        return;
    const bool stereo = !right.empty();
    const size_t n = stereo ? std::min(left.size(), right.size()) : left.size();
    float* const channels[2] = { left.data(), right.data() };
    for (size_t i0 = 0; i0 < n; i0 += kControlInterval) {
        // the coefficients only change with the cutoff
        if (!cents.empty() && i0 < cents.size() && cents[i0] != cents_)
            update(cents[i0]);
        const size_t end = std::min<size_t>(n, i0 + kControlInterval);
#if defined(__GNUC__)
        if (delay_ == 0) {
            float* const sub[2] = { channels[0] + i0, stereo ? channels[1] + i0 : nullptr };
            if (stereo)
                runDirect<2>(sub, end - i0);
            else
                runDirect<1>(sub, end - i0);
            continue;
        }
#endif
        for (size_t i = i0; i < end; ++i) {
            for (int ch = 0; ch < (stereo ? 2 : 1); ++ch) {
                float& sample = channels[ch][i];
                float x = sample;
                if (parallel_) {
                    x = run(sections_[0], ch, sample) + run(sections_[1], ch, sample);
                } else {
                    for (int s = 0; s < stages_; ++s)
                        x = run(sections_[s], ch, x);
                }
                sample = gain_ * x;
            }
        }
    }
}

} // namespace oscairn
} // namespace sfz

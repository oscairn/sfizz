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

ElementFilter::Coefficients ElementFilter::coefficients(float hz, float damping, bool highPass) const noexcept
{
    // The section at the reference rate: F, its damping and its limits
    const double h = std::min<double>(hz, kTopHz);
    const double f = 2.0 * std::sin(kPi * h / kReferenceRate);
    const double d = std::min<double>(damping, dampingLimit(h));
    // Its transfer function in z^-1: low-pass F^2 z^-1, high-pass (1 - z^-1)^2, over
    // 1 - (2 - F^2 - F d) z^-1 + (1 - F d) z^-2
    double b[3] = { 0.0, f * f, 0.0 };
    if (highPass) {
        b[0] = 1.0;
        b[1] = -2.0;
        b[2] = 1.0;
    }
    double a[3] = { 1.0, -(2.0 - f * f - f * d), 1.0 - f * d };
    if (sampleRate_ != kReferenceRate) {
        // z^-1 -> (w - k) / (1 - k w), mapping the cutoff at 44.1 kHz onto the same
        // frequency at the host rate
        const double t44 = 2.0 * std::asin(f / 2.0);
        const double tn = t44 * kReferenceRate / sampleRate_;
        const double k = std::sin((t44 - tn) / 2.0) / std::sin((t44 + tn) / 2.0);
        auto transform = [k](double* p) {
            const double p0 = p[0], p1 = p[1], p2 = p[2];
            p[0] = p0 - k * p1 + k * k * p2;
            p[1] = -2.0 * k * p0 + (1.0 + k * k) * p1 - 2.0 * k * p2;
            p[2] = k * k * p0 - k * p1 + p2;
        };
        transform(b);
        transform(a);
    }
    Coefficients c;
    c.b0 = static_cast<float>(b[0] / a[0]);
    c.b1 = static_cast<float>(b[1] / a[0]);
    c.b2 = static_cast<float>(b[2] / a[0]);
    c.a1 = static_cast<float>(a[1] / a[0]);
    c.a2 = static_cast<float>(a[2] / a[0]);
    return c;
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

void ElementFilter::start(const ElementFilterParameters& parameters, int note) noexcept
{
    p_ = parameters;
    keyCents_ = static_cast<float>(p_.keyFollow - 64) * 3.125f * static_cast<float>(note - 60);
    gain_ = filterGain(p_.gain);
    for (auto& s : states_)
        s = State {};
    damping_ = filterDamping(p_.resonance);
    high_ = coefficients(filterCutoffHz(p_.hpfCutoff), 1.0f, true);
    update(0.0f);
}

void ElementFilter::update(float cents) noexcept
{
    main_ = coefficients(filterCutoffHz(p_.cutoff, keyCents_ + cents), damping_, p_.type == 6 || p_.type == 7);
}

void ElementFilter::process(absl::Span<float> left, absl::Span<float> right, absl::Span<const float> cents) noexcept
{
    if (!filterTypeSupported(p_.type))
        return;
    const bool stereo = !right.empty();
    const size_t n = stereo ? std::min(left.size(), right.size()) : left.size();
    const int stages = (p_.type == 0 || p_.type == 6 || p_.type == 4) ? 2 : 1;
    // Transposed direct form II
    auto step = [](float x, float& s1, float& s2, const Coefficients& c) noexcept {
        const float y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;
        return y;
    };

    for (size_t i = 0; i < n; ++i) {
        if (!cents.empty() && i % kControlInterval == 0 && i < cents.size())
            update(cents[i]);
        for (int ch = 0; ch < (stereo ? 2 : 1); ++ch) {
            float& sample = ch == 0 ? left[i] : right[i];
            float x = sample;
            for (int s = 0; s < stages; ++s)
                x = step(x, states_[s].s1[ch], states_[s].s2[ch], (p_.type == 4 && s == 1) ? high_ : main_);
            sample = gain_ * x;
        }
    }
}

} // namespace oscairn
} // namespace sfz

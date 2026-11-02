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
// Cutoff measured on reference renders (white noise, resonance 0), in cents from 440 Hz.
// shortcut: knots every 16-32 indices from 128 to 832, linear in cents between them and
// 11.5 cents per index outside; replace with the denser fl2 knots
constexpr Knot kCutoffKnots[] = {
    { 128, -4325.f }, { 144, -3952.f }, { 160, -3666.f }, { 192, -3486.f }, { 208, -3360.f },
    { 224, -3117.f }, { 256, -2749.f }, { 272, -2647.f }, { 288, -2502.f }, { 320, -2065.f },
    { 336, -1809.f }, { 352, -1627.f }, { 384, -1356.f }, { 400, -1247.f }, { 416, -1029.f },
    { 448, -519.f }, { 464, -337.f }, { 480, -225.f }, { 512, 23.f }, { 528, 347.f },
    { 544, 598.f }, { 576, 925.f }, { 592, 1057.f }, { 608, 1171.f }, { 640, 1701.f },
    { 656, 1895.f }, { 672, 2059.f }, { 704, 2316.f }, { 720, 2502.f }, { 736, 2802.f },
    { 768, 3214.f }, { 784, 3363.f }, { 800, 3490.f }, { 832, 3906.f },
};
constexpr float kOutsideSlope = 11.5f;

float cutoffCents(int32_t index) noexcept
{
    const int32_t i = std::max<int32_t>(0, std::min<int32_t>(1023, index));
    const Knot* first = std::begin(kCutoffKnots);
    const Knot* last = std::end(kCutoffKnots) - 1;
    if (i <= first->index)
        return first->cents - kOutsideSlope * static_cast<float>(first->index - i);
    if (i >= last->index)
        return last->cents + kOutsideSlope * static_cast<float>(i - last->index);
    const Knot* hi = std::upper_bound(first, last + 1, i, [](int32_t v, const Knot& k) { return v < k.index; });
    const Knot* lo = hi - 1;
    const float f = static_cast<float>(i - lo->index) / static_cast<float>(hi->index - lo->index);
    return lo->cents + f * (hi->cents - lo->cents);
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
    const double f = std::min(1.0, 2.0 * std::sin(kPi * std::min<double>(hz, kReferenceRate / 4) / kReferenceRate));
    const double d = std::min<double>(damping, 1.0 / f);
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

void ElementFilter::start(const ElementFilterParameters& parameters) noexcept
{
    p_ = parameters;
    for (auto& s : states_)
        s = State {};
    damping_ = filterDamping(p_.resonance);
    high_ = coefficients(filterCutoffHz(p_.hpfCutoff), 1.0f, true);
    update(0.0f);
}

void ElementFilter::update(float cents) noexcept
{
    main_ = coefficients(filterCutoffHz(p_.cutoff, cents), damping_, p_.type == 6 || p_.type == 7);
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
            sample = x;
        }
    }
}

} // namespace oscairn
} // namespace sfz

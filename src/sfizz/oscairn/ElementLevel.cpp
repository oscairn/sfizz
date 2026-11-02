// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "ElementLevel.h"
#include <algorithm>
#include <cmath>

namespace sfz {
namespace oscairn {

namespace {
// Velocity curves 0, 1, 2 and 4 in dB at measured velocities (sensitivity +32, offset 0);
// interpolated linearly in dB between them. Curve 3 is (velocity / 127)^2.
// shortcut: knots every 8-16 velocities; intermediate velocities are not checked yet.
constexpr int knotVelocities[12] = { 1, 8, 16, 32, 48, 64, 80, 96, 104, 112, 120, 127 };
constexpr float curveDb[5][12] = {
    { -84.29f, -64.35f, -56.07f, -45.54f, -37.63f, -30.10f, -22.58f, -15.43f, -11.29f, -7.53f, -3.76f, 0.0f },
    { -95.21f, -89.93f, -83.92f, -71.88f, -59.83f, -47.79f, -35.75f, -23.71f, -17.31f, -11.29f, -5.27f, 0.0f },
    { -94.45f, -85.42f, -75.64f, -58.33f, -42.90f, -29.73f, -18.82f, -10.28f, -6.40f, -3.77f, -1.51f, 0.0f },
    {},
    { -95.21f, -89.94f, -83.54f, -71.12f, -57.58f, -30.10f, -9.79f, -5.27f, -3.39f, -2.26f, -1.13f, 0.0f },
};

float curveVelocity(int curve, float v)
{
    if (curve == 3 || curve < 0 || curve > 4)
        return v;
    int i = 1;
    while (i < 11 && v > knotVelocities[i])
        ++i;
    const float a = static_cast<float>(knotVelocities[i - 1]), b = static_cast<float>(knotVelocities[i]);
    const float f = std::max(0.0f, std::min(1.0f, (v - a) / (b - a)));
    const float db = curveDb[curve][i - 1] + f * (curveDb[curve][i] - curveDb[curve][i - 1]);
    return 127.0f * std::pow(10.0f, db / 40.0f); // the velocity whose level is that
}

float spread(int sensitivity)
{
    const float s = static_cast<float>(std::abs(sensitivity));
    return s <= 32.0f ? s / 32.0f : 31.5f / (63.5f - s); // fitted: 1.35 at 40, 2.03 at 48, 4.2 at 56
}
} // namespace

float effectiveVelocity(const ElementVelocity& v, int velocity) noexcept
{
    const float offsetVelocity = std::min(127.0f, static_cast<float>(std::max(0, velocity) + std::max(0, v.offset)));
    const float c = curveVelocity(v.curve, offsetVelocity);
    if (v.sensitivity <= -64) // only a velocity of 1 sounds, about 85 dB down
        return c <= 1.0f ? 1.0f : 0.0f;
    const float k = spread(v.sensitivity);
    float out = 127.0f;
    if (v.sensitivity > 0)
        out = 127.0f - (127.0f - c) * k;
    else if (v.sensitivity < 0)
        out = 127.0f - (c - 1.0f) * k;
    return std::max(0.0f, std::min(127.0f, out));
}

float velocityGain(const ElementVelocity& v, int velocity) noexcept
{
    // (v / 127)^2 on the same 0.375 dB grid, computed at a finer velocity resolution: the
    // effective velocity times 16.05 against 2048 (fitted: velocity 80 lands on 22 steps,
    // 104 on 9)
    const double scaled = effectiveVelocity(v, velocity) * 16.05;
    if (scaled < 1.0)
        return 0.0f;
    const double steps = std::max(0.0, std::round(40.0 * std::log10(2048.0 / scaled) / 0.375));
    return static_cast<float>(std::pow(10.0, -0.375 * steps / 20.0));
}

void balanceGains(int32_t pan, float& left, float& right) noexcept
{
    const int32_t p = std::max<int32_t>(1, std::min<int32_t>(127, pan));
    constexpr float halfPi = 1.5707963267948966f;
    // The far side: sin^2 of the distance from the edge over 64, on the 0.375 dB grid
    auto far = [&](int32_t distance) {
        const double s = std::sin(halfPi * distance / 64.0);
        if (s <= 0.0)
            return 0.0f;
        const double steps = std::round(-40.0 * std::log10(s) / 0.375);
        return static_cast<float>(std::pow(10.0, -0.375 * steps / 20.0));
    };
    left = right = 1.0f;
    if (p < 64)
        right = far(p - 1);
    else if (p > 64)
        left = far(127 - p);
}

} // namespace oscairn
} // namespace sfz

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
// Velocity curves 0, 1, 2 and 4 in dB at measured velocities (sensitivity +32, offset 0):
// velocity 1 and every fourth velocity (renders fl3 vk_*); interpolated linearly in dB
// between them. Curve 3 is (velocity / 127)^2.
// shortcut: velocities between the knots are not checked; curve 4 rises 14 dB per 4
// velocities between 60 and 68
constexpr int kKnots = 33;
constexpr int knotVelocities[kKnots] = { 1, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 127 };
constexpr float curveDb[5][kKnots] = {
    {
        -84.29f, -71.49f, -64.34f, -59.83f, -56.07f, -53.06f, -50.42f, -47.79f, -45.53f, -43.27f, -41.39f, -39.51f,
        -37.63f, -35.37f, -33.49f, -31.61f, -30.10f, -28.22f, -26.34f, -24.46f, -22.58f, -21.07f, -19.19f, -17.31f,
        -15.43f, -13.17f, -11.29f, -9.41f, -7.53f, -5.64f, -3.76f, -1.88f, 0.00f },
    {
        -95.20f, -92.94f, -89.93f, -86.92f, -83.91f, -80.90f, -77.89f, -74.88f, -71.87f, -68.86f, -65.85f, -62.84f,
        -59.83f, -56.82f, -53.81f, -50.80f, -47.79f, -44.78f, -41.77f, -38.76f, -35.75f, -32.74f, -29.73f, -26.72f,
        -23.71f, -20.32f, -17.31f, -14.30f, -11.29f, -8.28f, -5.27f, -2.26f, 0.00f },
    {
        -94.45f, -90.69f, -85.42f, -80.90f, -75.63f, -71.12f, -66.98f, -62.46f, -58.32f, -54.19f, -50.42f, -46.66f,
        -42.90f, -39.51f, -36.12f, -32.74f, -29.73f, -26.72f, -24.08f, -21.45f, -18.81f, -16.56f, -14.30f, -12.04f,
        -10.16f, -8.28f, -6.40f, -5.27f, -3.76f, -2.26f, -1.51f, -0.38f, 0.00f },
    {},
    {
        -95.20f, -92.94f, -89.93f, -86.92f, -83.54f, -80.52f, -77.51f, -74.50f, -71.12f, -67.73f, -64.72f, -61.33f,
        -57.57f, -53.81f, -49.67f, -44.03f, -30.10f, -16.93f, -13.55f, -11.29f, -9.78f, -8.28f, -7.15f, -6.02f,
        -5.27f, -4.14f, -3.39f, -3.01f, -2.26f, -1.51f, -1.13f, -0.38f, 0.00f },
};

float curveVelocity(int curve, float v)
{
    if (curve == 3 || curve < 0 || curve > 4)
        return v;
    int i = 1;
    while (i < kKnots - 1 && v > knotVelocities[i])
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

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
// Velocity curves 0, 1, 2 and 4 in dB at measured velocities (sensitivity +32, offset 0, where
// the scale is the velocity's): velocity 1 and every fourth velocity (renders fl3 vk_*);
// interpolated linearly in dB between them. Curve 3 is (scale / 2047)^2.
// shortcut: points between the knots are not checked; curve 4 rises 14 dB per 4 velocities
// between 60 and 68
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

// A note velocity, or a velocity offset, 1-127 on the 0-2047 scale the response works on:
// 16 per step up to 64, then 1023 / 63 per step (renders lv1 and lv2; checked on sv1)
int32_t scale2048(int32_t v)
{
    v = std::max<int32_t>(0, std::min<int32_t>(127, v));
    return v <= 64 ? 16 * v : static_cast<int32_t>(std::lround(1024.0 + (v - 64) * 1023.0 / 63.0));
}

// The sensitivity's factor, in 2048ths: |s| / 31.75 up to 31, then 31.75 / (63.75 - |s|),
// and 127 at 64 (so that only the end of the range sounds)
int64_t factor2048(int32_t s)
{
    const int32_t a = std::abs(s);
    if (a >= 64)
        return 127 * 2048;
    return std::lround(a < 32 ? 2048.0 * a / 31.75 : 2048.0 * 31.75 / (63.75 - a));
}

// Level in dB of a curve other than 3 at a point of the 0-2047 scale: the measured knots sit
// at the scale of their velocities, linear in dB between them
double curveDbAt(int curve, int32_t r)
{
    int i = 1;
    while (i < kKnots - 1 && r > scale2048(knotVelocities[i]))
        ++i;
    const double a = scale2048(knotVelocities[i - 1]), b = scale2048(knotVelocities[i]);
    const double f = std::max(0.0, std::min(1.0, (r - a) / (b - a)));
    return curveDb[curve][i - 1] + f * (curveDb[curve][i] - curveDb[curve][i - 1]);
}
} // namespace

int32_t velocityScale(const ElementVelocity& v, int velocity) noexcept
{
    const int32_t s = v.sensitivity >= 63 ? 64 : std::max<int32_t>(-64, v.sensitivity); // +63 counts as 64
    const int32_t vel = scale2048(velocity);
    const int32_t offset = v.offset > 0 ? scale2048(v.offset) : 0;
    const int64_t k = factor2048(s);
    // positive sensitivities scale the distance below the top, negative ones the distance
    // above 0, which the offset lowers; the scaled distance is truncated
    const int64_t moved = s >= 0 ? (2047 - std::min(vel + offset, 2047)) * k
                                 : std::max(0, std::min(2047, vel - offset)) * k;
    return 2047 - static_cast<int32_t>(std::min<int64_t>(2047, moved / 2048));
}

float velocityGain(const ElementVelocity& v, int velocity) noexcept
{
    const int32_t r = velocityScale(v, velocity);
    if (r <= 0)
        return 0.0f;
    // on the 0.375 dB grid; curve 3 is (r / 2047)^2, the others the measured curves; 255 steps
    // and more are silent
    const double steps = (v.curve == 3 || v.curve < 0 || v.curve > 4)
        ? std::round(40.0 * std::log10(2047.0 / r) / 0.375)
        : std::round(-curveDbAt(v.curve, r) / 0.375);
    if (steps >= 255.0)
        return 0.0f;
    return static_cast<float>(std::pow(10.0, -0.375 * std::max(0.0, steps) / 20.0));
}

float levelScalingGain(const ElementLevelScaling& s, int key) noexcept
{
    double offset = s.offsets[0];
    if (key >= s.keys[3])
        offset = s.offsets[3];
    else {
        for (int i = 0; i < 3; ++i) {
            if (key >= s.keys[i] && key < s.keys[i + 1]) {
                const double f = static_cast<double>(key - s.keys[i]) / (s.keys[i + 1] - s.keys[i]);
                offset = s.offsets[i] + f * (s.offsets[i + 1] - s.offsets[i]);
            }
        }
    }
    // fitted (renders ks1): offsets interpolated between break points round to whole steps
    // (note 85 between 0 at 84 and -90 at 105 reads -4, note 94 -43); +63 counts as 64
    const int32_t follow = s.keyFollow >= 63 ? 64 : s.keyFollow;
    offset = std::round(offset) + follow * (key - 60) / 32.0;
    if (s.level <= 0)
        return 1.0f;
    auto gridDb = [](double level) {
        return -0.375 * std::round(40.0 * std::log10(127.0 / level) / 0.375);
    };
    const double scaled = std::min(127.0, s.level * (128.0 + offset) / 128.0);
    if (scaled <= 0.0)
        return 0.0f;
    return static_cast<float>(std::pow(10.0, (gridDb(scaled) - gridDb(s.level)) / 20.0));
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

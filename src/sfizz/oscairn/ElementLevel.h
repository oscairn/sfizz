// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// An element's velocity response and pan, as measured on the reference (oscairn/measurement):
//
// - Velocity: the velocity and the offset on a 0-2047 scale (16 per step up to 64, 1023 / 63
//   per step above); a positive sensitivity scales the distance of velocity + offset below
//   2047 by k, a negative one the distance of velocity - offset above 0, with k = |s| / 31.75
//   up to 31, 31.75 / (63.75 - |s|) above and 127 at 64 (+63 counts as 64), the scaled
//   distance truncated. One of five curves then gives the level of the result r: curve 3 is
//   (r / 2047)^2, the level law of the envelope; 0, 1, 2 and 4 are measured.
// - Pan 1-127 (64 centre) is a balance: the far side falls as sin^2 of the distance from the
//   edge (over 64 steps), the near side keeps its level. Levels sit on 0.375 dB steps.

#pragma once
#include <cstdint>

namespace sfz {
namespace oscairn {

struct ElementVelocity {
    int32_t sensitivity { 0 }; // -64..+63
    int32_t offset { 0 };      // 0..127
    int32_t curve { 3 };       // 0..4
};

/**
 * @brief Where a note velocity 1-127 lands on the 0-2047 scale after offset and sensitivity.
 */
int32_t velocityScale(const ElementVelocity& v, int velocity) noexcept;

/**
 * @brief The amplitude gain of a note velocity 1-127.
 */
float velocityGain(const ElementVelocity& v, int velocity) noexcept;

// Level scaling by key: offsets at four break points, linear in key between them (rounded to
// whole steps) and held beyond the outer ones, plus the level key follow sensitivity's
// sensitivity * (key - 60) / 32 steps. An offset multiplies the element level by
// (128 + offset) / 128 (-128 silences), at most up to level 127, on the 0.375 dB grid.
struct ElementLevelScaling {
    int32_t keys[4] { 36, 48, 60, 72 };
    int32_t offsets[4] { 0, 0, 0, 0 }; // -128..+127
    int32_t keyFollow { 0 };           // -64..+63
    int32_t level { 127 };             // the element level, for the limit at 127
};

/**
 * @brief The gain of level scaling for a key, relative to the element level alone.
 */
float levelScalingGain(const ElementLevelScaling& s, int key) noexcept;

/**
 * @brief Left and right gains of a pan value 0-127 (0 and 1 are hard left).
 */
void balanceGains(int32_t pan, float& left, float& right) noexcept;

/**
 * @brief Gain of the reference's output staging relative to sfizz's (-2.075 dB, measured: a
 * full-scale sample at element level 127, Part Volume 127, pan centre plays at -9.43 dB per
 * channel on the reference, mono and stereo alike, and -7.35 dB in sfizz at CC 7 127: the
 * same sample through both differs by 2.07 to 2.09 dB across 44 probes).
 */
constexpr float referenceLevelGain = 0.78750f; // 10^(-2.075 / 20)

} // namespace oscairn
} // namespace sfz

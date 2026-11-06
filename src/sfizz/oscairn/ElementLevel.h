// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// An element's velocity response and pan, as measured on the reference (oscairn/measurement):
//
// - Velocity: the velocity plus the offset (at most 127) goes through one of five curves
//   (curve 3 leaves it as it is); the sensitivity then spreads it: positive values scale the
//   distance below 127 by k, negative ones the distance above 1, with k = s / 32 up to 32 and
//   31.5 / (63.5 - s) above. The gain is (result / 127)^2, the level law of the envelope.
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
 * @brief The effective velocity (0-127, fractional) of a note velocity 1-127.
 */
float effectiveVelocity(const ElementVelocity& v, int velocity) noexcept;

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
 * @brief Gain of the reference's output staging relative to sfizz's (-1.84 dB, measured: a
 * full-scale sample at Part Volume 127, pan centre, plays at -9.19 dB per channel on the
 * reference and -7.35 dB in sfizz at CC 7 127).
 */
constexpr float referenceLevelGain = 0.80910f; // 10^(-1.84 / 20)

} // namespace oscairn
} // namespace sfz

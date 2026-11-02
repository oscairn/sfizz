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
//   edge, the near side keeps its level.

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

/**
 * @brief Left and right gains of a pan value 0-127 (0 and 1 are hard left).
 */
void balanceGains(int32_t pan, float& left, float& right) noexcept;

} // namespace oscairn
} // namespace sfz

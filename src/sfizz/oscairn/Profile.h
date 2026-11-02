// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Oscairn's opt-in extensions (doc/oscairn-plan.md, section 3). A patch enables them with
// `oscairn_profile=1` in a <control> header; regions that come after it may use the
// `oscairn_*` opcodes. Without it every `oscairn_*` opcode stays unknown, as in stock
// sfizz, and nothing here runs.

#pragma once
#include "../Opcode.h"
#include "ElementEnvelope.h"
#include "ElementLevel.h"
#include "ElementPitch.h"

namespace sfz {
namespace oscairn {

struct RegionData {
    // Amplitude envelope with the reference's raw parameters (0-127). Replaces the SFZ amp EG
    // when any of its opcodes is given.
    bool ampegEnabled { false };
    ElementEnvelopeParameters ampeg;
    // Velocity response; multiplies sfizz's own velocity gain (the converter sets
    // amp_veltrack=0 so that only this one acts)
    bool velocityEnabled { false };
    ElementVelocity velocity;
    // Balance-law pan, after sfizz's pan stage
    bool panEnabled { false };
    int32_t pan { 64 };
    // Pitch envelope, added to the voice's pitch in cents
    bool pegEnabled { false };
    ElementPitchParameters peg;
};

/**
 * @brief Handles a <control> opcode of the profile.
 *
 * @return true if the opcode was the profile's
 */
bool parseControlOpcode(const Opcode& opcode, bool& profileEnabled);

/**
 * @brief Whether an opcode is one of the profile's and the profile is enabled: such an
 * opcode is parsed even when an earlier region, before the profile, reported it unknown.
 */
bool enabledFor(bool profileEnabled, const Opcode& opcode);

/**
 * @brief Parses an `oscairn_*` region opcode; only called when the profile is enabled.
 *
 * @return false if the opcode is not one of the profile's
 */
bool parseRegionOpcode(const Opcode& opcode, RegionData& data);

} // namespace oscairn
} // namespace sfz

// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "Profile.h"
#include "../utility/StringViewHelpers.h"
#include <absl/strings/match.h>

namespace sfz {
namespace oscairn {

namespace {
const OpcodeSpec<int32_t> index127 { 0, Range<int32_t>(0, 127), kEnforceBounds };
const OpcodeSpec<int32_t> profileSpec { 0, Range<int32_t>(0, 1), kEnforceBounds };
const OpcodeSpec<int32_t> bipolar64 { 0, Range<int32_t>(-64, 63), kEnforceBounds };
const OpcodeSpec<int32_t> segments { 4, Range<int32_t>(0, 4), kEnforceBounds };
const OpcodeSpec<int32_t> curves { 3, Range<int32_t>(0, 4), kEnforceBounds };
const OpcodeSpec<int32_t> level255 { 128, Range<int32_t>(0, 255), kEnforceBounds };
const OpcodeSpec<int32_t> index1023 { 0, Range<int32_t>(0, 1023), kEnforceBounds };
const OpcodeSpec<int32_t> gain255 { 230, Range<int32_t>(0, 255), kEnforceBounds };
const OpcodeSpec<int32_t> filterTypes { 18, Range<int32_t>(0, 18), kEnforceBounds };
const OpcodeSpec<int32_t> note { 24, Range<int32_t>(0, 127), kEnforceBounds | kCanBeNote };
}

bool parseControlOpcode(const Opcode& opcode, bool& profileEnabled)
{
    if (opcode.lettersOnlyHash != hash("oscairn_profile"))
        return false;
    profileEnabled = opcode.read(profileSpec) == 1;
    return true;
}

bool enabledFor(bool profileEnabled, const Opcode& opcode)
{
    return profileEnabled && absl::StartsWith(opcode.name, "oscairn_");
}

bool parseRegionOpcode(const Opcode& opcode, RegionData& data)
{
    ElementEnvelopeParameters& eg = data.ampeg;
    int32_t* target = nullptr;
    switch (opcode.lettersOnlyHash) {
    case hash("oscairn_ampeg_initial_level"): target = &eg.initialLevel; break;
    case hash("oscairn_ampeg_attack"): target = &eg.attackTime; break;
    case hash("oscairn_ampeg_attack_level"): target = &eg.attackLevel; break;
    case hash("oscairn_ampeg_decay&"):
        if (opcode.parameters.empty() || opcode.parameters.back() < 1 || opcode.parameters.back() > 2)
            return false;
        target = opcode.parameters.back() == 1 ? &eg.decay1Time : &eg.decay2Time;
        break;
    case hash("oscairn_ampeg_decay&_level"):
        if (opcode.parameters.empty() || opcode.parameters.back() < 1 || opcode.parameters.back() > 2)
            return false;
        target = opcode.parameters.back() == 1 ? &eg.decay1Level : &eg.decay2Level;
        break;
    case hash("oscairn_ampeg_release"): target = &eg.releaseTime; break;
    case hash("oscairn_ampeg_veltime_segments"): eg.timeVelocitySegments = opcode.read(segments); data.ampegEnabled = true; return true;
    case hash("oscairn_ampeg_veltime"): eg.timeVelocitySensitivity = opcode.read(bipolar64); data.ampegEnabled = true; return true;
    case hash("oscairn_ampeg_keytime"): eg.timeKeyFollowSensitivity = opcode.read(bipolar64); data.ampegEnabled = true; return true;
    case hash("oscairn_ampeg_keytime_center"): eg.timeKeyFollowCenter = opcode.read(note); data.ampegEnabled = true; return true;
    case hash("oscairn_ampeg_keytime_release"): target = &eg.timeKeyFollowReleaseAdjustment; break;
    case hash("oscairn_amp_velsens"): data.velocity.sensitivity = opcode.read(bipolar64); data.velocityEnabled = true; return true;
    case hash("oscairn_amp_veloffset"): data.velocity.offset = opcode.read(index127); data.velocityEnabled = true; return true;
    case hash("oscairn_amp_velcurve"): data.velocity.curve = opcode.read(curves); data.velocityEnabled = true; return true;
    case hash("oscairn_pan"): data.pan = opcode.read(index127); data.panEnabled = true; return true;
    case hash("oscairn_pitcheg_hold"): data.peg.holdTime = opcode.read(index127); data.pegEnabled = true; return true;
    case hash("oscairn_pitcheg_attack"): data.peg.attackTime = opcode.read(index127); data.pegEnabled = true; return true;
    case hash("oscairn_pitcheg_decay&"):
    case hash("oscairn_pitcheg_decay&_level"): {
        // the opcode hash replaces the segment number with &
        const unsigned n = opcode.parameters.empty() ? 0 : opcode.parameters.back();
        if (n < 1 || n > 2)
            return false;
        const bool level = opcode.lettersOnlyHash == hash("oscairn_pitcheg_decay&_level");
        int32_t& v = level ? (n == 1 ? data.peg.decay1Level : data.peg.decay2Level) : (n == 1 ? data.peg.decay1Time : data.peg.decay2Time);
        v = opcode.read(level ? level255 : index127);
        data.pegEnabled = true;
        return true;
    }
    case hash("oscairn_pitcheg_release"): data.peg.releaseTime = opcode.read(index127); data.pegEnabled = true; return true;
    case hash("oscairn_pitcheg_hold_level"): data.peg.holdLevel = opcode.read(level255); data.pegEnabled = true; return true;
    case hash("oscairn_pitcheg_attack_level"): data.peg.attackLevel = opcode.read(level255); data.pegEnabled = true; return true;
    case hash("oscairn_pitcheg_release_level"): data.peg.releaseLevel = opcode.read(level255); data.pegEnabled = true; return true;
    case hash("oscairn_pitcheg_depth"): data.peg.depth = opcode.read(bipolar64); data.pegEnabled = true; return true;
    case hash("oscairn_fil_type"): data.filter.type = opcode.read(filterTypes); data.filterEnabled = true; return true;
    case hash("oscairn_fil_cutoff"): data.filter.cutoff = opcode.read(index1023); data.filterEnabled = true; return true;
    case hash("oscairn_fil_resonance"): data.filter.resonance = opcode.read(index127); data.filterEnabled = true; return true;
    case hash("oscairn_fil_hpf_cutoff"): data.filter.hpfCutoff = opcode.read(index1023); data.filterEnabled = true; return true;
    case hash("oscairn_fil_keyfollow"): data.filter.keyFollow = opcode.read(index127); data.filterEnabled = true; return true;
    case hash("oscairn_fil_gain"): data.filter.gain = opcode.read(gain255); data.filterEnabled = true; return true;
    default:
        return false;
    }
    *target = opcode.read(index127);
    data.ampegEnabled = true;
    return true;
}

} // namespace oscairn
} // namespace sfz

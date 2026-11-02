// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "Profile.h"
#include "../Config.h"
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
const OpcodeSpec<int32_t> filterTypes { 18, Range<int32_t>(0, 21), kEnforceBounds };
const OpcodeSpec<int32_t> fegCurves { 2, Range<int32_t>(0, 4), kEnforceBounds };
const OpcodeSpec<int32_t> switchSpec { 0, Range<int32_t>(0, 1), kEnforceBounds };
const OpcodeSpec<int32_t> lfoWaves { 1, Range<int32_t>(0, 2), kEnforceBounds };
const OpcodeSpec<int32_t> speed63 { 38, Range<int32_t>(0, 63), kEnforceBounds };
const OpcodeSpec<int32_t> speed415 { 60, Range<int32_t>(0, 415), kEnforceBounds };
const OpcodeSpec<int32_t> ratioSpec { 0, Range<int32_t>(-128, 127), kEnforceBounds };
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
    case hash("oscairn_fil_distance"): data.filter.distance = opcode.read(ratioSpec); data.filterEnabled = true; return true;
    case hash("oscairn_fil_velsens"): data.filter.velocitySensitivity = opcode.read(bipolar64); data.filterEnabled = true; return true;
    case hash("oscairn_fileg_hold"): data.feg.holdTime = opcode.read(index127); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_attack"): data.feg.attackTime = opcode.read(index127); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_release"): data.feg.releaseTime = opcode.read(index127); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_hold_level"): data.feg.holdLevel = opcode.read(level255); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_attack_level"): data.feg.attackLevel = opcode.read(level255); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_release_level"): data.feg.releaseLevel = opcode.read(level255); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_decay&"):
    case hash("oscairn_fileg_decay&_level"): {
        const unsigned n = opcode.parameters.empty() ? 0 : opcode.parameters.back();
        if (n < 1 || n > 2)
            return false;
        const bool level = opcode.lettersOnlyHash == hash("oscairn_fileg_decay&_level");
        int32_t& v = level ? (n == 1 ? data.feg.decay1Level : data.feg.decay2Level) : (n == 1 ? data.feg.decay1Time : data.feg.decay2Time);
        v = opcode.read(level ? level255 : index127);
        data.fegEnabled = true;
        return true;
    }
    case hash("oscairn_fileg_depth"): data.feg.depth = opcode.read(bipolar64); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_veltime_segments"): data.feg.timeVelocitySegments = opcode.read(segments); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_veltime"): data.feg.timeVelocitySensitivity = opcode.read(bipolar64); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_keytime"): data.feg.timeKeyFollowSensitivity = opcode.read(bipolar64); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_keytime_center"): data.feg.timeKeyFollowCenter = opcode.read(note); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_veldepth"): data.fegDepthVelocitySensitivity = opcode.read(bipolar64); data.fegEnabled = true; return true;
    case hash("oscairn_fileg_veldepth_curve"): data.fegDepthVelocityCurve = opcode.read(fegCurves); data.fegEnabled = true; return true;
    case hash("oscairn_elfo_mode"): data.lfo.mode = opcode.read(switchSpec); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_wave"): data.lfo.wave = opcode.read(lfoWaves); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_speed"): data.lfo.speed = opcode.read(speed63); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_xspeed"): data.lfo.extendedSpeed = opcode.read(speed415); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_delay"): data.lfo.delay = opcode.read(index127); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_fade"): data.lfo.fade = opcode.read(index127); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_reset"): data.lfo.keyOnReset = opcode.read(switchSpec); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_pitch_depth"): data.lfo.pitchDepth = opcode.read(index127); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_amp_depth"): data.lfo.ampDepth = opcode.read(index127); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_filter_depth"): data.lfo.filterDepth = opcode.read(index127); data.lfoEnabled = true; return true;
    case hash("oscairn_elfo_pitch_oncc&"):
    case hash("oscairn_elfo_amp_oncc&"): {
        if (opcode.parameters.empty() || opcode.parameters.back() >= config::numCCs)
            return false;
        const bool pitch = opcode.lettersOnlyHash == hash("oscairn_elfo_pitch_oncc&");
        (pitch ? data.lfo.pitchCC : data.lfo.ampCC) = static_cast<int32_t>(opcode.parameters.back());
        (pitch ? data.lfo.pitchRatio : data.lfo.ampRatio) = opcode.read(ratioSpec);
        data.lfoEnabled = true;
        return true;
    }
    default:
        return false;
    }
    *target = opcode.read(index127);
    data.ampegEnabled = true;
    return true;
}

} // namespace oscairn
} // namespace sfz

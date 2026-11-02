// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Oscairn's opt-in extensions (doc/oscairn-plan.md): the profile switch and the element
// amplitude envelope.

#include "sfizz/Synth.h"
#include "sfizz/Region.h"
#include "sfizz/oscairn/ElementEnvelope.h"
#include "sfizz/oscairn/ElementFilter.h"
#include "TestHelpers.h"
#include "catch2/catch.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

using namespace Catch::literals;
using sfz::oscairn::ElementEnvelope;
using sfz::oscairn::ElementEnvelopeParameters;

namespace {

bool isUnknown(const sfz::Synth& synth, const std::string& name)
{
    const auto& u = synth.getUnknownOpcodes();
    return std::find(u.begin(), u.end(), name) != u.end();
}

// Renders the envelope alone for `seconds`, releasing after `hold` seconds.
std::vector<float> renderEnvelope(const ElementEnvelopeParameters& p, double rate, double seconds, double hold)
{
    ElementEnvelope eg;
    eg.setSampleRate(rate);
    eg.start(p, 0);
    eg.release(static_cast<int>(hold * rate));
    std::vector<float> out(static_cast<size_t>(seconds * rate));
    eg.process(absl::MakeSpan(out));
    return out;
}

// First time (s) at which the envelope reaches `gain` (rising) or falls to it.
double crossing(const std::vector<float>& v, double rate, float gain, bool rising, double after = 0.0)
{
    for (size_t i = static_cast<size_t>(after * rate); i < v.size(); ++i)
        if (rising ? v[i] >= gain : v[i] <= gain)
            return static_cast<double>(i) / rate;
    return -1.0;
}

} // namespace

TEST_CASE("[Oscairn] Profile opcodes are unknown without the profile")
{
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(
        <region> sample=*sine oscairn_ampeg_attack=96 oscairn_ampeg_release=80
    )");
    REQUIRE(synth.getNumRegions() == 1);
    REQUIRE(isUnknown(synth, "oscairn_ampeg_attack"));
    REQUIRE(isUnknown(synth, "oscairn_ampeg_release"));
    REQUIRE(!synth.getRegionView(0)->oscairn.ampegEnabled);
}

TEST_CASE("[Oscairn] The profile enables the opcodes for the regions after it")
{
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(
        <region> sample=*sine oscairn_ampeg_attack=10
        <control> oscairn_profile=1
        <region> sample=*sine oscairn_ampeg_attack=96 oscairn_ampeg_decay1=70 oscairn_ampeg_decay2_level=64
            oscairn_ampeg_release=200 oscairn_ampeg_initial_level=12
        <region> sample=*sine
    )");
    REQUIRE(synth.getNumRegions() == 3);
    REQUIRE(!synth.getRegionView(0)->oscairn.ampegEnabled);
    const auto& r = *synth.getRegionView(1);
    REQUIRE(r.oscairn.ampegEnabled);
    REQUIRE(r.oscairn.ampeg.attackTime == 96);
    REQUIRE(r.oscairn.ampeg.decay1Time == 70);
    REQUIRE(r.oscairn.ampeg.decay2Level == 64);
    REQUIRE(r.oscairn.ampeg.releaseTime == 127); // clamped
    REQUIRE(r.oscairn.ampeg.initialLevel == 12);
    REQUIRE(r.oscairn.ampeg.decay2Time == ElementEnvelopeParameters {}.decay2Time);
    REQUIRE(!synth.getRegionView(2)->oscairn.ampegEnabled);
    // The opcode was unknown in the first region, before the profile
    REQUIRE(isUnknown(synth, "oscairn_ampeg_attack"));
    REQUIRE(!isUnknown(synth, "oscairn_ampeg_decay1"));
}

TEST_CASE("[Oscairn] A profile alone changes nothing; reloading clears it")
{
    const std::string plain = "<region> sample=*saw ampeg_attack=0.01 ampeg_release=0.05";
    auto render = [](const std::string& text) {
        sfz::Synth synth;
        synth.setSamplesPerBlock(256);
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", text);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        std::vector<float> out;
        synth.noteOn(0, 60, 100);
        for (int i = 0; i < 40; ++i) {
            if (i == 20)
                synth.noteOff(0, 60, 0);
            synth.renderBlock(buffer);
            out.insert(out.end(), buffer.getConstSpan(0).begin(), buffer.getConstSpan(0).end());
        }
        return out;
    };
    REQUIRE(render(plain) == render("<control> oscairn_profile=1\n" + plain));

    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<control> oscairn_profile=1 <region> sample=*sine oscairn_ampeg_attack=1");
    REQUIRE(synth.getRegionView(0)->oscairn.ampegEnabled);
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<region> sample=*sine oscairn_ampeg_attack=1");
    REQUIRE(!synth.getRegionView(0)->oscairn.ampegEnabled);
}

TEST_CASE("[Oscairn] Element envelope levels")
{
    REQUIRE(sfz::oscairn::levelGain(127) == 1.0f);
    REQUIRE(sfz::oscairn::levelGain(0) == 0.0f);
    // (level / 127)^2 rounded to 0.375 dB steps: 64 is 32 steps, 80 is 21
    REQUIRE(20 * std::log10(sfz::oscairn::levelGain(64)) == Approx(-12.0).margin(1e-4));
    REQUIRE(20 * std::log10(sfz::oscairn::levelGain(80)) == Approx(-21 * 0.375).margin(1e-4));
    REQUIRE(20 * std::log10(sfz::oscairn::levelGain(120)) == Approx(-3 * 0.375).margin(1e-4));
    REQUIRE(sfz::oscairn::levelGain(200) == 1.0f);
}

TEST_CASE("[Oscairn] Element envelope segments take the table times at every sample rate")
{
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        INFO("rate " << rate);
        ElementEnvelopeParameters p;
        p.attackTime = 96;
        p.decay1Time = 80;
        p.decay1Level = 64;
        p.decay2Level = 64;
        p.releaseTime = 72;
        const double attack = sfz::oscairn::attackSeconds(96);
        const double decay60 = sfz::oscairn::decaySecondsPer60dB(80);
        const double release60 = sfz::oscairn::releaseSecondsPer60dB(72);
        const double hold = attack + 2 * decay60;
        const auto v = renderEnvelope(p, rate, hold + 2 * release60, hold);
        const double sustainDb = 20 * std::log10(sfz::oscairn::levelGain(64));
        const double tol = 2.0 / rate;
        // the attack is a straight line in amplitude reaching 1 after `attack` seconds
        REQUIRE(crossing(v, rate, 0.5f, true) == Approx(0.5 * attack).margin(tol));
        REQUIRE(crossing(v, rate, 1.0f, true) == Approx(attack).margin(tol));
        // decay 1 is a straight line in dB to (64/127)^2, lasting a full-scale decay's time
        // (1.5 times the time per 60 dB: 0 dB to the -90 dB floor)
        const float sustain = sfz::oscairn::levelGain(64);
        REQUIRE(crossing(v, rate, std::pow(10.0f, -6.0f / 20.0f), false, attack) - attack == Approx(1.5 * decay60 * 6 / -sustainDb).margin(tol));
        REQUIRE(crossing(v, rate, sustain * 1.000001f, false, attack) - attack == Approx(1.5 * decay60).margin(tol));
        REQUIRE(v[static_cast<size_t>((hold - 0.01) * rate)] == Approx(sustain).epsilon(1e-4));
        // the release is a straight line in dB to -90 dB over 1.5 times release60, then silence
        const double t20 = crossing(v, rate, sustain * std::pow(10.0f, -20.0f / 20.0f), false, hold);
        REQUIRE(t20 - hold == Approx(1.5 * release60 * 20 / (90 + sustainDb)).margin(tol));
        REQUIRE(crossing(v, rate, 0.0f, false, hold) - hold == Approx(1.5 * release60).margin(tol));
        REQUIRE(v.back() == 0.0f);
    }
}

TEST_CASE("[Oscairn] Element envelope: initial level, release before the end of the attack, delay")
{
    const double rate = 48000;
    ElementEnvelopeParameters p;
    p.initialLevel = 127;
    p.attackLevel = 0;
    p.attackTime = 64;
    auto v = renderEnvelope(p, rate, 1.0, 10.0);
    REQUIRE(v[0] == Approx(1.0f).margin(1e-3));
    REQUIRE(crossing(v, rate, 0.0f, false) == Approx(sfz::oscairn::attackSeconds(64)).margin(2 / rate));

    p = ElementEnvelopeParameters {};
    p.attackTime = 127;
    p.releaseTime = 0;
    v = renderEnvelope(p, rate, 1.0, 0.1);
    REQUIRE(v[static_cast<size_t>(0.09 * rate)] > 0.0f);
    REQUIRE(v[static_cast<size_t>(0.09 * rate)] < 0.1f);
    REQUIRE(v.back() == 0.0f);

    ElementEnvelope eg;
    eg.setSampleRate(rate);
    eg.start(ElementEnvelopeParameters {}, 100);
    REQUIRE(eg.getRemainingDelay() == 100);
    std::vector<float> out(200);
    eg.process(absl::MakeSpan(out));
    REQUIRE(out[98] == 0.0f);
    REQUIRE(out[150] > 0.0f);
}

TEST_CASE("[Oscairn] A region with the element envelope plays through it and ends after its release")
{
    sfz::Synth synth;
    synth.setSampleRate(48000);
    synth.setSamplesPerBlock(480);
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(
        <control> oscairn_profile=1
        <region> sample=*sine oscairn_ampeg_attack=80 oscairn_ampeg_release=40 amp_veltrack=0
    )");
    sfz::AudioBuffer<float> buffer { 2, 480 };
    synth.noteOn(0, 69, 127);
    float early = 0, late = 0;
    for (int i = 0; i < 100; ++i) { // 1 s
        synth.renderBlock(buffer);
        const auto s = buffer.getConstSpan(0);
        const float peak = *std::max_element(s.begin(), s.end());
        if (i == 1)
            early = peak;
        if (i == 99)
            late = peak;
    }
    // ~10 ms into a ~320 ms linear attack against the full level a second later
    REQUIRE(early < 0.1f * late);
    REQUIRE(late > 0.0f);
    synth.noteOff(0, 69, 0);
    for (int i = 0; i < 100 && synth.getNumActiveVoices() > 0; ++i)
        synth.renderBlock(buffer);
    REQUIRE(synth.getNumActiveVoices() == 0);
}

TEST_CASE("[Oscairn] Per-note voice count, gain and fade-out")
{
    sfz::Synth synth;
    synth.setSampleRate(48000);
    synth.setSamplesPerBlock(256);
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<region> sample=*sine amp_veltrack=0 ampeg_release=10");
    sfz::AudioBuffer<float> buffer { 2, 256 };
    synth.noteOn(0, 60, 100);
    synth.noteOn(0, 64, 100);
    synth.renderBlock(buffer);
    REQUIRE(synth.getNumActiveVoicesForNote(60) == 1);
    REQUIRE(synth.getNumActiveVoicesForNote(64) == 1);
    REQUIRE(synth.getNumActiveVoicesForNote(61) == 0);
    synth.fadeOutNote(10, 64, 144);
    synth.renderBlock(buffer);
    REQUIRE(synth.getNumActiveVoicesForNote(64) == 0);
    REQUIRE(synth.getNumActiveVoicesForNote(60) == 1);
}

TEST_CASE("[Oscairn] Element velocity response")
{
    using sfz::oscairn::ElementVelocity;
    using sfz::oscairn::effectiveVelocity;
    ElementVelocity v;
    REQUIRE(effectiveVelocity(v, 1) == 127.0f); // sensitivity 0: no response
    v.sensitivity = 32;
    REQUIRE(effectiveVelocity(v, 64) == Approx(64.0f));
    REQUIRE(effectiveVelocity(v, 127) == Approx(127.0f));
    v.sensitivity = 16;
    REQUIRE(effectiveVelocity(v, 1) == Approx(64.0f)); // half the distance below 127
    v.sensitivity = 48;
    REQUIRE(effectiveVelocity(v, 96) == Approx(127.0f - 31.0f * 31.5f / 15.5f));
    v.sensitivity = 63;
    REQUIRE(effectiveVelocity(v, 120) == 0.0f);
    REQUIRE(effectiveVelocity(v, 127) == 127.0f);
    v.sensitivity = -32;
    REQUIRE(effectiveVelocity(v, 1) == Approx(127.0f));
    REQUIRE(effectiveVelocity(v, 64) == Approx(64.0f));
    v.sensitivity = 32;
    v.offset = 16;
    REQUIRE(effectiveVelocity(v, 1) == Approx(17.0f));
    REQUIRE(effectiveVelocity(v, 120) == Approx(127.0f));
    v.offset = 0;
    v.curve = 0; // measured curve: -30.1 dB at 64
    REQUIRE(40 * std::log10(effectiveVelocity(v, 64) / 127.0f) == Approx(-30.10f).margin(0.01f));
    REQUIRE(40 * std::log10(effectiveVelocity(v, 72) / 127.0f) == Approx(-26.34f).margin(0.01f)); // halfway in dB
    // on the 0.375 dB grid at 2048-step resolution: velocity 64 is 32 steps, 80 is 22, 104 is 9
    REQUIRE(20 * std::log10(sfz::oscairn::velocityGain(ElementVelocity { 32, 0, 3 }, 64)) == Approx(-32 * 0.375).margin(1e-4));
    REQUIRE(20 * std::log10(sfz::oscairn::velocityGain(ElementVelocity { 32, 0, 3 }, 80)) == Approx(-22 * 0.375).margin(1e-4));
    REQUIRE(20 * std::log10(sfz::oscairn::velocityGain(ElementVelocity { 32, 0, 3 }, 104)) == Approx(-9 * 0.375).margin(1e-4));
}

TEST_CASE("[Oscairn] Element pan is a balance")
{
    float l, r;
    sfz::oscairn::balanceGains(64, l, r);
    REQUIRE((l == 1.0f && r == 1.0f));
    sfz::oscairn::balanceGains(1, l, r);
    REQUIRE((l == 1.0f && r == 0.0f));
    sfz::oscairn::balanceGains(127, l, r);
    REQUIRE((l == 0.0f && r == 1.0f));
    sfz::oscairn::balanceGains(32, l, r);
    REQUIRE(l == 1.0f);
    REQUIRE(20 * std::log10(r) == Approx(-17 * 0.375).margin(1e-4)); // measured -6.40 dB
    sfz::oscairn::balanceGains(8, l, r);
    REQUIRE(20 * std::log10(r) == Approx(-82 * 0.375).margin(1e-4)); // measured -30.86 dB
    sfz::oscairn::balanceGains(96, l, r);
    REQUIRE(r == 1.0f);
    REQUIRE(20 * std::log10(l) == Approx(-17 * 0.375).margin(1e-4));
}

TEST_CASE("[Oscairn] Velocity and pan opcodes reach the voice")
{
    auto peaks = [](const std::string& text, int velocity) {
        sfz::Synth synth;
        synth.setSamplesPerBlock(512);
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", text);
        sfz::AudioBuffer<float> buffer { 2, 512 };
        synth.noteOn(0, 69, velocity);
        float left = 0, right = 0;
        for (int i = 0; i < 20; ++i) {
            synth.renderBlock(buffer);
            if (i > 10) {
                for (float x : buffer.getConstSpan(0)) left = std::max(left, std::abs(x));
                for (float x : buffer.getConstSpan(1)) right = std::max(right, std::abs(x));
            }
        }
        return std::make_pair(left, right);
    };
    const auto full = peaks("<region> sample=*sine amp_veltrack=0", 64);
    const auto vel = peaks("<control> oscairn_profile=1 <region> sample=*sine amp_veltrack=0 oscairn_amp_velsens=32", 64);
    REQUIRE(vel.first / full.first == Approx(std::pow(10.0f, -12.0f / 20.0f)).epsilon(0.01));
    const auto off = peaks("<region> sample=*sine amp_veltrack=0 oscairn_amp_velsens=32 oscairn_pan=1", 64);
    REQUIRE(off.first == Approx(full.first).epsilon(0.001)); // no profile: unknown, no effect
    REQUIRE(off.second == Approx(full.second).epsilon(0.001));
    const auto left = peaks("<control> oscairn_profile=1 <region> sample=*sine amp_veltrack=0 oscairn_pan=1", 64);
    REQUIRE(left.first == Approx(full.first).epsilon(0.001));
    REQUIRE(left.second < 1e-6f);
}

TEST_CASE("[Oscairn] Pitch envelope levels and segment times")
{
    using sfz::oscairn::pitchLevelCents;
    REQUIRE(pitchLevelCents(144, 63) == Approx(600.0f)); // +63 counts as 64
    REQUIRE(pitchLevelCents(144, 32) == Approx(300.0f));
    REQUIRE(pitchLevelCents(64, 32) == Approx(-1200.0f));
    REQUIRE(pitchLevelCents(255, 32) == Approx(2400.0f)); // 255 counts as +128 steps
    REQUIRE(pitchLevelCents(0, 63) == Approx(-4800.0f));
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        sfz::oscairn::ElementPitchParameters p;
        p.holdLevel = 192;
        p.attackLevel = p.decay1Level = p.decay2Level = 128;
        p.attackTime = 64;
        p.depth = 63;
        p.releaseLevel = 160;
        p.releaseTime = 56;
        sfz::oscairn::ElementPitchEnvelope eg;
        eg.setSampleRate(rate);
        eg.start(p, 0);
        const double full = 1.5 * sfz::oscairn::decaySecondsPer60dB(64);
        eg.release(static_cast<int>(2 * rate));
        std::vector<float> c(static_cast<size_t>(3 * rate), 0.0f);
        eg.processAdd(absl::MakeSpan(c));
        REQUIRE(c[0] == Approx(2400.0f).margin(1.0f));
        REQUIRE(c[static_cast<size_t>(full / 2 * rate)] == Approx(1200.0f).margin(2.0f)); // straight line in cents
        REQUIRE(c[static_cast<size_t>(1.9 * rate)] == Approx(0.0f).margin(0.01f));
        const double rel = 1.5 * sfz::oscairn::decaySecondsPer60dB(56);
        REQUIRE(c[static_cast<size_t>((2 + rel / 2) * rate)] == Approx(600.0f).margin(2.0f));
        REQUIRE(c.back() == Approx(1200.0f).margin(0.01f));
    }
}

TEST_CASE("[Oscairn] Pitch envelope opcodes")
{
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(
        <control> oscairn_profile=1
        <region> sample=*sine oscairn_pitcheg_hold=3 oscairn_pitcheg_attack=70 oscairn_pitcheg_decay1=71 oscairn_pitcheg_decay2=72
            oscairn_pitcheg_release=73 oscairn_pitcheg_hold_level=10 oscairn_pitcheg_attack_level=20 oscairn_pitcheg_decay1_level=30
            oscairn_pitcheg_decay2_level=40 oscairn_pitcheg_release_level=50 oscairn_pitcheg_depth=-12
    )");
    const auto& peg = synth.getRegionView(0)->oscairn.peg;
    REQUIRE(synth.getRegionView(0)->oscairn.pegEnabled);
    REQUIRE(peg.holdTime == 3);
    REQUIRE(peg.attackTime == 70);
    REQUIRE(peg.decay1Time == 71);
    REQUIRE(peg.decay2Time == 72);
    REQUIRE(peg.releaseTime == 73);
    REQUIRE(peg.holdLevel == 10);
    REQUIRE(peg.attackLevel == 20);
    REQUIRE(peg.decay1Level == 30);
    REQUIRE(peg.decay2Level == 40);
    REQUIRE(peg.releaseLevel == 50);
    REQUIRE(peg.depth == -12);
    REQUIRE(synth.getUnknownOpcodes().empty());
}

TEST_CASE("[Oscairn] Attack time between two levels")
{
    using sfz::oscairn::attackDurationFactor;
    using sfz::oscairn::levelGain;
    REQUIRE(attackDurationFactor(0, 127) == Approx(1.0f));
    REQUIRE(attackDurationFactor(0, 64) == Approx(levelGain(64) * 1.166f));
    REQUIRE(attackDurationFactor(64, 127) == Approx(attackDurationFactor(127, 64))); // the same up and down
    REQUIRE(attackDurationFactor(32, 127) == Approx(3.82f).epsilon(0.02));
    REQUIRE(attackDurationFactor(32, 64) == Approx(levelGain(64) * attackDurationFactor(64, 127)).epsilon(0.02));
    REQUIRE(attackDurationFactor(0, 0) == 0.0f);
}

TEST_CASE("[Oscairn] Envelope time shifts for velocity and key")
{
    sfz::oscairn::ElementEnvelopeParameters p;
    auto shifts = sfz::oscairn::timeShifts(p, 60, 127);
    REQUIRE((shifts.attack == 0 && shifts.decay == 0 && shifts.release == 0));
    p.timeVelocitySensitivity = 32;
    shifts = sfz::oscairn::timeShifts(p, 60, 64);
    REQUIRE((shifts.attack == 16 && shifts.decay == 16 && shifts.release == 16)); // segments "all"
    p.timeVelocitySegments = 0; // attack only
    shifts = sfz::oscairn::timeShifts(p, 60, 32);
    REQUIRE((shifts.attack == 8 && shifts.decay == 0 && shifts.release == 0));
    p.timeVelocitySegments = 2; // decay only
    shifts = sfz::oscairn::timeShifts(p, 60, 32);
    REQUIRE((shifts.attack == 0 && shifts.decay == 8 && shifts.release == 0));
    p.timeVelocitySensitivity = 0;
    p.timeKeyFollowSensitivity = 63;
    p.timeKeyFollowCenter = 60;
    shifts = sfz::oscairn::timeShifts(p, 84, 127);
    REQUIRE((shifts.attack == 24 && shifts.decay == 24));
    REQUIRE(shifts.release == 6); // release adjustment 64: a quarter
    p.timeKeyFollowReleaseAdjustment = 127;
    REQUIRE(sfz::oscairn::timeShifts(p, 84, 127).release == 24);
    p.timeKeyFollowReleaseAdjustment = 0;
    REQUIRE(sfz::oscairn::timeShifts(p, 84, 127).release == 1);
    p.timeKeyFollowSensitivity = -32;
    REQUIRE(sfz::oscairn::timeShifts(p, 36, 127).attack == 12); // lower notes, shorter times with negative values
}

TEST_CASE("[Oscairn] Envelope velocity shifts truncate, +63 counts as 64")
{
    sfz::oscairn::ElementEnvelopeParameters p;
    p.timeVelocitySensitivity = 32;
    REQUIRE(sfz::oscairn::timeShifts(p, 60, 127).decay == 31); // 31.75
    REQUIRE(sfz::oscairn::timeShifts(p, 60, 66).decay == 16);  // 16.5
    p.timeVelocitySensitivity = 63;
    REQUIRE(sfz::oscairn::timeShifts(p, 60, 60).decay == 30);
    REQUIRE(sfz::oscairn::timeShifts(p, 60, 65).decay == 32);
    p.timeVelocitySensitivity = -32;
    REQUIRE(sfz::oscairn::timeShifts(p, 60, 127).decay == -31); // toward zero
}

TEST_CASE("[Oscairn] Pitch envelope hold")
{
    REQUIRE(sfz::oscairn::pitchHoldSeconds(0) == 0.0f);
    REQUIRE(sfz::oscairn::pitchHoldSeconds(64) == Approx(0.421f));
    REQUIRE(sfz::oscairn::pitchHoldSeconds(48) == Approx(std::sqrt(0.075f * 0.421f)));
    sfz::oscairn::ElementPitchParameters p;
    p.holdLevel = 192;
    p.attackLevel = p.decay1Level = p.decay2Level = 128;
    p.holdTime = 64;
    p.attackTime = 64;
    p.depth = 63;
    sfz::oscairn::ElementPitchEnvelope eg;
    eg.setSampleRate(48000);
    eg.start(p, 0);
    std::vector<float> c(48000, 0.0f);
    eg.processAdd(absl::MakeSpan(c));
    REQUIRE(c[static_cast<size_t>(0.41 * 48000)] == Approx(2400.0f));
    REQUIRE(c[static_cast<size_t>(0.6 * 48000)] < 2400.0f);
}

namespace {
struct ShiftResult {
    int position { -1 };
    bool finite { true };
    int voices { 0 };
};

// Renders `blocks` blocks of one note on `region` with the pitch wheel at `wheel`
ShiftResult renderShift(const std::string& region, int wheel, int blocks, int blockSize = 1024)
{
    sfz::Synth synth;
    synth.setSampleRate(44100);
    synth.setSamplesPerBlock(blockSize);
    // whole files in memory: at these ratios playback reaches the end of a file at once,
    // and this test is about pitch range, not streaming
    synth.setPreloadSize(1 << 20);
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", region);
    sfz::AudioBuffer<float> buffer { 2, static_cast<unsigned>(blockSize) };
    synth.pitchWheel(0, wheel);
    synth.noteOn(0, 60, 100);
    ShiftResult r;
    for (int b = 0; b < blocks; ++b) {
        synth.renderBlock(buffer);
        for (int c = 0; c < 2; ++c)
            for (float s : buffer.getConstSpan(c))
                r.finite = r.finite && std::isfinite(s);
    }
    r.voices = synth.getNumActiveVoices();
    if (const sfz::Voice* v = synth.getVoiceView(0))
        r.position = v->getSourcePosition();
    return r;
}
} // namespace

TEST_CASE("[Oscairn] Note shift and bend range are not limited to the reference's ranges")
{
    // Decision 17: wider than the reference's +-48 semitone note shift and -48..+24 semitone bend
    // range; the native opcodes stay permissive and nothing in playback clamps the ratio.
    const std::string flute = "<region> sample=looped_flute.wav pitch_keycenter=60 loop_mode=no_loop ";
    // 100 blocks of 1024 frames down 60 semitones and a full -24 semitone bend: 1/128 speed
    auto low = renderShift(flute + "transpose=-60 bend_down=-2400", -8192, 100);
    REQUIRE(low.finite);
    REQUIRE(low.position == Approx(102400.0 / 128.0).margin(1.0));
    // Down 127 semitones: 102400 * 2^(-127/12) = 66.5 frames
    auto lowest = renderShift(flute + "transpose=-127", 0, 100);
    REQUIRE(lowest.position == Approx(102400.0 * std::exp2(-127.0 / 12.0)).margin(1.0));
    // Down 127 semitones and a -120 semitone bend: still playing, still finite
    auto floor = renderShift(flute + "transpose=-127 bend_down=-12000", -8192, 20);
    REQUIRE(floor.finite);
    REQUIRE(floor.voices == 1);
    REQUIRE(floor.position <= 1);
    // Up 127 semitones plus a +120 semitone bend on a loop: wraps many times per frame
    for (int blockSize : { 64, 1024, 4096 }) {
        auto high = renderShift("<region> sample=looped_flute.wav pitch_keycenter=60 loop_mode=loop_continuous loop_start=4 loop_end=124 transpose=127 bend_up=12000", 8191, 10, blockSize);
        REQUIRE(high.finite);
        REQUIRE(high.voices == 1);
        REQUIRE(high.position >= 0);
        REQUIRE(high.position <= 124);
    }
    // Up 127 semitones without a loop: the one-shot runs out at once and the voice ends
    auto shot = renderShift("<region> sample=kick.wav pitch_keycenter=60 transpose=127", 0, 20);
    REQUIRE(shot.finite);
    REQUIRE(shot.voices == 0);
}

namespace {
// Steady-state gain (dB) of the element filter for a sine at `hz`, relative to its passband
// (the filter gain taken out)
double filterGainDb(const sfz::oscairn::ElementFilterParameters& p, double rate, double hz, int note = 60)
{
    sfz::oscairn::ElementFilter filter;
    filter.setSampleRate(rate);
    filter.start(p, note);
    const size_t n = static_cast<size_t>(rate);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i)
        x[i] = static_cast<float>(std::sin(2.0 * M_PI * hz * static_cast<double>(i) / rate));
    filter.process(absl::MakeSpan(x), {}, {});
    double in = 0.0, out = 0.0;
    for (size_t i = n / 2; i < n; ++i) {
        const double s = std::sin(2.0 * M_PI * hz * static_cast<double>(i) / rate);
        in += s * s;
        out += static_cast<double>(x[i]) * x[i];
    }
    return 10.0 * std::log10(out / in) - 20.0 * std::log10(sfz::oscairn::filterGain(p.gain));
}
} // namespace

TEST_CASE("[Oscairn] Element filter responses match the reference")
{
    // Reference renders (white noise) at 250, 500, 1000 and 2000 Hz, relative to the passband;
    // proposed tolerance 1 dB (largest residual 0.85 dB), at 44.1, 48, 88.2, 96 and 192 kHz.
    struct Case {
        int type, cutoff, resonance;
        double db[4];
    };
    const Case cases[] = {
        { 4, 512, 0, { -2.81, -6.73, -15.27, -25.26 } },
        { 4, 512, 64, { 2.71, 11.65, -12.06, -24.90 } },
        { 0, 576, 0, { -1.98, -6.18, -17.38, -34.49 } },
        { 6, 576, 0, { -39.93, -20.67, -7.43, -1.07 } },
        { 7, 576, 0, { -19.88, -10.31, -3.72, -0.58 } },
    };
    const double hz[4] = { 250.0, 500.0, 1000.0, 2000.0 };
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        for (const Case& c : cases) {
            sfz::oscairn::ElementFilterParameters p;
            p.type = c.type;
            p.cutoff = c.cutoff;
            p.resonance = c.resonance;
            for (int k = 0; k < 4; ++k) {
                INFO("rate " << rate << " type " << c.type << " cutoff " << c.cutoff << " resonance " << c.resonance << " at " << hz[k] << " Hz");
                REQUIRE(filterGainDb(p, rate, hz[k]) == Approx(c.db[k]).margin(1.0));
            }
        }
    }
}

TEST_CASE("[Oscairn] Element filter near the top of the range, key follow and gain")
{
    using namespace sfz::oscairn;
    // Reference renders relative to their passband; proposed tolerance 1 dB, all five rates
    struct Case {
        int cutoff, keyFollow, note;
        double hz[4], db[4];
    };
    const Case cases[] = {
        { 864, 64, 60, { 2000, 4000, 6000, 8000 }, { -0.05, -2.44, -3.53, -4.49 } }, // damping limited to 1.47
        { 880, 64, 60, { 2000, 4000, 6000, 8000 }, { -0.44, -1.31, -2.44, -3.36 } }, // 1.32
        { 624, 64, 60, { 500, 1000, 2000, 4000 }, { -1.96, -5.88, -13.22, -23.08 } },
        { 512, 96, 84, { 500, 1000, 2000, 4000 }, { -1.21, -2.39, -6.69, -14.03 } }, // +100 cents per key
        { 512, 32, 36, { 250, 500, 1000, 2000 }, { 0.02, -0.51, -2.23, -6.78 } }, // -100 cents per key
    };
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        for (const Case& c : cases) {
            ElementFilterParameters p;
            p.type = 4;
            p.cutoff = c.cutoff;
            p.keyFollow = c.keyFollow;
            for (int k = 0; k < 4; ++k) {
                INFO("rate " << rate << " cutoff " << c.cutoff << " key follow " << c.keyFollow << " note " << c.note << " at " << c.hz[k] << " Hz");
                REQUIRE(filterGainDb(p, rate, c.hz[k], c.note) == Approx(c.db[k]).margin(1.0));
            }
        }
    }
    // Filter gain: passband levels measured at the default 230 and at 255; 0 is silent
    REQUIRE(20 * std::log10(filterGain(230)) == Approx(-5.45).margin(0.01));
    REQUIRE(20 * std::log10(filterGain(255)) == Approx(3.25).margin(0.01));
    REQUIRE(filterGain(0) == 0.0f);
    // The top of the range stays at 7290 Hz
    REQUIRE(filterCutoffHz(1023) == Approx(7290.0f).epsilon(1e-4));
    REQUIRE(filterCutoffHz(960) == Approx(7290.0f).epsilon(1e-4));
}

TEST_CASE("[Oscairn] Element filter cutoff and damping laws")
{
    using namespace sfz::oscairn;
    // Measured: index 512 is 446 Hz and 576 is 751 Hz; damping halves every 16 resonance steps
    REQUIRE(filterCutoffHz(512) == Approx(445.9f).epsilon(0.01));
    REQUIRE(filterCutoffHz(576) == Approx(750.7f).epsilon(0.01));
    REQUIRE(filterCutoffHz(512, 1200.0f) == Approx(2.0f * filterCutoffHz(512)).epsilon(1e-4));
    REQUIRE(filterDamping(0) == Approx(2.0f));
    REQUIRE(filterDamping(32) == Approx(0.5f));
    REQUIRE(filterDamping(64) == Approx(0.125f));
    // Types without a model pass the signal through unchanged
    REQUIRE_FALSE(filterTypeSupported(18));
    ElementFilterParameters thru;
    REQUIRE(std::abs(filterGainDb(thru, 48000.0, 1000.0) + 20.0 * std::log10(filterGain(thru.gain))) < 1e-4);
}

TEST_CASE("[Oscairn] The element filter only acts with the profile")
{
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<region> sample=*sine oscairn_fil_type=0 oscairn_fil_cutoff=100");
    REQUIRE_FALSE(synth.getRegionView(0)->oscairn.filterEnabled);
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<control> oscairn_profile=1 <region> sample=*sine oscairn_fil_type=0 oscairn_fil_cutoff=100 oscairn_fil_resonance=32 oscairn_fil_hpf_cutoff=7 oscairn_fil_keyfollow=96 oscairn_fil_gain=200");
    const auto& f = synth.getRegionView(0)->oscairn;
    REQUIRE(f.filterEnabled);
    REQUIRE(f.filter.type == 0);
    REQUIRE(f.filter.cutoff == 100);
    REQUIRE(f.filter.resonance == 32);
    REQUIRE(f.filter.hpfCutoff == 7);
    REQUIRE(f.filter.keyFollow == 96);
    REQUIRE(f.filter.gain == 200);
}

namespace {
// Gains (dB, filter gain taken out) of the element filter at `hz`, from its impulse response
// under an exponential window with a 0.25 s time constant (the response smoothed over about
// 1.3 Hz, the same at every rate, so that truncation does not leak around sharp peaks), until
// the windowed response has fallen 200 dB below its largest value or for 4 s
std::vector<double> filterResponseDb(const sfz::oscairn::ElementFilterParameters& p, double rate, const std::vector<double>& hz)
{
    sfz::oscairn::ElementFilter filter;
    filter.setSampleRate(rate);
    filter.start(p, 60);
    const size_t total = static_cast<size_t>(std::lround(4.0 * rate));
    const size_t block = 4096;
    std::vector<float> x(block);
    std::vector<std::complex<double>> sum(hz.size()), phasor(hz.size()), step(hz.size());
    const double decay = std::exp(-1.0 / (0.25 * rate));
    double weight = 1.0, largest = 0.0;
    for (size_t k = 0; k < hz.size(); ++k) {
        phasor[k] = 1.0;
        step[k] = std::polar(decay, -2.0 * M_PI * hz[k] / rate);
    }
    for (size_t done = 0; done < total; done += block) {
        std::fill(x.begin(), x.end(), 0.0f);
        if (done == 0)
            x[0] = 1.0f;
        filter.process(absl::MakeSpan(x), {}, {});
        const size_t n = std::min(block, total - done);
        double blockLargest = 0.0;
        for (size_t i = 0; i < n; ++i) {
            blockLargest = std::max(blockLargest, std::abs(static_cast<double>(x[i])) * weight);
            weight *= decay;
            for (size_t k = 0; k < hz.size(); ++k) {
                sum[k] += static_cast<double>(x[i]) * phasor[k];
                phasor[k] *= step[k];
            }
        }
        largest = std::max(largest, blockLargest);
        if (blockLargest < largest * 1e-10)
            break;
    }
    std::vector<double> db(hz.size());
    for (size_t k = 0; k < hz.size(); ++k)
        db[k] = 20.0 * std::log10(std::max(std::abs(sum[k]), 1e-30)) - 20.0 * std::log10(sfz::oscairn::filterGain(p.gain));
    return db;
}
} // namespace

TEST_CASE("[Oscairn] The element filter follows its 44.1 kHz response at every rate")
{
    // Accepted tolerances (Jacob, 2026-11-01): within 0.75 dB of the 44.1 kHz response where
    // that is within 20 dB of its peak, 1.5 dB from 20 to 40 dB below it; every type, the
    // whole cutoff and resonance ranges, tones from 20 Hz to 20 kHz
    std::vector<double> hz;
    for (int k = 0; k <= 40; ++k)
        hz.push_back(20.0 * std::pow(1000.0, k / 40.0));
    std::vector<int> cutoffs;
    for (int c = 0; c < 1024; c += 32)
        cutoffs.push_back(c);
    for (int c : { 816, 864, 880, 896, 912, 1023 })
        cutoffs.push_back(c);
    for (int type : { 0, 4, 6, 7 }) {
        for (int cutoff : cutoffs) {
            for (int resonance : { 0, 16, 32, 48, 64, 80, 96, 112, 127 }) {
                sfz::oscairn::ElementFilterParameters p;
                p.type = type;
                p.cutoff = cutoff;
                p.resonance = resonance;
                p.hpfCutoff = type == 4 ? cutoff / 2 : 0;
                const auto reference = filterResponseDb(p, 44100.0, hz);
                const double peak = *std::max_element(reference.begin(), reference.end());
                for (double rate : { 48000.0, 88200.0, 96000.0, 192000.0 }) {
                    const auto got = filterResponseDb(p, rate, hz);
                    for (size_t k = 0; k < hz.size(); ++k) {
                        if (reference[k] < peak - 40.0)
                            continue;
                        INFO("type " << type << " cutoff " << cutoff << " resonance " << resonance << " " << hz[k] << " Hz at " << rate);
                        REQUIRE(got[k] == Approx(reference[k]).margin(reference[k] >= peak - 20.0 ? 0.75 : 1.5));
                    }
                }
            }
        }
    }
}

TEST_CASE("[Oscairn] A profile voice sounds the same at every sample rate")
{
    // Envelope, velocity, balance pan, pitch envelope and filter together: the steady level
    // and the time the release reaches -20 dB agree with 44.1 kHz at 48, 88.2, 96 and 192 kHz
    const char* text = R"(<control> oscairn_profile=1
        <region> sample=*sine pitch_keycenter=60 oscairn_ampeg_attack=40 oscairn_ampeg_decay1_level=96
        oscairn_ampeg_decay2_level=96 oscairn_ampeg_release=64 oscairn_amp_velsens=20 oscairn_pan=40
        oscairn_pitcheg_hold_level=140 oscairn_pitcheg_attack=60 oscairn_pitcheg_depth=32
        oscairn_fil_type=0 oscairn_fil_cutoff=560 oscairn_fil_resonance=24)";
    struct Result {
        double left, right, release;
    };
    auto run = [&](double rate) {
        sfz::Synth synth;
        synth.setSampleRate(static_cast<float>(rate));
        synth.setSamplesPerBlock(256);
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", text);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        std::vector<float> l, r;
        synth.noteOn(0, 60, 90);
        const size_t total = static_cast<size_t>(2.5 * rate), off = static_cast<size_t>(1.0 * rate);
        for (size_t done = 0; done < total; done += 256) {
            if (done <= off && off < done + 256)
                synth.noteOff(static_cast<int>(off - done), 60, 0);
            synth.renderBlock(buffer);
            l.insert(l.end(), buffer.getConstSpan(0).begin(), buffer.getConstSpan(0).end());
            r.insert(r.end(), buffer.getConstSpan(1).begin(), buffer.getConstSpan(1).end());
        }
        auto rms = [&](const std::vector<float>& v, double t0, double t1) {
            double s = 0.0;
            const size_t a = static_cast<size_t>(t0 * rate), b = static_cast<size_t>(t1 * rate);
            for (size_t i = a; i < b; ++i)
                s += static_cast<double>(v[i]) * v[i];
            return std::sqrt(s / static_cast<double>(b - a));
        };
        Result res { rms(l, 0.8, 0.95), rms(r, 0.8, 0.95), -1.0 };
        const double target = res.left * 0.1;
        for (double t = 1.0; t < 2.4; t += 0.005) {
            if (rms(l, t, t + 0.02) <= target) {
                res.release = t - 1.0;
                break;
            }
        }
        return res;
    };
    const Result ref = run(44100.0);
    REQUIRE(ref.release > 0.0);
    for (double rate : { 48000.0, 88200.0, 96000.0, 192000.0 }) {
        INFO("rate " << rate);
        const Result r = run(rate);
        REQUIRE(20 * std::log10(r.left / ref.left) == Approx(0.0).margin(0.1));
        REQUIRE(20 * std::log10(r.right / ref.right) == Approx(0.0).margin(0.1));
        REQUIRE(r.release == Approx(ref.release).margin(0.01));
    }
}

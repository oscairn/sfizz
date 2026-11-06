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
#include "sfizz/oscairn/ElementLevel.h"
#include "sfizz/oscairn/ElementLfo.h"
#include "sfizz/oscairn/ElementPitch.h"
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
    REQUIRE(40 * std::log10(effectiveVelocity(v, 72) / 127.0f) == Approx(-26.34f).margin(0.01f));
    REQUIRE(40 * std::log10(effectiveVelocity(v, 74) / 127.0f) == Approx(-25.40f).margin(0.01f)); // halfway in dB
    // curve 4 between the old knots 48, 64 and 80 (K2): measured -49.67 dB at 56, -13.55 at 72
    v.curve = 4;
    REQUIRE(40 * std::log10(effectiveVelocity(v, 56) / 127.0f) == Approx(-49.67f).margin(0.01f));
    REQUIRE(40 * std::log10(effectiveVelocity(v, 72) / 127.0f) == Approx(-13.55f).margin(0.01f));
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
    // accepted tolerance 1 dB (largest residual 0.85 dB), at 44.1, 48, 88.2, 96 and 192 kHz.
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

TEST_CASE("[Oscairn] Element filter types against the reference")
{
    // Reference renders (white noise, fl1-fl3) relative to the passband, two per type, at up to
    // four tones (notch centres left out); accepted tolerance 1 dB, at every rate. These are
    // renders the models were chosen from; the points outside 1 dB are in the plan's register
    struct Case {
        int type, cutoff, resonance;
        std::vector<double> hz, db;
    };
    const Case cases[] = {
        { 1, 512, 32, { 223, 446, 892, 111 }, { 2.49, 5.89, -20.53, 0.48 } },
        { 1, 768, 64, { 1408, 2817, 5633, 704 }, { 3.30, 18.15, -17.70, 1.41 } },
        { 2, 512, 64, { 446, 892, 111 }, { 14.46, -16.38, -0.18 } },
        { 2, 768, 0, { 1408, 2817, 5633, 11266 }, { -3.01, -8.20, -17.13, -28.18 } },
        { 3, 512, 32, { 223, 446, 892, 1784 }, { -1.52, -3.09, -6.85, -11.30 } },
        { 3, 512, 64, { 223, 446, 892, 1784 }, { -1.20, -2.99, -6.60, -10.83 } },
        { 5, 384, 0, { 101, 201, 402, 804 }, { -0.70, -3.45, -6.98, -11.78 } },
        { 5, 768, 0, { 1408, 2817, 5633, 11266 }, { -0.65, -2.17, -5.44, -9.61 } },
        { 8, 512, 64, { 223, 446, 892, 1784 }, { -7.41, 35.21, -6.96, -22.90 } },
        { 8, 640, 0, { 588, 1175, 2351, 4701 }, { -16.25, -12.30, -14.63, -22.26 } },
        { 10, 512, 32, { 446, 892, 1784 }, { 2.21, -0.76, -12.79 } },
        { 10, 768, 64, { 1408, 2817, 5633, 11266 }, { 1.32, 0.67, 0.45, 0.36 } },
        { 11, 512, 64, { 223, 446, 892, 1784 }, { -4.11, 17.34, -3.75, -11.64 } },
        { 11, 384, 0, { 101, 201, 402, 804 }, { -8.38, -5.97, -8.01, -12.99 } },
        { 12, 512, 0, { 892, 1784, 111 }, { -8.63, -1.38, -2.80 } },
        { 12, 640, 0, { 588, 2351, 4701, 294 }, { -9.01, -7.64, 0.42, -2.07 } },
        { 13, 512, 64, { 223, 892, 1784 }, { -0.41, -0.13, -0.07 } },
        { 13, 384, 0, { 101, 402, 804 }, { -4.47, -4.51, -1.23 } },
        { 14, 512, 64, { 446, 892, 1784 }, { 23.27, -3.56, -17.49 } },
        { 14, 640, 0, { 588, 1175, 2351, 4701 }, { 4.51, -0.02, -7.31, -17.02 } },
        { 15, 512, 64, { 223, 446, 892, 1784 }, { -3.95, 23.45, 8.38, 6.44 } },
        { 15, 384, 0, { 101, 201, 402, 804 }, { -8.28, -0.38, 4.14, 6.15 } },
        { 16, 512, 64, { 223, 446, 892, 1784 }, { 2.24, 23.46, 2.34, -5.62 } },
        { 16, 640, 0, { 588, 1175, 2351, 4701 }, { -2.18, -0.27, -1.42, -5.20 } },
        { 17, 512, 64, { 892, 1784 }, { -0.18, -0.10 } },
        { 17, 384, 0, { 101, 402, 804, 1608 }, { -9.39, -8.65, -1.58, -0.05 } },
    };
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        for (const Case& c : cases) {
            sfz::oscairn::ElementFilterParameters p;
            p.type = c.type;
            p.cutoff = c.cutoff;
            p.resonance = c.resonance;
            p.keyFollow = 64;
            for (size_t k = 0; k < c.hz.size(); ++k) {
                INFO("rate " << rate << " type " << c.type << " cutoff " << c.cutoff << " resonance " << c.resonance << " at " << c.hz[k] << " Hz");
                REQUIRE(filterGainDb(p, rate, c.hz[k]) == Approx(c.db[k]).margin(1.0));
            }
        }
    }
    // Type 21 applies the filter gain only; type 9 and 20 pass the signal through untouched
    sfz::oscairn::ElementFilterParameters p;
    p.type = 21;
    REQUIRE(filterGainDb(p, 48000.0, 1000.0) == Approx(0.0).margin(1e-3));
    REQUIRE(sfz::oscairn::filterTypeSupported(21));
    REQUIRE_FALSE(sfz::oscairn::filterTypeSupported(9));
    REQUIRE_FALSE(sfz::oscairn::filterTypeSupported(20));
}

TEST_CASE("[Oscairn] Element filter near the top of the range, key follow and gain")
{
    using namespace sfz::oscairn;
    // Reference renders relative to their passband; accepted tolerance 1 dB, all five rates
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
    std::vector<int> coarse;
    for (int c = 0; c < 1024; c += 64)
        coarse.push_back(c);
    for (int c : { 816, 880, 912, 1023 })
        coarse.push_back(c);
    for (int type : { 0, 4, 6, 7, 1, 2, 3, 5, 8, 10, 11, 12, 13, 14, 15, 16, 17, 19, 21 }) {
        // the first four on the full grid, the others on a coarser one
        const bool full = type == 0 || type == 4 || type == 6 || type == 7;
        const std::vector<int> resonances = full ? std::vector<int> { 0, 16, 32, 48, 64, 80, 96, 112, 127 } : std::vector<int> { 0, 32, 64, 96, 127 };
        for (int cutoff : full ? cutoffs : coarse) {
            for (int resonance : resonances) {
                sfz::oscairn::ElementFilterParameters p;
                p.type = type;
                p.cutoff = cutoff;
                p.resonance = resonance;
                p.hpfCutoff = type == 4 || type == 5 ? cutoff / 2 : 0;
                p.distance = type >= 14 && type <= 17 ? 40 : 0;
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

namespace {
// The element LFO's pitch cents over `seconds` at `rate`, with the controllers at `cc` (0-1)
std::vector<float> lfoPitch(const sfz::oscairn::ElementLfoParameters& p, double rate, double seconds, float cc, unsigned clock = 0)
{
    sfz::oscairn::ElementLfo lfo;
    lfo.setSampleRate(rate);
    lfo.start(p, clock, 0);
    std::vector<float> out;
    const size_t block = 256;
    std::vector<float> cents(block);
    for (size_t done = 0; done < static_cast<size_t>(seconds * rate); done += block) {
        lfo.beginBlock(block, cc, cc);
        std::fill(cents.begin(), cents.end(), 0.0f);
        lfo.addPitch(absl::MakeSpan(cents));
        out.insert(out.end(), cents.begin(), cents.end());
    }
    return out;
}
std::vector<float> lfoGainDb(const sfz::oscairn::ElementLfoParameters& p, double rate, double seconds, float cc)
{
    sfz::oscairn::ElementLfo lfo;
    lfo.setSampleRate(rate);
    lfo.start(p, 0, 0);
    std::vector<float> out;
    const size_t block = 256;
    std::vector<float> gain(block);
    for (size_t done = 0; done < static_cast<size_t>(seconds * rate); done += block) {
        lfo.beginBlock(block, cc, cc);
        std::fill(gain.begin(), gain.end(), 1.0f);
        lfo.applyGain(absl::MakeSpan(gain));
        for (float g : gain)
            out.push_back(20.0f * std::log10(g));
    }
    return out;
}
float at(const std::vector<float>& v, double rate, double t) { return v[static_cast<size_t>(std::lround(t * rate))]; }
} // namespace

TEST_CASE("[Oscairn] Element LFO, reference mode")
{
    // Measured: 4.848 Hz triangle from zero, falling; the element's speed, wave, delay, fade
    // and depths change nothing; depth only through the controller-set destinations
    sfz::oscairn::ElementLfoParameters p;
    p.speed = 5;
    p.wave = 2;
    p.delay = 100;
    p.fade = 100;
    p.pitchDepth = 127;
    p.ampDepth = 127;
    p.filterDepth = 127;
    p.pitchCC = 1;
    p.pitchRatio = 63;
    const double period = 1.0 / 4.848;
    // p-p 1118 * 2^((126 + 127 - 253) / 16) = 1118 cents at controller 127
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        INFO("rate " << rate);
        const auto c = lfoPitch(p, rate, 1.0, 1.0f);
        REQUIRE(at(c, rate, 0.0) == Approx(0.0f).margin(0.5f));
        REQUIRE(at(c, rate, 0.25 * period) == Approx(-559.0f).margin(1.0f));
        REQUIRE(at(c, rate, 0.5 * period) == Approx(0.0f).margin(1.0f));
        REQUIRE(at(c, rate, 0.75 * period) == Approx(559.0f).margin(1.0f));
        REQUIRE(at(c, rate, 2.25 * period) == Approx(-559.0f).margin(1.0f));
    }
    // Depth law: ratio 32 at 127: 1118 * 2^(-62 / 16) = 76.3 cents; controller 64 at ratio 63
    REQUIRE(sfz::oscairn::lfoPitchDepthCents(32, 127.0f) == Approx(76.3f).epsilon(0.01));
    REQUIRE(sfz::oscairn::lfoPitchDepthCents(63, 64.0f) == Approx(1118.0f * std::exp2(-63.0f / 16.0f)).epsilon(1e-4));
    REQUIRE(sfz::oscairn::lfoPitchDepthCents(127, 127.0f) == Approx(sfz::oscairn::lfoPitchDepthCents(64, 127.0f)));
    // Nothing at controller 0, nothing without a destination
    auto zero = lfoPitch(p, 48000.0, 0.5, 0.0f);
    REQUIRE(*std::max_element(zero.begin(), zero.end()) == 0.0f);
    p.pitchCC = -1;
    zero = lfoPitch(p, 48000.0, 0.5, 1.0f);
    REQUIRE(*std::max_element(zero.begin(), zero.end()) == 0.0f);
    REQUIRE(*std::min_element(zero.begin(), zero.end()) == 0.0f);
    // Amplitude: a dip in dB from 0 at note-on; ratio 63 at controller 64 acts as ratio 32
    // (11.8 dB peak to peak)
    sfz::oscairn::ElementLfoParameters a;
    a.ampCC = 1;
    a.ampRatio = 63;
    const auto g = lfoGainDb(a, 48000.0, 1.0, 64.0f / 127.0f);
    REQUIRE(at(g, 48000.0, 0.0) == Approx(0.0f).margin(0.01f));
    REQUIRE(at(g, 48000.0, 0.5 * period) == Approx(-11.8f * 63.0f * 64.0f / 127.0f / 32.0f).margin(0.15f));
    REQUIRE(at(g, 48000.0, period) == Approx(0.0f).margin(0.05f));
    REQUIRE(*std::max_element(g.begin(), g.end()) <= 0.0f);
}

TEST_CASE("[Oscairn] Element LFO, settings mode")
{
    sfz::oscairn::ElementLfoParameters p;
    p.mode = 1;
    p.pitchDepth = 127;
    // Speed: the default 38 runs at 4.848 Hz, 8 steps double it; extended speed in 1/6.5 steps
    REQUIRE(sfz::oscairn::lfoSpeedHz(p) == Approx(4.848f));
    p.speed = 46;
    REQUIRE(sfz::oscairn::lfoSpeedHz(p) == Approx(9.696f));
    p.extendedSpeed = 13;
    REQUIRE(sfz::oscairn::lfoSpeedHz(p) == Approx(4.848f * std::exp2(-36.0f / 8.0f)));
    p.extendedSpeed = -1;
    const double period = 1.0 / 9.696;
    // depth 127: 1118 * 2^(1 / 16) cents peak to peak
    const float half = 0.5f * 1118.0f * std::exp2(1.0f / 16.0f);
    for (double rate : { 44100.0, 48000.0, 192000.0 }) {
        INFO("rate " << rate);
        // triangle falling from zero
        auto c = lfoPitch(p, rate, 1.0, 0.0f);
        REQUIRE(at(c, rate, 0.25 * period) == Approx(-half).margin(1.0f));
        REQUIRE(at(c, rate, 0.75 * period) == Approx(half).margin(1.0f));
        // saw rising from zero, square high first
        p.wave = 0;
        c = lfoPitch(p, rate, 1.0, 0.0f);
        REQUIRE(at(c, rate, 0.25 * period) == Approx(0.5f * half).margin(1.0f));
        REQUIRE(at(c, rate, 0.75 * period) == Approx(-0.5f * half).margin(1.0f));
        p.wave = 2;
        c = lfoPitch(p, rate, 1.0, 0.0f);
        REQUIRE(at(c, rate, 0.25 * period) == Approx(half));
        REQUIRE(at(c, rate, 0.75 * period) == Approx(-half));
        p.wave = 1;
        // delay 5 s * (64 / 127)^2 = 1.27 s with nothing, then a fade-in of the same length
        p.delay = 64;
        p.fade = 64;
        c = lfoPitch(p, rate, 4.0, 0.0f);
        const double d = 5.0 * (64.0 / 127.0) * (64.0 / 127.0);
        float before = 0.0f;
        for (size_t i = 0; i < static_cast<size_t>(d * rate) - 1; ++i)
            before = std::max(before, std::abs(c[i]));
        REQUIRE(before == 0.0f);
        // the fade: half the depth halfway through (at a peak of the triangle)
        const double peakTime = std::ceil((1.5 * d) / period - 0.75) * period + 0.75 * period;
        const double fade = std::min(1.0, (peakTime - d) / d);
        REQUIRE(at(c, rate, peakTime) == Approx(half * fade).margin(2.0f));
        REQUIRE(std::abs(at(c, rate, 3.0 * d + 0.25 * period)) <= half + 0.01f);
        p.delay = 0;
        p.fade = 0;
    }
    // Key-on reset off: the phase follows the synth's clock; on: it starts at zero
    p.keyOnReset = 0;
    const auto shifted = lfoPitch(p, 48000.0, 0.2, 0.0f, static_cast<unsigned>(0.25 * period * 48000.0));
    REQUIRE(shifted[0] == Approx(-half).margin(1.0f));
    p.keyOnReset = 1;
    REQUIRE(lfoPitch(p, 48000.0, 0.2, 0.0f, 12345)[0] == 0.0f);
    // Controller-set destinations add to the element depth
    p.pitchDepth = 64;
    p.pitchCC = 1;
    p.pitchRatio = 16;
    const auto added = lfoPitch(p, 48000.0, 0.5, 1.0f);
    REQUIRE(at(added, 48000.0, 0.75 * period) == Approx(0.5f * 1118.0f * std::exp2((96.0f - 126.0f) / 16.0f)).margin(0.5f));
}

TEST_CASE("[Oscairn] Element LFO opcodes and voice")
{
    // Reference mode by default; the opcodes reach the region, and the LFO moves the pitch of
    // a voice only through its controller
    sfz::Synth synth;
    synth.setSampleRate(48000.0f);
    synth.setSamplesPerBlock(256);
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(<control> oscairn_profile=1
        <region> sample=*sine pitch_keycenter=69 oscairn_elfo_pitch_oncc1=63 oscairn_elfo_amp_oncc2=-5
        oscairn_elfo_speed=10 oscairn_elfo_xspeed=200 oscairn_elfo_wave=0 oscairn_elfo_delay=3 oscairn_elfo_fade=4
        oscairn_elfo_reset=0 oscairn_elfo_pitch_depth=5 oscairn_elfo_amp_depth=6 oscairn_elfo_filter_depth=7)");
    const auto& l = synth.getRegionView(0)->oscairn;
    REQUIRE(l.lfoEnabled);
    REQUIRE(l.lfo.mode == 0);
    REQUIRE(l.lfo.pitchCC == 1);
    REQUIRE(l.lfo.pitchRatio == 63);
    REQUIRE(l.lfo.ampCC == 2);
    REQUIRE(l.lfo.ampRatio == -5);
    REQUIRE(l.lfo.speed == 10);
    REQUIRE(l.lfo.extendedSpeed == 200);
    REQUIRE(l.lfo.wave == 0);
    REQUIRE(l.lfo.delay == 3);
    REQUIRE(l.lfo.fade == 4);
    REQUIRE(l.lfo.keyOnReset == 0);
    REQUIRE(l.lfo.pitchDepth == 5);
    REQUIRE(l.lfo.ampDepth == 6);
    REQUIRE(l.lfo.filterDepth == 7);
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<control> oscairn_profile=1 <region> sample=*sine oscairn_elfo_mode=1");
    REQUIRE(synth.getRegionView(0)->oscairn.lfo.mode == 1);

    // Zero-crossing intervals of a 440 Hz sine: steady without the controller, swinging by
    // about +-559 cents with it
    auto spread = [](int cc) {
        sfz::Synth s;
        s.setSampleRate(48000.0f);
        s.setSamplesPerBlock(256);
        s.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<control> oscairn_profile=1 <region> sample=*sine pitch_keycenter=69 oscairn_elfo_pitch_oncc1=63");
        s.cc(0, 1, cc);
        s.noteOn(0, 69, 100);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        std::vector<float> l;
        for (int b = 0; b < 48000 / 256; ++b) {
            s.renderBlock(buffer);
            l.insert(l.end(), buffer.getConstSpan(0).begin(), buffer.getConstSpan(0).end());
        }
        std::vector<double> periods;
        double last = -1.0;
        for (size_t i = 4800; i + 1 < l.size(); ++i) {
            if (l[i] <= 0.0f && l[i + 1] > 0.0f) {
                const double t = static_cast<double>(i) + l[i] / (l[i] - l[i + 1]);
                if (last >= 0.0)
                    periods.push_back(t - last);
                last = t;
            }
        }
        const auto mm = std::minmax_element(periods.begin(), periods.end());
        return 1200.0 * std::log2(*mm.second / *mm.first);
    };
    REQUIRE(spread(0) < 1.0);
    REQUIRE(spread(127) == Approx(1118.0).margin(40.0));
}

TEST_CASE("[Oscairn] Filter envelope levels, times and velocity")
{
    using namespace sfz::oscairn;
    // 75 cents per level step at full depth, scaled by depth / 64; the pitch envelope's times
    // (the decay table's full-scale time), at every rate
    ElementPitchParameters p;
    p.holdLevel = 192;
    p.attackLevel = p.decay1Level = p.decay2Level = 128;
    p.attackTime = 72;
    p.holdTime = 0;
    p.depth = 63;
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        INFO("rate " << rate);
        ElementPitchEnvelope eg { 75.0f };
        eg.setSampleRate(rate);
        eg.start(p, 0);
        std::vector<float> c(static_cast<size_t>(3.0 * rate));
        eg.processAdd(absl::MakeSpan(c));
        REQUIRE(c[0] == Approx(4800.0f).epsilon(1e-3));
        const double t = 1.5 * decaySecondsPer60dB(72);
        REQUIRE(at(c, rate, 0.5 * t) == Approx(2400.0f).margin(5.0f));
        REQUIRE(at(c, rate, t + 0.01) == Approx(0.0f).margin(0.5f));
        // velocity 64 at time sensitivity +63 shifts the attack index by 32
        p.timeVelocitySensitivity = 63;
        eg.start(p, 0, 60, 64);
        std::fill(c.begin(), c.end(), 0.0f);
        eg.processAdd(absl::MakeSpan(c));
        const double shifted = 1.5 * decaySecondsPer60dB(40);
        REQUIRE(at(c, rate, 0.5 * shifted) == Approx(2400.0f).margin(60.0f));
        // key 84 at key follow +63 around 60 shifts it by 24
        p.timeVelocitySensitivity = 0;
        p.timeKeyFollowSensitivity = 63;
        p.timeKeyFollowCenter = 60;
        eg.start(p, 0, 84, 127);
        std::fill(c.begin(), c.end(), 0.0f);
        eg.processAdd(absl::MakeSpan(c));
        const double keyed = 1.5 * decaySecondsPer60dB(48);
        REQUIRE(at(c, rate, 0.5 * keyed) == Approx(2400.0f).margin(30.0f));
        p.timeKeyFollowSensitivity = 0;
    }
    // Depth velocity sensitivity (measured at +63 on curve 2, scaled for others, mirrored when
    // negative)
    REQUIRE(filterDepthVelocityFactor(0, 2, 1) == 1.0f);
    REQUIRE(filterDepthVelocityFactor(63, 2, 127) == Approx(1.125f));
    REQUIRE(filterDepthVelocityFactor(63, 2, 1) == Approx(0.0f));
    REQUIRE(filterDepthVelocityFactor(63, 2, 64) == Approx(0.538f));
    REQUIRE(filterDepthVelocityFactor(32, 2, 1) == Approx(0.5f));
    REQUIRE(filterDepthVelocityFactor(-64, 2, 1) == Approx(1.125f));
    REQUIRE(filterDepthVelocityFactor(-32, 2, 64) == Approx(filterDepthVelocityFactor(32, 2, 64)));
    REQUIRE(filterDepthVelocityFactor(63, 4, 80) == Approx(0.816f));
    // Cutoff velocity sensitivity: 0.115 * sensitivity * (velocity - 115) index steps
    REQUIRE(cutoffVelocityCents(704, 32, 115) == 0.0f);
    REQUIRE(cutoffVelocityCents(704, 0, 1) == 0.0f);
    REQUIRE(cutoffVelocityCents(704, 32, 1) == Approx(1200.0f * std::log2(filterCutoffHz(704 - 420) / filterCutoffHz(704)) + 0.0f).margin(8.0f));
    REQUIRE(cutoffVelocityCents(704, -32, 127) == Approx(cutoffVelocityCents(704, 32, 1)));
    REQUIRE(cutoffVelocityCents(704, 63, 127) == Approx(cutoffVelocityCents(704, 64, 127)));
}

TEST_CASE("[Oscairn] Filter envelope opcodes and voice")
{
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<region> sample=*sine oscairn_fileg_depth=10");
    REQUIRE_FALSE(synth.getRegionView(0)->oscairn.fegEnabled);
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(<control> oscairn_profile=1
        <region> sample=*sine oscairn_fil_type=0 oscairn_fil_velsens=-20 oscairn_fileg_hold=1 oscairn_fileg_attack=2
        oscairn_fileg_decay1=3 oscairn_fileg_decay2=4 oscairn_fileg_release=5 oscairn_fileg_hold_level=6
        oscairn_fileg_attack_level=7 oscairn_fileg_decay1_level=8 oscairn_fileg_decay2_level=9
        oscairn_fileg_release_level=10 oscairn_fileg_depth=-11 oscairn_fileg_veltime_segments=2 oscairn_fileg_veltime=12
        oscairn_fileg_keytime=13 oscairn_fileg_keytime_center=14 oscairn_fileg_veldepth=15 oscairn_fileg_veldepth_curve=4)");
    const auto& r = synth.getRegionView(0)->oscairn;
    REQUIRE(r.fegEnabled);
    REQUIRE(r.filter.velocitySensitivity == -20);
    const auto& f = r.feg;
    REQUIRE(f.holdTime == 1);
    REQUIRE(f.attackTime == 2);
    REQUIRE(f.decay1Time == 3);
    REQUIRE(f.decay2Time == 4);
    REQUIRE(f.releaseTime == 5);
    REQUIRE(f.holdLevel == 6);
    REQUIRE(f.attackLevel == 7);
    REQUIRE(f.decay1Level == 8);
    REQUIRE(f.decay2Level == 9);
    REQUIRE(f.releaseLevel == 10);
    REQUIRE(f.depth == -11);
    REQUIRE(f.timeVelocitySegments == 2);
    REQUIRE(f.timeVelocitySensitivity == 12);
    REQUIRE(f.timeKeyFollowSensitivity == 13);
    REQUIRE(f.timeKeyFollowCenter == 14);
    REQUIRE(r.fegDepthVelocitySensitivity == 15);
    REQUIRE(r.fegDepthVelocityCurve == 4);
    // Defaults are the published ones
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<control> oscairn_profile=1 <region> sample=*sine oscairn_fileg_depth=0");
    const auto& d = synth.getRegionView(0)->oscairn.feg;
    REQUIRE(d.releaseTime == 80);
    REQUIRE(d.attackLevel == 255);
    REQUIRE(d.holdLevel == 128);

    // A 2 kHz sine through a low-pass at about 450 Hz, opened by the filter envelope: louder
    auto level = [](const char* feg) {
        sfz::Synth s;
        s.setSampleRate(48000.0f);
        s.setSamplesPerBlock(256);
        s.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz",
            std::string("<control> oscairn_profile=1 <region> sample=*sine pitch_keycenter=36 oscairn_fil_type=0 oscairn_fil_cutoff=512 oscairn_fil_keyfollow=64 ") + feg);
        s.noteOn(0, 84, 127);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        double sum = 0.0;
        for (int b = 0; b < 100; ++b) {
            s.renderBlock(buffer);
            if (b >= 50)
                for (float x : buffer.getConstSpan(0))
                    sum += static_cast<double>(x) * x;
        }
        return 10.0 * std::log10(sum);
    };
    const double closed = level("");
    const double open = level("oscairn_fileg_hold_level=160 oscairn_fileg_attack_level=160 oscairn_fileg_decay1_level=160 oscairn_fileg_decay2_level=160 oscairn_fileg_attack=0 oscairn_fileg_decay1=0 oscairn_fileg_decay2=0 oscairn_fileg_depth=63");
    REQUIRE(open - closed > 20.0);
}

TEST_CASE("[Oscairn] Reference output level is an option")
{
    // Off by default (sfizz's own level); oscairn_ref_level=1 plays 1.84 dB lower, as the
    // reference does (decision 25)
    auto level = [](const std::string& opcodes) {
        sfz::Synth s;
        s.setSampleRate(48000.0f);
        s.setSamplesPerBlock(256);
        s.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<control> oscairn_profile=1 <region> sample=*sine amp_veltrack=0 " + opcodes);
        s.noteOn(0, 69, 127);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        double sum = 0.0;
        for (int b = 0; b < 40; ++b) {
            s.renderBlock(buffer);
            if (b >= 20)
                for (float x : buffer.getConstSpan(0))
                    sum += static_cast<double>(x) * x;
        }
        return 10.0 * std::log10(sum);
    };
    const double plain = level("");
    REQUIRE(level("oscairn_ref_level=0") == Approx(plain).margin(1e-6));
    REQUIRE(level("oscairn_ref_level=1") - plain == Approx(-1.84).margin(0.01));
    // Without the profile the opcode is unknown and changes nothing
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<region> sample=*sine oscairn_ref_level=1");
    REQUIRE_FALSE(synth.getRegionView(0)->oscairn.referenceLevel);
    REQUIRE(isUnknown(synth, "oscairn_ref_level"));
}

TEST_CASE("[Oscairn] Element level scaling by key")
{
    using sfz::oscairn::ElementLevelScaling;
    auto db = [](const ElementLevelScaling& s, int key) { return 20.0 * std::log10(sfz::oscairn::levelScalingGain(s, key)); };
    // Reference renders (ks1), sine, element level 127 unless given: break points 36/48/84/105
    // with offsets 0/0/0/-90 (Rd 73's) read 0 dB at 84, -0.42 at 85, -2.68 at 88, -7.16 at 94
    ElementLevelScaling rd { { 36, 48, 84, 105 }, { 0, 0, 0, -90 } };
    REQUIRE(db(rd, 84) == Approx(0.0).margin(1e-6));
    REQUIRE(db(rd, 85) == Approx(-0.42).margin(0.1));
    REQUIRE(db(rd, 88) == Approx(-2.68).margin(0.1));
    REQUIRE(db(rd, 94) == Approx(-7.16).margin(0.1));
    REQUIRE(db(rd, 120) == Approx(db(rd, 105)).margin(1e-6)); // held beyond the outer points
    REQUIRE(db(rd, 0) == Approx(0.0).margin(1e-6));
    // -64 halves the level (-12 dB), -96 quarters it, -128 silences
    ElementLevelScaling s { { 36, 48, 60, 72 }, { 0, 0, -64, 0 } };
    REQUIRE(db(s, 60) == Approx(-12.04).margin(0.1));
    REQUIRE(db(s, 54) == Approx(-4.89).margin(0.1)); // halfway: -32
    s.offsets[2] = -96;
    REQUIRE(db(s, 60) == Approx(-24.08).margin(0.1));
    s.offsets[2] = -128;
    REQUIRE(sfz::oscairn::levelScalingGain(s, 60) == 0.0f);
    // positive offsets raise a lower element level, up to 127: level 100 is -4.14 dB
    s.level = 100;
    s.offsets[2] = 32;
    REQUIRE(db(s, 60) == Approx(-0.38 + 4.14).margin(0.1));
    s.offsets[2] = 64;
    REQUIRE(db(s, 60) == Approx(4.125).margin(1e-4));
    // key follow: sensitivity * (key - 60) / 32 steps of offset
    ElementLevelScaling kf;
    kf.level = 100;
    kf.keyFollow = 32;
    REQUIRE(db(kf, 36) == Approx(-7.74 + 4.14).margin(0.2));
    REQUIRE(db(kf, 84) == Approx(-1.12 + 4.14).margin(0.1));
    kf.keyFollow = -32;
    REQUIRE(db(kf, 84) == Approx(-7.89 + 4.14).margin(0.1));
    // The opcodes reach the voice, only with the profile
    auto level = [](const std::string& text, int key) {
        sfz::Synth synth;
        synth.setSamplesPerBlock(256);
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", text);
        synth.noteOn(0, key, 127);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        double sum = 0.0;
        for (int b = 0; b < 40; ++b) {
            synth.renderBlock(buffer);
            if (b >= 20)
                for (float x : buffer.getConstSpan(0))
                    sum += static_cast<double>(x) * x;
        }
        return 10.0 * std::log10(sum);
    };
    const std::string opcodes = " sample=*sine amp_veltrack=0 oscairn_amp_scaling_key3=60 oscairn_amp_scaling_offset3=-64";
    const double plain = level("<region> sample=*sine amp_veltrack=0", 60);
    REQUIRE(level("<control> oscairn_profile=1 <region>" + opcodes, 60) - plain == Approx(-12.0).margin(0.01));
    REQUIRE(level("<region>" + opcodes, 60) == Approx(plain).margin(1e-6));
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz",
        "<control> oscairn_profile=1 <region> sample=*sine oscairn_amp_scaling_key4=105 oscairn_amp_scaling_offset4=-90 oscairn_amp_keyfollow=-5 oscairn_amp_level=120");
    const auto& ls = synth.getRegionView(0)->oscairn.levelScaling;
    REQUIRE(synth.getRegionView(0)->oscairn.levelScalingEnabled);
    REQUIRE((ls.keys[3] == 105 && ls.offsets[3] == -90 && ls.keyFollow == -5 && ls.level == 120));
}

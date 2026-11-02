// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Oscairn's opt-in extensions (doc/oscairn-plan.md): the profile switch and the element
// amplitude envelope.

#include "sfizz/Synth.h"
#include "sfizz/Region.h"
#include "sfizz/oscairn/ElementEnvelope.h"
#include "TestHelpers.h"
#include "catch2/catch.hpp"
#include <algorithm>
#include <cmath>
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
    REQUIRE(20 * std::log10(sfz::oscairn::levelGain(64)) == Approx(40 * std::log10(64.0 / 127.0)).margin(1e-4));
    REQUIRE(sfz::oscairn::levelGain(200) == 1.0f);
}

TEST_CASE("[Oscairn] Element envelope segments take the table times at every sample rate")
{
    for (double rate : { 44100.0, 48000.0, 96000.0 }) {
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
        const double sustainDb = 40 * std::log10(64.0 / 127.0);
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

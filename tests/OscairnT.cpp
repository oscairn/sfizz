// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Oscairn's opt-in extensions (doc/oscairn-plan.md): the profile switch and the element
// amplitude envelope.

#include "sfizz/Synth.h"
#include "sfizz/Panning.h"
#include "sfizz/Interpolators.h"
#include "sfizz/Region.h"
#include "sfizz/oscairn/ElementEnvelope.h"
#include "sfizz/oscairn/ElementFilter.h"
#include "sfizz/oscairn/ElementLevel.h"
#include "sfizz/oscairn/ElementLfo.h"
#include "sfizz/oscairn/ElementPitch.h"
#include "TestHelpers.h"
#include "catch2/catch.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <random>
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

TEST_CASE("[Oscairn] A profile alone changes only the output staging; reloading clears it")
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
    const auto stock = render(plain), staged = render("<control> oscairn_profile=1\n" + plain);
    REQUIRE(stock.size() == staged.size());
    for (size_t i = 0; i < stock.size(); ++i)
        REQUIRE(staged[i] == Approx(stock[i] * sfz::oscairn::referenceLevelGain).margin(1e-7));

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
        // the release is a straight line in dB to -96.45 dB over release60 * 96.45 / 60, then silence
        const double t20 = crossing(v, rate, sustain * std::pow(10.0f, -20.0f / 20.0f), false, hold);
        REQUIRE(t20 - hold == Approx(release60 * 96.45 / 60 * 20 / (96.45 + sustainDb)).margin(tol));
        REQUIRE(crossing(v, rate, 0.0f, false, hold) - hold == Approx(release60 * 96.45 / 60).margin(tol));
        REQUIRE(v.back() == 0.0f);
    }
}

TEST_CASE("[Oscairn] A release ends at -96.45 dB, where the reference's voices end")
{
    // Reference (pcm round 2, probes c_end*): a sine at element level 127 released after 0.7 s ends
    // at -96.46 (release 96), -96.47 (104) and -96.47 dB (112) below its held level, 13.39, 26.08 and
    // 51.33 s after the onset, 0.7 s of it the hold: the fork ended at -90 dB, about 6 % early
    for (int index : { 96, 104, 112 }) {
        ElementEnvelopeParameters p;
        p.releaseTime = index;
        const double rate = 44100.0, hold = 0.7;
        const double end = hold + sfz::oscairn::releaseSecondsPer60dB(index) * 96.45 / 60;
        const auto v = renderEnvelope(p, rate, end + 1.0, hold);
        size_t last = 0;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] > 0.0f)
                last = i;
        INFO("release " << index);
        REQUIRE(last / rate == Approx(end).margin(2.0 / rate));
        REQUIRE(20 * std::log10(v[last]) == Approx(-96.45).margin(0.05));
    }
}

TEST_CASE("[Oscairn] Element envelope: initial level, release before the end of the attack, delay")
{
    const double rate = 48000;
    ElementEnvelopeParameters p;
    p.initialLevel = 127;
    p.attackLevel = 0;
    p.attackTime = 64;
    p.decay1Level = p.decay2Level = 0; // held at 0 after the attack
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

TEST_CASE("[Oscairn] Fast segments: attack 0 at once, releases below 32 straight lines in amplitude")
{
    using sfz::oscairn::releaseRampSeconds;
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
        INFO("rate " << rate);
        ElementEnvelopeParameters p;
        p.attackTime = 0;
        p.decay1Time = p.decay2Time = 0;
        auto v = renderEnvelope(p, rate, 0.01, 1.0);
        REQUIRE(v[0] == 1.0f); // attack index 0: full level from the first sample
        // release index 16 from level 127: a straight line in amplitude to silence over the
        // table's time (measured: renders sv1); index 32 is a straight line in dB again
        p.releaseTime = 16;
        const double hold = 0.05, ramp = releaseRampSeconds(16);
        REQUIRE(ramp == Approx(0.0037).epsilon(0.05));
        v = renderEnvelope(p, rate, hold + 0.03, hold);
        const double tol = 2.0 / rate;
        REQUIRE(crossing(v, rate, 0.75f, false, hold) - hold == Approx(0.25 * ramp).margin(tol));
        REQUIRE(crossing(v, rate, 0.25f, false, hold) - hold == Approx(0.75 * ramp).margin(tol));
        REQUIRE(crossing(v, rate, 0.0f, false, hold) - hold == Approx(ramp).margin(tol));
        // from level 64: the same rate, so the level's amplitude times as long (renders sv2)
        p.decay1Level = p.decay2Level = 64;
        v = renderEnvelope(p, rate, hold + 0.03, hold);
        REQUIRE(crossing(v, rate, 0.0f, false, hold) - hold == Approx(ramp * sfz::oscairn::levelGain(64)).margin(tol));
        p.decay1Level = p.decay2Level = 127;
        // release index 0: silent at once
        p.releaseTime = 0;
        v = renderEnvelope(p, rate, hold + 0.01, hold);
        REQUIRE(crossing(v, rate, 0.0f, false, hold) - hold <= tol);
        p.releaseTime = 32;
        v = renderEnvelope(p, rate, hold + 0.1, hold);
        const double per60 = sfz::oscairn::releaseSecondsPer60dB(32);
        REQUIRE(crossing(v, rate, 0.1f, false, hold) - hold == Approx(per60 * 20.0 / 60.0).margin(tol));
        // a choke's fast release stays a straight line in dB whatever the index
        ElementEnvelope eg;
        eg.setSampleRate(rate);
        p.releaseTime = 16;
        eg.start(p, 0);
        eg.release(16);
        eg.setReleaseSeconds(0.02);
        std::vector<float> out(16 + static_cast<size_t>(0.01 * rate));
        eg.process(absl::MakeSpan(out));
        REQUIRE(out.back() == Approx(std::pow(10.0f, -30.0f / 20.0f)).epsilon(0.02));
    }
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

TEST_CASE("[Oscairn] A voice keeps the host bus it started on")
{
    // Two notes, the second after the synth moved to bus 2: rendered with bus channels, the first
    // plays on bus 0 from then on and the second in the buffer; together they are the plain render.
    auto render = [](bool buses, std::vector<float>& main, std::vector<float>& bus0) {
        sfz::Synth synth;
        synth.setSampleRate(48000);
        synth.setSamplesPerBlock(256);
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<region> sample=*sine ampeg_release=0.01");
        sfz::AudioBuffer<float> buffer { 2, 256 };
        std::vector<std::vector<float>> bus(6, std::vector<float>(256));
        float* channels[6] = { bus[0].data(), bus[1].data(), bus[2].data(), bus[3].data(), bus[4].data(), bus[5].data() };
        synth.noteOn(0, 60, 100);
        for (int block = 0; block < 8; ++block) {
            if (block == 2) {
                synth.setHostBus(buses ? 2 : 0);
                synth.noteOn(10, 67, 90);
            }
            if (block == 5)
                synth.noteOff(0, 60, 0);
            for (auto& c : bus)
                std::fill(c.begin(), c.end(), 0.0f);
            synth.renderBlock(buffer, buses ? channels : nullptr, buses ? 3 : 0);
            main.insert(main.end(), buffer.getConstSpan(0).begin(), buffer.getConstSpan(0).end());
            bus0.insert(bus0.end(), bus[0].begin(), bus[0].end());
            for (size_t c = 2; c < 6; ++c) // nothing on the buses no voice is on
                REQUIRE(*std::max_element(bus[c].begin(), bus[c].end()) == 0.0f);
        }
    };
    std::vector<float> plain, none, main, bus0;
    render(false, plain, none);
    render(true, main, bus0);
    REQUIRE(*std::max_element(none.begin(), none.end()) == 0.0f);
    float before = 0, gap = 0, moved = 0, worst = 0;
    for (size_t i = 0; i < plain.size(); ++i) {
        if (i < 512)
            before = std::max(before, std::fabs(bus0[i])); // on the synth's bus, in the buffer
        else if (i < 512 + 10)
            gap = std::max(gap, std::fabs(main[i])); // the first note left, the second not started
        moved = std::max(moved, std::fabs(bus0[i]));
        worst = std::max(worst, std::fabs(main[i] + bus0[i] - plain[i]));
    }
    REQUIRE(before == 0.0f);
    REQUIRE(gap == 0.0f);
    REQUIRE(moved > 0.1f);
    REQUIRE(worst < 1e-6f);
}

TEST_CASE("[Oscairn] Element velocity response")
{
    using sfz::oscairn::ElementVelocity;
    using sfz::oscairn::velocityScale;
    using sfz::oscairn::velocityGain;
    auto steps = [](ElementVelocity v, int velocity) {
        return std::round(-20 * std::log10(velocityGain(v, velocity)) / 0.375);
    };
    ElementVelocity v;
    REQUIRE(velocityScale(v, 1) == 2047); // sensitivity 0: no response
    v.sensitivity = 32;
    REQUIRE(velocityScale(v, 1) == 16);
    REQUIRE(velocityScale(v, 8) == 128);
    REQUIRE(velocityScale(v, 64) == 1024);
    REQUIRE(velocityScale(v, 80) == 1284);
    REQUIRE(velocityScale(v, 127) == 2047);
    v.sensitivity = 63; // counts as 64: only the top of the range sounds
    REQUIRE(velocityScale(v, 120) == 0);
    REQUIRE(velocityScale(v, 126) == 15);
    REQUIRE(velocityScale(v, 127) == 2047);
    v.sensitivity = -64; // only velocity 1 sounds, about 85 dB down
    REQUIRE(velocityScale(v, 1) == 15);
    REQUIRE(velocityScale(v, 2) == 0);
    // reference levels (renders lv1), on the 0.375 dB grid
    REQUIRE(steps(ElementVelocity { -64, 0, 3 }, 1) == 228); // -85.8 dB
    REQUIRE(steps(ElementVelocity { -56, 0, 3 }, 8) == 14);  // -5.27 dB
    REQUIRE(steps(ElementVelocity { -56, 0, 3 }, 16) == 33); // -12.42 dB
    REQUIRE(steps(ElementVelocity { -32, 0, 3 }, 80) == 46); // -17.31 dB
    REQUIRE(steps(ElementVelocity { 16, 0, 3 }, 8) == 30);   // -11.29 dB
    REQUIRE(steps(ElementVelocity { 28, 0, 3 }, 32) == 50);  // -18.82 dB
    REQUIRE(steps(ElementVelocity { 32, 0, 3 }, 64) == 32);
    REQUIRE(steps(ElementVelocity { 32, 0, 3 }, 80) == 22);
    REQUIRE(steps(ElementVelocity { 32, 0, 3 }, 104) == 9);
    REQUIRE(steps(ElementVelocity { 32, 16, 3 }, 8) == 78);  // -29.35 dB, offset 16
    REQUIRE(velocityGain(ElementVelocity { -56, 0, 3 }, 32) == 0.0f);
    // measured curves at sensitivity +32, where the scale is the velocity's
    REQUIRE(20 * std::log10(velocityGain(ElementVelocity { 32, 0, 0 }, 64)) == Approx(-30.0f).margin(0.01f));
    REQUIRE(20 * std::log10(velocityGain(ElementVelocity { 32, 0, 0 }, 72)) == Approx(-26.25f).margin(0.01f));
    // curve 4 between the old knots 48, 64 and 80 (K2): measured -49.67 dB at 56, -13.55 at 72
    REQUIRE(20 * std::log10(velocityGain(ElementVelocity { 32, 0, 4 }, 56)) == Approx(-49.5f).margin(0.01f));
    REQUIRE(20 * std::log10(velocityGain(ElementVelocity { 32, 0, 4 }, 72)) == Approx(-13.5f).margin(0.01f));
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
    const auto stock = peaks("<region> sample=*sine amp_veltrack=0", 64);
    const auto full = peaks("<control> oscairn_profile=1 <region> sample=*sine amp_veltrack=0", 64);
    const auto vel = peaks("<control> oscairn_profile=1 <region> sample=*sine amp_veltrack=0 oscairn_amp_velsens=32", 64);
    REQUIRE(vel.first / full.first == Approx(std::pow(10.0f, -12.0f / 20.0f)).epsilon(0.01));
    const auto off = peaks("<region> sample=*sine amp_veltrack=0 oscairn_amp_velsens=32 oscairn_pan=1", 64);
    REQUIRE(off.first == Approx(stock.first).epsilon(0.001)); // no profile: unknown, no effect
    REQUIRE(off.second == Approx(stock.second).epsilon(0.001));
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
    // Below index 264, measured on the reference's harmonics (pcm round 2, probes b_lad0 to b_lad256: a
    // saw at note 36 through type 0 at resonance 0, the cutoff fitted to the harmonic levels), in cents
    // from 440 Hz every 16 indices; the earlier table put index 160 at -3666 cents and 0 at -5603
    const float measured[17] = { -5932, -5612, -5382, -5200, -5058, -4934, -4814, -4539, -4281, -4082, -3921, -3784, -3658, -3465, -3173, -2955, -2773 };
    for (int i = 0; i < 17; ++i) {
        INFO("index " << 16 * i);
        REQUIRE(1200.0f * std::log2(filterCutoffHz(16 * i) / 440.0f) == Approx(measured[i]).margin(0.5f));
    }
    // between knots linear in cents; from 264 up unchanged
    REQUIRE(1200.0f * std::log2(filterCutoffHz(168) / 440.0f) == Approx(0.5f * (-3921 - 3784)).margin(0.5f));
    REQUIRE(1200.0f * std::log2(filterCutoffHz(264) / 440.0f) == Approx(-2703.0f).margin(0.5f));
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

TEST_CASE("[Oscairn] Output staging is the reference's")
{
    // A full-scale sample at element level 127, Part Volume 127 (CC 7 127) and pan centre plays at
    // -9.43 dB per channel on the reference (pcm round 2, probes a_saw_p64 and a_st_p64: the fork
    // played the same samples 2.07 and 2.05 dB higher), and at -7.35 dB in stock sfizz: a patch with
    // the profile plays 2.075 dB below the same patch without it.
    auto level = [](const std::string& control, const std::string& opcodes) {
        sfz::Synth s;
        s.setSampleRate(48000.0f);
        s.setSamplesPerBlock(256);
        s.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz",
            "<control> set_cc7=127 " + control + " <region> sample=*square amp_veltrack=0 " + opcodes);
        s.noteOn(0, 69, 127);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        double sum[2] = { 0.0, 0.0 };
        for (int b = 0; b < 40; ++b) {
            s.renderBlock(buffer);
            if (b >= 20)
                for (int c = 0; c < 2; ++c)
                    for (float x : buffer.getConstSpan(c))
                        sum[c] += static_cast<double>(x) * x;
        }
        return std::array<double, 2> { 10.0 * std::log10(sum[0] / (20 * 256)), 10.0 * std::log10(sum[1] / (20 * 256)) };
    };
    const auto stock = level("", "");
    const auto staged = level("oscairn_profile=1", "");
    REQUIRE(staged[0] - stock[0] == Approx(-2.075).margin(0.02));
    REQUIRE(staged[1] - stock[1] == Approx(-2.075).margin(0.02));
    // Regions of a profile patch with their own opcodes play at the same level, and the opcode of the
    // old option is no longer one
    REQUIRE(level("oscairn_profile=1", "oscairn_ampeg_attack=0")[0] - stock[0] == Approx(-2.075).margin(0.02));
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<control> oscairn_profile=1 <region> sample=*sine oscairn_ref_level=1");
    REQUIRE(isUnknown(synth, "oscairn_ref_level"));
}

TEST_CASE("[Oscairn] Key bank pan and element pan add under the balance law")
{
    // Reference (pcm round 2, probes a_saw_p*, a_st_p*, a_fat_p*): the key bank's pan and the element
    // pan add in raw steps (a key bank's -100..100 as 1..127), clamped, and the sum takes the balance
    // law, mono and stereo samples alike. A hard-panned mono key bank keeps the centre level (the fork
    // played it 3.01 dB louder under sfizz's pan law), and with the element pan hard left a
    // right-panned key bank sounds at the centre level on both sides.
    auto levels = [](const std::string& sample, const std::string& opcodes) {
        sfz::Synth s;
        s.setSampleRate(48000.0f);
        s.setSamplesPerBlock(256);
        s.setPreloadSize(1 << 20); // whole files in memory: no streaming race under load
        s.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz",
            "<control> oscairn_profile=1 <region> sample=" + sample + " amp_veltrack=0 loop_mode=loop_continuous " + opcodes);
        s.noteOn(0, 69, 127);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        double sum[2] = { 0.0, 0.0 };
        for (int b = 0; b < 60; ++b) {
            s.renderBlock(buffer);
            if (b >= 20)
                for (int c = 0; c < 2; ++c)
                    for (float x : buffer.getConstSpan(c))
                        sum[c] += static_cast<double>(x) * x;
        }
        return std::array<double, 2> { 10.0 * std::log10(sum[0] + 1e-30), 10.0 * std::log10(sum[1] + 1e-30) };
    };
    struct Case { const char* opcodes; double left, right; }; // dB against the centre; -300: silent
    const Case cases[] = {
        { "pan=-100", 0.0, -300.0 },                        // a hard-left key bank: centre level on the left
        { "pan=-100 oscairn_pan=1", 0.0, -300.0 },          // pushed further left: clamped
        { "pan=100 oscairn_pan=1", 0.0, 0.0 },              // hard right plus hard left: centre, both sides
        { "pan=100 oscairn_pan=32", -6.0, 0.0 },            // raw 95: the left side sin^2(pi/4) on the grid
        { "oscairn_pan=96", -6.375, 0.0 },                  // raw 96, 31 steps from the edge: 17 grid steps
        { "oscairn_pan=8", 0.0, -30.75 },                   // raw 8, 7 steps from the edge (probe a_saw_p8: -30.85)
    };
    for (const char* sample : { "*sine", "stereo_sample.wav" }) {
        const auto centre = levels(sample, "");
        for (const Case& c : cases) {
            INFO(sample << " " << c.opcodes);
            const auto got = levels(sample, c.opcodes);
            if (c.left > -299.0)
                REQUIRE(got[0] - centre[0] == Approx(c.left).margin(0.05));
            else
                REQUIRE(got[0] < -200.0);
            if (c.right > -299.0)
                REQUIRE(got[1] - centre[1] == Approx(c.right).margin(0.05));
            else
                REQUIRE(got[1] < -200.0);
        }
    }
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
    const double staged = level("<control> oscairn_profile=1 <region> sample=*sine amp_veltrack=0", 60);
    REQUIRE(level("<control> oscairn_profile=1 <region>" + opcodes, 60) - staged == Approx(-12.0).margin(0.01));
    REQUIRE(level("<region>" + opcodes, 60) == Approx(plain).margin(1e-6));
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz",
        "<control> oscairn_profile=1 <region> sample=*sine oscairn_amp_scaling_key4=105 oscairn_amp_scaling_offset4=-90 oscairn_amp_keyfollow=-5 oscairn_amp_level=120");
    const auto& ls = synth.getRegionView(0)->oscairn.levelScaling;
    REQUIRE(synth.getRegionView(0)->oscairn.levelScalingEnabled);
    REQUIRE((ls.keys[3] == 105 && ls.offsets[3] == -90 && ls.keyFollow == -5 && ls.level == 120));
}

TEST_CASE("[Oscairn] Pitch envelope velocity and key sensitivities")
{
    sfz::Synth parsed;
    parsed.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(
        <control> oscairn_profile=1
        <region> sample=*sine oscairn_pitcheg_veltime_segments=2 oscairn_pitcheg_veltime=-20 oscairn_pitcheg_keytime=30
            oscairn_pitcheg_keytime_center=48 oscairn_pitcheg_veldepth=40 oscairn_pitcheg_veldepth_curve=4
    )");
    const auto& o = parsed.getRegionView(0)->oscairn;
    REQUIRE((o.peg.timeVelocitySegments == 2 && o.peg.timeVelocitySensitivity == -20 && o.peg.timeKeyFollowSensitivity == 30));
    REQUIRE((o.peg.timeKeyFollowCenter == 48 && o.pegDepthVelocitySensitivity == 40 && o.pegDepthVelocityCurve == 4));
    REQUIRE(parsed.getUnknownOpcodes().empty());
    // Frequency (from zero crossings) of a sine voice 0.15-0.25 s after note-on, in cents from A4
    auto cents = [](const std::string& opcodes, int velocity) {
        sfz::Synth synth;
        synth.setSampleRate(48000.0f);
        synth.setSamplesPerBlock(256);
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz",
            "<control> oscairn_profile=1 <region> sample=*sine amp_veltrack=0 oscairn_pitcheg_attack=64 oscairn_pitcheg_hold_level=160 "
            "oscairn_pitcheg_attack_level=128 oscairn_pitcheg_decay1_level=128 oscairn_pitcheg_decay2_level=128 oscairn_pitcheg_depth=63 "
            "oscairn_pitcheg_keytime_center=60 " + opcodes);
        synth.noteOn(0, 69, velocity);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        std::vector<float> out;
        for (int b = 0; b < 47; ++b) {
            synth.renderBlock(buffer);
            const auto l = buffer.getConstSpan(0);
            out.insert(out.end(), l.begin(), l.end());
        }
        std::vector<double> z;
        for (size_t i = 7200; i + 1 < 12000; ++i)
            if (out[i] <= 0.0f && out[i + 1] > 0.0f)
                z.push_back(static_cast<double>(i) + out[i] / (out[i] - out[i + 1]));
        return 1200.0 * std::log2(48000.0 * (z.size() - 1) / (z.back() - z.front()) / 440.0);
    };
    // Reference renders (ks2): attack 64 from +1200 cents is still about half way at 0.2 s;
    // time velocity sensitivity +63 at velocity 127 ends it within 15 ms
    const double plain = cents("", 127);
    REQUIRE(plain > 400.0);
    REQUIRE(cents("oscairn_pitcheg_veltime=63", 127) == Approx(0.0).margin(10.0));
    REQUIRE(cents("oscairn_pitcheg_veltime=63 oscairn_pitcheg_veltime_segments=2", 127) == Approx(plain).margin(1.0)); // decays only
    REQUIRE(cents("oscairn_pitcheg_veltime=63", 64) < 50.0);
    // a static +1200 cents scaled by depth velocity sensitivity: +63, curve 3, velocity 64 read 1003.5 cents
    const std::string held = "oscairn_pitcheg_attack_level=160 oscairn_pitcheg_decay1_level=160 oscairn_pitcheg_decay2_level=160 ";
    REQUIRE(cents(held, 64) == Approx(1200.0).margin(5.0));
    REQUIRE(cents(held + "oscairn_pitcheg_veldepth=63 oscairn_pitcheg_veldepth_curve=3", 64) == Approx(1003.5).margin(20.0));
}

TEST_CASE("[Oscairn] Element envelope ends once it holds level 0")
{
    // a release-triggered voice is never released: it must end when nothing more can sound
    ElementEnvelopeParameters p;
    p.decay1Time = 40;
    p.decay1Level = 0;
    p.decay2Level = 0;
    ElementEnvelope eg;
    eg.setSampleRate(48000.0);
    eg.start(p, 0);
    std::vector<float> out(48000 * 4);
    eg.process(absl::MakeSpan(out));
    REQUIRE(eg.isFinished());
    REQUIRE(out.back() == 0.0f);
    p.decay2Level = 1; // a level above 0 holds until the release
    eg.start(p, 0);
    eg.process(absl::MakeSpan(out));
    REQUIRE_FALSE(eg.isFinished());
    REQUIRE(out.back() > 0.0f);
}

TEST_CASE("[Oscairn] A decay from level 0 rises from the floor")
{
    // Measured on the reference (eg7): a decay that starts at level 0 rises as a straight line
    // in dB from -90 dB, over a full-scale decay's time, like a fall to level 0 in reverse;
    // after a decay to level 0 the next one rises from there
    auto db = [](float g) { return 20.0 * std::log10(static_cast<double>(g)); };
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        INFO("rate " << rate);
        const double tol = 2.0 / rate;
        ElementEnvelopeParameters p;
        p.attackLevel = 0;
        p.decay1Time = 72;
        const double full72 = 1.5 * sfz::oscairn::decaySecondsPer60dB(72);
        auto v = renderEnvelope(p, rate, full72 + 0.5, 10.0);
        REQUIRE(db(v[1]) == Approx(-90.0).margin(0.1));
        REQUIRE(crossing(v, rate, std::pow(10.0f, -45.0f / 20.0f), true) == Approx(0.5 * full72).margin(tol));
        REQUIRE(crossing(v, rate, 0.999f, true) == Approx(full72).margin(0.01 * full72));
        REQUIRE(v.back() == 1.0f);

        p = ElementEnvelopeParameters {};
        p.decay1Time = 64;
        p.decay1Level = 0;
        p.decay2Time = 72;
        const double full64 = 1.5 * sfz::oscairn::decaySecondsPer60dB(64);
        v = renderEnvelope(p, rate, full64 + full72 + 0.5, 10.0);
        const double top = crossing(v, rate, 1.0f, true); // the end of the attack (index 0)
        REQUIRE(crossing(v, rate, std::pow(10.0f, -45.0f / 20.0f), false, top) - top == Approx(0.5 * full64).margin(tol));
        const auto low = std::min_element(v.begin(), v.end());
        const double bottom = static_cast<double>(low - v.begin()) / rate;
        REQUIRE(bottom - top == Approx(full64).margin(tol));
        REQUIRE(db(*low) == Approx(-90.0).margin(0.1)); // no step down to silence between the two
        REQUIRE(crossing(v, rate, std::pow(10.0f, -45.0f / 20.0f), true, bottom) - bottom == Approx(0.5 * full72).margin(tol));
        REQUIRE(v.back() == 1.0f);
    }
}

TEST_CASE("[Oscairn] Key-off elements release 2 s after they start")
{
    // Reference (ks4): a key-off element holding its sustain level falls 6 dB 2.007-2.010 s after
    // it starts, whenever the note-off came
    auto render = [](const std::string& opcodes, std::vector<float>& out, int& voicesAt3s) {
        sfz::Synth synth;
        synth.setSampleRate(48000.0f);
        synth.setSamplesPerBlock(480);
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz",
            "<control> oscairn_profile=1 <region> sample=*sine trigger=release rt_dead=1 amp_veltrack=0 oscairn_ampeg_release=40 " + opcodes);
        sfz::AudioBuffer<float> buffer { 2, 480 };
        synth.noteOn(0, 60, 100);
        out.clear();
        for (int b = 0; b < 350; ++b) {
            if (b == 50)
                synth.noteOff(0, 60, 0);
            synth.renderBlock(buffer);
            float peak = 0.0f;
            for (float x : buffer.getConstSpan(0))
                peak = std::max(peak, std::abs(x));
            out.push_back(peak);
            if (b == 349)
                voicesAt3s = synth.getNumActiveVoices();
        }
    };
    std::vector<float> keyOff, plain;
    int voicesKeyOff = -1, voicesPlain = -1;
    render("oscairn_keyoff=1", keyOff, voicesKeyOff);
    render("", plain, voicesPlain);
    // blocks of 10 ms: silent before the note-off at 0.5 s, sounding from it
    REQUIRE(keyOff[49] == 0.0f);
    REQUIRE(keyOff[52] > 0.1f);
    REQUIRE(keyOff[248] == Approx(keyOff[60]).epsilon(0.01)); // held until 2.0 s after it started
    REQUIRE(keyOff[256] < 0.01f * keyOff[60]);                // then released
    REQUIRE(voicesKeyOff == 0);
    REQUIRE(plain[300] == Approx(plain[60]).epsilon(0.01)); // without the opcode it holds
    REQUIRE(voicesPlain == 1);
}

TEST_CASE("[Oscairn] Chokes and polyphony limits end an element voice at the off time")
{
    // off() shortens the stock amplitude EG; under the profile the element envelope is the
    // audible one, so a choked voice (off_by, polyphony, note_polyphony, a one-shot sample's
    // end) must release at the off time, not its own release index
    sfz::Synth synth;
    synth.setSamplesPerBlock(256);
    sfz::AudioBuffer<float> buffer { 2, 256 };
    auto render = [&](int blocks) {
        for (int i = 0; i < blocks; ++i)
            synth.renderBlock(buffer);
    };
    SECTION("off_by")
    {
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(
<control> oscairn_profile=1
<global> sample=*sine oscairn_ampeg_release=100
<region> key=60 group=1 off_by=2
<region> key=62 group=2
)");
        synth.noteOn(0, 60, 100);
        render(10);
        REQUIRE(synth.getNumActiveVoices() == 1);
        synth.noteOn(0, 62, 100);
        render(10); // 53 ms: the 6 ms off time has passed, release index 100 has not
        REQUIRE(synth.getNumActiveVoices() == 1);
    }
    SECTION("note_polyphony")
    {
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(
<control> oscairn_profile=1
<region> sample=*sine oscairn_ampeg_release=100 note_polyphony=1
)");
        synth.noteOn(0, 60, 100);
        render(10);
        synth.noteOn(0, 60, 100);
        render(10);
        REQUIRE(synth.getNumActiveVoices() == 1);
    }
    SECTION("a choke while already releasing")
    {
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(
<control> oscairn_profile=1
<global> sample=*sine oscairn_ampeg_release=100
<region> key=60 group=1 off_by=2
<region> key=62 group=2
)");
        synth.noteOn(0, 60, 100);
        render(10);
        synth.noteOff(0, 60, 0);
        render(10);
        REQUIRE(synth.getNumActiveVoices() == 1);
        synth.noteOn(0, 62, 100);
        render(10);
        REQUIRE(synth.getNumActiveVoices() == 1);
    }
}

TEST_CASE("[Oscairn] Element filter stays finite below the reference rate")
{
    // Below 44.1 kHz the allpass delay cannot be pinned (its points lie above Nyquist) and
    // went unstable; such rates run the plain 44.1 kHz sections instead: the response is
    // off, the output finite
    for (double rate : { 22050.0, 32000.0 }) {
        for (int type : { 0, 2, 4, 10, 14, 19 }) {
            for (int cutoff : { 0, 512, 1023 }) {
                sfz::oscairn::ElementFilterParameters p;
                p.type = type;
                p.cutoff = cutoff;
                p.resonance = 127;
                sfz::oscairn::ElementFilter filter;
                filter.setSampleRate(rate);
                filter.start(p, 127);
                std::vector<float> left(4096, 0.0f), right(4096, 0.0f);
                left[0] = right[0] = 1.0f;
                filter.process(absl::MakeSpan(left), absl::MakeSpan(right), {});
                float peak = 0.0f;
                for (float x : left)
                    peak = std::max(peak, std::fabs(x));
                INFO("rate " << rate << " type " << type << " cutoff " << cutoff);
                REQUIRE(std::isfinite(peak));
                REQUIRE(peak < 1e3f);
            }
        }
    }
}

TEST_CASE("[Oscairn] Hermite interpolation four outputs at a time: the same samples")
{
#if SFIZZ_HERMITE3_FOUR
    // Against interpolate<kInterpolatorHermite3> one output at a time, to the bit: random and
    // edge positions (0, just below 1, tiny), sources from silence to full scale and beyond
    std::minstd_rand rng(7);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f), wide(-4.0f, 4.0f);
    std::vector<float> left(4096), right(4096);
    for (size_t i = 0; i < left.size(); ++i) {
        left[i] = wide(rng) * (i % 7 == 0 ? 1e-6f : 1.0f);
        right[i] = wide(rng);
    }
    const float edges[] = { 0.0f, 0x1.fffffep-1f, 1e-30f, 0.5f, 0.25f, 1e-7f };
    for (int round = 0; round < 20000; ++round) {
        int indices[4];
        float coeffs[4], out[2][4];
        for (int j = 0; j < 4; ++j) {
            indices[j] = 1 + static_cast<int>(unit(rng) * 4000.0f);
            coeffs[j] = round % 3 == 0 ? edges[(round / 3 + j) % 6] : std::min(unit(rng), 0x1.fffffep-1f);
        }
        const float* sources[2] = { left.data(), right.data() };
        float* outputs[2] = { out[0], out[1] };
        sfz::interpolateHermite3Four(sources, 2, indices, coeffs, outputs);
        for (int j = 0; j < 4; ++j) {
            const float l = sfz::interpolate<sfz::kInterpolatorHermite3>(&left[indices[j]], coeffs[j]);
            const float r = sfz::interpolate<sfz::kInterpolatorHermite3>(&right[indices[j]], coeffs[j]);
            INFO("index " << indices[j] << " coefficient " << coeffs[j]);
            REQUIRE(std::memcmp(&l, &out[0][j], sizeof(float)) == 0);
            REQUIRE(std::memcmp(&r, &out[1][j], sizeof(float)) == 0);
        }
    }
#else
    SUCCEED("no four-output path on this architecture");
#endif
}

TEST_CASE("[Oscairn] Pan and width of an unmodulated voice: the same samples as per-sample")
{
    // panConstant and widthConstant (the pan stages without modulation) against pan() and
    // width() with the envelope filled, to the bit: every 1/4096 of the range and the values
    // between table steps, at every alignment and at lengths around the vector size
    alignas(16) float env[160], l0[160], r0[160], l1[160], r1[160];
    std::minstd_rand rng(1);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (int width = 0; width < 2; ++width) {
        for (int step = -4200; step <= 4200; ++step) {
            const float value = static_cast<float>(step) / 4096.0f + (step % 3) * 0.37f / 4096.0f;
            for (unsigned offset : { 0u, 1u, 3u }) {
                for (unsigned size : { 1u, 3u, 4u, 7u, 128u }) {
                    for (unsigned i = 0; i < 160; ++i) {
                        env[i] = value;
                        l0[i] = l1[i] = noise(rng);
                        r0[i] = r1[i] = noise(rng);
                    }
                    if (width) {
                        sfz::width(env + offset, l0 + offset, r0 + offset, size);
                        sfz::widthConstant(env + offset, l1 + offset, r1 + offset, size);
                    } else {
                        sfz::pan(env + offset, l0 + offset, r0 + offset, size);
                        sfz::panConstant(env + offset, l1 + offset, r1 + offset, size);
                    }
                    INFO("width " << width << " value " << value << " offset " << offset << " size " << size);
                    REQUIRE(std::memcmp(l0, l1, sizeof(l0)) == 0);
                    REQUIRE(std::memcmp(r0, r1, sizeof(r0)) == 0);
                }
            }
        }
    }
}

TEST_CASE("[Oscairn] Element filter: the loops without branches match the earlier ones")
{
    // The vector loops (two sections in lanes at 48 and 96 kHz, the channels in lanes at the
    // other rates) round differently from the earlier loops where the compiler orders a sum
    // differently (-ffast-math), so the check is a level: the difference stays more than
    // 85 dB below the output (measured: -97 dB at worst, a resonant low cutoff), for every
    // type, mono and stereo, with the cutoff moving and blocks of any length
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 }) {
        for (int type : { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 19, 21 }) {
            for (int resonance : { 0, 127 }) {
                for (int stereo = 0; stereo < 2; ++stereo) {
                    sfz::oscairn::ElementFilterParameters p;
                    p.type = type;
                    p.cutoff = 300;
                    p.resonance = resonance;
                    p.hpfCutoff = 200;
                    p.distance = -20;
                    std::vector<float> out[2][2];
                    for (int k = 0; k < 2; ++k) {
                        sfz::oscairn::ElementFilter f;
                        f.setLaneLoops(k == 0);
                        f.setSampleRate(rate);
                        f.start(p, 64, 100);
                        std::minstd_rand rng(type);
                        std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
                        for (auto& ch : out[k]) {
                            ch.resize(6000);
                            for (float& x : ch)
                                x = noise(rng);
                        }
                        const size_t lengths[] = { 1, 128, 37, 16, 2, 300 };
                        for (size_t i = 0, b = 0; i < out[k][0].size(); ++b) {
                            const size_t len = std::min(lengths[b % 6], out[k][0].size() - i);
                            std::vector<float> cents(len);
                            for (size_t j = 0; j < len; ++j)
                                cents[j] = 1200.0f * std::sin(0.002f * static_cast<float>(i + j));
                            f.process(absl::MakeSpan(out[k][0]).subspan(i, len),
                                stereo ? absl::MakeSpan(out[k][1]).subspan(i, len) : absl::Span<float>(), cents);
                            i += len;
                        }
                    }
                    double difference = 0.0, level = 0.0;
                    for (int ch = 0; ch <= stereo; ++ch) {
                        for (size_t i = 0; i < out[0][ch].size(); ++i) {
                            const double d = static_cast<double>(out[0][ch][i]) - out[1][ch][i];
                            difference += d * d;
                            level += static_cast<double>(out[1][ch][i]) * out[1][ch][i];
                        }
                    }
                    INFO("rate " << rate << " type " << type << " resonance " << resonance << " stereo " << stereo);
                    REQUIRE(level > 0.0);
                    REQUIRE(difference <= level * 3.2e-9); // -85 dB
                }
            }
        }
    }
}

namespace {
// A patch of sine elements, element i + 1 with opcodes elements[i]
std::string elementPatch(const std::vector<std::string>& elements, const std::string& common = "")
{
    std::string text = "<control> oscairn_profile=1 <global> amp_veltrack=0 " + common + "\n";
    for (size_t i = 0; i < elements.size(); ++i)
        text += "<region> sample=*sine oscairn_element=" + std::to_string(i + 1) + " " + elements[i] + "\n";
    return text;
}

struct ElementPlayer {
    sfz::Synth synth;
    sfz::AudioBuffer<float> buffer { 2, 64 };
    explicit ElementPlayer(const std::string& text)
    {
        synth.setSamplesPerBlock(64);
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", text);
    }
    void on(int key, int velocity = 100) { synth.noteOn(0, key, velocity); synth.renderBlock(buffer); }
    void off(int key) { synth.noteOff(0, key, 0); synth.renderBlock(buffer); }
    void cc(int number, float value) { synth.hdcc(0, number, value); }
    // The elements sounding for a key (started and not released), in element order
    std::vector<int> sounding(int key) const
    {
        std::vector<int> out;
        for (const sfz::Voice* v : synth.getActiveVoices())
            if (!v->released() && v->getTriggerEvent().number == key)
                out.push_back(v->getRegion()->oscairn.element.number);
        std::sort(out.begin(), out.end());
        return out;
    }
    // Plays a short note and returns its elements
    std::vector<int> note(int key, int velocity = 100)
    {
        on(key, velocity);
        auto s = sounding(key);
        off(key);
        return s;
    }
};

using Elements = std::vector<int>;
} // namespace

TEST_CASE("[Oscairn] Element selection opcodes")
{
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", R"(
        <region> sample=*sine oscairn_element=3 oscairn_xa=3
        <control> oscairn_profile=1
        <region> sample=*sine oscairn_element=7 oscairn_xa=4 oscairn_element_group=2 oscairn_element_lokey=c4
            oscairn_element_hikey=72 oscairn_element_lovel=10 oscairn_element_hivel=90 oscairn_mono=1
        <region> sample=*sine oscairn_element=200 oscairn_xa=9 oscairn_element_group=0
    )");
    REQUIRE(isUnknown(synth, "oscairn_element"));
    REQUIRE(synth.getRegionView(0)->oscairn.element.number == 0);
    const auto& e = synth.getRegionView(1)->oscairn;
    REQUIRE(e.element.number == 7);
    REQUIRE(e.element.mode == sfz::oscairn::kRandom);
    REQUIRE(e.element.group == 2);
    REQUIRE(e.element.loKey == 60);
    REQUIRE(e.element.hiKey == 72);
    REQUIRE(e.element.loVel == 10);
    REQUIRE(e.element.hiVel == 90);
    REQUIRE(e.mono);
    const auto& c = synth.getRegionView(2)->oscairn.element; // clamped
    REQUIRE(c.number == 128);
    REQUIRE(c.mode == sfz::oscairn::kSwitch2On);
    REQUIRE(c.group == 1);
}

TEST_CASE("[Oscairn] XA Cycle: one position per element group, across keys")
{
    // Reference renders (xa1 cyc3, cycrange, cyc2grp, cycheld)
    ElementPlayer three { elementPatch({ "oscairn_xa=3", "oscairn_xa=3", "oscairn_xa=3" }) };
    std::vector<Elements> got;
    for (int i = 0; i < 7; ++i)
        got.push_back(three.note(60));
    REQUIRE(got == std::vector<Elements> { { 1 }, { 2 }, { 3 }, { 1 }, { 2 }, { 3 }, { 1 } });

    // an element whose limits leave a key out is passed over on that key
    ElementPlayer range { elementPatch({ "oscairn_xa=3", "oscairn_xa=3 oscairn_element_lokey=60 oscairn_element_hikey=60", "oscairn_xa=3" }) };
    got.clear();
    for (int key : { 60, 62, 60, 62, 62, 60, 60, 62, 60, 60 })
        got.push_back(range.note(key));
    REQUIRE(got == std::vector<Elements> { { 1 }, { 3 }, { 1 }, { 3 }, { 1 }, { 2 }, { 3 }, { 1 }, { 2 }, { 3 } });

    // two groups count apart; a Normal element plays beside them
    ElementPlayer groups { elementPatch({ "oscairn_xa=3", "oscairn_xa=3", "oscairn_xa=3 oscairn_element_group=2",
                                          "oscairn_xa=3 oscairn_element_group=2", "oscairn_xa=3 oscairn_element_group=2", "" }) };
    got.clear();
    for (int i = 0; i < 6; ++i)
        got.push_back(groups.note(60));
    REQUIRE(got == std::vector<Elements> { { 1, 3, 6 }, { 2, 4, 6 }, { 1, 5, 6 }, { 2, 3, 6 }, { 1, 4, 6 }, { 2, 5, 6 } });

    // the position is the part's, whatever keys are held
    ElementPlayer held { elementPatch({ "oscairn_xa=3", "oscairn_xa=3", "oscairn_xa=3" }) };
    held.on(48);
    REQUIRE(held.note(60) == Elements { 2 });
    REQUIRE(held.note(64) == Elements { 3 });
    REQUIRE(held.sounding(48) == Elements { 1 });
    held.off(48);
    REQUIRE(held.note(60) == Elements { 1 });

    // a reload or a restart starts over
    held.note(60);
    held.synth.restart();
    REQUIRE(held.note(60) == Elements { 1 });
    held.note(60);
    held.synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", elementPatch({ "oscairn_xa=3", "oscairn_xa=3", "oscairn_xa=3" }));
    REQUIRE(held.note(60) == Elements { 1 });
}

TEST_CASE("[Oscairn] XA Random: each covering element alike, the same after a restart")
{
    // Reference (xa1 rand1, rand3, rand3b, rand2grp): one random element always plays; three
    // play alike (206, 214, 180 of 600 notes); groups draw apart
    ElementPlayer one { elementPatch({ "oscairn_xa=4" }) };
    for (int i = 0; i < 600; ++i)
        REQUIRE(one.note(60) == Elements { 1 });

    ElementPlayer three { elementPatch({ "oscairn_xa=4", "oscairn_xa=4", "oscairn_xa=4" }) };
    std::vector<Elements> first;
    int counts[4] {};
    for (int i = 0; i < 3000; ++i) {
        first.push_back(three.note(60));
        REQUIRE(first.back().size() == 1);
        ++counts[first.back()[0]];
    }
    for (int k = 1; k <= 3; ++k)
        REQUIRE(std::abs(counts[k] - 1000) < 100);
    three.synth.restart();
    for (int i = 0; i < 50; ++i)
        REQUIRE(three.note(60) == first[static_cast<size_t>(i)]);

    // limits and groups: element 2 covers note 60 only; group 2 draws on its own
    ElementPlayer groups { elementPatch({ "oscairn_xa=4", "oscairn_xa=4 oscairn_element_lokey=60 oscairn_element_hikey=60",
                                          "oscairn_xa=4 oscairn_element_group=2", "oscairn_xa=4 oscairn_element_group=2", "" }) };
    // (a draw of 255 takes the sound's first element if it is one of the group's: group 2,
    // without it, then plays none, one note in 256)
    int two = 0, g3 = 0, g4 = 0;
    for (int i = 0; i < 400; ++i) {
        const Elements a = groups.note(62);
        REQUIRE(a.front() == 1);
        REQUIRE(a.back() == 5);
        const Elements b = groups.note(60);
        two += b.front() == 2;
        g3 += std::count(b.begin(), b.end(), 3);
        g4 += std::count(b.begin(), b.end(), 4);
    }
    REQUIRE(std::abs(two - 200) < 40);
    REQUIRE(std::abs(g3 - 200) < 40);
    REQUIRE(std::abs(g4 - 200) < 40);
}

TEST_CASE("[Oscairn] XA Legato: a mono part's notes while another key is held")
{
    // Reference (xa1 leg_mono, leg_poly, leg_groups, leg_cycle)
    auto play = [](const std::vector<std::string>& elements, bool mono) {
        ElementPlayer p { elementPatch(elements, mono ? "oscairn_mono=1" : "") };
        std::vector<Elements> got;
        p.on(60);
        got.push_back(p.sounding(60));
        p.on(62);
        got.push_back(p.sounding(62));
        got.push_back(p.sounding(60)); // a mono part's newer note cuts the held one
        p.off(60);
        p.on(64);
        got.push_back(p.sounding(64));
        p.off(62);
        p.off(64);
        got.push_back(p.note(60));
        p.on(60);
        p.on(62);
        p.off(62); // a mono part starts the held key again, as a legato note
        got.push_back(p.sounding(60));
        p.off(60);
        got.push_back(p.note(62));
        return got;
    };
    REQUIRE(play({ "", "oscairn_xa=1" }, true) == std::vector<Elements> { { 1 }, { 2 }, {}, { 2 }, { 1 }, { 2 }, { 1 } });
    REQUIRE(play({ "", "oscairn_xa=1" }, false) == std::vector<Elements> { { 1 }, { 1 }, { 1 }, { 1 }, { 1 }, { 1 }, { 1 } });
    // a Legato element plays only beside a plain element of its group, which counts whatever
    // its limits; a Normal element gives way only to a Legato element of its own group
    REQUIRE(play({ "", "oscairn_xa=1 oscairn_element_group=2", "oscairn_xa=1 oscairn_element_group=3",
                   "oscairn_element_group=3 oscairn_element_lokey=100 oscairn_element_hikey=110" }, true)
        == std::vector<Elements> { { 1 }, { 1, 3 }, {}, { 1, 3 }, { 1 }, { 1, 3 }, { 1 } });
    // Cycle elements keep cycling under legato notes
    REQUIRE(play({ "oscairn_xa=3", "oscairn_xa=3", "oscairn_xa=1" }, true)
        == std::vector<Elements> { { 1 }, { 2, 3 }, {}, { 1, 3 }, { 2 }, { 1, 3 }, { 2 } });
}

TEST_CASE("[Oscairn] XA A.SW: the assignable switches at note-on")
{
    // Reference (xa1 asw, asw_held, asw_leg): on from 64 of 127, read at note-on
    using sfz::oscairn::kAssignSwitchCC;
    ElementPlayer p { elementPatch({ "oscairn_xa=5", "oscairn_xa=6", "oscairn_xa=7", "" }) };
    const float on = 64 / 127.0f, off = 63 / 127.0f;
    std::vector<Elements> got;
    got.push_back(p.note(60));
    p.cc(kAssignSwitchCC[0], 1.0f);
    got.push_back(p.note(60));
    p.cc(kAssignSwitchCC[1], 1.0f);
    got.push_back(p.note(60));
    p.cc(kAssignSwitchCC[0], 0.0f);
    got.push_back(p.note(60));
    p.cc(kAssignSwitchCC[1], off);
    got.push_back(p.note(60));
    p.cc(kAssignSwitchCC[1], on);
    got.push_back(p.note(60));
    REQUIRE(got == std::vector<Elements> { { 1, 4 }, { 2, 4 }, { 2, 3, 4 }, { 3, 4 }, { 1, 4 }, { 3, 4 } });

    // a held note keeps its elements
    p.cc(kAssignSwitchCC[1], 0.0f);
    p.on(60);
    p.cc(kAssignSwitchCC[0], 1.0f);
    p.synth.renderBlock(p.buffer);
    REQUIRE(p.sounding(60) == Elements { 1, 4 });
    p.off(60);

    // an A.SW element counts as plain for legato only while its switch is on
    ElementPlayer leg { elementPatch({ "oscairn_xa=6", "oscairn_xa=1" }, "oscairn_mono=1") };
    leg.on(60);
    leg.on(62);
    REQUIRE(leg.sounding(60).empty());
    REQUIRE(leg.sounding(62).empty());
    leg.off(60);
    leg.off(62);
    leg.cc(kAssignSwitchCC[0], 1.0f);
    leg.on(60);
    leg.on(62);
    REQUIRE(leg.sounding(62) == Elements { 2 });
}

TEST_CASE("[Oscairn] Key On Delay: time length and tempo-synced note length")
{
    // First sounding sample of a note, in seconds; -1 if none within `seconds`
    auto onset = [](const std::string& opcodes, double rate, double seconds, float bpm = 0.0f, double noteOff = -1.0) {
        sfz::Synth synth;
        synth.setSampleRate(static_cast<float>(rate));
        synth.setSamplesPerBlock(256);
        synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", "<control> oscairn_profile=1 <region> sample=*sine " + opcodes);
        if (bpm > 0.0f)
            synth.bpmTempo(0, bpm);
        sfz::AudioBuffer<float> buffer { 2, 256 };
        synth.noteOn(0, 60, 100);
        const long off = noteOff < 0 ? -1 : static_cast<long>(noteOff * rate);
        for (long start = 0; start < static_cast<long>(seconds * rate); start += 256) {
            if (off >= start && off < start + 256)
                synth.noteOff(static_cast<int>(off - start), 60, 0);
            synth.renderBlock(buffer);
            const auto left = buffer.getConstSpan(0);
            for (size_t i = 0; i < left.size(); ++i)
                if (left[i] != 0.0f)
                    return static_cast<double>(start + static_cast<long>(i)) / rate;
        }
        return -1.0;
    };
    // Reference delays (xa1, xa1b kod_*): onset of the delayed element after the plain one
    for (double rate : { 44100.0, 48000.0, 96000.0 }) {
        REQUIRE(onset("", rate, 0.1) < 1e-4); // a sine's first sample is 0
        REQUIRE(onset("oscairn_delay=1", rate, 1.0) == Approx(0.0096).margin(0.0002));
        REQUIRE(onset("oscairn_delay=16", rate, 1.0) == Approx(0.3651).margin(0.0002));
        REQUIRE(onset("oscairn_delay=64", rate, 4.0) == Approx(2.8961).margin(0.0002));
        REQUIRE(onset("oscairn_delay=127", rate, 9.0) == Approx(8.0847).margin(0.0002));
        REQUIRE(onset("oscairn_delay=10", rate, 1.0) == Approx((0.1304 + 0.2388) / 2).margin(0.0002)); // between knots
        // note lengths at the default 120 BPM (measured: a 1/16 0.124 s, a 1/4 0.499 s, a 1/2 1.004 s)
        REQUIRE(onset("oscairn_delay_note=5", rate, 1.0) == Approx(0.125).margin(0.001));
        REQUIRE(onset("oscairn_delay_note=11", rate, 1.0) == Approx(0.5).margin(0.001));
        REQUIRE(onset("oscairn_delay_note=14", rate, 2.0) == Approx(1.0).margin(0.001));
        // triplets (xa2: an 1/8 triplet 0.164 s, a 1/4 triplet 0.337 s, a 1/2 triplet 0.660 s, a
        // whole triplet 1.330 s)
        REQUIRE(onset("oscairn_delay_note=6", rate, 1.0) == Approx(1.0 / 6).margin(0.001));
        REQUIRE(onset("oscairn_delay_note=9", rate, 1.0) == Approx(1.0 / 3).margin(0.001));
        REQUIRE(onset("oscairn_delay_note=12", rate, 1.0) == Approx(2.0 / 3).margin(0.001));
        REQUIRE(onset("oscairn_delay_note=15", rate, 2.0) == Approx(4.0 / 3).margin(0.001));
    }
    // the tempo sets the note length; the note length wins over a time length
    REQUIRE(onset("oscairn_delay_note=11 oscairn_delay=100", 48000.0, 2.0, 60.0f) == Approx(1.0).margin(0.001));
    // released before the delay ends, it never sounds (xa1 kod_short)
    REQUIRE(onset("oscairn_delay=80", 48000.0, 5.0, 0.0f, 0.1) < 0.0);
}

TEST_CASE("[Oscairn] Element voices take the element sample quality when they set none")
{
    sfz::Synth synth;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/element_quality.sfz", R"(
        <region> key=60 sample=*sine
        <region> key=62 sample=*sine sample_quality=4
        <control> oscairn_profile=1
        <group> oscairn_ampeg_attack=0 oscairn_ampeg_decay1=0 oscairn_ampeg_decay2=0 oscairn_ampeg_release=0
        <region> key=64 sample=*sine
        <region> key=65 sample=*sine sample_quality=3
    )");
    auto quality = [&](int key) {
        synth.allSoundOff();
        synth.noteOn(0, key, 100);
        for (int i = 0; i < synth.getNumVoices(); ++i)
            if (const sfz::Voice* v = synth.getVoiceView(i))
                if (!v->isFree())
                    return v->getCurrentSampleQuality();
        return -1;
    };
    const int live = synth.getSampleQuality(sfz::Synth::ProcessLive);
    REQUIRE(quality(64) == live); // unset: as every voice
    synth.setElementSampleQuality(1);
    REQUIRE(quality(64) == 1);
    REQUIRE(quality(65) == 3); // the sound's own sample_quality comes first
    REQUIRE(quality(60) == live); // a stock voice keeps the synth's quality
    REQUIRE(quality(62) == 4);
    synth.enableFreeWheeling(); // offline renders keep the freewheeling quality
    REQUIRE(quality(64) == synth.getSampleQuality(sfz::Synth::ProcessFreewheeling));
    synth.disableFreeWheeling();
    synth.setElementSampleQuality(-1);
    REQUIRE(quality(64) == live);
}

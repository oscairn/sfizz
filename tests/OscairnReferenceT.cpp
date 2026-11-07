// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// The profile's amplitude envelope and level response against statistics of reference renders
// (doc/oscairn-plan.md, sections 6 and 11): OscairnReferenceData.h holds numbers measured from
// the renders, never audio, for probes that were not used to fit the engine's tables and laws.
// Each probe is rendered here through the whole voice and measured as the measurement
// repository measures the renders (oscairn/measurement awm2/refstats.py): a sine of the
// probe's note, its level demodulated at the sine's frequency over a Hann window of four
// periods.

#include "sfizz/Synth.h"
#include "TestHelpers.h"
#include "catch2/catch.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

enum Kind { AttackSeconds, DecayPer60, ReleasePer60, ReleaseRampSeconds, LevelDb };

struct ReferenceProbe {
    const char* name;    // the reference render
    const char* opcodes; // region opcodes, under the profile
    int note, velocity;
    double hold, tail;   // seconds
    Kind kind;
    double value, value2; // the reference's measurement (levels: left and right, dB)
    double top, bottom;   // falls: the dB range of the straight-line fit below the start level
};

#include "OscairnReferenceData.h"

struct Voice {
    std::vector<float> left, right;
    double rate;
};

Voice render(const ReferenceProbe& p, double rate)
{
    sfz::Synth synth;
    synth.setSampleRate(static_cast<float>(rate));
    synth.setSamplesPerBlock(256);
    const std::string text = std::string("<control> oscairn_profile=1 <region> sample=*sine amp_veltrack=0 ") + p.opcodes;
    synth.loadSfzString(fs::current_path() / "tests/TestFiles/oscairn.sfz", text);
    sfz::AudioBuffer<float> buffer { 2, 256 };
    Voice v { {}, {}, rate };
    const size_t total = static_cast<size_t>((p.hold + p.tail) * rate), off = static_cast<size_t>(p.hold * rate);
    v.left.reserve(total + 256);
    v.right.reserve(total + 256);
    synth.noteOn(0, p.note, p.velocity);
    for (size_t done = 0; done < total; done += 256) {
        if (done <= off && off < done + 256)
            synth.noteOff(static_cast<int>(off - done), p.note, 0);
        synth.renderBlock(buffer);
        v.left.insert(v.left.end(), buffer.getConstSpan(0).begin(), buffer.getConstSpan(0).end());
        v.right.insert(v.right.end(), buffer.getConstSpan(1).begin(), buffer.getConstSpan(1).end());
    }
    return v;
}

// First frame whose magnitude exceeds 1e-3 of the largest
size_t onset(const std::vector<float>& x)
{
    float peak = 0.0f;
    for (float s : x)
        peak = std::max(peak, std::fabs(s));
    for (size_t i = 0; i < x.size(); ++i)
        if (std::fabs(x[i]) > 1e-3f * peak)
            return i;
    return 0;
}

// Amplitude of the sine at `hz` around frame `centre`: the signal shifted to 0 Hz, averaged
// over a Hann window of four of its periods
double demodulated(const std::vector<float>& x, double rate, double hz, long centre)
{
    const long n = std::max(3L, std::lround(4.0 * rate / hz));
    const double w = 2.0 * M_PI * hz / rate;
    double re = 0.0, im = 0.0, sum = 0.0;
    for (long k = 0; k < n; ++k) {
        const long i = centre - n / 2 + k;
        const double h = 0.5 - 0.5 * std::cos(2.0 * M_PI * (k + 1) / (n + 1));
        sum += h;
        if (i < 0 || i >= static_cast<long>(x.size()))
            continue;
        re += h * x[i] * std::cos(w * i);
        im -= h * x[i] * std::sin(w * i);
    }
    return 2.0 * std::sqrt(re * re + im * im) / sum;
}

struct Envelope {
    std::vector<double> t, a; // seconds, amplitude
};

Envelope envelope(const std::vector<float>& x, double rate, double hz, double t0, double t1, double hop)
{
    Envelope e;
    const long step = std::max(1L, std::lround(hop * rate));
    for (long i = std::max(0L, std::lround(t0 * rate)); i < std::min(static_cast<long>(x.size()), std::lround(t1 * rate)); i += step) {
        e.t.push_back(i / rate);
        e.a.push_back(demodulated(x, rate, hz, i));
    }
    return e;
}

// Least-squares slope of y over x
double slope(const std::vector<double>& x, const std::vector<double>& y)
{
    const double n = static_cast<double>(x.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < x.size(); ++i)
        sx += x[i], sy += y[i], sxx += x[i] * x[i], sxy += x[i] * y[i];
    return (n * sxy - sx * sy) / (n * sxx - sx * sx);
}

double measure(const ReferenceProbe& p, const Voice& v, double& second)
{
    const double rate = v.rate;
    const double hz = 440.0 * std::exp2((p.note - 69) / 12.0);
    const double on = onset(v.left) / rate;
    if (p.kind == LevelDb) {
        auto level = [&](const std::vector<float>& x) {
            double s = 0.0;
            const size_t a = static_cast<size_t>((on + 0.15) * rate), b = static_cast<size_t>((on + 0.5) * rate);
            for (size_t i = a; i < b; ++i)
                s += static_cast<double>(x[i]) * x[i];
            return 20.0 * std::log10(std::sqrt(2.0 * s / static_cast<double>(b - a)) + 1e-30);
        };
        second = level(v.right);
        return level(v.left);
    }
    if (p.kind == AttackSeconds) {
        const Envelope e = envelope(v.left, rate, hz, on - 0.002, on + 3.0 * p.value + 0.004, p.value / 400.0);
        const double peak = *std::max_element(e.a.begin(), e.a.end());
        std::vector<double> t, a;
        bool started = false;
        for (size_t i = 0; i < e.t.size(); ++i) {
            const double r = e.a[i] / peak;
            if (r >= 0.8)
                break;
            started = started || r >= 0.2;
            if (started)
                t.push_back(e.t[i]), a.push_back(r);
        }
        return t.size() >= 3 ? 1.0 / slope(t, a) : 0.0;
    }
    // falls: the start, then a straight line in dB (or in amplitude for the fast release)
    const bool release = p.kind == ReleasePer60 || p.kind == ReleaseRampSeconds;
    const double expected = p.kind == ReleaseRampSeconds ? p.value : p.value * p.bottom / 60.0;
    const double hop = std::max(1.0 / rate, expected / 400.0);
    double start, startA;
    if (!release) {
        const Envelope e = envelope(v.left, rate, hz, on, on + 0.05, 1.0 / rate);
        const size_t k = std::max_element(e.a.begin(), e.a.end()) - e.a.begin();
        start = e.t[k], startA = e.a[k];
    } else {
        const double off = on + p.hold;
        const Envelope held = envelope(v.left, rate, hz, off - 0.1, off - 0.02, 0.5e-3);
        std::vector<double> a = held.a;
        std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end());
        startA = a[a.size() / 2];
        const Envelope e = envelope(v.left, rate, hz, off - 0.05, off + 0.05, 1.0 / rate);
        start = off;
        for (size_t i = 0; i < e.t.size(); ++i)
            if (e.a[i] <= startA * std::pow(10.0, -0.2 / 20.0)) {
                start = e.t[i];
                break;
            }
    }
    const double end = release ? p.hold + p.tail : p.hold - 0.005;
    const Envelope e = envelope(v.left, rate, hz, start, std::min(on + end, start + 3.0 * expected + 0.01), hop);
    std::vector<double> t, y;
    if (p.kind == ReleaseRampSeconds) {
        for (size_t i = 0; i < e.t.size(); ++i) {
            const double r = e.a[i] / startA;
            if (r < 0.2)
                break;
            if (r <= 0.8)
                t.push_back(e.t[i]), y.push_back(r);
        }
        return t.size() >= 3 ? -1.0 / slope(t, y) : 0.0;
    }
    for (size_t i = 0; i < e.t.size(); ++i) {
        const double db = 20.0 * std::log10(e.a[i] / startA + 1e-30);
        if (db < -p.bottom)
            break;
        if (db <= -p.top)
            t.push_back(e.t[i]), y.push_back(db);
    }
    return t.size() >= 4 ? -60.0 / slope(t, y) : 0.0;
}

// Proposed tolerance for segment times (doc/oscairn-plan.md, decision 10); accepted for levels
// (decision 14)
constexpr double timeTolerance = 0.025;
constexpr double levelToleranceDb = 0.4;

} // namespace

TEST_CASE("[Oscairn] Envelope and level against reference statistics")
{
    for (double rate : { 44100.0, 48000.0, 96000.0 }) {
        double fullLeft = 0.0, fullRight = 0.0;
        for (const ReferenceProbe& p : referenceProbes) {
            if (std::string(p.name) == "full") {
                fullLeft = measure(p, render(p, rate), fullRight);
                break;
            }
        }
        for (const ReferenceProbe& p : referenceProbes) {
            if (std::string(p.name) == "full")
                continue;
            INFO("rate " << rate << " probe " << p.name << " (" << p.opcodes << ", note " << p.note << ", velocity " << p.velocity << ")");
            double second = 0.0;
            const double got = measure(p, render(p, rate), second);
            if (p.kind == LevelDb) {
                // relative to the full-level probe: silent where the reference is, within the
                // accepted tolerance at -24 dB and above (decision 14; below it, K1)
                auto check = [&](double engine, double reference) {
                    if (reference < -90.0)
                        REQUIRE(engine < -90.0);
                    else if (reference >= -24.0)
                        REQUIRE(engine == Approx(reference).margin(levelToleranceDb));
                };
                check(got - fullLeft, p.value);
                check(second - fullRight, p.value2);
            } else {
                REQUIRE(got == Approx(p.value).epsilon(timeTolerance));
            }
        }
    }
}

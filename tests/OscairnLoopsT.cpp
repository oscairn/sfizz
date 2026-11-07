// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Loop continuity over 30 s notes (doc/oscairn-plan.md, milestone 1; RFC 0013 step 1), on
// samples that need no imported library: the click measure of the measurement repository's
// awm2/loops.py, which checks every looped region of a library the same way. At each pass
// through the loop point: the largest third difference within 1 ms of the pass (an eighth of
// the loop if shorter) over the largest in the 50 ms around it, the pass windows left out;
// passes under -80 dBFS count as continuous.

#include "sfizz/Synth.h"
#include "TestHelpers.h"
#include "catch2/catch.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Proposed threshold (doc/oscairn-plan.md, decision 29): a pass twice as rough as anything
// around it; continuous loops measure about 1 or less
constexpr double loopThreshold = 2.0;

// The largest click measure over a render's loop passes; `first` and `period` in output frames
double clickScore(const std::vector<float>& x, double rate, double first, double period)
{
    const size_t n = x.size() - 3;
    std::vector<double> d(n);
    for (size_t i = 0; i < n; ++i)
        d[i] = std::fabs(x[i + 3] - 3.0 * x[i + 2] + 3.0 * x[i + 1] - x[i]);
    const long w = std::max(2L, static_cast<long>(std::min(1e-3 * rate, period / 8.0)));
    const long around = static_cast<long>(0.05 * rate);
    std::vector<long> passes;
    for (double p = first; p + around < static_cast<double>(n); p += period)
        passes.push_back(std::lround(p));
    std::vector<bool> excluded(n, false);
    for (long c : passes)
        for (long i = std::max(0L, c - w); i <= std::min(static_cast<long>(n) - 1, c + w); ++i)
            excluded[i] = true;
    double worst = 0.0;
    for (long c : passes) {
        double peak = 0.0, ref = 0.0;
        for (long i = std::max(0L, c - w); i <= std::min(static_cast<long>(n) - 1, c + w); ++i)
            peak = std::max(peak, d[i]);
        for (long i = std::max(0L, c - around); i <= std::min(static_cast<long>(n) - 1, c + around); ++i)
            if (!excluded[i])
                ref = std::max(ref, d[i]);
        if (peak >= 1e-4) // under -80 dBFS nothing audible happens
            worst = std::max(worst, ref > 0.0 ? peak / ref : 1e9);
    }
    return worst;
}

void put16(std::FILE* f, unsigned v) { std::fputc(v & 0xff, f), std::fputc((v >> 8) & 0xff, f); }
void put32(std::FILE* f, unsigned v) { put16(f, v & 0xffff), put16(f, v >> 16); }

// A 16-bit mono WAV
void writeWav(const fs::path& path, const std::vector<float>& x, unsigned rate)
{
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f);
    const unsigned bytes = static_cast<unsigned>(2 * x.size());
    std::fwrite("RIFF", 1, 4, f), put32(f, 36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16), put16(f, 1), put16(f, 1), put32(f, rate), put32(f, 2 * rate), put16(f, 2), put16(f, 16);
    std::fwrite("data", 1, 4, f), put32(f, bytes);
    for (float s : x)
        put16(f, static_cast<unsigned>(static_cast<int>(std::lround(32767.0f * s))) & 0xffff);
    std::fclose(f);
}

// A held note at the sample's own pitch, `seconds` long at `rate`, left channel
std::vector<float> renderLoop(const fs::path& sfzPath, const std::string& region, double rate, double seconds)
{
    sfz::Synth synth;
    synth.setSampleRate(static_cast<float>(rate));
    synth.setSamplesPerBlock(512);
    synth.enableFreeWheeling(); // each block waits for the samples it streams, however busy the machine
    synth.loadSfzString(sfzPath, "<region> pitch_keycenter=60 " + region);
    REQUIRE(synth.getNumRegions() == 1);
    sfz::AudioBuffer<float> buffer { 2, 512 };
    std::vector<float> out;
    synth.noteOn(0, 60, 127);
    for (size_t done = 0; done < static_cast<size_t>(seconds * rate); done += 512) {
        synth.renderBlock(buffer);
        out.insert(out.end(), buffer.getConstSpan(0).begin(), buffer.getConstSpan(0).end());
    }
    return out;
}

// A harmonic tone at 44.1 kHz whose period is a whole number of frames, so that a loop of
// whole periods is seamless in the data; before `from` it swells into that level, as a recording
// does before the sustain a loop is cut from (continuous into it)
std::vector<float> tone(size_t frames, int period, size_t from = 0)
{
    std::vector<float> x(frames);
    for (size_t i = 0; i < frames; ++i) {
        const double ph = 2.0 * M_PI * static_cast<double>(i % period) / period;
        const double gain = i < from ? 0.2 + 0.8 * static_cast<double>(i) / static_cast<double>(from) : 1.0;
        x[i] = static_cast<float>(gain * (0.4 * std::sin(ph) + 0.15 * std::sin(2 * ph) + 0.05 * std::sin(5 * ph)));
    }
    return x;
}

} // namespace

TEST_CASE("[Oscairn] Loops are continuous over 30 s notes")
{
    const double rate = 48000.0, seconds = 30.0, src = 44100.0, q = src / rate;
    const fs::path dir = fs::temp_directory_path() / "sfizz-oscairn-loops";
    fs::create_directories(dir);
    const fs::path sfzPath = dir / "loops.sfz";
    const int period = 147; // 300 Hz at 44.1 kHz
    const int loopStart = 30000, loopEnd = loopStart + 200 * period - 1, length = loopEnd - loopStart + 1;
    auto score = [&](const std::string& region, int start, int end) {
        const auto x = renderLoop(sfzPath, region, rate, seconds);
        return clickScore(x, rate, (end + 1) / q, (end - start + 1) / q);
    };
    const std::string loop = " loop_mode=loop_continuous loop_start=" + std::to_string(loopStart) + " loop_end=" + std::to_string(loopEnd);

    SECTION("a sampled instrument's own loop")
    {
        // looped_flute.wav's embedded loop, 77554-186581: its loop end is the file's last frame
        const fs::path flute = fs::current_path() / "tests/TestFiles/looped_flute.wav";
        const auto x = renderLoop(flute.parent_path() / "flute.sfz", "sample=looped_flute.wav loop_mode=loop_continuous", rate, seconds);
        REQUIRE(clickScore(x, rate, 186582 / q, (186581 - 77554 + 1) / q) < loopThreshold);
    }
    SECTION("a loop with the loop's own frames around it")
    {
        writeWav(dir / "mid.wav", tone(loopEnd + 1 + 4000, period), 44100);
        REQUIRE(score("sample=mid.wav" + loop, loopStart, loopEnd) < loopThreshold);
    }
    SECTION("a loop as a wave provider writes it")
    {
        // cut from the sustain of a recording that swells into it, its loop end on the decoded last
        // frame, then 64 frames repeating the loop's start: the interpolator's taps past the loop
        // end read what the loop plays next, and sfizz's own crossfade at the loop point (at least
        // 1 ms, into the frames before the loop start) covers the join
        auto x = tone(loopEnd + 1, period, loopStart);
        for (int k = 0; k < 64; ++k)
            x.push_back(x[loopStart + k % length]);
        writeWav(dir / "provider.wav", x, 44100);
        REQUIRE(score("sample=provider.wav" + loop, loopStart, loopEnd) < loopThreshold);
        // the same loop as it was decoded clicks at every pass: past the end of the file the taps
        // find nothing
        writeWav(dir / "decoded.wav", tone(loopEnd + 1, period, loopStart), 44100);
        REQUIRE(score("sample=decoded.wav" + loop, loopStart, loopEnd) > loopThreshold);
    }
    SECTION("a loop whose end does not meet its start is found")
    {
        writeWav(dir / "broken.wav", tone(loopEnd + 1 + 4000, period), 44100);
        const std::string broken = " loop_mode=loop_continuous loop_start=" + std::to_string(loopStart) + " loop_end=" + std::to_string(loopEnd - 40);
        REQUIRE(score("sample=broken.wav" + broken, loopStart, loopEnd - 40) > loopThreshold);
    }
}

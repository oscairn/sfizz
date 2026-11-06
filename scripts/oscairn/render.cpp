// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Renders an SFZ file through a fixed event script to raw 32-bit float stereo, using only
// the public C++ API, so the same source builds against stock sfizz and against this fork
// (scripts/oscairn/compare-stock.sh). Prints what the patch loaded, for the load check.
//
// usage: oscairn-render <patch.sfz> <out.f32> [block size] [sample rate] [quality]

#include <sfizz.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

enum Kind { On, Off, CC, Bend, ChanAT, PolyAT };
struct Event { int frame; Kind kind; int a; int b; };

// Times are in milliseconds and converted with the sample rate.
const Event script[] = {
    { 0, CC, 7, 100 }, { 0, CC, 10, 64 }, { 0, CC, 11, 127 },
    { 10, On, 60, 100 },
    { 400, On, 64, 64 }, { 450, On, 67, 30 },
    { 700, Off, 60, 0 },
    { 800, CC, 1, 32 }, { 900, CC, 1, 96 }, { 1000, CC, 1, 127 },
    { 1100, Off, 64, 0 }, { 1100, Off, 67, 0 },
    { 1200, On, 48, 127 }, { 1250, Bend, 12000, 0 }, { 1400, Bend, 4000, 0 }, { 1550, Bend, 8192, 0 },
    { 1600, ChanAT, 90, 0 }, { 1700, PolyAT, 48, 50 },
    { 1800, Off, 48, 0 },
    { 1900, CC, 64, 127 }, { 1950, On, 72, 90 }, { 2000, Off, 72, 0 }, { 2100, On, 74, 1 },
    { 2150, Off, 74, 0 }, { 2400, CC, 64, 0 },
    // repeated and overlapping notes on one key
    { 2500, On, 62, 80 }, { 2550, On, 62, 110 }, { 2600, Off, 62, 0 }, { 2700, Off, 62, 0 },
    // a legato run, then a chord at the extremes of the key and velocity ranges
    { 2800, On, 36, 70 }, { 2900, On, 38, 70 }, { 2950, Off, 36, 0 }, { 3000, On, 40, 70 },
    { 3050, Off, 38, 0 }, { 3200, Off, 40, 0 },
    { 3300, On, 0, 127 }, { 3300, On, 127, 127 }, { 3300, On, 84, 1 }, { 3300, On, 21, 64 },
    { 3300, CC, 1, 0 }, { 3500, CC, 74, 20 }, { 3700, CC, 74, 120 },
    { 3900, Off, 0, 0 }, { 3900, Off, 127, 0 }, { 3900, Off, 84, 0 }, { 3900, Off, 21, 0 },
    // round robin / random selection: the same note many times
    { 4000, On, 60, 100 }, { 4060, Off, 60, 0 }, { 4120, On, 60, 100 }, { 4180, Off, 60, 0 },
    { 4240, On, 60, 100 }, { 4300, Off, 60, 0 }, { 4360, On, 60, 100 }, { 4420, Off, 60, 0 },
    { 4480, On, 60, 100 }, { 4540, Off, 60, 0 }, { 4600, On, 60, 100 }, { 4660, Off, 60, 0 },
    // key switches in a common range
    { 4700, On, 24, 100 }, { 4710, Off, 24, 0 }, { 4720, On, 60, 100 }, { 4900, Off, 60, 0 },
    { 4950, On, 25, 100 }, { 4960, Off, 25, 0 }, { 4970, On, 60, 100 }, { 5200, Off, 60, 0 },
    // keys and velocities the notes above miss, which some test patches map
    { 5300, On, 2, 100 }, { 5300, On, 17, 110 }, { 5300, On, 61, 100 },
    { 5800, Off, 2, 0 }, { 5800, Off, 17, 0 }, { 5800, Off, 61, 0 },
};
const int lengthMs = 7000;

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <patch.sfz> <out.f32> [block size] [sample rate] [quality]\n", argv[0]);
        return 2;
    }
    const int block = argc > 3 ? std::atoi(argv[3]) : 256;
    const int rate = argc > 4 ? std::atoi(argv[4]) : 48000;
    const int quality = argc > 5 ? std::atoi(argv[5]) : 2;
    if (block <= 0 || rate <= 0)
        return 2;

    sfz::Sfizz synth;
    synth.setSamplesPerBlock(block);
    synth.setSampleRate(static_cast<float>(rate));
    synth.setSampleQuality(sfz::Sfizz::ProcessFreewheeling, quality);
    synth.enableFreeWheeling();
    // Whole files in memory: background streaming finishes at a time that depends on thread
    // scheduling, which would make renders differ from run to run
    synth.setPreloadSize(1u << 30);
    if (!synth.loadSfzFile(argv[1])) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    std::string unknown;
    for (const std::string& u : synth.getUnknownOpcodes())
        unknown += " " + u;
    std::printf("regions %d groups %d masters %d curves %d preloaded %zu unknown%s\n", synth.getNumRegions(),
        synth.getNumGroups(), synth.getNumMasters(), synth.getNumCurves(), synth.getNumPreloadedSamples(),
        unknown.c_str());

    std::FILE* out = std::fopen(argv[2], "wb");
    if (!out)
        return 1;

    std::vector<float> left(block), right(block), interleaved(2 * block);
    float* buffers[2] = { left.data(), right.data() };
    const long total = static_cast<long>(lengthMs) * rate / 1000;
    const size_t eventCount = sizeof(script) / sizeof(script[0]);
    size_t next = 0;
    for (long start = 0; start < total; start += block) {
        const int frames = static_cast<int>(total - start < block ? total - start : block);
        for (; next < eventCount; ++next) {
            const Event& e = script[next];
            const long at = static_cast<long>(e.frame) * rate / 1000;
            if (at >= start + frames)
                break;
            const int delay = static_cast<int>(at - start);
            switch (e.kind) {
            case On: synth.noteOn(delay, e.a, e.b); break;
            case Off: synth.noteOff(delay, e.a, e.b); break;
            case CC: synth.cc(delay, e.a, e.b); break;
            case Bend: synth.pitchWheel(delay, e.a - 8192); break;
            case ChanAT: synth.channelAftertouch(delay, e.a); break;
            case PolyAT: synth.polyAftertouch(delay, e.a, e.b); break;
            }
        }
        synth.renderBlock(buffers, static_cast<size_t>(frames), 1);
        for (int i = 0; i < frames; ++i) {
            interleaved[2 * i] = left[i];
            interleaved[2 * i + 1] = right[i];
        }
        if (std::fwrite(interleaved.data(), sizeof(float), 2 * frames, out) != static_cast<size_t>(2 * frames))
            return 1;
    }
    return std::fclose(out) == 0 ? 0 : 1;
}

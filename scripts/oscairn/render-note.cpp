// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Renders one note through an SFZ file to a 32-bit float stereo WAV, for measuring the
// engine with the same analysis as the reference renders (oscairn/measurement, awm2/).
//
// usage: oscairn-render-note <patch.sfz> <out.wav> <sample rate> <key> <velocity> <hold s> <tail s>

#include <sfizz.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void put32(std::FILE* f, uint32_t v) { for (int i = 0; i < 4; ++i) std::fputc((v >> (8 * i)) & 0xff, f); }
void put16(std::FILE* f, uint16_t v) { std::fputc(v & 0xff, f); std::fputc(v >> 8, f); }
}

int main(int argc, char** argv)
{
    if (argc != 8) {
        std::fprintf(stderr, "usage: %s <patch.sfz> <out.wav> <rate> <key> <velocity> <hold s> <tail s>\n", argv[0]);
        return 2;
    }
    const int rate = std::atoi(argv[3]), key = std::atoi(argv[4]), velocity = std::atoi(argv[5]);
    const double hold = std::atof(argv[6]), tail = std::atof(argv[7]);
    const int block = 256;
    sfz::Sfizz synth;
    synth.setSamplesPerBlock(block);
    synth.setSampleRate(static_cast<float>(rate));
    synth.enableFreeWheeling();
    synth.setPreloadSize(1u << 30);
    if (!synth.loadSfzFile(argv[1])) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    const long total = static_cast<long>((hold + tail) * rate), off = static_cast<long>(hold * rate);
    std::vector<float> left(block), right(block), out;
    out.reserve(2 * static_cast<size_t>(total));
    float* buffers[2] = { left.data(), right.data() };
    synth.noteOn(0, key, velocity);
    for (long start = 0; start < total; start += block) {
        const int n = static_cast<int>(total - start < block ? total - start : block);
        if (off >= start && off < start + n)
            synth.noteOff(static_cast<int>(off - start), key, 0);
        synth.renderBlock(buffers, static_cast<size_t>(n), 1);
        for (int i = 0; i < n; ++i) {
            out.push_back(left[i]);
            out.push_back(right[i]);
        }
    }
    std::FILE* f = std::fopen(argv[2], "wb");
    if (!f)
        return 1;
    const uint32_t bytes = static_cast<uint32_t>(out.size() * sizeof(float));
    std::fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 3); put16(f, 2); put32(f, static_cast<uint32_t>(rate));
    put32(f, static_cast<uint32_t>(rate) * 8); put16(f, 8); put16(f, 32);
    std::fwrite("data", 1, 4, f); put32(f, bytes);
    std::fwrite(out.data(), sizeof(float), out.size(), f);
    return std::fclose(f) == 0 ? 0 : 1;
}

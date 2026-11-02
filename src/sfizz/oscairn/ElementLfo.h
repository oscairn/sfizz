// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// The element LFO with raw parameters (doc/oscairn-plan.md, decision 21), in two modes.
//
// Reference mode (the default, what the converter writes): the LFO acts only through the
// controller-set destinations for pitch and amplitude depth, each a ratio (-128..+127) on a
// controller. It runs at 4.848 Hz as a triangle that starts at zero and falls, from note-on;
// the element's own speed, wave, delay, fade-in, reset and depths change nothing, as measured.
// Pitch depth, peak to peak: 1118 * 2^((min(2 |ratio|, 127) + cc - 253) / 16) cents, nothing at
// controller value 0. Amplitude: a dip in dB from 0 at note-on to the depth and back, the
// depth read from the measured points at |ratio| * cc / 127.
//
// Settings mode: the element's parameters act as they describe, through these definitions
// (not measurements; the reference ignores them):
// - speed 0-63: 4.848 Hz * 2^((speed - 38) / 8) (the default 38 runs at the reference's rate);
//   the extended speed 0-415 is the same law in steps of 1/6.5;
// - wave: saw (rising from zero), triangle (falling from zero, as in reference mode), square
//   (high half first);
// - delay and fade-in 0-127: 5 s * (value / 127)^2 each; the depth is zero during the delay
//   and rises linearly over the fade-in;
// - key-on reset on: the phase starts at note-on; off: the phase follows a free-running
//   clock shared by every voice of the synth;
// - pitch depth d: the reference-mode law with min(2 |ratio|, 127) replaced by d at controller
//   value 127 (nothing at 0); amplitude depth d: the reference-mode points at d / 2; filter
//   depth d: 4800 * 2^((d - 127) / 16) cents peak to peak (nothing at 0);
// - the controller-set destinations add 2 * ratio * cc / 127 to the pitch depth and
//   ratio * cc / 127 to the amplitude depth (clamped to 0-127).

#pragma once
#include <absl/types/span.h>
#include <cstdint>

namespace sfz {
namespace oscairn {

struct ElementLfoParameters {
    int32_t mode { 0 }; // 0 reference, 1 settings
    int32_t wave { 1 }; // 0 saw, 1 triangle, 2 square
    int32_t speed { 38 }; // 0-63
    int32_t extendedSpeed { -1 }; // 0-415, used instead of speed when set
    int32_t delay { 0 }, fade { 0 }; // 0-127
    int32_t keyOnReset { 1 };
    int32_t pitchDepth { 0 }, ampDepth { 0 }, filterDepth { 0 }; // 0-127
    // Controller-set destinations: controller number (-1 for none) and ratio (-128..+127)
    int32_t pitchCC { -1 }, pitchRatio { 0 };
    int32_t ampCC { -1 }, ampRatio { 0 };
};

/**
 * @brief LFO rate in Hz of the settings mode.
 */
float lfoSpeedHz(const ElementLfoParameters& parameters) noexcept;

/**
 * @brief Peak-to-peak pitch depth in cents of the reference mode for a ratio and a controller
 * value (0-127).
 */
float lfoPitchDepthCents(int32_t ratio, float cc) noexcept;

/**
 * @brief Peak-to-peak amplitude depth in dB at an effective depth (0-63).
 */
float lfoAmpDepthDb(float depth) noexcept;

class ElementLfo {
public:
    void setSampleRate(double sampleRate) noexcept { sampleRate_ = sampleRate; }
    // `clock` is the synth's frame clock at note-on (for the free-running phase) and `delay`
    // the frames before the voice starts sounding
    void start(const ElementLfoParameters& parameters, unsigned clock, int delay) noexcept;
    // Starts a block of `frames`, reading the controllers of the destinations (0-1)
    void beginBlock(size_t frames, float pitchCC, float ampCC) noexcept;
    // Adds pitch cents to the last frames of the block (`cents` may be shorter than the block)
    void addPitch(absl::Span<float> cents) const noexcept;
    // Multiplies the block's gains by the amplitude modulation
    void applyGain(absl::Span<float> gain) const noexcept;
    // Adds filter cents to the block
    void addFilter(absl::Span<float> cents) const noexcept;
    bool hasPitch() const noexcept { return pitchPP_ != 0.0f; }
    bool hasAmplitude() const noexcept { return ampPP_ != 0.0f; }
    bool hasFilter() const noexcept { return filterPP_ != 0.0f; }

private:
    // Wave value (-1..1) and depth scale (0..1, delay and fade) at frame `i` of the block
    float wave(size_t i) const noexcept;
    float scale(size_t i) const noexcept;

    ElementLfoParameters p_;
    double sampleRate_ { 48000.0 };
    double hz_ { 4.848 };
    double phase0_ { 0.0 }; // phase at the voice's first sounding frame (cycles)
    long elapsed_ { 0 }; // frames since the voice started sounding at the block start
    size_t frames_ { 0 };
    long delayFrames_ { 0 }, fadeFrames_ { 0 };
    float pitchPP_ { 0.0f }, ampPP_ { 0.0f }, filterPP_ { 0.0f };
};

} // namespace oscairn
} // namespace sfz

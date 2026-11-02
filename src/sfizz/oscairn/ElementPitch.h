// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// A five-level pitch envelope with raw parameters (doc/oscairn-plan.md, section 4), as
// measured on the reference (oscairn/measurement): it starts at the hold level, holds, then
// moves to the attack, decay 1 and decay 2 levels and after note-off to the release level.
// The hold time has its own measured table (pitchHoldSeconds).
// Each move is a straight line in cents taking the decay table's full-scale time for its
// index (1.5 times the time per 60 dB), whatever distance it covers. A level step is
// 37.5 cents (raw 0-255, 128 = 0; 255 counts as +4800), scaled by depth / 64 (+63 counts
// as 64).

#pragma once
#include <absl/types/span.h>
#include <cstdint>

namespace sfz {
namespace oscairn {

struct ElementPitchParameters {
    int32_t holdTime { 0 }, attackTime { 40 }, decay1Time { 64 }, decay2Time { 64 }, releaseTime { 64 };
    int32_t holdLevel { 128 }, attackLevel { 128 }, decay1Level { 128 }, decay2Level { 128 }, releaseLevel { 128 };
    int32_t depth { 20 }; // -64..+63
};

/**
 * @brief Seconds the pitch envelope holds its hold level at a hold-time index.
 */
float pitchHoldSeconds(int32_t index) noexcept;

/**
 * @brief Cents of a raw level (0-255) at a depth (-64..+63).
 */
float pitchLevelCents(int32_t level, int32_t depth) noexcept;

class ElementPitchEnvelope {
public:
    void setSampleRate(double sampleRate) noexcept { sampleRate_ = sampleRate; }
    void start(const ElementPitchParameters& parameters, int delay) noexcept;
    void release(int delay) noexcept;
    // Adds the envelope's cents to `cents`
    void processAdd(absl::Span<float> cents) noexcept;

private:
    enum class Stage { Delay, Hold, Attack, Decay1, Decay2, Sustain, Release, Done };
    void enter(Stage stage) noexcept;

    ElementPitchParameters p_;
    double sampleRate_ { 48000.0 };
    Stage stage_ { Stage::Done };
    int delay_ { 0 };
    int releaseDelay_ { -1 };
    double cents_ { 0.0 };
    double step_ { 0.0 };
    double target_ { 0.0 };
    long left_ { 0 }; // samples left in the current segment
};

} // namespace oscairn
} // namespace sfz

// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// A five-segment element envelope with raw 0-127 parameters (doc/oscairn-plan.md,
// section 4): from the initial level, an attack to the attack level, decay 1 to the decay 1
// level, decay 2 to the decay 2 (sustain) level, and after note-off a release to silence.
//
// Shapes and times follow measurements of the reference (oscairn/measurement):
// levels are (level / 127)^2 in amplitude, with level 0 at -90 dB as a target; the attack
// is a straight line in amplitude whose time depends on the index and on both levels
// (attackDurationFactor);
// decays and the release are straight lines in dB that take the index's time whatever
// distance they cover.

#pragma once
#include <absl/types/span.h>
#include <cstdint>

namespace sfz {
namespace oscairn {

struct ElementEnvelopeParameters {
    // Defaults are the published defaults of the parameters this models
    int32_t initialLevel { 0 };
    int32_t attackTime { 0 };
    int32_t attackLevel { 127 };
    int32_t decay1Time { 64 };
    int32_t decay1Level { 127 };
    int32_t decay2Time { 64 };
    int32_t decay2Level { 127 };
    int32_t releaseTime { 50 };
    // Time scaling: each shifts the time indices (higher index, longer time)
    int32_t timeVelocitySegments { 4 };     // 0 attack, 1 attack+decay, 2 decay, 3 attack+release, 4 all
    int32_t timeVelocitySensitivity { 0 };  // -64..+63: index shift -sensitivity * velocity / 128 (truncated)
    int32_t timeKeyFollowSensitivity { 0 }; // -64..+63: index shift -(key - center) * sensitivity / 64
    int32_t timeKeyFollowCenter { 24 };     // note
    int32_t timeKeyFollowReleaseAdjustment { 64 }; // 0..127: share of the key shift the release gets
};

/**
 * @brief Time-index shifts for a note (`velocity` 1-127): what start() subtracts from each
 * segment's index. Exposed for tests.
 */
struct ElementTimeShifts {
    int32_t attack, decay, release;
};
ElementTimeShifts timeShifts(const ElementEnvelopeParameters& p, int key, int velocity) noexcept;

/**
 * @brief Amplitude of a level index, (level / 127)^2.
 */
float levelGain(int32_t level) noexcept;
/**
 * @brief Seconds a full-scale attack (silence to level 127) takes at an attack index.
 */
float attackSeconds(int32_t index) noexcept;
/**
 * @brief An attack's time between two levels, as a multiple of attackSeconds().
 */
float attackDurationFactor(int32_t from, int32_t to) noexcept;
/**
 * @brief Seconds a decay at a decay index takes to fall by 60 dB from level 127 toward level 0
 * (a full-scale decay to -90 dB takes 1.5 times as long, and so does every decay segment).
 */
float decaySecondsPer60dB(int32_t index) noexcept;
/**
 * @brief Seconds the release at a release index takes to fall by 60 dB.
 */
float releaseSecondsPer60dB(int32_t index) noexcept;

class ElementEnvelope {
public:
    void setSampleRate(double sampleRate) noexcept { sampleRate_ = sampleRate; }
    void start(const ElementEnvelopeParameters& parameters, int delay, int key = 60, int velocity = 127) noexcept;
    void release(int delay) noexcept;
    // The release lasts `secondsPer60dB` instead of its index's time (a choke's fast release);
    // cleared by start()
    void setReleaseSeconds(double secondsPer60dB) noexcept;
    void process(absl::Span<float> output) noexcept;
    bool isFinished() const noexcept { return stage_ == Stage::Done; }
    bool isReleased() const noexcept { return stage_ == Stage::Release || stage_ == Stage::Done || releaseDelay_ >= 0; }
    int getRemainingDelay() const noexcept { return delay_; }

private:
    enum class Stage { Delay, Attack, Decay1, Decay2, Sustain, Release, Done };
    void enter(Stage stage) noexcept;
    float tick() noexcept;

    ElementEnvelopeParameters p_; // with the time indices already shifted for the note
    double sampleRate_ { 48000.0 };
    Stage stage_ { Stage::Done };
    int delay_ { 0 };
    int releaseDelay_ { -1 };
    double releaseOverride_ { 0.0 }; // seconds per 60 dB; 0 for the index's time
    double gain_ { 0.0 };   // attack: linear amplitude
    double db_ { -200.0 };  // decays and release: level in dB
    double step_ { 0.0 };   // per sample, in the segment's unit
    double target_ { 0.0 }; // in the segment's unit
    double ratio_ { 1.0 };  // decays and release: gain factor per sample
};

} // namespace oscairn
} // namespace sfz

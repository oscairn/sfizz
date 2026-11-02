// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// The element filter with raw parameters (doc/oscairn-plan.md, section 4), as measured on
// the reference (oscairn/measurement): two-integrator state-variable sections whose
// responses match the reference at 44.1 kHz. A section's frequency coefficient is
// F = 2 sin(pi fc / 44100), fc up to 7290 Hz; its damping is 2 * 2^(-resonance / 16), held
// under a limit that falls from 2 to 1 between cutoff indices 816 and 912. Key follow moves
// the cutoff by (sensitivity - 64) * 3.125 cents per key from note 60; the filter gain
// follows the measured passband levels (the default 230 is -5.45 dB).
//
// Every rate gives the 44.1 kHz response, warping near the top included: each section is
// the 44.1 kHz section with its unit delays replaced by an allpass "reference-rate delay":
// a pure delay when the host rate is a whole multiple of 44.1 kHz, otherwise z^-M times a
// second-order allpass whose phase equals one 44.1 kHz sample's at the section's cutoff and
// at 18 kHz. The section's response at the host rate is then the 44.1 kHz response read at
// a frequency that differs from the true one only between and beyond those points (within
// 0.3 dB up to 20 kHz at 48 kHz, less at higher rates; known differences, K22).

#pragma once
#include <absl/types/span.h>
#include <cstdint>

namespace sfz {
namespace oscairn {

struct ElementFilterParameters {
    int32_t type { 18 }; // 18 = thru
    int32_t cutoff { 1023 }; // 0-1023
    int32_t resonance { 0 }; // 0-127
    int32_t hpfCutoff { 0 }; // 0-1023, the high-pass section of type 4
    int32_t keyFollow { 74 }; // 0-127, 64 = none
    int32_t gain { 230 }; // 0-255
    int32_t velocitySensitivity { 0 }; // cutoff velocity sensitivity, -64..+63
    int32_t distance { 0 }; // -128..+127, the second section of the dual types
};

/**
 * @brief Cents the cutoff velocity sensitivity moves a cutoff index by at a velocity (1-127).
 */
float cutoffVelocityCents(int32_t cutoff, int32_t sensitivity, int velocity) noexcept;

/**
 * @brief Linear gain of a filter gain value (0-255).
 */
float filterGain(int32_t gain) noexcept;

/**
 * @brief Whether a filter type is one this module implements (others pass through).
 */
bool filterTypeSupported(int32_t type) noexcept;

/**
 * @brief Cutoff in Hz of a cutoff index (0-1023) shifted by `cents`.
 */
float filterCutoffHz(int32_t index, float cents = 0.0f) noexcept;

/**
 * @brief Damping of a resonance index (0-127).
 */
float filterDamping(int32_t resonance) noexcept;

class ElementFilter {
public:
    void setSampleRate(double sampleRate) noexcept;
    void start(const ElementFilterParameters& parameters, int note, int velocity = 127) noexcept;
    // Filters `left` and `right` in place; `cents` shifts the cutoff per sample
    // (empty for no shift). `right` may be empty for a mono voice.
    void process(absl::Span<float> left, absl::Span<float> right, absl::Span<const float> cents) noexcept;

private:
    static constexpr int kMaxDelay = 16; // reference-rate delay up to 16 host samples (705.6 kHz)
    // The reference-rate delay of one signal: a delay line and the allpass's state
    struct Delay {
        float line[kMaxDelay] {};
        int pos { 0 };
        float s1 { 0.0f }, s2 { 0.0f };
    };
    enum class Output { Low, High, Band, Notch, LowMinusBand, OnePole };
    struct Section {
        Output output { Output::Low };
        // 44.1 kHz coefficients and the allpass pinned for them
        float f { 0.0f }, d { 2.0f }, c1 { 0.0f }, c2 { 0.0f };
        // per-sample solve: high = (x - k low' - cs band') * inv; band = k (f high + band');
        // low = k (low' + f band') + k f g band (primes: the delayed values)
        float g { 0.0f }, k { 1.0f }, cs { 0.0f }, inv { 1.0f };
        Delay low[2], band[2];
    };
    void design(Section& s, float hz, float damping) const noexcept;
    void designOnePole(Section& s, float hz) const noexcept;
    void pin(Section& s, double hz) const noexcept;
    float delayedValue(const Section& s, Delay& line) const noexcept;
    void push(const Section& s, Delay& line, float value) const noexcept;
    float run(Section& s, int channel, float x) const noexcept;
    void update(float cents) noexcept;

    ElementFilterParameters p_;
    double sampleRate_ { 48000.0 };
    // the reference-rate delay at this rate: z^-delay_, then the allpass if any
    int delay_ { 1 };
    bool allpass_ { false };
    float pin2_[3] {}; // the 18 kHz row of the allpass pin equations
    Section sections_[2];
    int stages_ { 0 }; // sections in use
    bool parallel_ { false }; // the two sections summed instead of in series
    float damping_ { 2.0f };
    float keyCents_ { 0.0f };
    float cents_ { 0.0f }; // the shift the coefficients are designed for
    float gain_ { 1.0f };
};

} // namespace oscairn
} // namespace sfz

// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// The element filter with raw parameters (doc/oscairn-plan.md, section 4), as measured on
// the reference (oscairn/measurement): two-integrator state-variable sections whose
// responses match the reference at 44.1 kHz. A section's frequency coefficient is
// F = 2 sin(pi fc / 44100), held at 1 or below (fc up to 7350 Hz); its damping is
// 2 * 2^(-resonance / 16), held at 1 / F or below. Each section runs as the biquad with
// that response; at other sample rates the biquad is carried over by the allpass frequency
// transform that keeps DC, the cutoff and Nyquist in place, so every rate follows the
// 44.1 kHz response (doc/oscairn-plan.md, known differences, for the residual).

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
};

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
    void setSampleRate(double sampleRate) noexcept { sampleRate_ = sampleRate; }
    void start(const ElementFilterParameters& parameters) noexcept;
    // Filters `left` and `right` in place; `cents` shifts the cutoff per sample
    // (empty for no shift). `right` may be empty for a mono voice.
    void process(absl::Span<float> left, absl::Span<float> right, absl::Span<const float> cents) noexcept;

private:
    struct Coefficients {
        float b0 { 1.0f }, b1 { 0.0f }, b2 { 0.0f }, a1 { 0.0f }, a2 { 0.0f };
    };
    struct State {
        float s1[2] {}, s2[2] {};
    };
    Coefficients coefficients(float hz, float damping, bool highPass) const noexcept;
    void update(float cents) noexcept;

    ElementFilterParameters p_;
    double sampleRate_ { 48000.0 };
    State states_[2];
    Coefficients main_, high_;
    float damping_ { 2.0f };
};

} // namespace oscairn
} // namespace sfz

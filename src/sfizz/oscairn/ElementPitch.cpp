// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "ElementPitch.h"
#include "ElementEnvelope.h"
#include <algorithm>
#include <cmath>

namespace sfz {
namespace oscairn {

float pitchLevelCents(int32_t level, int32_t depth) noexcept
{
    const int32_t steps = level >= 255 ? 128 : std::max<int32_t>(0, level) - 128;
    const int32_t d = depth >= 63 ? 64 : std::max<int32_t>(-64, depth);
    return static_cast<float>(steps) * 37.5f * static_cast<float>(d) / 64.0f;
}

float pitchHoldSeconds(int32_t index) noexcept
{
    // Measured at 16, 32, 64 and 80 (0.027, 0.075, 0.421, 0.978 s); interpolated in log time,
    // extended beyond 80 at the 64-80 slope.
    // shortcut: four knots, no checks between them yet
    static constexpr float knots[4] = { 16.0f, 32.0f, 64.0f, 80.0f };
    static constexpr float seconds[4] = { 0.027f, 0.075f, 0.421f, 0.978f };
    if (index <= 0)
        return 0.0f;
    if (index <= 16)
        return seconds[0] * static_cast<float>(index) / 16.0f;
    int i = 1;
    while (i < 3 && index > knots[i])
        ++i;
    const float f = (static_cast<float>(index) - knots[i - 1]) / (knots[i] - knots[i - 1]);
    return std::exp(std::log(seconds[i - 1]) + f * (std::log(seconds[i]) - std::log(seconds[i - 1])));
}

float filterDepthVelocityFactor(int32_t sensitivity, int32_t curve, int velocity) noexcept
{
    // Measured at sensitivity +63 (fl3 dv_*, dc_*): the depth at velocity v on each curve, as a
    // share of the depth without sensitivity; other sensitivities scale the distance from 1 by
    // sensitivity / 64 (+63 counts as 64), negative ones mirror the velocity (128 - v).
    // Linear between the measured velocities; velocity 1 is measured on curve 2 only.
    // shortcut: curves 0, 1, 3 and 4 at five velocities, assumed 0 at velocity 1
    static constexpr float v9[9] = { 1.f, 16.f, 32.f, 48.f, 64.f, 80.f, 96.f, 112.f, 127.f };
    static constexpr float curve2[9] = { 0.f, 0.123f, 0.281f, 0.418f, 0.538f, 0.715f, 0.854f, 0.985f, 1.125f };
    static constexpr float v6[6] = { 1.f, 16.f, 48.f, 80.f, 112.f, 127.f };
    static constexpr float others[5][6] = {
        { 0.f, 0.247f, 0.511f, 0.576f, 0.898f, 1.125f },
        { 0.f, 0.012f, 0.152f, 0.435f, 0.883f, 1.125f },
        {},
        { 0.f, 0.263f, 0.687f, 0.973f, 1.094f, 1.125f },
        { 0.f, 0.018f, 0.330f, 0.816f, 1.077f, 1.125f },
    };
    if (sensitivity == 0)
        return 1.0f;
    const int32_t s = sensitivity >= 63 ? 64 : std::max<int32_t>(-64, sensitivity);
    const float v = static_cast<float>(std::max(1, std::min(127, s > 0 ? velocity : 128 - velocity)));
    const int c = std::max<int32_t>(0, std::min<int32_t>(4, curve));
    const float* x = c == 2 ? v9 : v6;
    const float* y = c == 2 ? curve2 : others[c];
    const int n = c == 2 ? 9 : 6;
    float g = y[n - 1];
    for (int k = 1; k < n; ++k) {
        if (v <= x[k]) {
            g = y[k - 1] + (y[k] - y[k - 1]) * (v - x[k - 1]) / (x[k] - x[k - 1]);
            break;
        }
    }
    return 1.0f + static_cast<float>(std::abs(s)) / 64.0f * (g - 1.0f);
}

double ElementPitchEnvelope::levelCents(int32_t level) const noexcept
{
    return static_cast<double>(pitchLevelCents(level, p_.depth)) * (centsPerStep_ / 37.5f) * scale_;
}

void ElementPitchEnvelope::start(const ElementPitchParameters& parameters, int delay, int key, int velocity, float scale) noexcept
{
    p_ = parameters;
    scale_ = scale;
    if (p_.timeVelocitySensitivity != 0 || p_.timeKeyFollowSensitivity != 0) {
        ElementEnvelopeParameters e;
        e.timeVelocitySegments = p_.timeVelocitySegments;
        e.timeVelocitySensitivity = p_.timeVelocitySensitivity;
        e.timeKeyFollowSensitivity = p_.timeKeyFollowSensitivity;
        e.timeKeyFollowCenter = p_.timeKeyFollowCenter;
        // shortcut: no release adjustment parameter here; the release gets the whole key
        // shift (not measured)
        e.timeKeyFollowReleaseAdjustment = 127;
        const ElementTimeShifts shift = timeShifts(e, key, velocity);
        auto clamp = [](int32_t i) { return std::max<int32_t>(0, std::min<int32_t>(127, i)); };
        p_.attackTime = clamp(p_.attackTime - shift.attack);
        p_.decay1Time = clamp(p_.decay1Time - shift.decay);
        p_.decay2Time = clamp(p_.decay2Time - shift.decay);
        p_.releaseTime = clamp(p_.releaseTime - shift.release);
    }
    delay_ = std::max(0, delay);
    releaseDelay_ = -1;
    cents_ = levelCents(p_.holdLevel);
    enter(delay_ > 0 ? Stage::Delay : p_.holdTime > 0 ? Stage::Hold : Stage::Attack);
}

void ElementPitchEnvelope::release(int delay) noexcept
{
    if (stage_ == Stage::Release || stage_ == Stage::Done)
        return;
    releaseDelay_ = std::max(0, delay);
}

void ElementPitchEnvelope::enter(Stage stage) noexcept
{
    stage_ = stage;
    int32_t index = 0, level = 128;
    switch (stage) {
    case Stage::Hold: index = p_.holdTime; level = p_.holdLevel; break;
    case Stage::Attack: index = p_.attackTime; level = p_.attackLevel; break;
    case Stage::Decay1: index = p_.decay1Time; level = p_.decay1Level; break;
    case Stage::Decay2: index = p_.decay2Time; level = p_.decay2Level; break;
    case Stage::Release: index = p_.releaseTime; level = p_.releaseLevel; break;
    default: step_ = 0.0; left_ = 0; return;
    }
    const double seconds = stage == Stage::Hold ? pitchHoldSeconds(index) : 1.5 * decaySecondsPer60dB(index);
    left_ = std::max(1L, std::lround(seconds * sampleRate_));
    target_ = levelCents(level);
    step_ = (target_ - cents_) / static_cast<double>(left_);
}

void ElementPitchEnvelope::processAdd(absl::Span<float> out) noexcept
{
    for (float& c : out) {
        if (releaseDelay_ == 0) {
            releaseDelay_ = -1;
            enter(Stage::Release);
        } else if (releaseDelay_ > 0) {
            --releaseDelay_;
        }
        switch (stage_) {
        case Stage::Delay:
            if (--delay_ <= 0)
                enter(p_.holdTime > 0 ? Stage::Hold : Stage::Attack);
            break;
        case Stage::Hold:
        case Stage::Attack:
        case Stage::Decay1:
        case Stage::Decay2:
        case Stage::Release:
            cents_ += step_;
            if (--left_ <= 0) {
                cents_ = target_;
                enter(stage_ == Stage::Hold ? Stage::Attack : stage_ == Stage::Attack ? Stage::Decay1
                      : stage_ == Stage::Decay1 ? Stage::Decay2 : stage_ == Stage::Decay2 ? Stage::Sustain : Stage::Done);
            }
            break;
        case Stage::Sustain:
        case Stage::Done:
            break;
        }
        c += static_cast<float>(cents_);
    }
}

} // namespace oscairn
} // namespace sfz

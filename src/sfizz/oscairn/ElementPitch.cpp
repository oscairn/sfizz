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

void ElementPitchEnvelope::start(const ElementPitchParameters& parameters, int delay) noexcept
{
    p_ = parameters;
    delay_ = std::max(0, delay);
    releaseDelay_ = -1;
    cents_ = pitchLevelCents(p_.holdLevel, p_.depth);
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
    target_ = pitchLevelCents(level, p_.depth);
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

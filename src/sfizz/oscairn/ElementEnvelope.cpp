// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "ElementEnvelope.h"
#include "ElementTables.h"
#include <algorithm>
#include <cmath>

namespace sfz {
namespace oscairn {

namespace {
// Level 0 as a decay or release target: segments are straight lines in dB that end here,
// and then the envelope is silent. Fitted: decays from level 127 to levels 96, 64 and 32 at
// one index took the same time as the decay to level 0 when level 0 sits at -90 dB.
constexpr double floorDb = -90.0;

int32_t clampIndex(int32_t i) { return std::max<int32_t>(0, std::min<int32_t>(127, i)); }
double toDb(double gain) { return gain > 0.0 ? std::max(20.0 * std::log10(gain), floorDb) : floorDb; }
double levelDb(int32_t level) { return toDb(levelGain(level)); }
} // namespace

float levelGain(int32_t level) noexcept
{
    const float r = static_cast<float>(clampIndex(level)) / 127.0f;
    return r * r;
}

float attackSeconds(int32_t index) noexcept { return tables::attackSeconds[clampIndex(index)]; }
float decaySecondsPer60dB(int32_t index) noexcept { return tables::decaySecondsPer60dB[clampIndex(index)]; }
float releaseSecondsPer60dB(int32_t index) noexcept { return tables::releaseSecondsPer60dB[clampIndex(index)]; }

void ElementEnvelope::start(const ElementEnvelopeParameters& parameters, int delay) noexcept
{
    p_ = parameters;
    delay_ = std::max(0, delay);
    releaseDelay_ = -1;
    gain_ = levelGain(p_.initialLevel);
    db_ = toDb(gain_);
    enter(delay_ > 0 ? Stage::Delay : Stage::Attack);
}

void ElementEnvelope::release(int delay) noexcept
{
    if (stage_ == Stage::Release || stage_ == Stage::Done)
        return;
    releaseDelay_ = std::max(0, delay);
}

void ElementEnvelope::enter(Stage stage) noexcept
{
    stage_ = stage;
    switch (stage) {
    case Stage::Attack: {
        // A straight line in amplitude; the attack index gives the time of a full-scale rise
        target_ = levelGain(p_.attackLevel);
        const double seconds = std::max<double>(attackSeconds(p_.attackTime), 1e-6);
        step_ = (target_ >= gain_ ? 1.0 : -1.0) / (seconds * sampleRate_);
        break;
    }
    case Stage::Decay1:
    case Stage::Decay2:
    case Stage::Release: {
        // Straight lines in dB from the current level to the segment's level, lasting the
        // same time whatever the distance: the time of a full-scale fall (0 dB to the floor),
        // which the tables give per 60 dB of it
        const bool release = stage == Stage::Release;
        const int32_t index = release ? p_.releaseTime : stage == Stage::Decay1 ? p_.decay1Time : p_.decay2Time;
        const double per60 = release ? releaseSecondsPer60dB(index) : decaySecondsPer60dB(index);
        const double samples = std::max(1.0, per60 * (-floorDb / 60.0) * sampleRate_);
        target_ = release ? floorDb : levelDb(stage == Stage::Decay1 ? p_.decay1Level : p_.decay2Level);
        db_ = toDb(gain_);
        step_ = (target_ - db_) / samples;
        ratio_ = std::pow(10.0, step_ / 20.0);
        break;
    }
    case Stage::Sustain:
    case Stage::Delay:
        break;
    case Stage::Done:
        gain_ = 0.0;
        break;
    }
}

float ElementEnvelope::tick() noexcept
{
    if (releaseDelay_ == 0) {
        releaseDelay_ = -1;
        if (stage_ == Stage::Delay)
            gain_ = 0.0, enter(Stage::Done);
        else
            enter(Stage::Release);
    } else if (releaseDelay_ > 0) {
        --releaseDelay_;
    }

    switch (stage_) {
    case Stage::Delay:
        if (--delay_ <= 0)
            enter(Stage::Attack);
        return static_cast<float>(gain_);
    case Stage::Attack:
        gain_ += step_;
        if ((step_ >= 0.0 && gain_ >= target_) || (step_ < 0.0 && gain_ <= target_)) {
            gain_ = target_;
            enter(Stage::Decay1);
        }
        return static_cast<float>(gain_);
    case Stage::Decay1:
    case Stage::Decay2:
    case Stage::Release: {
        db_ += step_;
        if ((step_ >= 0.0 && db_ >= target_) || (step_ < 0.0 && db_ <= target_)) {
            db_ = target_;
            gain_ = db_ <= floorDb ? 0.0 : std::pow(10.0, db_ / 20.0);
            if (stage_ == Stage::Release)
                enter(Stage::Done);
            else
                enter(stage_ == Stage::Decay1 ? Stage::Decay2 : Stage::Sustain);
            return static_cast<float>(gain_);
        }
        gain_ *= ratio_;
        return static_cast<float>(gain_);
    }
    case Stage::Sustain:
        return static_cast<float>(gain_);
    case Stage::Done:
        break;
    }
    return 0.0f;
}

void ElementEnvelope::process(absl::Span<float> output) noexcept
{
    for (float& out : output)
        out = tick();
}

} // namespace oscairn
} // namespace sfz

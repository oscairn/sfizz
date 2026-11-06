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
    // (level / 127)^2 on a grid of 0.375 dB steps, rounded to the nearest step, as the
    // reference's levels measure (element levels and envelope levels 8-127)
    const int32_t l = clampIndex(level);
    if (l == 0)
        return 0.0f;
    const double steps = std::round(40.0 * std::log10(127.0 / l) / 0.375);
    return static_cast<float>(std::pow(10.0, -0.375 * steps / 20.0));
}

float attackSeconds(int32_t index) noexcept { return tables::attackSeconds[clampIndex(index)]; }

float attackDurationFactor(int32_t from, int32_t to) noexcept
{
    // Fitted to attacks at one index from levels 0-112 to 127 (eight renders) and 32 to 96; the factor
    // depends only on the ratio of the smaller level to the larger one, the same up and down.
    // shortcut: measured knots, linear in between; ratios between 0 and 1/16 are not measured.
    static constexpr float q[9] = { 0.0f, 0.063f, 0.126f, 0.252f, 0.333f, 0.504f, 0.756f, 0.882f, 1.0f };
    static constexpr float g[9] = { 1.0f, 2.02f, 2.60f, 3.82f, 4.28f, 6.11f, 8.31f, 8.97f, 9.3f };
    const int32_t lo = std::min(clampIndex(from), clampIndex(to)), hi = std::max(clampIndex(from), clampIndex(to));
    if (hi == 0)
        return 0.0f;
    const float r = static_cast<float>(lo) / static_cast<float>(hi);
    int i = 1;
    while (i < 8 && r > q[i])
        ++i;
    const float f = (r - q[i - 1]) / (q[i] - q[i - 1]);
    float factor = g[i - 1] + std::max(0.0f, std::min(1.0f, f)) * (g[i] - g[i - 1]);
    if (lo == 0) {
        // From (or to) level 0 the time grows a little as the other level falls: measured
        // 1.29, 1.166, 1.08 and 1 times at 32, 64, 96 and 127 (indices 80 and 96)
        static constexpr float levels[4] = { 32.0f, 64.0f, 96.0f, 127.0f };
        static constexpr float slow[4] = { 1.29f, 1.166f, 1.08f, 1.0f };
        const float h = static_cast<float>(hi);
        int k = 1;
        while (k < 3 && h > levels[k])
            ++k;
        const float u = std::max(0.0f, std::min(1.0f, (h - levels[k - 1]) / (levels[k] - levels[k - 1])));
        factor = h < levels[0] ? slow[0] : slow[k - 1] + u * (slow[k] - slow[k - 1]);
    }
    return levelGain(hi) * factor;
}
float decaySecondsPer60dB(int32_t index) noexcept { return tables::decaySecondsPer60dB[clampIndex(index)]; }
float releaseSecondsPer60dB(int32_t index) noexcept { return tables::releaseSecondsPer60dB[clampIndex(index)]; }

ElementTimeShifts timeShifts(const ElementEnvelopeParameters& p, int key, int velocity) noexcept
{
    // Fitted to renders: sensitivity * velocity / 128 index steps, and key-follow sensitivity
    // per 64 steps per semitone from the center, both truncated toward zero, with +63 counting
    // as 64 (velocity 60 at +63 shifts by 30). The release gets the key shift scaled by
    // 2^((adjustment - 127) / 32): all of it at 127, a quarter at 64.
    auto full = [](int32_t s) { return s >= 63 ? 64 : s; };
    const int32_t vel = full(p.timeVelocitySensitivity) * velocity / 128;
    const int32_t keyShift = (key - p.timeKeyFollowCenter) * full(p.timeKeyFollowSensitivity) / 64;
    // truncated: adjustment 0 gave a shift of 1 where 24 * 2^(-127/32) = 1.53
    const int32_t releaseKey = static_cast<int32_t>(keyShift * std::exp2((p.timeKeyFollowReleaseAdjustment - 127) / 32.0));
    const int s = p.timeVelocitySegments;
    const bool velAttack = s == 0 || s == 1 || s == 3 || s == 4;
    const bool velDecay = s == 1 || s == 2 || s == 4;
    const bool velRelease = s == 3 || s == 4;
    return { keyShift + (velAttack ? vel : 0), keyShift + (velDecay ? vel : 0), releaseKey + (velRelease ? vel : 0) };
}

void ElementEnvelope::start(const ElementEnvelopeParameters& parameters, int delay, int key, int velocity) noexcept
{
    p_ = parameters;
    const ElementTimeShifts shift = timeShifts(parameters, key, velocity);
    p_.attackTime = clampIndex(p_.attackTime - shift.attack);
    p_.decay1Time = clampIndex(p_.decay1Time - shift.decay);
    p_.decay2Time = clampIndex(p_.decay2Time - shift.decay);
    p_.releaseTime = clampIndex(p_.releaseTime - shift.release);
    delay_ = std::max(0, delay);
    releaseDelay_ = -1;
    releaseOverride_ = 0.0;
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

void ElementEnvelope::setReleaseSeconds(double secondsPer60dB) noexcept
{
    releaseOverride_ = secondsPer60dB;
    if (stage_ == Stage::Release) // already releasing: go on from the current level at the new rate
        enter(Stage::Release);
}

void ElementEnvelope::enter(Stage stage) noexcept
{
    stage_ = stage;
    switch (stage) {
    case Stage::Attack: {
        // A straight line in amplitude from the initial to the attack level. It takes the
        // index's full-scale time, times the larger level's amplitude, times a factor of the
        // smaller level over the larger one (in level steps)
        target_ = levelGain(p_.attackLevel);
        const double seconds = attackSeconds(p_.attackTime)
            * attackDurationFactor(p_.initialLevel, p_.attackLevel);
        const double distance = target_ - gain_;
        step_ = seconds > 0.0 ? distance / (seconds * sampleRate_) : distance;
        if (step_ == 0.0)
            step_ = 1e-12; // equal levels: the attack ends at once
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
        const double per60 = release ? (releaseOverride_ > 0.0 ? releaseOverride_ : releaseSecondsPer60dB(index))
                                     : decaySecondsPer60dB(index);
        const double samples = std::max(1.0, per60 * (-floorDb / 60.0) * sampleRate_);
        target_ = release ? floorDb : levelDb(stage == Stage::Decay1 ? p_.decay1Level : p_.decay2Level);
        db_ = toDb(gain_);
        step_ = (target_ - db_) / samples;
        ratio_ = std::pow(10.0, step_ / 20.0);
        break;
    }
    case Stage::Sustain:
        // held at level 0: nothing more can sound before a release, so the voice may end (a
        // release-triggered voice is never released)
        if (gain_ <= 0.0)
            stage_ = Stage::Done;
        break;
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
    size_t i = 0;
    const size_t n = output.size();
    while (i < n) {
        // held levels without a release due: a run of the same value
        if ((stage_ == Stage::Sustain || stage_ == Stage::Done) && releaseDelay_ != 0) {
            size_t m = n - i;
            if (releaseDelay_ > 0)
                m = std::min<size_t>(m, static_cast<size_t>(releaseDelay_)), releaseDelay_ -= static_cast<int>(m);
            std::fill_n(output.begin() + i, m, stage_ == Stage::Done ? 0.0f : static_cast<float>(gain_));
            i += m;
            continue;
        }
        // within a segment: the samples that cannot reach its target (two to spare for
        // rounding) without the per-sample checks
        const bool attack = stage_ == Stage::Attack;
        const bool falls = stage_ == Stage::Decay1 || stage_ == Stage::Decay2 || stage_ == Stage::Release;
        if ((attack || falls) && releaseDelay_ != 0 && step_ != 0.0) {
            const double left = ((attack ? gain_ : db_) - target_) / -step_ - 2.0;
            size_t m = left > 0.0 ? static_cast<size_t>(std::min(left, static_cast<double>(n - i))) : 0;
            if (releaseDelay_ > 0)
                m = std::min<size_t>(m, static_cast<size_t>(releaseDelay_));
            if (m > 0) {
                if (attack) {
                    for (size_t k = 0; k < m; ++k)
                        gain_ += step_, output[i + k] = static_cast<float>(gain_);
                } else {
                    for (size_t k = 0; k < m; ++k)
                        db_ += step_, gain_ *= ratio_, output[i + k] = static_cast<float>(gain_);
                }
                if (releaseDelay_ > 0)
                    releaseDelay_ -= static_cast<int>(m);
                i += m;
                continue;
            }
        }
        output[i++] = tick();
    }
}

} // namespace oscairn
} // namespace sfz

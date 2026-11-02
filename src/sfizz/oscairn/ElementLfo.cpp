// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "ElementLfo.h"
#include <algorithm>
#include <cmath>

namespace sfz {
namespace oscairn {

namespace {
constexpr double kReferenceHz = 4.848; // measured rate of the reference mode

float clamp127(float v) noexcept { return std::max(0.0f, std::min(127.0f, v)); }

float pitchLaw(float index, float cc) noexcept
{
    if (index <= 0.0f || cc <= 0.0f)
        return 0.0f;
    return 1118.0f * std::exp2((index + cc - 253.0f) / 16.0f);
}

long secondsFrames(int32_t value, double rate) noexcept
{
    const double v = static_cast<double>(std::max<int32_t>(0, std::min<int32_t>(127, value))) / 127.0;
    return static_cast<long>(5.0 * v * v * rate);
}
} // namespace

float lfoSpeedHz(const ElementLfoParameters& p) noexcept
{
    const float speed = p.extendedSpeed >= 0
        ? static_cast<float>(std::min<int32_t>(415, p.extendedSpeed)) / 6.5f
        : static_cast<float>(std::max<int32_t>(0, std::min<int32_t>(63, p.speed)));
    return static_cast<float>(kReferenceHz) * std::exp2((speed - 38.0f) / 8.0f);
}

float lfoPitchDepthCents(int32_t ratio, float cc) noexcept
{
    const float index = std::min(127.0f, 2.0f * static_cast<float>(std::abs(ratio)));
    const float cents = pitchLaw(index, cc);
    return ratio < 0 ? -cents : cents;
}

float lfoAmpDepthDb(float depth) noexcept
{
    // Peak-to-peak dips measured at these effective depths (K30), linear between
    static constexpr float at[8] = { 0.f, 8.f, 16.f, 24.f, 32.f, 40.f, 48.f, 63.f };
    static constexpr float db[8] = { 0.f, 2.2f, 4.8f, 8.1f, 11.8f, 16.9f, 23.9f, 83.f };
    const float e = std::max(0.0f, std::min(63.0f, depth));
    for (int k = 0; k < 7; ++k) {
        if (e <= at[k + 1])
            return db[k] + (db[k + 1] - db[k]) * (e - at[k]) / (at[k + 1] - at[k]);
    }
    return db[7];
}

void ElementLfo::start(const ElementLfoParameters& parameters, unsigned clock, int delay) noexcept
{
    p_ = parameters;
    elapsed_ = -static_cast<long>(delay);
    frames_ = 0;
    const bool settings = p_.mode == 1;
    hz_ = settings ? lfoSpeedHz(p_) : kReferenceHz;
    phase0_ = 0.0;
    if (settings && p_.keyOnReset == 0) {
        const double cycles = (static_cast<double>(clock) + delay) * hz_ / sampleRate_;
        phase0_ = cycles - std::floor(cycles);
    }
    delayFrames_ = settings ? secondsFrames(p_.delay, sampleRate_) : 0;
    fadeFrames_ = settings ? secondsFrames(p_.fade, sampleRate_) : 0;
    pitchPP_ = ampPP_ = filterPP_ = 0.0f;
}

void ElementLfo::beginBlock(size_t frames, float pitchCC, float ampCC) noexcept
{
    elapsed_ += static_cast<long>(frames_);
    frames_ = frames;
    const float pcc = 127.0f * pitchCC, acc = 127.0f * ampCC;
    if (p_.mode != 1) {
        pitchPP_ = p_.pitchCC >= 0 ? lfoPitchDepthCents(p_.pitchRatio, pcc) : 0.0f;
        const float amp = p_.ampCC >= 0 ? lfoAmpDepthDb(static_cast<float>(std::abs(p_.ampRatio)) * acc / 127.0f) : 0.0f;
        ampPP_ = p_.ampRatio < 0 ? -amp : amp;
        filterPP_ = 0.0f;
        return;
    }
    const float pitchIndex = clamp127(static_cast<float>(p_.pitchDepth) + (p_.pitchCC >= 0 ? 2.0f * static_cast<float>(p_.pitchRatio) * pcc / 127.0f : 0.0f));
    pitchPP_ = pitchLaw(pitchIndex, 127.0f);
    const float ampIndex = clamp127(static_cast<float>(p_.ampDepth) + (p_.ampCC >= 0 ? static_cast<float>(p_.ampRatio) * acc / 127.0f : 0.0f));
    ampPP_ = ampIndex > 0.0f ? lfoAmpDepthDb(ampIndex / 2.0f) : 0.0f;
    filterPP_ = p_.filterDepth > 0 ? 4800.0f * std::exp2((static_cast<float>(p_.filterDepth) - 127.0f) / 16.0f) : 0.0f;
}

float ElementLfo::scale(size_t i) const noexcept
{
    const long t = elapsed_ + static_cast<long>(i);
    if (t < delayFrames_)
        return 0.0f;
    if (fadeFrames_ <= 0)
        return 1.0f;
    return std::min(1.0f, static_cast<float>(t - delayFrames_) / static_cast<float>(fadeFrames_));
}

float ElementLfo::wave(size_t i) const noexcept
{
    const double cycles = phase0_ + static_cast<double>(elapsed_ + static_cast<long>(i)) * hz_ / sampleRate_;
    const float ph = static_cast<float>(cycles - std::floor(cycles));
    switch (p_.mode == 1 ? p_.wave : 1) {
    case 0: // saw, rising from zero
        return ph < 0.5f ? 2.0f * ph : 2.0f * ph - 2.0f;
    case 2: // square, high half first
        return ph < 0.5f ? 1.0f : -1.0f;
    default: // triangle, falling from zero
        return ph < 0.25f ? -4.0f * ph : (ph < 0.75f ? 4.0f * ph - 2.0f : 4.0f - 4.0f * ph);
    }
}

void ElementLfo::addPitch(absl::Span<float> cents) const noexcept
{
    if (pitchPP_ == 0.0f)
        return;
    const size_t offset = frames_ > cents.size() ? frames_ - cents.size() : 0;
    for (size_t k = 0; k < cents.size(); ++k)
        cents[k] += 0.5f * pitchPP_ * wave(offset + k) * scale(offset + k);
}

void ElementLfo::applyGain(absl::Span<float> gain) const noexcept
{
    if (ampPP_ == 0.0f)
        return;
    for (size_t k = 0; k < gain.size(); ++k) {
        // a dip from the unmodulated level: triangle 0 -> 1 -> 0 over a cycle from note-on,
        // saw a ramp, square the second half
        const float w = wave(k);
        float dip;
        switch (p_.mode == 1 ? p_.wave : 1) {
        case 0: dip = w >= 0.0f ? 0.5f * w : 1.0f + 0.5f * w; break;
        case 2: dip = w > 0.0f ? 0.0f : 1.0f; break;
        default: {
            const double cycles = phase0_ + static_cast<double>(elapsed_ + static_cast<long>(k)) * hz_ / sampleRate_;
            const float ph = static_cast<float>(cycles - std::floor(cycles));
            dip = 2.0f * std::min(ph, 1.0f - ph);
        }
        }
        gain[k] *= std::pow(10.0f, -ampPP_ * dip * scale(k) / 20.0f);
    }
}

void ElementLfo::addFilter(absl::Span<float> cents) const noexcept
{
    if (filterPP_ == 0.0f)
        return;
    for (size_t k = 0; k < cents.size(); ++k)
        cents[k] += 0.5f * filterPP_ * wave(k) * scale(k);
}

} // namespace oscairn
} // namespace sfz

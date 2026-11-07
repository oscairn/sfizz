// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "budget.h"
#include <oscairn-clap/voice-budget-plugin.hpp>

namespace oscairn_sampler {

struct VoiceBudget::Impl {
    oscairn::voices::Limits limits;
    std::array<oscairn::voices::Candidate, maxBudgetVoices> candidates {};
    size_t count = 0;
    oscairn::voices::Report<maxBudgetNotes> report;
};

VoiceBudget::VoiceBudget() : impl_(new Impl) {}
VoiceBudget::~VoiceBudget() = default;

void VoiceBudget::setLimits(uint32_t limit, const oscairn_voice_budget_context_t* contexts, uint32_t count) noexcept
{
    impl_->limits.set(limit, contexts, count);
}

bool VoiceBudget::limited() const noexcept { return impl_->limits.limit || impl_->limits.count; }
uint32_t VoiceBudget::limit() const noexcept { return impl_->limits.limit; }
uint32_t VoiceBudget::limitOf(uint32_t context, bool selected) const noexcept
{
    return impl_->limits.of(context, selected).limit;
}

void VoiceBudget::clearCandidates() noexcept { impl_->count = 0; }

void VoiceBudget::addCandidate(const BudgetCandidate& c) noexcept
{
    Impl& impl = *impl_;
    if (impl.count == maxBudgetVoices)
        return;
    oscairn::voices::Candidate& to = impl.candidates[impl.count];
    to.context = c.context;
    to.selected = c.selected;
    to.held = c.held;
    to.ending = c.ending;
    to.mono = false; // sfizz has no mono mode of its own
    to.loudness = c.loudness;
    to.age = c.age;
    ++impl.count;
}

void VoiceBudget::setEnding(size_t i) noexcept
{
    if (i < impl_->count)
        impl_->candidates[i].ending = true;
}

int VoiceBudget::victim(uint32_t incoming, uint32_t only) const noexcept
{
    const Impl& impl = *impl_;
    return oscairn::voices::victim(
        impl.count, [&](size_t i) { return &impl.candidates[i]; },
        impl.limits, incoming, only);
}

void VoiceBudget::clearReport() noexcept { impl_->report.count = 0; }

void VoiceBudget::addToReport(int32_t noteId, int16_t channel, int16_t key, double loudness) noexcept
{
    impl_->report.add(noteId, 0, channel, key, loudness);
}

uint32_t VoiceBudget::writeReport(oscairn_voice_budget_note_t* out, uint32_t capacity) const noexcept
{
    return impl_->report.write(out, capacity);
}

} // namespace oscairn_sampler

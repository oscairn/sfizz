// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// The Oscairn SDK's voice-budget helper (voice-budget-plugin.hpp: the host's limits, the order
// voices are given up in, the notes report) behind a C++14 interface. The helper needs C++17;
// sfizz builds as C++14, and its headers change ABI under C++17 (absl's string_view), so the
// helper lives in budget.cpp alone. Allocates only when constructed.

#pragma once
#include <oscairn-clap/voice-budget.h>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace oscairn_sampler {

constexpr size_t maxBudgetVoices = 8 * 512; // candidates: eight contexts of 512 voice slots
constexpr uint32_t maxBudgetNotes = 256;

// A voice as the SDK's order sees it (oscairn::voices::Candidate)
struct BudgetCandidate {
    uint32_t context;
    bool selected, held, ending;
    double loudness;
    uint64_t age; // larger: newer
};

class VoiceBudget {
public:
    VoiceBudget();
    ~VoiceBudget();
    static constexpr uint32_t none = OSCAIRN_VOICE_BUDGET_SELECTED; // no context

    // The host's settings (set_limits)
    void setLimits(uint32_t limit, const oscairn_voice_budget_context_t* contexts, uint32_t count) noexcept;
    bool limited() const noexcept; // a limit or a context's settings are set
    uint32_t limit() const noexcept;
    uint32_t limitOf(uint32_t context, bool selected) const noexcept; // a context's own limit

    // Candidates, numbered in the order added
    void clearCandidates() noexcept;
    void addCandidate(const BudgetCandidate& c) noexcept;
    void setEnding(size_t i) noexcept;
    // The candidate to give up for a new voice of `incoming` (or none), from `only` alone unless
    // none; -1 when none may go
    int victim(uint32_t incoming, uint32_t only) const noexcept;

    // The notes() report: cleared, a voice at a time, then written
    void clearReport() noexcept;
    void addToReport(int32_t noteId, int16_t channel, int16_t key, double loudness) noexcept;
    uint32_t writeReport(oscairn_voice_budget_note_t* out, uint32_t capacity) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oscairn_sampler

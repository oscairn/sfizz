// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "Elements.h"
#include <algorithm>

namespace sfz {
namespace oscairn {

namespace {
constexpr uint32_t kRandomSeed = 0x2545f491u;
// Key On Delay time lengths measured on the reference (batches xa1, xa1b: the onset of a delayed
// sine element after an undelayed one), index and seconds; linear between them
constexpr int kDelayKnots = 29;
constexpr int kDelayIndex[kDelayKnots] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72,
                                           80, 88, 96, 104, 112, 116, 120, 124, 127 };
constexpr double kDelaySeconds[kDelayKnots] = { 0.0, 0.0096, 0.0199, 0.0301, 0.0488, 0.0589, 0.0792, 0.1010,
                                                0.1304, 0.2388, 0.3651, 0.5089, 0.6670, 0.8382, 1.0226, 1.4259,
                                                1.8773, 2.3709, 2.8961, 3.4594, 4.0444, 4.6681, 5.3127, 5.9875,
                                                6.7014, 7.0541, 7.4254, 7.7998, 8.0847 };

// A note or velocity limit; a low limit above the high one selects what lies outside
bool within(int value, int lo, int hi)
{
    return lo <= hi ? (value >= lo && value <= hi) : (value >= lo || value <= hi);
}

bool switchedOn(int32_t mode, bool switch1, bool switch2)
{
    switch (mode) {
    case kSwitchesOff: return !switch1 && !switch2;
    case kSwitch1On: return switch1;
    case kSwitch2On: return switch2;
    default: return true;
    }
}
} // namespace

double keyOnDelaySeconds(int32_t timeLength, int32_t noteLength, double beatsPerSecond) noexcept
{
    // quarter notes per note length 5-21
    // (a triplet note is a third of the next longer note: an 1/8 triplet a third of a quarter)
    static constexpr double quarters[17] = { 1.0 / 4, 1.0 / 3, 3.0 / 8, 1.0 / 2, 2.0 / 3, 3.0 / 4, 1.0, 4.0 / 3,
                                             3.0 / 2, 2.0, 8.0 / 3, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };
    if (noteLength >= 5 && noteLength <= 21)
        return beatsPerSecond > 0.0 ? quarters[noteLength - 5] / beatsPerSecond : 0.0;
    const int i = std::min(127, std::max(0, timeLength));
    int k = 1;
    while (kDelayIndex[k] < i)
        ++k;
    const double f = static_cast<double>(i - kDelayIndex[k - 1]) / (kDelayIndex[k] - kDelayIndex[k - 1]);
    return kDelaySeconds[k - 1] + f * (kDelaySeconds[k] - kDelaySeconds[k - 1]);
}

void ElementSelection::clear() noexcept
{
    present_.reset();
    selected_.reset();
    count_ = 0;
    mono_ = false;
    restart();
}

void ElementSelection::add(const ElementParameters& element, bool mono) noexcept
{
    if (element.number < 1 || element.number > kMaxElements)
        return;
    const auto index = static_cast<size_t>(element.number - 1);
    if (!present_.test(index))
        ++count_;
    present_.set(index);
    elements_[index] = element;
    mono_ = mono_ || mono;
}

void ElementSelection::restart() noexcept
{
    cyclePosition_.fill(0);
    randomState_ = kRandomSeed;
}

uint8_t ElementSelection::draw() noexcept
{
    randomState_ = randomState_ * 1664525u + 1013904223u;
    return static_cast<uint8_t>(randomState_ >> 24);
}

void ElementSelection::select(int key, int velocity, int otherNotesHeld, bool switch1, bool switch2) noexcept
{
    selected_.reset();
    // Per group: whether it has an element a plain note would play (Normal, Cycle, Random, or
    // an A.SW element whose switch condition holds), whatever its limits; whether one of its
    // Legato elements covers this note; and its Cycle and Random elements covering this note.
    std::array<bool, kElementGroups> plain {}, legato {};
    std::array<std::bitset<kMaxElements>, kElementGroups> cycle {}, random {};
    std::bitset<kMaxElements> covering;
    for (size_t i = 0; i < kMaxElements; ++i) {
        if (!present_.test(i))
            continue;
        const ElementParameters& e = elements_[i];
        const auto g = static_cast<size_t>(e.group - 1);
        if (e.mode == kKeyOff)
            continue;
        if (e.mode != kLegato && switchedOn(e.mode, switch1, switch2))
            plain[g] = true;
        if (!within(key, e.loKey, e.hiKey) || !within(velocity, e.loVel, e.hiVel))
            continue;
        covering.set(i);
        if (e.mode == kLegato)
            legato[g] = true;
        else if (e.mode == kCycle)
            cycle[g].set(i);
        else if (e.mode == kRandom)
            random[g].set(i);
    }

    // Cycle: the group's next covering element in element order from where it left off, then
    // from the first; a note that no Cycle element of the group covers starts it over.
    // Random: each covering element alike, from an 8-bit draw r as the (r n / 255)th of n; the
    // draw of 255 takes the sound's first element if it is one of them, else none.
    std::bitset<kMaxElements> chosen;
    for (size_t g = 0; g < kElementGroups; ++g) {
        if (cycle[g].none())
            cyclePosition_[g] = 0;
        else {
            size_t i = static_cast<size_t>(cyclePosition_[g]);
            while (i < kMaxElements && !cycle[g].test(i))
                ++i;
            if (i == kMaxElements)
                for (i = 0; !cycle[g].test(i); ++i) { }
            chosen.set(i);
            cyclePosition_[g] = static_cast<int>((i + 1) % kMaxElements);
        }
        if (random[g].any()) {
            const auto n = random[g].count();
            const size_t k = draw() * n / 255;
            size_t seen = 0;
            for (size_t i = 0; i < kMaxElements; ++i)
                if (random[g].test(i) && seen++ == k)
                    chosen.set(i);
            if (k == n && random[g].test(0))
                chosen.set(0);
        }
    }

    for (size_t i = 0; i < kMaxElements; ++i) {
        if (!covering.test(i))
            continue;
        const ElementParameters& e = elements_[i];
        const auto g = static_cast<size_t>(e.group - 1);
        bool on = false;
        switch (e.mode) {
        case kLegato:
            // a mono part's note while another key is held, in a group with a plain element
            on = mono_ && plain[g] && otherNotesHeld > 0;
            break;
        case kCycle:
        case kRandom:
            on = chosen.test(i);
            break;
        default:
            // a mono part's legato note plays the group's Legato elements in their place
            on = switchedOn(e.mode, switch1, switch2) && !(mono_ && legato[g] && otherNotesHeld > 0);
            break;
        }
        selected_.set(i, on);
    }
}

bool ElementSelection::plays(int32_t number) const noexcept
{
    if (number < 1 || number > kMaxElements)
        return true;
    return selected_.test(static_cast<size_t>(number - 1));
}

} // namespace oscairn
} // namespace sfz

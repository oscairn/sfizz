// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Element selection (doc/oscairn-plan.md, milestone 3): which elements of a sound a note-on
// starts, by each element's XA Control, element group and note and velocity limits, the part's
// mono/poly mode, the notes held and the two assignable switches. The regions of one element
// share its number (`oscairn_element`); regions without one are not selected here.

#pragma once
#include <array>
#include <bitset>
#include <cstdint>

namespace sfz {
namespace oscairn {

// XA Control, as the reference numbers it
enum ElementMode : int32_t {
    kNormal = 0,
    kLegato,
    kKeyOff,
    kCycle,
    kRandom,
    kSwitchesOff, // A.SW Off: plays while neither assignable switch is on
    kSwitch1On,
    kSwitch2On,
};

constexpr int kMaxElements = 128;
constexpr int kElementGroups = 8;
// The extended controllers the CLAP plugin sets for Assign Switch 1 and 2 (on at 0.5 and up)
constexpr int kAssignSwitchCC[2] = { 300, 301 };

// An element's selection settings, as a region carries them
struct ElementParameters {
    int32_t number { 0 }; // 1-128; 0: not an element
    int32_t mode { kNormal };
    int32_t group { 1 }; // 1-8
    int32_t loKey { 0 }, hiKey { 127 }; // the element's note limits (low above high: outside)
    int32_t loVel { 1 }, hiVel { 127 };
};

class ElementSelection {
public:
    // No elements, as before a load
    void clear() noexcept;
    // Called for every region of a loaded patch
    void add(const ElementParameters& element, bool mono) noexcept;
    // Cycle positions and the random draws back to where a load leaves them
    void restart() noexcept;
    bool empty() const noexcept { return count_ == 0; }
    bool mono() const noexcept { return mono_; }

    /**
     * @brief Decides which elements a note-on starts.
     *
     * @param key the note
     * @param velocity 1-127
     * @param otherNotesHeld keys held besides this note (mono); a note a mono part starts again
     *                       when a newer note ends counts the keys still held
     * @param switch1, switch2 the assignable switches
     */
    void select(int key, int velocity, int otherNotesHeld, bool switch1, bool switch2) noexcept;
    // Whether the last select() starts element `number`; regions that are no element always play
    bool plays(int32_t number) const noexcept;

private:
    uint8_t draw() noexcept;
    std::array<ElementParameters, kMaxElements> elements_ {}; // by number - 1
    std::bitset<kMaxElements> present_;
    std::bitset<kMaxElements> selected_;
    int count_ { 0 };
    bool mono_ { false };
    std::array<int, kElementGroups> cyclePosition_ {}; // the element index a group's next cycle search starts at
    uint32_t randomState_ { 0 };
};

} // namespace oscairn
} // namespace sfz

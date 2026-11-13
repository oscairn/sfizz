// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#pragma once
#include "Defaults.h"

namespace sfz
{
struct SynthConfig
{
    bool freeWheeling { false };

    int liveSampleQuality { Default::sampleQuality };
    // Live sample quality of element voices (a region with the element amplitude or pitch
    // envelope) without a sample_quality of their own; -1: liveSampleQuality, as other voices
    int liveElementSampleQuality { -1 };
    int freeWheelingSampleQuality { Default::freewheelingSampleQuality };

    int liveOscillatorQuality { Default::oscillatorQuality };
    int freeWheelingOscillatorQuality { Default::freewheelingOscillatorQuality };

    int currentSampleQuality() const noexcept
    {
        return freeWheeling ? freeWheelingSampleQuality : liveSampleQuality;
    }

    int currentOscillatorQuality() const noexcept
    {
        return freeWheeling ? freeWheelingOscillatorQuality : liveOscillatorQuality;
    }

    bool sustainCancelsRelease { Default::sustainCancelsRelease };

    // Each voice's data, amplitude, filter and panning times in the callback breakdown (two
    // clock reads each, a voice and block); off, they read 0
    bool voiceTimings { true };
};
}

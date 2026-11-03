#pragma once

// tracker-mini-ce: WAVE (wavetable) and PHASE (phase distortion) synth engines, ported from
// Modular-Trackular's synths patch (MIT). The DSP is in core/libs/ceSynths/.

#include "PolySynth/sdk/InstrumentType.h"

#include <memory>

namespace polysynth
{
std::unique_ptr<InstrumentType> createWaveSynth();
std::unique_ptr<InstrumentType> createPhaseSynth();
} // namespace polysynth

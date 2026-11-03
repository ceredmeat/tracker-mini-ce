#pragma once

// tracker-mini-ce: DX7 engine on Google's msfa (Apache-2.0), see Dx7SynthVoice.cpp.

#include "PolySynth/sdk/InstrumentType.h"

#include <memory>

namespace polysynth
{
std::unique_ptr<InstrumentType> createDx7Synth();
} // namespace polysynth

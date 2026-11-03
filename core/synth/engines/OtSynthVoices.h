#pragma once

// tracker-mini-ce: OpenTracker synth engines (github.com/opentracker-fw/opentracker-synths,
// MIT, Copyright (c) 2026 Marcin Spoczynski) as engines of CE's synth SDK.

#include "PolySynth/sdk/InstrumentType.h"

#include <memory>

namespace polysynth
{
std::unique_ptr<InstrumentType> createOtFm4Synth();
std::unique_ptr<InstrumentType> createOtJu6Synth();
std::unique_ptr<InstrumentType> createOtDtypeSynth();
std::unique_ptr<InstrumentType> createOtAtypeSynth();
std::unique_ptr<InstrumentType> createOtCstypeSynth();
std::unique_ptr<InstrumentType> createOtTtypeSynth();
} // namespace polysynth

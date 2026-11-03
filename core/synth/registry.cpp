#include "config.h"
#ifdef FEATURE_SYNTH

#include "PolySynth/sdk/InstrumentTypeRegistry.h"
#include "engines/ChiptuneSynthVoice.h"
#include "engines/CeSynthVoices.h"
#include "engines/OtSynthVoices.h"

namespace polysynth
{
// Factory synth engines (provided by precompiled libPolySynth.a + libPolyDSP.a)
extern std::unique_ptr<InstrumentType> createMidiOut();
extern std::unique_ptr<InstrumentType> createSimpleAnalogSynth();
extern std::unique_ptr<InstrumentType> createFatAnalogSynth();
extern std::unique_ptr<InstrumentType> createVASynth();
extern std::unique_ptr<InstrumentType> createFMSynth();
extern std::unique_ptr<InstrumentType> createDrumSynth();
extern std::unique_ptr<InstrumentType> createShapingSynth();

void InstrumentTypeRegistry::registerInstrumentTypes(
  InstrumentTypeRegistry& registry,
  [[maybe_unused]] const Synth::Allocator& largeBuffersAlloc)
{
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kMidiOut), createMidiOut());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kSimpleAnalogSynth),
                               createSimpleAnalogSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kFatAnalogSynth),
                               createFatAnalogSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kVASynth), createVASynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kFMSynth), createFMSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kDrumSynth), createDrumSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kShapingSynth),
                               createShapingSynth());

    // Open-Source Custom Synth Engines
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kChiptuneSynth),
                               createChiptuneSynth());
    // tracker-mini-ce
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kWaveSynth), createWaveSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kPhaseSynth), createPhaseSynth());
    // tracker-mini-ce: OpenTracker engines (MIT, Marcin Spoczynski)
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtFm4Synth), createOtFm4Synth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtJu6Synth), createOtJu6Synth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtDtypeSynth), createOtDtypeSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtAtypeSynth), createOtAtypeSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtCstypeSynth), createOtCstypeSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtTtypeSynth), createOtTtypeSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtEightSynth), createOtEightSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtHandSynth), createOtHandSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtBtypeSynth), createOtBtypeSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kOtArcSynth), createOtArcSynth());
}

} // namespace polysynth

#endif

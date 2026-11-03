#ifndef CE_MODS_H_
#define CE_MODS_H_

// tracker-mini-ce: per-project settings of the community mods.
// They live in the reserved bytes of MIDI instrument 16 inside the project file, so
// they are saved, loaded and copied with the project, need no extra file, and are
// zero (= off) in projects made without these mods.

#include "mtStructs.h"

extern strMtProject mtProject;

namespace ceMods
{
inline uint8_t* settings()
{
    return mtProject.values.midiInstrument[kMidiInstrumentsCount - 1].reserved;
}

// Master FX: 0 = Stock; see core/libs/Audio/ceMasterFx/ceMasterFx.h for the lists
inline uint8_t& masterReverb() { return settings()[5]; }
inline uint8_t& masterDelay() { return settings()[6]; }
} // namespace ceMods

#endif

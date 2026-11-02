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

// Interpolate: CC FX ramps between steps. 0 = Off, 1 = On
inline uint8_t& interpolateMidi() { return settings()[3]; }
inline uint8_t& interpolateSynth() { return settings()[4]; }
} // namespace ceMods

#endif

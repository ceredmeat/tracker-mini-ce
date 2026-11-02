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

// Global Swing: 0 = Off, 1..51 = swing 25..75
inline uint8_t& swingSetting() { return settings()[0]; }
inline int swingValue() { return swingSetting() == 0 ? 0 : 24 + swingSetting(); }

// Humanize: largest random timing (micro-move) and volume change, 0..50
inline uint8_t& humanizeTiming() { return settings()[1]; }
inline uint8_t& humanizeVolume() { return settings()[2]; }
} // namespace ceMods

#endif

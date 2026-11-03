#ifndef CE_MODS_EXTRA_H_
#define CE_MODS_EXTRA_H_

// tracker-mini-ce: more per-project settings of the community mods (ceMods.h has the
// first seven). They live in the reserved bytes of MIDI instruments 15 down to 9 inside
// the project file (7 bytes each, 49 in all), so they save and load with the project,
// need no extra file, and are zero in projects made without these mods. Zero is always
// the default.

#include "mtStructs.h"

extern strMtProject mtProject;

namespace ceExtra
{
inline uint8_t& at(int i)
{
    return mtProject.values.midiInstrument[kMidiInstrumentsCount - 2 - i / 7].reserved[i % 7];
}

enum Slot : uint8_t
{
    kBackupKeep = 0,    // Auto Backup: 0 keep 5, 1 keep 1, 2 keep 2, 3 keep 3, 4 keep 10,
                        // 5 keep 20, 6 keep 50, 7 off
    kBackupSamples = 1, // 0 with the samples folder, 1 without
    kDjResonance = 2,   // DJ Filter: 0 low, 1 mid, 2 high
    kGenAlgorithm = 3,  // Generate: algorithm
    kGenDensity = 4,    // Generate: 0 = 50 %, else 1..100 %
    kGenMutate = 5,     // Mutate amount: 0 = 25 %, else 1..100 %
    kFxPack = 7,        // FX Pack: 21 bytes, 7..27 (see ceFxPack.h)
    kFxPackEnd = 28,
};
} // namespace ceExtra

#endif

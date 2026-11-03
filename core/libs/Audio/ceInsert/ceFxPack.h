#ifndef LIBS_AUDIO_CEINSERT_CEFXPACK_H_
#define LIBS_AUDIO_CEINSERT_CEFXPACK_H_

// tracker-mini-ce: FX Pack. Four effects on the master output, in this order:
//   Dirt   (Drive 0 = Off, Shape, Bit crush, Downsample, Darken, Dry blend)
//   LoFi   (Mix 0 = Off, Bit crush, Downsample, Speaker, Mono)
//   Tape   (Drive 0 = Off, Darken, Wow, Flutter, Dry blend)
//   Chorus (Mix 0 = Off, Rate, Depth, Narrow)
// Settings per project in ceModsExtra.h slots kFxPack.. (zero = off / neutral), set in
// Config -> Project Settings -> FX Pack. All Off leaves the audio untouched.

#include <stdint.h>

namespace ceFx
{
enum Param : uint8_t
{
    kDirtDrive,
    kDirtShape,   // 0 Soft, 1 Hard, 2 Fold, 3 Asym
    kDirtBits,    // 0 = 16 bits ... 12 = 4 bits
    kDirtRate,    // hold 1..16 samples (0 = 1)
    kDirtDarken,  // 0..100
    kDirtDry,     // dry blend 0..100

    kLofiMix,     // 0 = Off
    kLofiBits,    // 0 = 16 bits ... 14 = 2 bits
    kLofiRate,    // hold 1..32 samples (0 = 1)
    kLofiSpeaker, // 0 Off, 1 Radio, 2 Phone, 3 Handheld, 4 TV
    kLofiMono,    // 0 Off, 1 On

    kTapeDrive,   // 0 = Off
    kTapeDarken,
    kTapeWow,
    kTapeFlutter,
    kTapeDry,

    kChorusMix,   // 0 = Off
    kChorusRate,
    kChorusDepth,
    kChorusNarrow,

    kParamCount   // 20
};

uint8_t& param(Param p);
} // namespace ceFx

#endif

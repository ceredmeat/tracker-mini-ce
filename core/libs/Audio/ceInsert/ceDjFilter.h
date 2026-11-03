#ifndef LIBS_AUDIO_CEINSERT_CEDJFILTER_H_
#define LIBS_AUDIO_CEINSERT_CEDJFILTER_H_

// tracker-mini-ce: DJ Filter. One knob from -50 (lowpass, darker) through 0 (Off) to +50
// (highpass, thinner) on the master output. The position isn't saved; the resonance is
// (Config -> Project Settings -> DJ filter res., ceModsExtra.h).

#include <stdint.h>

namespace ceDj
{
constexpr int kMax = 50;
int position();
void setPosition(int pos);
void label(char* out, int len); // "Off", "LP 40%", "HP 20%"
} // namespace ceDj

#endif

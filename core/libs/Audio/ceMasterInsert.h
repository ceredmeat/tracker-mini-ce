#ifndef LIBS_AUDIO_CEMASTERINSERT_H_
#define LIBS_AUDIO_CEMASTERINSERT_H_

// tracker-mini-ce: stereo effects on the master output, just before the limiter.
// Called once per audio block from the left limiter with both channels' blocks
// (AUDIO_BLOCK_SAMPLES each), to be changed in place. Each effect has its own file in
// ceInsert/; one that is switched off must leave the samples untouched.

#include <stdint.h>

void ceMasterInsert(int16_t* left, int16_t* right);

void ceDjFilterBlock(int16_t* left, int16_t* right); // ceInsert/ceDjFilter.cpp
void ceFxPackBlock(int16_t* left, int16_t* right);   // ceInsert/ceFxPack.cpp

#endif

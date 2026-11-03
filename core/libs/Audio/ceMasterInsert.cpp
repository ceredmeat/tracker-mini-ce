// tracker-mini-ce: stereo master effects, see ceMasterInsert.h

#include "ceMasterInsert.h"
#include "AudioStream.h"

AUDIO_CORE_RAM void ceMasterInsert(int16_t* left, int16_t* right)
{
    ceFxPackBlock(left, right);   // Dirt, LoFi, Tape, Chorus
    ceDjFilterBlock(left, right); // then the DJ filter
}

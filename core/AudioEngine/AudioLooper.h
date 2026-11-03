#ifndef AUDIOENGINE_AUDIOLOOPER_H_
#define AUDIOENGINE_AUDIOLOOPER_H_

// tracker-mini-ce Looper: the audio object. Takes the codec input (stereo), runs the
// looper engine and sends its mix to the final mixer (input 2). Idle (no CPU) while the
// looper has nothing to play and its screen is closed.

#include "AudioStream.h"
#include "ceLooperConfig.h"

#ifdef CE_LOOPER

class AudioLooper : public AudioStream
{
  public:
    AudioLooper();
    virtual void update(void) override;

    static void begin();          // sets up the engine's memory (once)
    static void mainLoop();       // from the main loop: effect housekeeping
    static volatile bool screenOpen;
    static uint8_t* uiScratch();     // 64 KB for the screen (waveform bitmaps, file buffers)
    static constexpr uint32_t kUiScratchBytes = 64u * 1024u;

  private:
    audio_block_t* inputQueueArray[2];
};

#endif
#endif

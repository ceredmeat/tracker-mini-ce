#include "AudioLooper.h"

#ifdef CE_LOOPER

#include "ceLooper/lp_engine.h"

// 64 KB less than the sample memory gave up: room in the SDRAM for the engine and screen objects
__attribute__((section(".sample_memory"), used)) static int16_t looperPool[(kLooperPoolBytes - 65536) / 2];

volatile bool AudioLooper::screenOpen = false;
static bool looperReady = false;

uint8_t* AudioLooper::uiScratch()
{
    return (uint8_t*)looperPool + sizeof(looperPool) - kUiScratchBytes;
}

AudioLooper::AudioLooper()
  : AudioStream(2, inputQueueArray)
{
}

void AudioLooper::begin()
{
    if (looperReady) return;
    lp::constructEngine();
    lp::engine.init(looperPool, sizeof(looperPool) - kUiScratchBytes);
    looperReady = true;
}

void AudioLooper::mainLoop()
{
    if (!looperReady) return;
    for (int s = 0; s < lp::kFxSlots; s++)
        lp::engine.fx[s].prepare();
}

static bool looperBusy()
{
    if (AudioLooper::screenOpen) return true;
    if (lp::engine.running()) return true;
    for (int t = 0; t < lp::kTracks; t++)
        if (lp::engine.track[t].state != lp::TrackState::kEmpty &&
            lp::engine.track[t].state != lp::TrackState::kStopped)
            return true;
    return false;
}

void AudioLooper::update(void)
{
    audio_block_t* inL = receiveReadOnly(0);
    audio_block_t* inR = receiveReadOnly(1);

    if (!looperReady || !looperBusy())
    {
        lp::engine.audioRunning = false;
        if (inL) release(inL);
        if (inR) release(inR);
        return;
    }

    audio_block_t* outL = allocate();
    audio_block_t* outR = allocate();
    if (!outL || !outR)
    {
        if (outL) release(outL);
        if (outR) release(outR);
        if (inL) release(inL);
        if (inR) release(inR);
        return;
    }

    lp::engine.audioRunning = true;
    // monitoring only while the looper's screen is open
    bool mon = lp::engine.cfg.monitor;
    if (!screenOpen) lp::engine.cfg.monitor = false;
    lp::engine.process(inL ? inL->data : nullptr, inR ? inR->data : nullptr, outL->data, outR->data);
    lp::engine.cfg.monitor = mon;

    transmit(outL, 0);
    transmit(outR, 1);
    release(outL);
    release(outR);
    if (inL) release(inL);
    if (inR) release(inR);
}

#endif

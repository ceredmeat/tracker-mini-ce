#ifndef CE_LOOPER_ENGINE_H_
#define CE_LOOPER_ENGINE_H_

// tracker-mini-ce Looper engine: four stereo loop tracks on one bar clock, record /
// overdub / replace with undo, playback modes (tape and grain), performance effects,
// a mixer with two send-effect slots, and a metronome. Written for this firmware; the
// behaviour follows the Retrack looper's manual (github.com/math0ne/retrack-tracker),
// the code is new.
//
// Threading: process() runs in the audio interrupt. The UI calls the command methods
// (they post to a queue the audio side drains at the top of each block) and reads the
// public state for drawing. Parameters (levels, knobs) are plain stores.

#include "lp_dsp.h"
#include "lp_fx.h"

#include <stddef.h>
#include <stdint.h>

namespace lp
{

constexpr int kTracks = 4;
constexpr int kFxSlots = 2;
constexpr int kPerfFx = 8;
constexpr int kMaxGrains = 8;
constexpr int kUndoChunk = 2048;           // frames per copy-on-write chunk
constexpr int kMaxUndoChunks = 4096;       // up to 8 M frames
constexpr int kLoopXfade = 220;            // 5 ms loop-point blend
constexpr int kPunchRamp = 88;             // 2 ms punch / fade-in ramp
constexpr int kJumpXfade = 132;            // 3 ms click-free jumps
constexpr int kPreRoll = 265;              // 6 ms kept before an "on sound" start

// fixed point positions: 16 fractional bits
typedef int64_t pos_t;
constexpr int kFrac = 16;
constexpr pos_t kOne = (pos_t)1 << kFrac;

enum class TrackState : uint8_t
{
    kEmpty,
    kArmed,       // waiting for sound
    kWaiting,     // count-in / waiting for the bar line
    kRecording,   // first take
    kPlaying,
    kOverdub,
    kReplace,
    kStopped,
};

enum PlayMode : uint8_t
{
    kModeForward,
    kModeReverse,
    kModePingPong,
    kModeHalf,
    kModeDouble,
    kModeHalfRev,
    kModeDoubleRev,
    kModeHalfTime,
    kModeOctUp,
    kModeOctDown,
    kModeGranular,
    kModeCount
};

enum StopOpt : uint8_t { kStopFree, kStopBar, kStopBeat };
enum StartOpt : uint8_t { kStartNow, kStartCountIn, kStartSound };
enum AfterRec : uint8_t { kAfterPlay, kAfterOverdub, kAfterStop };
enum TempoOpt : uint8_t { kTempoTape, kTempoStretch };
enum ChannelOpt : uint8_t { kChStereo, kChMonoL, kChMonoR, kChMonoSum };
enum ClickSound : uint8_t { kClickBeep, kClickWood, kClickHat, kClickBell };
enum PerfFx : uint8_t { kPfRepeat1, kPfRepeat2, kPfLowPass, kPfHighPass, kPfSweepDown, kPfSweepUp, kPfVibrato, kPfTapeStop };

const char* modeName(int mode);
const char* perfName(int fx);
const char* repeatName(int knob);

// one grain (stretch / pitch / granular)
struct Grain
{
    bool on = false;
    pos_t pos = 0;      // read position (frames, fixed)
    pos_t step = 0;     // per sample
    int age = 0, len = 0;
    float gainL = 1, gainR = 1, amp = 1;
    float texture = 0.5f;
    // Clouds-style envelope
    float envPhase = 0, envInc = 0, smooth = 0, slope = 0;
};

struct GrainParams
{
    uint8_t spread = 20;    // 0..100
    uint8_t size = 30;      // 0..100 -> 20..500 ms
    uint8_t density = 70;   // 0..100: below the middle even, above random (Clouds); the middle is silent
    uint8_t texture = 50;   // 0..100: window shape (Clouds)
    int8_t pitch = 0;       // semitones -24..24
    uint8_t speed = 1;      // 0 = 1/2, 1 = 1x, 2 = 2x
    uint8_t direction = 0;  // 0 fwd, 1 rev, 2 ping
};

struct Track
{
    // memory
    int16_t* mem = nullptr;
    uint32_t memSamples = 0;
    uint8_t ch = 2;              // channels of the take (1 for the mono options)
    uint32_t capFrames() const { return memSamples / ch; }

    volatile TrackState state = TrackState::kEmpty;
    uint32_t start = 0;          // frames into the buffer where the loop starts (trim)
    uint32_t len = 0;            // frames
    pos_t anchor = 0;            // clock phase at loop position 0
    uint32_t version = 0;        // bumps when the audio changes (for the waveform cache)

    // playback
    uint8_t mode = kModeForward;
    pos_t off = 0;               // mode offset so a switch carries on
    pos_t pos = 0;               // current read position (for the playhead)
    pos_t lastStep = 0;          // signed step per sample, for the playhead trail
    bool muted = false;
    uint8_t volume = 80;
    uint8_t send[kFxSlots] = {0, 0};
    int8_t djFilter = 0;         // -50..50
    bool busy = false;           // UI is editing the buffer: silent, no writes

    // options
    uint8_t stopOpt = kStopBar;
    uint8_t fixedBars = 0;       // 0 = off
    uint8_t startOpt = kStartCountIn;
    uint8_t afterRec = kAfterPlay;
    uint8_t tempoOpt = kTempoTape;
    uint8_t channels = kChStereo;
    GrainParams grain;

    // recording
    pos_t recStartPhase = 0;
    pos_t recEnd = -1;           // phase where the take ends, -1 = open
    int tail = -1;               // frames of tail written past the end (loop blend)
    int64_t lastWrite = -1;      // last buffer frame written
    float lastInL = 0, lastInR = 0;
    float punchGain = 0;         // overdub / replace ramp
    float fadeIn = 1;            // fade-in of a take that starts mid-loop
    int armTimeout = 0;
    uint32_t recFramesTotal = 0; // for the display while recording

    // stop fade
    float fadeGain = 1;
    float fadeStep = 0;

    // undo (copy-on-write chunks kept in the upper half of the memory)
    uint8_t* dirty = nullptr;    // kMaxUndoChunks / 8 bytes, in the pool
    bool undoAvail = false, undone = false;
    int swapCursor = -1;         // >= 0 while an undo / redo swap is running

    // jump crossfade
    pos_t xfPos = 0;
    int xfLeft = 0;

    // performance effects
    uint8_t perfKnob[kPerfFx];
    uint8_t perfHeld = 0, perfLatched = 0;   // bitmasks
    uint8_t perfOrder = 0;                   // last repeat engaged (0/1)
    pos_t repStart = 0;                      // repeat: segment start phase
    pos_t repLen = 0;
    bool repOn = false;
    pos_t tapePhase = 0;                     // tape stop: frozen effective phase
    float tapeRate = 1, tapeT = 0;
    bool tapeOn = false;
    float vibPh = 0;
    float vibPrev = 0;
    float sweepT[2] = {0, 0};
    Svf filt[2][2];                          // [stage][channel] perf lp / hp
    Svf dj[2];
    float djState = 0;

    // grains
    Grain grains[kMaxGrains];
    int spawnIn = 0;
    int nextGrain = 0;
    pos_t center = 0;
    bool pingRev = false;
    float grainPhasor = 0, grainCount = 0, grainNorm = 1, grainSizeHint = 1024;

    // meters
    float meter = 0;
    float drawGrainPos[kMaxGrains];          // for the lane: grain read positions (frames)
    float drawGrainAmp[kMaxGrains];

    bool hasAudio() const { return len > 0; }
};

struct Config
{
    float bpm = 94.0f;
    uint8_t beatsPerBar = 4;
    bool metronome = false;
    uint8_t clickLevel = 60;
    uint8_t clickSound = kClickBeep;
    bool accent = true;
    bool clickOnlyRec = false;
    uint8_t countInBars = 1;      // 0, 1, 2, 4
    int8_t thresholdDb = -30;
    uint8_t stopFade = 0;         // 0 off, 1 beat, 1 bar, 2 bars
    uint8_t inputGain = 50;       // 0..100, 50 = unity
    bool monitor = true;
    uint8_t feedback = 100;       // overdub feedback %
    int16_t latency = 0;          // recording latency compensation, frames
    uint8_t master = 100;
    bool masterMute = false;
};

struct Cmd
{
    uint8_t op, a, b, c;
};

class Engine
{
  public:
    // pool: one block of memory for everything (tracks, fx, clipboard)
    void init(int16_t* pool, uint32_t poolBytes);

    // audio interrupt
    void process(const int16_t* inL, const int16_t* inR, int16_t* outL, int16_t* outR);

    // --- UI commands (queued) ---
    void pressRec(int t);              // record / overdub cycle
    void pressReplace(int t);
    void pressPlay(int t);             // stop / play one track
    void pressMute(int t);
    void clear(int t);
    void clearAll();
    void transport();                  // Play key: stop all / start from bar 1
    void playAll();
    void undo(int t);
    void redo(int t);
    void jump(int t, int eighth);
    void setMode(int t, int mode);
    void perfHold(int t, int fx, bool on);
    void perfLatch(int t, int fx);
    void perfClear();

    // --- direct parameter setters / getters ---
    Config cfg;
    Track track[kTracks];
    FxSlot fx[kFxSlots];
    uint8_t fxReturn[kFxSlots] = {80, 80};
    bool fxMute[kFxSlots] = {false, false};
    uint8_t chainSend[kFxSlots] = {0, 0};    // slot -> other slot (only one way runs)

    bool running() const { return running_; }
    pos_t phase() const { return phase_; }
    pos_t barLen() const { return barLen_; }
    pos_t beatLen() const { return barLen_ / cfg.beatsPerBar; }
    bool hasLoops() const;
    float tempoPct() const { return tempoPct_; }
    void setTempoPct(float pct);      // varispeed once there is a loop
    void setBpm(float bpm);           // sets the bar before the first loop
    float currentBpm() const;
    float inputLevel() const { return inLevel_; }
    float returnMeter(int s) const { return retMeter_[s]; }
    float masterMeter(int c) const { return masterMeter_[c]; }
    uint32_t blockCount() const { return blocks_; }
    bool anyRecording() const;
    bool anyArmedOrWaiting() const;
    int selected = 0;              // the selected track (input monitoring uses its sends)
    bool audioRunning = false;     // set by the audio wrapper; edits wait for a block only then
    int countInBeat() const { return countInBeat_; }

    // read positions for the playheads (frames, within the loop)
    float playhead(int t) const;
    float playheadStep(int t) const;

    // editing helpers (UI thread; they lock the track while they work)
    bool trim(int t, uint32_t a, uint32_t b);
    bool untrim(int t);
    bool cut(int t, uint32_t a, uint32_t b);
    bool erase(int t, uint32_t a, uint32_t b);
    bool copySel(int t, uint32_t a, uint32_t b);
    bool paste(int t, uint32_t at, bool insert);
    bool duplicate(int from, int to);
    uint32_t clipLen() const { return clipLen_; }
    // sample access for waveforms (frames within the loop, channel 0/1)
    inline int16_t sample(int t, uint32_t frame, int c) const
    {
        const Track& tr = track[t];
        uint32_t i = tr.start + frame;
        return tr.ch == 2 ? tr.mem[i * 2 + c] : tr.mem[i];
    }
    // projects: the clock as saved
    uint32_t barFrames() const { return (uint32_t)(barLen_ >> kFrac); }
    float loopBpm() const { return loopBpm_; }
    void restoreClock(uint32_t barFrames, float loopBpm, float tempoPct);
    // load a take from the outside (WAV import): returns a pointer to write frames
    int16_t* beginLoad(int t, uint8_t channels, uint32_t frames);
    void endLoad(int t, uint32_t frames, bool setsBar);

  private:
    void exec(const Cmd& c);
    void post(uint8_t op, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0);
    void lock(int t);
    void unlock(int t);

    void startRecording(Track& t, pos_t at);
    void finishTake(Track& t);
    void beginPunch(Track& t, TrackState s);
    void endPunch(Track& t);
    void markDirty(Track& t, uint32_t frame);
    void runSwap(Track& t);
    pos_t stopTarget(Track& t, pos_t now);
    pos_t mapPos(Track& t, pos_t effPhase);
    int rateNum(int mode) const;
    void resyncOffset(Track& t, int newMode);
    void renderTrack(int ti, const float* inL, const float* inR, float* outL, float* outR);
    void renderGrains(Track& t, int ti, int n, const pos_t* centers, float* outL, float* outR);
    void renderCloudGrains(Track& t, int n, const pos_t* centers, float* outL, float* outR);
    inline void readFrame(const Track& t, pos_t p, float& l, float& r) const;
    void writeFrame(Track& t, uint32_t frame, float l, float r, bool mix, float fb);
    void click(bool accent);
    void renderClick(float* outL, float* outR);
    pos_t repeatLen(int knob) const;

    // queue
    Cmd queue_[64];
    volatile uint8_t qHead_ = 0, qTail_ = 0;

    // clock
    bool running_ = false;
    pos_t phase_ = 0;
    pos_t barLen_ = 0;
    float tempoPct_ = 100.0f;
    float loopBpm_ = 94.0f;      // tempo of the loops at 100 %
    pos_t speed_ = kOne;
    bool firstTake_ = true;
    int countInBeat_ = 0;

    // clicks
    int clickAge_ = -1;
    bool clickAccent_ = false;
    Rng rng_;
    float clickZ_[4] = {0, 0, 0, 0};

    // input
    float inLevel_ = 0;
    float* preRoll_[2] = {nullptr, nullptr};
    int preIdx_ = 0;

    // fx memory and clipboard
    int16_t* clip_ = nullptr;
    uint32_t clipCap_ = 0, clipLen_ = 0;
    uint8_t clipCh_ = 2;

    // trim undo
    uint32_t trimUndoStart_[kTracks], trimUndoLen_[kTracks];
    pos_t trimUndoAnchor_[kTracks];
    bool trimUndo_[kTracks];

    // per-block work buffers, in the pool (the on-chip RAM is full)
    float* inL_ = nullptr;
    float* inR_ = nullptr;
    float* mixL_ = nullptr;
    float* mixR_ = nullptr;
    float* sendL_[kFxSlots];
    float* sendR_[kFxSlots];
    float* tL_ = nullptr;
    float* tR_ = nullptr;
    float* fxL_[kFxSlots];
    float* fxR_[kFxSlots];
    pos_t* centers_ = nullptr;

    float retMeter_[kFxSlots] = {0, 0};
    float masterMeter_[2] = {0, 0};
    volatile uint32_t blocks_ = 0;
};

// lives in the SDRAM; constructEngine() builds it once the SDRAM is up
extern Engine& engine;
void constructEngine();

} // namespace lp

#endif

// tracker-mini-ce Looper engine. New code for this firmware (see lp_engine.h).

#include "lp_engine.h"

#include <new>
#include <string.h>

namespace lp
{

alignas(8) LP_NOINIT static uint8_t s_engineMem[sizeof(Engine)];
Engine& engine = *reinterpret_cast<Engine*>(s_engineMem);

void constructEngine() { new (s_engineMem) Engine(); }

static const char* const kModeNames[kModeCount] = {"Forward",   "Reverse",   "Ping-pong", "Half speed",
                                                   "Double",    "Half rev",  "Double rev", "Half time",
                                                   "Octave up", "Octave dn", "Granular"};
static const char* const kPerfNames[kPerfFx] = {"Repeat", "Repeat", "Low-pass", "High-pass",
                                                "Sweep dn", "Sweep up", "Vibrato", "Tape stop"};

const char* modeName(int mode) { return (mode >= 0 && mode < kModeCount) ? kModeNames[mode] : ""; }
const char* perfName(int fx) { return (fx >= 0 && fx < kPerfFx) ? kPerfNames[fx] : ""; }

enum Op : uint8_t
{
    kOpRec,
    kOpReplace,
    kOpPlay,
    kOpMute,
    kOpClear,
    kOpClearAll,
    kOpTransport,
    kOpPlayAll,
    kOpUndo,
    kOpRedo,
    kOpJump,
    kOpMode,
    kOpPerfHold,
    kOpPerfLatch,
    kOpPerfClear,
};

static inline pos_t floorDiv(pos_t a, pos_t b)
{
    pos_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

static inline pos_t wrapP(pos_t v, pos_t L)
{
    if (v >= 0 && v < L) return v;
    if (v >= L && v < 2 * L) return v - L;
    if (v < 0 && v >= -L) return v + L;
    pos_t m = v % L;
    return m < 0 ? m + L : m;
}

//------------------------------------------------------------------------------

void Engine::init(int16_t* pool, uint32_t poolBytes)
{
    uint8_t* p = (uint8_t*)pool;
    // work buffers and undo maps first
    auto take = [&](uint32_t bytes) {
        uint8_t* r = p;
        p += (bytes + 15) & ~15u;
        poolBytes -= (bytes + 15) & ~15u;
        memset(r, 0, bytes);
        return r;
    };
    const uint32_t blk = kBlock * sizeof(float);
    inL_ = (float*)take(blk);
    inR_ = (float*)take(blk);
    mixL_ = (float*)take(blk);
    mixR_ = (float*)take(blk);
    tL_ = (float*)take(blk);
    tR_ = (float*)take(blk);
    for (int s = 0; s < kFxSlots; s++)
    {
        sendL_[s] = (float*)take(blk);
        sendR_[s] = (float*)take(blk);
        fxL_[s] = (float*)take(blk);
        fxR_[s] = (float*)take(blk);
    }
    centers_ = (pos_t*)take(kBlock * sizeof(pos_t));
    preRoll_[0] = (float*)take(kPreRoll * sizeof(float));
    preRoll_[1] = (float*)take(kPreRoll * sizeof(float));
    for (int t = 0; t < kTracks; t++)
        track[t].dirty = take(kMaxUndoChunks / 8);

    uint32_t fxBytes = 1024u * 1024u;
    uint32_t clipBytes = poolBytes / 12;
    clipBytes &= ~3u;
    uint32_t trackBytes = (poolBytes - kFxSlots * fxBytes - clipBytes) / kTracks;
    trackBytes &= ~15u;

    for (int s = 0; s < kFxSlots; s++)
    {
        fx[s].init(p, fxBytes);
        p += fxBytes;
    }
    fx[0].setType(kFxDelay);
    fx[1].setType(kFxReverb);

    clip_ = (int16_t*)p;
    clipCap_ = clipBytes / 2;
    p += clipBytes;

    for (int t = 0; t < kTracks; t++)
    {
        Track& tr = track[t];
        tr.mem = (int16_t*)p;
        tr.memSamples = trackBytes / 2;
        p += trackBytes;
        tr.state = TrackState::kEmpty;
        tr.len = 0;
        static const uint8_t defKnob[kPerfFx] = {1, 3, 45, 45, 2, 2, 50, 1};
        memcpy(tr.perfKnob, defKnob, sizeof(defKnob));
        trimUndo_[t] = false;
    }
    setBpm(cfg.bpm);
}

void Engine::post(uint8_t op, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t next = (uint8_t)((qHead_ + 1) & 63);
    if (next == qTail_) return;   // full: drop
    queue_[qHead_] = Cmd{op, a, b, c};
    qHead_ = next;
}

void Engine::pressRec(int t) { post(kOpRec, (uint8_t)t); }
void Engine::pressReplace(int t) { post(kOpReplace, (uint8_t)t); }
void Engine::pressPlay(int t) { post(kOpPlay, (uint8_t)t); }
void Engine::pressMute(int t) { post(kOpMute, (uint8_t)t); }
void Engine::clear(int t) { post(kOpClear, (uint8_t)t); }
void Engine::clearAll() { post(kOpClearAll); }
void Engine::transport() { post(kOpTransport); }
void Engine::playAll() { post(kOpPlayAll); }
void Engine::undo(int t) { post(kOpUndo, (uint8_t)t); }
void Engine::redo(int t) { post(kOpRedo, (uint8_t)t); }
void Engine::jump(int t, int e) { post(kOpJump, (uint8_t)t, (uint8_t)e); }
void Engine::setMode(int t, int m) { post(kOpMode, (uint8_t)t, (uint8_t)m); }
void Engine::perfHold(int t, int f, bool on) { post(kOpPerfHold, (uint8_t)t, (uint8_t)f, on); }
void Engine::perfLatch(int t, int f) { post(kOpPerfLatch, (uint8_t)t, (uint8_t)f); }
void Engine::perfClear() { post(kOpPerfClear); }

bool Engine::hasLoops() const
{
    for (int t = 0; t < kTracks; t++)
        if (track[t].len > 0 && track[t].state != TrackState::kRecording) return true;
    return false;
}

void Engine::setBpm(float bpm)
{
    bpm = clampf(bpm, 40.0f, 240.0f);
    cfg.bpm = bpm;
    if (!hasLoops() && firstTake_)
    {
        // whole frames: loop lengths are whole bars, so nothing drifts against the clock
        float frames = cfg.beatsPerBar * 60.0f / bpm * kSampleRate;
        uint32_t f = (uint32_t)(frames + 0.5f);
        f -= f % cfg.beatsPerBar;     // whole frames per beat too
        barLen_ = (pos_t)f << kFrac;
        loopBpm_ = bpm;
        tempoPct_ = 100.0f;
        speed_ = kOne;
    }
    else
    {
        setTempoPct(bpm / loopBpm_ * 100.0f);
    }
}

void Engine::setTempoPct(float pct)
{
    tempoPct_ = clampf(pct, 50.0f, 200.0f);
    speed_ = (pos_t)(tempoPct_ * 0.01f * 65536.0f);
    cfg.bpm = loopBpm_ * tempoPct_ * 0.01f;
}

float Engine::currentBpm() const { return loopBpm_ * tempoPct_ * 0.01f; }

float Engine::playhead(int t) const { return (float)(track[t].pos >> kFrac); }
float Engine::playheadStep(int t) const { return (float)track[t].lastStep / (float)kOne; }

//------------------------------------------------------------------------------
// positions

int Engine::rateNum(int mode) const
{
    switch (mode)
    {
        case kModeForward: return 2;
        case kModeReverse: return -2;
        case kModePingPong: return 2;
        case kModeHalf: return 1;
        case kModeDouble: return 4;
        case kModeHalfRev: return -1;
        case kModeDoubleRev: return -4;
        case kModeHalfTime: return 1;
        case kModeOctUp: return 2;
        case kModeOctDown: return 2;
        default: return 2;
    }
}

static int granularRate(const GrainParams& g)
{
    int r = g.speed == 0 ? 1 : (g.speed == 1 ? 2 : 4);
    return g.direction == 1 ? -r : r;
}

pos_t Engine::mapPos(Track& t, pos_t effPhase)
{
    if (t.len == 0) return 0;
    pos_t L = (pos_t)t.len << kFrac;
    pos_t u = effPhase - t.anchor;
    int rn = (t.mode == kModeGranular) ? granularRate(t.grain) : rateNum(t.mode);
    bool ping = (t.mode == kModePingPong) || (t.mode == kModeGranular && t.grain.direction == 2);
    pos_t v = t.off + (u * rn) / 2;
    if (ping)
    {
        pos_t m = wrapP(v, 2 * L);
        t.pingRev = m >= L;
        return m < L ? m : (2 * L - 1 - m);
    }
    return wrapP(v, L);
}

// choose a new offset so the new mode carries on from the current position
void Engine::resyncOffset(Track& t, int newMode)
{
    if (t.len == 0)
    {
        t.mode = (uint8_t)newMode;
        t.off = 0;
        return;
    }
    pos_t cur = mapPos(t, phase_);
    t.mode = (uint8_t)newMode;
    t.off = 0;
    pos_t base = mapPos(t, phase_);
    pos_t L = (pos_t)t.len << kFrac;
    bool ping = (t.mode == kModePingPong) || (t.mode == kModeGranular && t.grain.direction == 2);
    t.off = wrapP(cur - base, ping ? 2 * L : L);
}

inline void Engine::readFrame(const Track& t, pos_t p, float& l, float& r) const
{
    uint32_t i0 = (uint32_t)(p >> kFrac);
    if (i0 >= t.len) i0 %= t.len;
    float fr = (float)(p & (kOne - 1)) * (1.0f / 65536.0f);
    uint32_t i1 = i0 + 1;
    if (i1 >= t.len) i1 = 0;
    uint32_t a = t.start + i0, b = t.start + i1;
    if (t.ch == 2)
    {
        float l0 = t.mem[a * 2], r0 = t.mem[a * 2 + 1];
        float l1 = t.mem[b * 2], r1 = t.mem[b * 2 + 1];
        l = l0 + (l1 - l0) * fr;
        r = r0 + (r1 - r0) * fr;
    }
    else
    {
        float m0 = t.mem[a], m1 = t.mem[b];
        l = r = m0 + (m1 - m0) * fr;
    }
}

void Engine::writeFrame(Track& t, uint32_t frame, float l, float r, bool mix, float fb)
{
    uint32_t i = t.start + frame;
    if (t.ch == 2)
    {
        if (mix)
        {
            l += t.mem[i * 2] * fb;
            r += t.mem[i * 2 + 1] * fb;
        }
        t.mem[i * 2] = sat16(l);
        t.mem[i * 2 + 1] = sat16(r);
    }
    else
    {
        float m = (t.channels == kChMonoR) ? r : ((t.channels == kChMonoSum) ? (l + r) * 0.5f : l);
        if (mix) m += t.mem[i] * fb;
        t.mem[i] = sat16(m);
    }
}

//------------------------------------------------------------------------------
// recording

pos_t Engine::stopTarget(Track& t, pos_t now)
{
    if (firstTake_ && t.stopOpt == kStopFree && t.fixedBars == 0) return now;
    pos_t unit = barLen_;
    if (t.stopOpt == kStopBeat) unit = beatLen();
    if (t.stopOpt == kStopFree && !firstTake_) unit = beatLen();
    pos_t rel = now - t.anchor;
    pos_t n = floorDiv(rel, unit);
    pos_t into = rel - n * unit;
    pos_t e;
    if (t.stopOpt == kStopFree && !firstTake_)
        e = (into * 2 >= unit) ? (n + 1) * unit : n * unit;   // nearest beat
    else
        e = (into < unit / 8) ? n * unit : (n + 1) * unit;    // late by less than an eighth: the line just passed
    if (e <= t.recStartPhase - t.anchor) e += unit;
    if (e < unit) e = unit;
    return t.anchor + e;
}

void Engine::startRecording(Track& t, pos_t at)
{
    // channels are chosen while the track is empty
    t.ch = (t.channels == kChStereo) ? 2 : 1;
    t.start = 0;
    t.mode = kModeForward;
    t.off = 0;
    t.recStartPhase = at;
    if (firstTake_)
        t.anchor = 0;
    else
        t.anchor = floorDiv(at, barLen_) * barLen_;
    t.recEnd = -1;
    if (t.fixedBars > 0) t.recEnd = t.anchor + barLen_ * t.fixedBars;
    t.tail = -1;
    uint32_t head = (uint32_t)((at - t.anchor) >> kFrac);
    // silence before a mid-bar start (the head is short: at most a bar)
    if (head > 0)
    {
        uint32_t n = head;
        if (n > t.capFrames()) n = t.capFrames();
        memset(t.mem, 0, n * t.ch * 2);
    }
    t.lastWrite = (int64_t)head - 1;
    t.fadeIn = (head > 0) ? 0.0f : 1.0f;
    t.recFramesTotal = 0;
    t.len = 0;
    t.undoAvail = false;
    t.undone = false;
    t.swapCursor = -1;
    t.state = TrackState::kRecording;
    t.version++;
}

void Engine::finishTake(Track& t)
{
    // blend the tail written past the end into the start: no click at the loop point
    uint32_t n = kLoopXfade;
    if ((int64_t)t.len + n > t.lastWrite + 1) n = (uint32_t)(t.lastWrite + 1 - t.len);
    if (n > t.len) n = t.len;
    for (uint32_t k = 0; k < n; k++)
    {
        float g = (float)k / (float)kLoopXfade;
        uint32_t h = t.start + k, e = t.start + t.len + k;
        if (t.ch == 2)
        {
            t.mem[h * 2] = sat16(t.mem[h * 2] * g + t.mem[e * 2] * (1.0f - g));
            t.mem[h * 2 + 1] = sat16(t.mem[h * 2 + 1] * g + t.mem[e * 2 + 1] * (1.0f - g));
        }
        else
            t.mem[h] = sat16(t.mem[h] * g + t.mem[e] * (1.0f - g));
    }
    if (firstTake_)
    {
        firstTake_ = false;
        if (t.stopOpt == kStopFree && t.fixedBars == 0)
        {
            // a free first take sets the bar to its own length
            barLen_ = (pos_t)t.len << kFrac;
            loopBpm_ = cfg.beatsPerBar * 60.0f * kSampleRate / (float)t.len;
            tempoPct_ = 100.0f;
            speed_ = kOne;
            cfg.bpm = loopBpm_;
        }
        else
            loopBpm_ = cfg.bpm;
    }
    t.version++;
    t.tail = -1;
    t.recEnd = -1;
    switch (t.afterRec)
    {
        case kAfterOverdub: beginPunch(t, TrackState::kOverdub); break;
        case kAfterStop: t.state = TrackState::kStopped; break;
        default: t.state = TrackState::kPlaying; break;
    }
}

void Engine::beginPunch(Track& t, TrackState s)
{
    uint32_t half = t.capFrames() / 2;
    if (t.start + t.len <= half)
    {
        uint32_t chunks = (t.len + kUndoChunk - 1) / kUndoChunk;
        if (chunks <= kMaxUndoChunks)
        {
            memset(t.dirty, 0, (chunks + 7) / 8);
            t.undoAvail = true;
            t.undone = false;
        }
        else
            t.undoAvail = false;
    }
    else
        t.undoAvail = false;
    t.punchGain = 0.0f;
    t.state = s;
}

void Engine::endPunch(Track& t)
{
    t.state = TrackState::kPlaying;
}

void Engine::markDirty(Track& t, uint32_t frame)
{
    if (!t.undoAvail) return;
    uint32_t c = frame / kUndoChunk;
    uint8_t bit = (uint8_t)(1u << (c & 7));
    if (t.dirty[c >> 3] & bit) return;
    t.dirty[c >> 3] |= bit;
    uint32_t base = t.capFrames() / 2;
    uint32_t f0 = c * kUndoChunk;
    uint32_t n = kUndoChunk;
    if (f0 + n > t.len) n = t.len - f0;
    memcpy(t.mem + (size_t)(base + f0) * t.ch, t.mem + (size_t)(t.start + f0) * t.ch, (size_t)n * t.ch * 2);
}

// swap dirty chunks between the loop and its undo copy, a few per block
void Engine::runSwap(Track& t)
{
    if (t.swapCursor < 0) return;
    uint32_t chunks = (t.len + kUndoChunk - 1) / kUndoChunk;
    uint32_t base = t.capFrames() / 2;
    int done = 0;
    while ((uint32_t)t.swapCursor < chunks && done < 6)
    {
        uint32_t c = (uint32_t)t.swapCursor++;
        if (!(t.dirty[c >> 3] & (1u << (c & 7)))) continue;
        uint32_t f0 = c * kUndoChunk;
        uint32_t n = kUndoChunk;
        if (f0 + n > t.len) n = t.len - f0;
        int16_t* a = t.mem + (size_t)(t.start + f0) * t.ch;
        int16_t* b = t.mem + (size_t)(base + f0) * t.ch;
        uint32_t s = n * t.ch;
        for (uint32_t i = 0; i < s; i++)
        {
            int16_t x = a[i];
            a[i] = b[i];
            b[i] = x;
        }
        done++;
    }
    if ((uint32_t)t.swapCursor >= chunks)
    {
        t.swapCursor = -1;
        t.version++;
    }
}

//------------------------------------------------------------------------------
// commands

void Engine::exec(const Cmd& c)
{
    if (c.op == kOpClearAll || c.op == kOpTransport || c.op == kOpPlayAll || c.op == kOpPerfClear)
    {
        switch (c.op)
        {
            case kOpClearAll:
                for (int i = 0; i < kTracks; i++)
                {
                    Track& t = track[i];
                    t.state = TrackState::kEmpty;
                    t.len = 0;
                    t.undoAvail = false;
                    t.perfHeld = t.perfLatched = 0;
                    t.version++;
                }
                running_ = false;
                firstTake_ = true;
                phase_ = 0;
                setBpm(cfg.bpm);
                tempoPct_ = 100.0f;
                speed_ = kOne;
                break;
            case kOpTransport:
                if (running_)
                {
                    running_ = false;
                    for (int i = 0; i < kTracks; i++)
                    {
                        Track& t = track[i];
                        if (t.state == TrackState::kRecording)
                        {
                            t.recEnd = stopTarget(t, phase_);
                            if (t.recEnd > phase_) t.recEnd = phase_;
                            t.len = (uint32_t)((t.recEnd - t.anchor) >> kFrac);
                            if (firstTake_) t.len -= t.len % cfg.beatsPerBar;
                            if (t.len > 0)
                                finishTake(t);
                            else
                                t.state = TrackState::kEmpty;
                        }
                        else if (t.state == TrackState::kOverdub || t.state == TrackState::kReplace)
                            endPunch(t);
                        else if (t.state == TrackState::kWaiting || t.state == TrackState::kArmed)
                            t.state = t.len ? TrackState::kPlaying : TrackState::kEmpty;
                    }
                }
                else
                {
                    phase_ = 0;
                    running_ = true;
                }
                break;
            case kOpPlayAll:
                for (int i = 0; i < kTracks; i++)
                    if (track[i].len && track[i].state == TrackState::kStopped) track[i].state = TrackState::kPlaying;
                if (!running_)
                {
                    phase_ = 0;
                    running_ = true;
                }
                break;
            case kOpPerfClear:
                for (int i = 0; i < kTracks; i++)
                    track[i].perfHeld = track[i].perfLatched = 0;
                break;
        }
        return;
    }

    if (c.a >= kTracks) return;
    Track& t = track[c.a];
    switch (c.op)
    {
        case kOpRec:
        case kOpReplace:
        {
            bool replace = c.op == kOpReplace;
            switch (t.state)
            {
                case TrackState::kEmpty:
                {
                    if (replace) break;
                    if (!hasLoops() && firstTake_ && !anyRecording())
                    {
                        // the first take starts the clock
                        if (t.startOpt == kStartSound)
                        {
                            t.state = TrackState::kArmed;
                            running_ = false;
                        }
                        else if (t.startOpt == kStartCountIn && cfg.countInBars > 0)
                        {
                            phase_ = -(pos_t)cfg.countInBars * barLen_;
                            running_ = true;
                            t.recStartPhase = 0;
                            t.state = TrackState::kWaiting;
                        }
                        else
                        {
                            phase_ = 0;
                            running_ = true;
                            startRecording(t, 0);
                        }
                    }
                    else
                    {
                        if (!running_)
                        {
                            // start everything from bar 1, with the count-in if there is one
                            int cb = (t.startOpt == kStartCountIn) ? cfg.countInBars : 0;
                            phase_ = -(pos_t)cb * barLen_;
                            running_ = true;
                            if (t.startOpt == kStartSound)
                                t.state = TrackState::kArmed;
                            else if (cb > 0)
                            {
                                t.recStartPhase = 0;
                                t.state = TrackState::kWaiting;
                            }
                            else
                                startRecording(t, 0);
                        }
                        else if (t.startOpt == kStartSound)
                            t.state = TrackState::kArmed;
                        else if (t.startOpt == kStartCountIn)
                        {
                            // next bar line at least half a bar away
                            pos_t target = (floorDiv(phase_ + barLen_ / 2, barLen_) + 1) * barLen_;
                            t.recStartPhase = target;
                            t.state = TrackState::kWaiting;
                        }
                        else
                            startRecording(t, phase_);
                    }
                    break;
                }
                case TrackState::kArmed:
                case TrackState::kWaiting:
                    // press again: cancel
                    t.state = TrackState::kEmpty;
                    if (!hasLoops() && !anyRecording()) running_ = false;
                    break;
                case TrackState::kRecording:
                    if (t.recEnd < 0) t.recEnd = stopTarget(t, phase_);
                    break;
                case TrackState::kPlaying:
                case TrackState::kStopped:
                    if (t.mode <= kModePingPong && !(t.tempoOpt == kTempoStretch && speed_ != kOne))
                    {
                        if (t.state == TrackState::kStopped) t.state = TrackState::kPlaying;
                        beginPunch(t, replace ? TrackState::kReplace : TrackState::kOverdub);
                    }
                    break;
                case TrackState::kOverdub:
                    if (replace)
                        t.state = TrackState::kReplace;
                    else
                        endPunch(t);
                    break;
                case TrackState::kReplace:
                    if (replace)
                        endPunch(t);
                    else
                        t.state = TrackState::kOverdub;
                    break;
            }
            break;
        }
        case kOpPlay:
            switch (t.state)
            {
                case TrackState::kPlaying:
                case TrackState::kOverdub:
                case TrackState::kReplace:
                    if (t.fadeStep > 0)
                    {
                        // second press while fading: stop at once
                        t.fadeStep = 0;
                        t.fadeGain = 1;
                        t.state = TrackState::kStopped;
                    }
                    else if (cfg.stopFade > 0)
                    {
                        float beats = cfg.stopFade == 1 ? 1.0f : (cfg.stopFade == 2 ? cfg.beatsPerBar : 2.0f * cfg.beatsPerBar);
                        float frames = beats * (float)(beatLen() >> kFrac) * 65536.0f / (float)speed_;
                        t.fadeStep = 1.0f / (frames > 1 ? frames : 1);
                        if (t.state != TrackState::kPlaying) endPunch(t);
                    }
                    else
                        t.state = TrackState::kStopped;
                    break;
                case TrackState::kStopped:
                    t.state = TrackState::kPlaying;
                    t.xfLeft = 0;
                    t.fadeGain = 1;
                    t.fadeStep = 0;
                    if (!running_)
                    {
                        phase_ = 0;
                        running_ = true;
                    }
                    break;
                case TrackState::kRecording:
                    if (t.recEnd < 0) t.recEnd = stopTarget(t, phase_);
                    break;
                default: break;
            }
            break;
        case kOpMute:
            t.muted = !t.muted;
            break;
        case kOpClear:
            t.state = TrackState::kEmpty;
            t.len = 0;
            t.undoAvail = false;
            t.perfHeld = t.perfLatched = 0;
            t.version++;
            trimUndo_[c.a] = false;
            if (!hasLoops() && !anyRecording())
            {
                firstTake_ = true;
                running_ = false;
                tempoPct_ = 100.0f;
                speed_ = kOne;
                setBpm(cfg.bpm);
            }
            break;
        case kOpUndo:
            if (t.undoAvail && !t.undone && t.swapCursor < 0 && t.state != TrackState::kOverdub &&
                t.state != TrackState::kReplace)
            {
                t.undone = true;
                t.swapCursor = 0;
            }
            break;
        case kOpRedo:
            if (t.undoAvail && t.undone && t.swapCursor < 0)
            {
                t.undone = false;
                t.swapCursor = 0;
            }
            break;
        case kOpJump:
            if (t.len)
            {
                pos_t L = (pos_t)t.len << kFrac;
                pos_t cur = mapPos(t, phase_);
                pos_t e = L / 8;
                pos_t into = cur % e;
                pos_t target = e * (c.b & 7) + into;
                t.xfPos = t.pos;
                t.xfLeft = kJumpXfade;
                bool ping = (t.mode == kModePingPong) || (t.mode == kModeGranular && t.grain.direction == 2);
                t.off = wrapP(t.off + (t.pingRev ? cur - target : target - cur), ping ? 2 * L : L);
            }
            break;
        case kOpMode:
            if (c.b < kModeCount && c.b != t.mode)
            {
                if (t.state == TrackState::kOverdub || t.state == TrackState::kReplace) endPunch(t);
                t.xfPos = t.pos;
                t.xfLeft = kJumpXfade;
                resyncOffset(t, c.b);
            }
            break;
        case kOpPerfHold:
            if (c.c)
                t.perfHeld |= (uint8_t)(1u << c.b);
            else
                t.perfHeld &= (uint8_t) ~(1u << c.b);
            if (c.c && (c.b == kPfRepeat1 || c.b == kPfRepeat2)) t.perfOrder = c.b;
            break;
        case kOpPerfLatch:
            t.perfLatched ^= (uint8_t)(1u << c.b);
            if ((t.perfLatched >> c.b) & 1 && (c.b == kPfRepeat1 || c.b == kPfRepeat2)) t.perfOrder = c.b;
            break;
    }
}

bool Engine::anyRecording() const
{
    for (int i = 0; i < kTracks; i++)
    {
        TrackState s = track[i].state;
        if (s == TrackState::kRecording || s == TrackState::kOverdub || s == TrackState::kReplace) return true;
    }
    return false;
}

bool Engine::anyArmedOrWaiting() const
{
    for (int i = 0; i < kTracks; i++)
    {
        TrackState s = track[i].state;
        if (s == TrackState::kArmed || s == TrackState::kWaiting) return true;
    }
    return false;
}

pos_t Engine::repeatLen(int knob) const
{
    // 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 of a whole note; a beat is a quarter
    static const int num[6] = {1, 1, 1, 1, 1, 1};
    static const int den[6] = {1, 2, 3, 4, 6, 8};
    knob = clampi(knob, 0, 5);
    return beatLen() * num[knob] / den[knob];
}

const char* repeatName(int knob)
{
    static const char* const n[6] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
    return n[clampi(knob, 0, 5)];
}

//------------------------------------------------------------------------------
// audio

void Engine::click(bool accent)
{
    clickAge_ = 0;
    clickAccent_ = accent;
}

void Engine::renderClick(float* outL, float* outR)
{
    if (clickAge_ < 0) return;
    float lvl = cfg.clickLevel * 0.01f * 9000.0f;
    bool acc = clickAccent_ && cfg.accent;
    if (acc) lvl *= 1.4f;
    for (int i = 0; i < kBlock; i++)
    {
        float tt = clickAge_ / kSampleRate;
        float v = 0;
        switch (cfg.clickSound)
        {
            case kClickBeep:
            {
                float f = acc ? 1500.0f : 1000.0f;
                v = sinf(kTwoPi * f * tt) * expf(-tt * 120.0f);
                break;
            }
            case kClickWood:
            {
                float f = acc ? 2400.0f : 1800.0f;
                v = sinf(kTwoPi * f * tt) * expf(-tt * 260.0f) + rng_.bi() * 0.3f * expf(-tt * 900.0f);
                break;
            }
            case kClickHat:
            {
                float nz = rng_.bi();
                clickZ_[0] += 0.6f * (nz - clickZ_[0]);
                v = (nz - clickZ_[0]) * 1.6f * expf(-tt * 90.0f);
                break;
            }
            case kClickBell:
            {
                float a = fmodf(562.0f * tt, 1.0f) < 0.5f ? 1.0f : -1.0f;
                float b = fmodf(845.0f * tt, 1.0f) < 0.5f ? 1.0f : -1.0f;
                float x = (a + b) * 0.5f;
                clickZ_[1] += 0.25f * (x - clickZ_[1]);
                v = clickZ_[1] * expf(-tt * 30.0f) * 1.3f;
                break;
            }
        }
        outL[i] += v * lvl;
        outR[i] += v * lvl;
        if (++clickAge_ > (int)(kSampleRate * 0.15f))
        {
            clickAge_ = -1;
            break;
        }
    }
}

// one track's block: positions, perf effects, grains, recording, filters
void Engine::renderTrack(int ti, const float* inL, const float* inR, float* outL, float* outR)
{
    Track& t = track[ti];
    for (int i = 0; i < kBlock; i++)
        outL[i] = outR[i] = 0;
    if (t.busy) return;

    runSwap(t);

    const bool clockOn = running_;
    const TrackState st = t.state;

    // ---- recording a take ----
    if (st == TrackState::kRecording)
    {
        bool wrote = false;
        for (int i = 0; i < kBlock; i++)
        {
            pos_t ph = phase_ + speed_ * i;
            int64_t idx = (int64_t)((ph - t.anchor) >> kFrac) - cfg.latency;
            if (t.recEnd >= 0 && ph >= t.recEnd && t.tail < 0)
            {
                t.len = (uint32_t)((t.recEnd - t.anchor) >> kFrac);
                if (firstTake_ && t.stopOpt == kStopFree && t.fixedBars == 0)
                    t.len -= t.len % cfg.beatsPerBar;   // the bar it sets has whole frames per beat
                t.tail = 0;
            }
            if (idx < 0) continue;
            if ((uint64_t)idx + 1 >= t.capFrames())
            {
                // memory full: end here
                if (t.tail < 0)
                {
                    t.len = (uint32_t)(idx > kLoopXfade ? idx - kLoopXfade : idx);
                    t.tail = 0;
                }
                break;
            }
            float l = inL[i], r = inR[i];
            if (t.fadeIn < 1.0f)
            {
                t.fadeIn += 1.0f / kPunchRamp;
                if (t.fadeIn > 1.0f) t.fadeIn = 1.0f;
                l *= t.fadeIn;
                r *= t.fadeIn;
            }
            // fill skipped frames (tempo above 100 %) by interpolation
            int64_t from = t.lastWrite + 1;
            if (idx >= from)
            {
                int64_t gap = idx - from;
                for (int64_t k = from; k <= idx; k++)
                {
                    float g = gap > 0 ? (float)(k - from + 1) / (float)(gap + 1) : 1.0f;
                    writeFrame(t, (uint32_t)k, lerpf(t.lastInL, l, g), lerpf(t.lastInR, r, g), false, 0);
                }
                t.lastWrite = idx;
                wrote = true;
            }
            t.lastInL = l;
            t.lastInR = r;
            if (t.tail >= 0 && t.lastWrite + 1 >= (int64_t)t.len + kLoopXfade)
            {
                finishTake(t);
                break;
            }
        }
        if (wrote)
        {
            t.recFramesTotal = (uint32_t)(t.lastWrite + 1);
            t.version++;
        }
        t.pos = t.lastWrite > 0 ? ((pos_t)t.lastWrite << kFrac) : 0;
        t.lastStep = speed_;
        if (t.state == TrackState::kRecording) return;
    }

    if (t.len == 0 || !clockOn || t.state == TrackState::kEmpty || t.state == TrackState::kArmed ||
        (t.state == TrackState::kWaiting && t.len == 0))
        return;
    if (t.state == TrackState::kStopped) return;

    // ---- performance effects: the effective phase ----
    uint8_t act = t.perfHeld | t.perfLatched;
    bool repWant = (act & 3) != 0;
    if (repWant)
    {
        int which = ((act >> t.perfOrder) & 1) ? t.perfOrder : ((act & 1) ? 0 : 1);
        pos_t R = repeatLen(t.perfKnob[which]);
        if (!t.repOn || R != t.repLen)
        {
            pos_t ph = phase_ < 0 ? 0 : phase_;
            t.repStart = floorDiv(ph, R) * R;
            t.repLen = R;
            if (!t.repOn)
            {
                t.xfPos = t.pos;
                t.xfLeft = kJumpXfade;
            }
            t.repOn = true;
        }
    }
    else if (t.repOn)
    {
        t.repOn = false;
        t.xfPos = t.pos;
        t.xfLeft = kJumpXfade;
    }
    bool tapeWant = (act >> kPfTapeStop) & 1;
    if (tapeWant && !t.tapeOn)
    {
        t.tapeOn = true;
        t.tapePhase = phase_;
        t.tapeRate = 1.0f;
        t.tapeT = 0;
    }
    else if (!tapeWant && t.tapeOn)
    {
        t.tapeOn = false;
        t.xfPos = t.pos;
        t.xfLeft = kJumpXfade;
    }
    float tapeTime = 0;
    if (t.tapeOn)
    {
        static const float beats[4] = {1, 2, 4, 8};
        tapeTime = beats[clampi(t.perfKnob[kPfTapeStop], 0, 3)] * (float)(beatLen() >> kFrac) / (tempoPct_ * 0.01f);
    }
    bool vib = (act >> kPfVibrato) & 1;
    float vibAmp = t.perfKnob[kPfVibrato] * 0.01f * 150.0f;    // frames, up to about 2 semitones

    // ---- grains? ----
    bool grainMode = t.mode >= kModeHalfTime;
    bool stretch = t.tempoOpt == kTempoStretch && speed_ != kOne && t.mode < kModeHalfTime;
    bool useGrains = grainMode || stretch;

    // ---- punch (overdub / replace) ----
    bool punching = (t.state == TrackState::kOverdub || t.state == TrackState::kReplace) && !useGrains &&
                    !t.repOn && !t.tapeOn && !vib && t.swapCursor < 0;
    float fb = (t.state == TrackState::kReplace) ? 0.0f : cfg.feedback * 0.01f;

    pos_t* centers = centers_;
    bool wrote = false;
    pos_t L = (pos_t)t.len << kFrac;

    for (int i = 0; i < kBlock; i++)
    {
        pos_t ph = phase_ + speed_ * i;
        pos_t eff = ph;
        if (t.repOn)
        {
            pos_t d = ph - t.repStart;
            eff = t.repStart + wrapP(d, t.repLen);
        }
        if (t.tapeOn)
        {
            t.tapeT += 1.0f;
            float k = 1.0f - t.tapeT / (tapeTime > 1 ? tapeTime : 1);
            t.tapeRate = k > 0 ? k * k : 0.0f;
            t.tapePhase += (pos_t)((float)speed_ * t.tapeRate);
            eff = t.tapePhase;
        }
        pos_t p = mapPos(t, eff);
        if (vib)
        {
            t.vibPh += 5.5f / kSampleRate;
            if (t.vibPh >= 1) t.vibPh -= 1;
            p = wrapP(p + (pos_t)(sin01(t.vibPh) * vibAmp * 65536.0f), L);
        }
        t.lastStep = p - t.pos;
        t.pos = p;
        centers[i] = p;

        if (!useGrains)
        {
            float l, r;
            readFrame(t, p, l, r);
            if (t.xfLeft > 0)
            {
                // continue the old position and blend it out
                t.xfPos = wrapP(t.xfPos + (pos_t)speed_ * (rateNum(t.mode) > 0 ? 1 : -1), L);
                float ol, orr;
                readFrame(t, t.xfPos, ol, orr);
                float g = (float)t.xfLeft / kJumpXfade;
                l = l * (1.0f - g) + ol * g;
                r = r * (1.0f - g) + orr * g;
                t.xfLeft--;
            }
            if (t.tapeOn)
            {
                float g = t.tapeRate < 0.15f ? t.tapeRate / 0.15f : 1.0f;
                l *= g;
                r *= g;
            }
            outL[i] = l;
            outR[i] = r;
        }

        if (punching)
        {
            int dir = (t.mode == kModeReverse || (t.mode == kModePingPong && t.pingRev)) ? -1 : 1;
            int64_t idx = (int64_t)(p >> kFrac) - (int64_t)cfg.latency * dir;
            while (idx < 0) idx += t.len;
            while (idx >= (int64_t)t.len) idx -= t.len;
            if (t.punchGain < 1.0f) t.punchGain += 1.0f / kPunchRamp;
            float pg = t.punchGain > 1.0f ? 1.0f : t.punchGain;
            markDirty(t, (uint32_t)idx);
            float keep = (t.state == TrackState::kReplace) ? (1.0f - pg) : (fb * pg + (1.0f - pg));
            writeFrame(t, (uint32_t)idx, inL[i] * pg, inR[i] * pg, true, keep);
            wrote = true;
        }
    }
    if (wrote) t.version++;

    if (useGrains) renderGrains(t, ti, kBlock, centers, outL, outR);

    // ---- filters: perf low/high-pass and sweeps, then the DJ filter ----
    bool lpOn = ((act >> kPfLowPass) & 1) || ((act >> kPfSweepDown) & 1);
    bool hpOn = ((act >> kPfHighPass) & 1) || ((act >> kPfSweepUp) & 1);
    float blockSec = kBlock / kSampleRate;
    static const float sweepBars[5] = {0.25f, 0.5f, 1, 2, 4};
    float barSec = (float)(barLen_ >> kFrac) / kSampleRate / (tempoPct_ * 0.01f);
    if ((act >> kPfSweepDown) & 1)
        t.sweepT[0] += blockSec;
    else
        t.sweepT[0] = 0;
    if ((act >> kPfSweepUp) & 1)
        t.sweepT[1] += blockSec;
    else
        t.sweepT[1] = 0;
    if (lpOn)
    {
        float hz = 20000.0f;
        if ((act >> kPfLowPass) & 1) hz = 80.0f * powf(100.0f, t.perfKnob[kPfLowPass] * 0.01f);
        if ((act >> kPfSweepDown) & 1)
        {
            float T = sweepBars[clampi(t.perfKnob[kPfSweepDown], 0, 4)] * barSec;
            float k = clampf(t.sweepT[0] / T, 0, 1);
            float s = 18000.0f * powf(200.0f / 18000.0f, k);
            if (s < hz) hz = s;
        }
        for (int c = 0; c < 2; c++)
        {
            t.filt[0][c].set(hz, 2.0f);
            float* o = c ? outR : outL;
            for (int i = 0; i < kBlock; i++)
                o[i] = t.filt[0][c].lp(o[i]);
        }
    }
    else
        t.filt[0][0].reset(), t.filt[0][1].reset();
    if (hpOn)
    {
        float hz = 20.0f;
        if ((act >> kPfHighPass) & 1) hz = 100.0f * powf(80.0f, t.perfKnob[kPfHighPass] * 0.01f);
        if ((act >> kPfSweepUp) & 1)
        {
            float T = sweepBars[clampi(t.perfKnob[kPfSweepUp], 0, 4)] * barSec;
            float k = clampf(t.sweepT[1] / T, 0, 1);
            float s = 20.0f * powf(4000.0f / 20.0f, k);
            if (s > hz) hz = s;
        }
        for (int c = 0; c < 2; c++)
        {
            t.filt[1][c].set(hz, 2.0f);
            float* o = c ? outR : outL;
            for (int i = 0; i < kBlock; i++)
                o[i] = t.filt[1][c].hp(o[i]);
        }
    }
    else
        t.filt[1][0].reset(), t.filt[1][1].reset();

    if (t.djFilter != 0)
    {
        float a = (t.djFilter < 0 ? -t.djFilter : t.djFilter) / 50.0f;
        for (int c = 0; c < 2; c++)
        {
            float* o = c ? outR : outL;
            if (t.djFilter < 0)
            {
                t.dj[c].set(20000.0f * powf(100.0f / 20000.0f, a), 0.9f);
                for (int i = 0; i < kBlock; i++)
                    o[i] = t.dj[c].lp(o[i]);
            }
            else
            {
                t.dj[c].set(20.0f * powf(8000.0f / 20.0f, a), 0.9f);
                for (int i = 0; i < kBlock; i++)
                    o[i] = t.dj[c].hp(o[i]);
            }
        }
    }

    // ---- stop fade ----
    if (t.fadeStep > 0)
    {
        for (int i = 0; i < kBlock; i++)
        {
            t.fadeGain -= t.fadeStep;
            if (t.fadeGain < 0) t.fadeGain = 0;
            outL[i] *= t.fadeGain;
            outR[i] *= t.fadeGain;
        }
        if (t.fadeGain <= 0)
        {
            t.fadeStep = 0;
            t.fadeGain = 1;
            t.state = TrackState::kStopped;
        }
    }
}

void Engine::renderGrains(Track& t, int ti, int n, const pos_t* centers, float* outL, float* outR)
{
    (void)ti;
    pos_t L = (pos_t)t.len << kFrac;
    bool tape = t.tempoOpt == kTempoTape;
    float sp = (float)speed_ / (float)kOne;
    float pitch;
    int maxG;
    int glen;
    float interval;
    bool rnd = false;
    float spreadFrames = 0;
    float panAmt = 0;
    switch (t.mode)
    {
        case kModeHalfTime: pitch = 1.0f; break;
        case kModeOctUp: pitch = 2.0f; break;
        case kModeOctDown: pitch = 0.5f; break;
        case kModeGranular: pitch = powf(2.0f, t.grain.pitch / 12.0f); break;
        default:
        {
            int rn = rateNum(t.mode);
            pitch = (rn < 0 ? -rn : rn) * 0.5f;
            break;
        }
    }
    if (tape) pitch *= sp;
    if (t.mode == kModeGranular)
    {
        float ms = 20.0f * powf(25.0f, t.grain.size * 0.01f);
        glen = (int)(ms * kSampleRate / 1000.0f);
        if (glen >= (int)t.len) glen = (int)t.len - 1;
        maxG = kMaxGrains;
        // density: a grain every four lengths ... eight overlapping
        float d = t.grain.density * 0.01f;
        float overlap = 0.25f * powf(32.0f, d);
        interval = glen / overlap;
        rnd = true;
        float s = t.grain.spread * 0.01f;
        float maxSpread = kSampleRate < (float)t.len / 4 ? kSampleRate : (float)t.len / 4;
        spreadFrames = s * s * maxSpread;
        panAmt = s * 0.7f;
    }
    else
    {
        glen = 2029;     // 46 ms
        if (glen * 2 > (int)t.len) glen = (int)t.len / 2;
        maxG = 2;
        interval = glen * 0.5f;
    }
    if (glen < 16) return;
    int dirSign = 1;
    {
        int rn = (t.mode == kModeGranular) ? granularRate(t.grain) : rateNum(t.mode);
        bool ping = t.mode == kModePingPong || (t.mode == kModeGranular && t.grain.direction == 2);
        if (rn < 0 || (ping && t.pingRev)) dirSign = -1;
    }
    pos_t step = (pos_t)(pitch * 65536.0f) * dirSign;

    for (int i = 0; i < n; i++)
    {
        if (--t.spawnIn <= 0)
        {
            t.spawnIn = (int)(rnd ? interval * (0.4f + 1.2f * rng_.uni()) : interval);
            if (t.spawnIn < 1) t.spawnIn = 1;
            Grain* g = nullptr;
            for (int k = 0; k < maxG; k++)
            {
                int idx = (t.nextGrain + k) % maxG;
                if (!t.grains[idx].on)
                {
                    g = &t.grains[idx];
                    t.nextGrain = (idx + 1) % maxG;
                    break;
                }
            }
            if (g)
            {
                g->on = true;
                g->age = 0;
                g->len = glen;
                g->step = step;
                pos_t start = centers[i];
                if (spreadFrames > 0) start += (pos_t)(rng_.bi() * spreadFrames * 65536.0f);
                g->pos = wrapP(start, L);
                float pan = rng_.bi() * panAmt;
                g->gainL = 1.0f - (pan > 0 ? pan : 0);
                g->gainR = 1.0f + (pan < 0 ? pan : 0);
                g->amp = 1.0f;
            }
        }
        float l = 0, r = 0;
        for (int k = 0; k < maxG; k++)
        {
            Grain& g = t.grains[k];
            if (!g.on) continue;
            float w = hann((float)g.age / g.len);
            float a, b;
            readFrame(t, g.pos, a, b);
            l += a * w * g.gainL;
            r += b * w * g.gainR;
            g.pos = wrapP(g.pos + g.step, L);
            if (++g.age >= g.len) g.on = false;
        }
        if (t.mode == kModeGranular)
        {
            // keep the level about even across densities
            float d = t.grain.density * 0.01f;
            float norm = 1.0f / sqrtf(clampf(0.25f * powf(32.0f, d) * 0.5f, 1.0f, 8.0f));
            l *= norm;
            r *= norm;
        }
        outL[i] = l;
        outR[i] = r;
    }
    for (int k = 0; k < kMaxGrains; k++)
    {
        const Grain& g = t.grains[k];
        t.drawGrainPos[k] = g.on ? (float)(g.pos >> kFrac) : -1.0f;
        t.drawGrainAmp[k] = g.on ? hann((float)g.age / g.len) : 0.0f;
    }
}

void Engine::process(const int16_t* inL16, const int16_t* inR16, int16_t* outL16, int16_t* outR16)
{
    // commands
    while (qTail_ != qHead_)
    {
        exec(queue_[qTail_]);
        qTail_ = (uint8_t)((qTail_ + 1) & 63);
    }

    // input
    float* inL = inL_;
    float* inR = inR_;
    float ig = cfg.inputGain <= 50 ? (cfg.inputGain / 50.0f) * (cfg.inputGain / 50.0f) : 1.0f + (cfg.inputGain - 50) * 0.06f;
    float peak = 0;
    for (int i = 0; i < kBlock; i++)
    {
        inL[i] = (inL16 ? inL16[i] : 0) * ig;
        inR[i] = (inR16 ? inR16[i] : 0) * ig;
        float a = fabsf(inL[i]), b = fabsf(inR[i]);
        if (a > peak) peak = a;
        if (b > peak) peak = b;
        preRoll_[0][preIdx_] = inL[i];
        preRoll_[1][preIdx_] = inR[i];
        if (++preIdx_ >= kPreRoll) preIdx_ = 0;
    }
    float lvl = peak / 32768.0f;
    inLevel_ = lvl > inLevel_ ? lvl : inLevel_ * 0.92f;

    // armed tracks: start on sound
    float thr = dbToGain((float)cfg.thresholdDb);
    if (lvl >= thr)
    {
        for (int ti = 0; ti < kTracks; ti++)
        {
            Track& t = track[ti];
            if (t.state != TrackState::kArmed) continue;
            pos_t at;
            if (!running_)
            {
                running_ = true;
                phase_ = 0;
                at = 0;
                startRecording(t, 0);
                // the 6 ms before the sound go first
                pos_t pre = (pos_t)kPreRoll << kFrac;
                phase_ = pre;
                for (int k = 0; k < kPreRoll; k++)
                {
                    int s = (preIdx_ + k) % kPreRoll;
                    writeFrame(t, (uint32_t)k, preRoll_[0][s], preRoll_[1][s], false, 0);
                }
                t.lastWrite = kPreRoll - 1;
            }
            else
            {
                at = phase_;
                startRecording(t, at);
                int64_t h = (int64_t)((at - t.anchor) >> kFrac);
                for (int k = 0; k < kPreRoll; k++)
                {
                    int64_t f = h - kPreRoll + k;
                    if (f < 0) continue;
                    int s = (preIdx_ + k) % kPreRoll;
                    writeFrame(t, (uint32_t)f, preRoll_[0][s], preRoll_[1][s], false, 0);
                }
                t.fadeIn = 1.0f;
            }
        }
    }

    // waiting tracks: start on their bar line
    if (running_)
    {
        pos_t end = phase_ + speed_ * kBlock;
        for (int ti = 0; ti < kTracks; ti++)
        {
            Track& t = track[ti];
            if (t.state == TrackState::kWaiting && end > t.recStartPhase) startRecording(t, t.recStartPhase);
        }
    }

    // tracks
    float* mixL = mixL_;
    float* mixR = mixR_;
    float** sendL = sendL_;
    float** sendR = sendR_;
    float* tL = tL_;
    float* tR = tR_;
    for (int i = 0; i < kBlock; i++)
    {
        mixL[i] = mixR[i] = 0;
        for (int s = 0; s < kFxSlots; s++)
            sendL[s][i] = sendR[s][i] = 0;
    }
    for (int ti = 0; ti < kTracks; ti++)
    {
        Track& t = track[ti];
        renderTrack(ti, inL, inR, tL, tR);
        float mg = t.muted ? 0.0f : 1.0f;
        float vg = faderGain(t.volume) * mg;
        float s0 = t.send[0] * 0.01f * mg, s1 = t.send[1] * 0.01f * mg;
        float pk = 0;
        for (int i = 0; i < kBlock; i++)
        {
            float l = tL[i], r = tR[i];
            mixL[i] += l * vg;
            mixR[i] += r * vg;
            sendL[0][i] += l * s0;
            sendR[0][i] += r * s0;
            sendL[1][i] += l * s1;
            sendR[1][i] += r * s1;
            float a = fabsf(l * vg);
            if (a > pk) pk = a;
        }
        pk /= 32768.0f;
        t.meter = pk > t.meter ? pk : t.meter * 0.9f;
    }

    // input monitoring, through the selected track's sends
    if (cfg.monitor)
    {
        Track& sel = track[selected < kTracks ? selected : 0];
        float s0 = sel.send[0] * 0.01f, s1 = sel.send[1] * 0.01f;
        for (int i = 0; i < kBlock; i++)
        {
            mixL[i] += inL[i];
            mixR[i] += inR[i];
            sendL[0][i] += inL[i] * s0;
            sendR[0][i] += inR[i] * s0;
            sendL[1][i] += inL[i] * s1;
            sendR[1][i] += inR[i] * s1;
        }
    }

    // send effects, with the chain between them (one way at a time)
    float** fxL = fxL_;
    float** fxR = fxR_;
    float barFrames = (float)(barLen_ >> kFrac) / (tempoPct_ * 0.01f);
    int first = 0, second = 1;
    float chain = chainSend[0] * 0.01f;
    if (chainSend[0] == 0 && chainSend[1] > 0)
    {
        first = 1;
        second = 0;
        chain = chainSend[1] * 0.01f;
    }
    fx[first].process(sendL[first], sendR[first], fxL[first], fxR[first], kBlock, barFrames);
    if (chain > 0)
        for (int i = 0; i < kBlock; i++)
        {
            sendL[second][i] += fxL[first][i] * chain;
            sendR[second][i] += fxR[first][i] * chain;
        }
    fx[second].process(sendL[second], sendR[second], fxL[second], fxR[second], kBlock, barFrames);
    for (int s = 0; s < kFxSlots; s++)
    {
        float g = fxMute[s] ? 0.0f : faderGain(fxReturn[s]);
        float pk = 0;
        for (int i = 0; i < kBlock; i++)
        {
            float l = fxL[s][i] * g, r = fxR[s][i] * g;
            mixL[i] += l;
            mixR[i] += r;
            float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            if (a > pk) pk = a;
        }
        pk /= 32768.0f;
        retMeter_[s] = pk > retMeter_[s] ? pk : retMeter_[s] * 0.9f;
    }

    // metronome
    if (running_)
    {
        pos_t bl = beatLen();
        pos_t a = phase_, b = phase_ + speed_ * kBlock;
        pos_t ba = floorDiv(a - 1, bl), bb = floorDiv(b - 1, bl);
        if (bb != ba || a == 0)
        {
            pos_t beat = (a == 0) ? 0 : bb;
            bool countIn = (beat * bl) < 0;
            bool want = countIn || (cfg.metronome && (!cfg.clickOnlyRec || anyRecording() || anyArmedOrWaiting()));
            if (want)
            {
                pos_t inBar = beat % cfg.beatsPerBar;
                if (inBar < 0) inBar += cfg.beatsPerBar;
                click(inBar == 0);
            }
            countInBeat_ = countIn ? (int)(-beat) : 0;
        }
    }
    else
        countInBeat_ = 0;

    // master
    float mg = cfg.masterMute ? 0.0f : faderGain(cfg.master);
    float pkL = 0, pkR = 0;
    for (int i = 0; i < kBlock; i++)
    {
        mixL[i] *= mg;
        mixR[i] *= mg;
    }
    renderClick(mixL, mixR);
    for (int i = 0; i < kBlock; i++)
    {
        float l = softClip(mixL[i] * (1.0f / 32768.0f)) * 32767.0f;
        float r = softClip(mixR[i] * (1.0f / 32768.0f)) * 32767.0f;
        if (outL16) outL16[i] = (int16_t)l;
        if (outR16) outR16[i] = (int16_t)r;
        if (fabsf(l) > pkL) pkL = fabsf(l);
        if (fabsf(r) > pkR) pkR = fabsf(r);
    }
    pkL /= 32768.0f;
    pkR /= 32768.0f;
    masterMeter_[0] = pkL > masterMeter_[0] ? pkL : masterMeter_[0] * 0.9f;
    masterMeter_[1] = pkR > masterMeter_[1] ? pkR : masterMeter_[1] * 0.9f;

    // clock
    if (running_) phase_ += speed_ * kBlock;
    blocks_ = blocks_ + 1;
}

//------------------------------------------------------------------------------
// editing (UI thread)

void Engine::lock(int t)
{
    track[t].busy = true;
    uint32_t b = blocks_;
    for (uint32_t spin = 0; spin < 20000000u && blocks_ - b < 2; spin++)
    {
        if (!audioRunning) break;
    }
}

void Engine::unlock(int t)
{
    track[t].busy = false;
    track[t].version++;
}

bool Engine::trim(int ti, uint32_t a, uint32_t b)
{
    Track& t = track[ti];
    if (b <= a || b > t.len || b - a < 64) return false;
    lock(ti);
    trimUndoStart_[ti] = t.start;
    trimUndoLen_[ti] = t.len;
    trimUndoAnchor_[ti] = t.anchor;
    trimUndo_[ti] = true;
    // the kept part stays where it was on the bar grid
    t.anchor += (pos_t)a << kFrac;
    t.start += a;
    t.len = b - a;
    t.undoAvail = false;
    t.off = 0;
    unlock(ti);
    return true;
}

bool Engine::untrim(int ti)
{
    if (!trimUndo_[ti]) return false;
    Track& t = track[ti];
    lock(ti);
    t.start = trimUndoStart_[ti];
    t.len = trimUndoLen_[ti];
    t.anchor = trimUndoAnchor_[ti];
    t.off = 0;
    trimUndo_[ti] = false;
    unlock(ti);
    return true;
}

bool Engine::cut(int ti, uint32_t a, uint32_t b)
{
    Track& t = track[ti];
    if (b <= a || b > t.len || t.len - (b - a) < 64) return false;
    lock(ti);
    int16_t* base = t.mem + (size_t)t.start * t.ch;
    memmove(base + (size_t)a * t.ch, base + (size_t)b * t.ch, (size_t)(t.len - b) * t.ch * 2);
    t.len -= (b - a);
    t.undoAvail = false;
    trimUndo_[ti] = false;
    t.off = 0;
    unlock(ti);
    return true;
}

bool Engine::erase(int ti, uint32_t a, uint32_t b)
{
    Track& t = track[ti];
    if (b <= a || b > t.len) return false;
    lock(ti);
    memset(t.mem + (size_t)(t.start + a) * t.ch, 0, (size_t)(b - a) * t.ch * 2);
    t.undoAvail = false;
    unlock(ti);
    return true;
}

bool Engine::copySel(int ti, uint32_t a, uint32_t b)
{
    Track& t = track[ti];
    if (b <= a || b > t.len) return false;
    uint32_t n = b - a;
    if ((size_t)n * t.ch > clipCap_) n = clipCap_ / t.ch;
    memcpy(clip_, t.mem + (size_t)(t.start + a) * t.ch, (size_t)n * t.ch * 2);
    clipLen_ = n;
    clipCh_ = t.ch;
    return true;
}

bool Engine::paste(int ti, uint32_t at, bool insert)
{
    Track& t = track[ti];
    if (clipLen_ == 0 || t.len == 0 || at > t.len) return false;
    uint32_t n = clipLen_;
    if (insert)
    {
        if ((size_t)(t.start + t.len + n + kLoopXfade) > t.capFrames()) return false;
    }
    else if (at + n > t.len)
        n = t.len - at;
    lock(ti);
    int16_t* base = t.mem + (size_t)t.start * t.ch;
    if (insert)
    {
        memmove(base + (size_t)(at + n) * t.ch, base + (size_t)at * t.ch, (size_t)(t.len - at) * t.ch * 2);
        t.len += n;
    }
    for (uint32_t k = 0; k < n; k++)
    {
        float l, r;
        if (clipCh_ == 2)
            l = clip_[k * 2], r = clip_[k * 2 + 1];
        else
            l = r = clip_[k];
        uint32_t f = t.start + at + k;
        if (t.ch == 2)
        {
            t.mem[f * 2] = (int16_t)l;
            t.mem[f * 2 + 1] = (int16_t)r;
        }
        else
            t.mem[f] = (int16_t)((l + r) * 0.5f);
    }
    t.undoAvail = false;
    trimUndo_[ti] = false;
    unlock(ti);
    return true;
}

bool Engine::duplicate(int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= kTracks || to >= kTracks) return false;
    Track& s = track[from];
    Track& d = track[to];
    if (s.len == 0 || d.state != TrackState::kEmpty) return false;
    if ((size_t)s.len * s.ch > d.memSamples) return false;
    lock(to);
    memcpy(d.mem, s.mem + (size_t)s.start * s.ch, (size_t)s.len * s.ch * 2);
    d.ch = s.ch;
    d.channels = s.channels;
    d.start = 0;
    d.len = s.len;
    d.anchor = s.anchor;
    d.mode = s.mode;
    d.off = s.off;
    d.grain = s.grain;
    d.tempoOpt = s.tempoOpt;
    d.undoAvail = false;
    d.state = (s.state == TrackState::kStopped) ? TrackState::kStopped : TrackState::kPlaying;
    unlock(to);
    return true;
}

void Engine::restoreClock(uint32_t barFrames, float loopBpm, float tempoPct)
{
    if (barFrames == 0) return;
    barLen_ = (pos_t)barFrames << kFrac;
    loopBpm_ = loopBpm;
    firstTake_ = false;
    setTempoPct(tempoPct);
}

int16_t* Engine::beginLoad(int ti, uint8_t channels, uint32_t frames)
{
    Track& t = track[ti];
    if (t.state != TrackState::kEmpty) return nullptr;
    uint8_t ch = channels >= 2 ? 2 : 1;
    if ((size_t)frames * ch > t.memSamples) return nullptr;
    lock(ti);
    t.ch = ch;
    t.channels = ch == 2 ? kChStereo : kChMonoL;
    return t.mem;
}

void Engine::endLoad(int ti, uint32_t frames, bool setsBar)
{
    Track& t = track[ti];
    t.start = 0;
    t.len = frames;
    t.anchor = 0;
    t.mode = kModeForward;
    t.off = 0;
    t.undoAvail = false;
    if (setsBar && firstTake_ && !hasLoops())
    {
        firstTake_ = false;
        barLen_ = (pos_t)frames << kFrac;
        loopBpm_ = cfg.beatsPerBar * 60.0f * kSampleRate / (float)frames;
        while (loopBpm_ < 60.0f) loopBpm_ *= 2.0f;
        while (loopBpm_ > 200.0f) loopBpm_ *= 0.5f;
        // keep whole bars: the bar becomes a fraction of the take
        float bars = cfg.beatsPerBar * 60.0f * kSampleRate / loopBpm_;
        uint32_t f = (uint32_t)(bars + 0.5f);
        f -= f % cfg.beatsPerBar;
        barLen_ = (pos_t)f << kFrac;
        tempoPct_ = 100.0f;
        speed_ = kOne;
        cfg.bpm = loopBpm_;
    }
    firstTake_ = false;
    t.state = TrackState::kStopped;
    unlock(ti);
}

} // namespace lp

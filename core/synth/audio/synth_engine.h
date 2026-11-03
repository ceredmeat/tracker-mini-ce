// tracker-mini-ce: a compatibility layer for OpenTracker's synth engine modules
// (github.com/opentracker-fw/opentracker-synths, MIT). OpenTracker's own core headers
// aren't public; this file declares what the modules use, from how they use it, so the
// module sources compile unchanged. The host side is core/synth/engines/OtSynthVoices.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ot {
namespace audio {

constexpr uint32_t kBlock = 128;
constexpr float kSampleRate = 44100.0f;
constexpr int kSynthSlots = 3;
constexpr size_t kSynthVoiceStateBytes = 1024;
constexpr size_t kSynthSlotScratchBytes = 2048;

struct SynthSlotScratch
{
    alignas(8) uint8_t bytes[kSynthSlotScratchBytes];
    template<class T> T& as() { return *reinterpret_cast<T*>(bytes); }
    template<class T> const T& as() const { return *reinterpret_cast<const T*>(bytes); }
};

// The notes held on the slot (note priority, Keyboard Duo).
struct SynthHeld
{
    bool mono = false;
    int count = 0;
    int low = 60;
    int high = 60;
};

struct SynthParams
{
    const float* start = nullptr; // the values at the block's start
    const float* end = nullptr;   // ... and end (the same unless `moving`)
    int count = 0;
    uint32_t serial = 0; // changes whenever a value changes
    bool moving = false;
    SynthHeld held;
};

struct SynthNote
{
    float pitch = 60.0f; // note number, may be fractional
    int note = 60;
    float velocity = 1.0f; // 0..1
    bool reused = false;   // the voice was still sounding
    bool overlap = false;  // legato: a held note on this voice continues (no new attack)
    bool fxGlide = false;  // a glide step FX sets the glide (none here)
};

struct SynthVoice
{
    alignas(8) uint8_t state[kSynthVoiceStateBytes];
    SynthSlotScratch* slotScratch = nullptr;
    int slot = 0;
    float level = 0.0f;     // the voice's loudness, for stealing
    float bend = 0.0f;      // semitones at the block's end
    float bendStart = 0.0f; // ... and start
    template<class T> T& as() { return *reinterpret_cast<T*>(state); }
    template<class T> const T& as() const { return *reinterpret_cast<const T*>(state); }
};

class ISynthEngine
{
  public:
    virtual ~ISynthEngine() = default;
    virtual int32_t id() const = 0;
    virtual const char* name() const = 0;
    virtual void slotReset(SynthSlotScratch& sc) { std::memset(sc.bytes, 0, sizeof(sc.bytes)); }
    virtual void prepare(SynthSlotScratch&, const SynthParams&) {}
    virtual void setTempo(float) {}
    virtual void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) = 0;
    virtual void noteOff(SynthVoice& v, const SynthParams& p) = 0;
    // Adds one block into L and R with the gain ramping from g0 by gInc per frame;
    // false when the voice has ended.
    virtual bool render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) = 0;
    // The slot's bus after its voices; true while it may still sound.
    virtual bool slotPost(int, SynthSlotScratch&, const SynthParams&, float*, float*) { return false; }
    virtual bool setSendTap(int, float*, float*, float, float) { return false; }
};

} // namespace audio
} // namespace ot

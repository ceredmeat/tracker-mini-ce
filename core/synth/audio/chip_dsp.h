// tracker-mini-ce: OpenTracker compatibility layer (see synth_engine.h). The chip-style
// helpers OpenTracker's 8-bit engines share, written for this port from how the engines
// use them (OpenTracker's own are in its private core): the CHIP tricks (an arpeggio
// stepped at 50 / 60 Hz or tempo divisions, vibrato with a delay, a slide into the
// note), a band-limited pulse, shift-register noise, 4-bit stepped envelopes and DAC
// quantisers.
#pragma once

#include "audio/va_dsp.h"

namespace ot {
namespace audio {
namespace chip {

using va::fastExp2;
using va::fastLog2;

enum ArpMode : int
{
    kArpOff = 0,
    kArpMajor,
    kArpMinor,
    kArpSus4,
    kArpOctave,
    kArpPower,
    kArpCustom,
};
inline const char* const kArpNames[] = { "Off", "Major", "Minor", "Sus4", "Octave", "Power", "Custom" };

enum ArpRate : int
{
    kRate50Hz = 0,
    kRate60Hz,
    kDiv8,
    kDiv16,
    kDiv32,
    kDiv64,
    kDiv128,
};
inline const char* const kRateNames[] = { "50 Hz", "60 Hz", "1/8", "1/16", "1/32", "1/64", "1/128" };

// Frames to the next tick.
struct Tick
{
    uint32_t period = 882;
    uint32_t left = 882;
    uint32_t chunk(uint32_t n) const { return n < left ? n : left; }
    // m frames went by: true on a tick.
    bool advance(uint32_t m)
    {
        if (m >= left)
        {
            left = period;
            return true;
        }
        left -= m;
        return false;
    }
};

struct TricksCfg
{
    uint8_t arp;
    uint8_t rate;
    int8_t notes[3];
    float vibRate, vibDepth, vibDelay, slideStart, slideTime;
};

struct Tricks
{
    Tick tick;
    uint8_t step;
    float t; // seconds since the note started
};

inline void setTricks(TricksCfg& c, int arp, int note2, int note3, int rate, float vibRate, float vibDepth,
                      float vibDelay, float slideStart, float slideTime)
{
    static const int8_t kChords[kArpCustom][3] = {
        { 0, 0, 0 }, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 5, 7 }, { 0, 12, 24 }, { 0, 7, 12 },
    };
    c.arp = uint8_t(arp);
    c.rate = uint8_t(rate);
    if (arp >= kArpCustom)
    {
        c.notes[0] = 0;
        c.notes[1] = int8_t(note2);
        c.notes[2] = int8_t(note3);
    }
    else
    {
        for (int k = 0; k < 3; k++)
            c.notes[k] = kChords[arp < 0 ? 0 : arp][k];
    }
    c.vibRate = vibRate;
    c.vibDepth = vibDepth;
    c.vibDelay = vibDelay;
    c.slideStart = slideStart;
    c.slideTime = slideTime;
}

inline uint32_t tickPeriod(const TricksCfg& c, float bpm)
{
    if (c.rate == kRate50Hz)
        return 882;
    if (c.rate == kRate60Hz)
        return 735;
    static const float kPerBeat[] = { 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
    const float b = bpm < 20.0f ? 120.0f : bpm;
    const float frames = 60.0f / b / kPerBeat[c.rate - kDiv8 < 5 ? c.rate - kDiv8 : 4] * kSampleRate;
    return frames < 16.0f ? 16u : uint32_t(frames);
}

inline void tricksStart(Tricks& t, const TricksCfg& c, float bpm)
{
    t.step = 0;
    t.t = 0.0f;
    t.tick.period = tickPeriod(c, bpm);
    t.tick.left = t.tick.period;
}
// The arpeggio is stepping (the block gets split at its ticks).
inline bool tricksTick(const TricksCfg& c) { return c.arp != kArpOff; }
inline void tricksRate(Tricks& t, const TricksCfg& c, float bpm)
{
    t.tick.period = tickPeriod(c, bpm);
    if (t.tick.left > t.tick.period)
        t.tick.left = t.tick.period;
}
// The pitch offset in semitones dt seconds from now.
inline float tricksOffset(const Tricks& t, const TricksCfg& c, float dt)
{
    float o = c.arp != kArpOff ? float(c.notes[t.step % 3]) : 0.0f;
    const float now = t.t + dt;
    if (c.vibDepth != 0.0f && now > c.vibDelay)
        o += c.vibDepth * va::sinePar(c.vibRate * (now - c.vibDelay));
    if (c.slideStart != 0.0f && now < c.slideTime)
        o += c.slideStart * (1.0f - now / c.slideTime);
    return o;
}
inline void tricksAdvance(Tricks& t, const TricksCfg& c, uint32_t m, bool ticked)
{
    t.t += float(m) * (1.0f / kSampleRate);
    if (ticked && c.arp != kArpOff)
        t.step = uint8_t((t.step + 1) % 3);
}

// The polynomial counters of the classic 8-bit sound chips: maximal-length shift
// registers of 4, 5 and 9 bits (periods 15, 31, 511), their output bit per position.
template<int kBits, int kTap, uint32_t kPeriod>
struct PolyTable
{
    uint32_t words[(kPeriod + 31) / 32] = {};
    constexpr PolyTable()
    {
        uint32_t s = (1u << kBits) - 1u;
        for (uint32_t i = 0; i < kPeriod; i++)
        {
            if (s & 1u)
                words[i >> 5] |= 1u << (i & 31);
            const uint32_t fb = (s ^ (s >> kTap)) & 1u;
            s = (s >> 1) | (fb << (kBits - 1));
        }
    }
    constexpr uint32_t bit(uint32_t i) const { return (words[i >> 5] >> (i & 31)) & 1u; }
};
inline constexpr PolyTable<4, 1, 15> kPoly4{};
inline constexpr PolyTable<5, 2, 31> kPoly5{};
inline constexpr PolyTable<9, 4, 511> kPoly9{};

// A band-limited pulse, +1 below the duty, -1 above, less its mean.
inline float pulseFrame(float& p, float& z, bool& hi, float d, float duty)
{
    duty = va::clampf(duty, 0.01f, 0.99f);
    return va::vcoFrame<va::kShPulse>(p, z, hi, d, duty);
}

// k steps of a Fibonacci shift register of `bits` bits (feedback: bit 0 XOR bit tap);
// returns its output bit.
inline uint32_t lfsrSteps(uint32_t& s, int bits, int tap, int k)
{
    if (!s)
        s = 1;
    const uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    for (int i = 0; i < k; i++)
    {
        const uint32_t fb = (s ^ (s >> tap)) & 1u;
        s = ((s >> 1) | (fb << (bits - 1))) & mask;
    }
    return s & 1u;
}

// A 4-bit envelope level (0..15, fractional while moving) one step towards target every
// `per` frames, over `frames` frames; true when it is there.
inline bool stepEnvTo(float& env, float target, float per, float frames)
{
    const float steps = per > 0.0f ? frames / per : 16.0f;
    if (env < target)
    {
        env += steps;
        if (env >= target)
        {
            env = target;
            return true;
        }
        return false;
    }
    env -= steps;
    if (env <= target)
    {
        env = target;
        return true;
    }
    return false;
}
// The envelope's gain 0..1: stepped to its whole steps (or smooth).
inline float env4(float e, bool steps, bool rising)
{
    if (steps)
        e = rising ? float(int(e)) : float(int(e + 0.999f));
    e = e < 0.0f ? 0.0f : (e > 15.0f ? 15.0f : e);
    return e * (1.0f / 15.0f);
}

// A value rounded to steps of 1 / scale (inverse: 1 / scale).
inline float quantQ(float y, float scale, float inverse)
{
    const float q = y * scale;
    return float(int(q + (q >= 0.0f ? 0.5f : -0.5f))) * inverse;
}
// A `bits`-bit resistor-ladder DAC: the codes' steps grow a little towards the top.
inline float ladder(float y, int bits, float full)
{
    const float steps = float(1 << (bits - 1));
    float a = __builtin_fabsf(y) / full;
    a = a > 1.0f ? 1.0f : a;
    const float code = float(int(a * steps + 0.5f));
    const float warped = code / steps * (1.0f + 0.03f * code / steps);
    return (y < 0.0f ? -warped : warped) * full / 1.03f;
}

} // namespace chip
} // namespace audio
} // namespace ot

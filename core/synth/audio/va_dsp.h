// tracker-mini-ce: OpenTracker compatibility layer (see synth_engine.h). The virtual
// analog helpers OpenTracker's engines share, written for this port from how the engines
// use them (OpenTracker's own are in its private core): clamps, fast exp/log, tan, a
// band-limited VCO (polyBLEP / polyBLAMP; the same method as D-Type's own copy), hard
// sync, the TPT state-variable filter and a 4-pole TPT ladder.
#pragma once

#include "audio/synth_engine.h"

#include <cstring>

namespace ot {
namespace audio {
namespace va {

constexpr float kPi = 3.14159265f;
constexpr float kInvFs = 1.0f / kSampleRate;
constexpr float kLog2e = 1.44269504f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline int enumOf(float x, int max)
{
    const int i = int(x + 0.5f);
    return i < 0 ? 0 : (i > max ? max : i);
}
inline float fastExp2(float x)
{
    x = clampf(x, -60.0f, 60.0f);
    const int i = int(x + (x >= 0.0f ? 0.5f : -0.5f));
    const float f = x - float(i);
    const float p =
      1.0f + f * (0.693147181f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f + f * 0.00133335581f))));
    const uint32_t b = uint32_t(i + 127) << 23;
    float s;
    std::memcpy(&s, &b, 4);
    return p * s;
}
inline float fastExp(float x) { return fastExp2(x * kLog2e); }
inline float fastLog2(float x)
{
    uint32_t b;
    std::memcpy(&b, &x, 4);
    const int e = int((b >> 23) & 0xFF) - 127;
    b = (b & 0x007FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &b, 4); // 1..2
    const float t = m - 1.0f;
    return float(e) + t * (1.4425449f - t * (0.7181452f - t * 0.2755766f)); // ~1e-3
}
// tan(x) for 0 <= x < pi / 2 (Pade 7/6).
inline float tanPade(float x)
{
    const float x2 = x * x;
    return x * (135135.0f - x2 * (17325.0f - x2 * (378.0f - x2))) /
           (135135.0f - x2 * (62370.0f - x2 * (3150.0f - 28.0f * x2)));
}
inline float flush(float x) { return __builtin_fabsf(x) < 1.0e-20f ? 0.0f : x; }
// Rational tanh, exact at 0 and +-3 (where it reaches +-1), clamped beyond.
inline float tanhR(float x)
{
    x = clampf(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
// White noise -1..1.
inline float lcg(uint32_t& s)
{
    s = s * 1664525u + 1013904223u;
    return float(int32_t(s)) * (1.0f / 2147483648.0f);
}

constexpr float kBlockT = float(kBlock) / kSampleRate;
// Per-block factor reaching ~1 % after t seconds (0 = instant).
inline float blockCoef(float t) { return t <= 0.0005f ? 0.0f : fastExp2(-4.6f * kLog2e * kBlockT / t); }
// sin(2 pi p), p in [0, 1): a refined parabola (error < 0.1 %).
inline float sinePar(float p)
{
    p -= float(int(p));
    if (p < 0.0f)
        p += 1.0f;
    const float t = 2.0f * p - 1.0f;
    const float y = 4.0f * t * (1.0f - __builtin_fabsf(t));
    return -(y + 0.225f * (y * __builtin_fabsf(y) - y));
}

// ---- envelopes (one step per block) ------------------------------------------------
enum Stage : uint8_t
{
    kIdle = 0,
    kAtt,
    kDec,
    kRel
};
constexpr float kAttOver = 1.3f; // the attack aims past 1 (an RC charging curve)

// ADSR: an RC attack reaching 1 after Attack seconds, exponential decay to Sustain and
// release.
struct Contour
{
    float attC, decC, relC, sustain;
    uint8_t attInstant;
};
inline void setContour(Contour& c, float att, float dec, float sus, float rel)
{
    c.attInstant = att <= kBlockT;
    c.attC = c.attInstant ? 0.0f : fastExp2(-1.466f * kLog2e * kBlockT / att);
    c.decC = blockCoef(dec);
    c.relC = blockCoef(rel);
    c.sustain = clampf(sus, 0.0f, 1.0f);
}
inline float step(uint8_t& st, float e, const Contour& c)
{
    switch (st)
    {
        case kAtt:
            e = c.attInstant ? 1.0f : kAttOver - (kAttOver - e) * c.attC;
            if (e >= 1.0f)
            {
                e = 1.0f;
                st = kDec;
            }
            return e;
        case kDec:
            e = c.sustain + (e - c.sustain) * c.decC;
            if (__builtin_fabsf(e - c.sustain) < 1.0e-5f)
                e = c.sustain;
            return e;
        case kRel:
            e *= c.relC;
            if (e < 1.0e-4f)
            {
                e = 0.0f;
                st = kIdle;
            }
            return e;
        default: return 0.0f;
    }
}

// The Initial Level / Attack Level contour (as on the big late-70s polysynths): from the
// Initial Level up (or down) to the Attack Level over Attack, then Decay to 0, and on
// release back to the Initial Level over Release.
struct IlAl
{
    float attC, decC, relC;
};
inline void setIlAl(IlAl& c, float att, float dec, float rel)
{
    c.attC = blockCoef(att);
    c.decC = blockCoef(dec);
    c.relC = blockCoef(rel);
}
inline float stepIlAl(uint8_t& st, float e, const IlAl& c, float il, float al)
{
    switch (st)
    {
        case kAtt:
            e = al + (e - al) * c.attC;
            if (__builtin_fabsf(e - al) < 1.0e-3f)
            {
                e = al;
                st = kDec;
            }
            return e;
        case kDec:
            e *= c.decC;
            if (__builtin_fabsf(e) < 1.0e-5f)
                e = 0.0f;
            return e;
        case kRel:
            e = il + (e - il) * c.relC;
            if (__builtin_fabsf(e - il) < 1.0e-4f)
            {
                e = il;
                st = kIdle;
            }
            return e;
        default: return il;
    }
}

// A block-rate value ramped across a pass: its value now and the per-frame increment.
struct Lin
{
    float v, d;
};

// Ramped from a to b over the block.
inline Lin lin(float a, float b) { return Lin{ a, (b - a) * (1.0f / float(kBlock)) }; }

constexpr float kMaxDt = 0.45f; // phase increment ceiling (polyBLEP needs < 0.5)
// Phase increment of a VCO at note + offset semitones + drift cents.
inline float incOf(float note, float semi, float drift, float maxDt)
{
    return clampf(440.0f * kInvFs * fastExp2((note + semi + 0.01f * drift - 69.0f) * (1.0f / 12.0f)), 1.0e-7f, maxDt);
}
// Ladder stage gain G = g / (1 + g), g = tan(pi f / fs), at cutoff x 2^oct.
inline float stageGain(float cutoff, float oct)
{
    const float hz = clampf(cutoff * fastExp2(oct), 8.0f, 0.45f * kSampleRate);
    const float t = tanPade(kPi * hz * kInvFs);
    return t / (1.0f + t);
}

// ---- the VCO ---------------------------------------------------------------------
enum Shape : uint8_t
{
    kShTri = 0,
    kShTriSaw,
    kShSaw,
    kShPulse,
    kShOff
};

// One frame, band-limited: steps get the two-frame polyBLEP residual, the triangle's
// corners the polyBLAMP one; the VCO sounds one frame late (z holds the frame).
template<int kSh>
inline float vcoFrame(float& p, float& z, bool& hi, float d, float duty)
{
    constexpr float kSaw = kSh == kShSaw ? 1.0f : (kSh == kShTriSaw ? 0.5f : 0.0f);
    constexpr float kTri = kSh == kShTri ? 1.0f : (kSh == kShTriSaw ? 0.5f : 0.0f);
    p += d;
    const bool wrap = p >= 1.0f;
    if (wrap)
        p -= 1.0f;
    float out = z, cur = 0.0f;
    if (kSaw > 0.0f)
    {
        cur = kSaw * (p + p - 1.0f);
        if (wrap)
        {
            const float x = p / d, y = 1.0f - x;
            cur += kSaw * y * y;
            out -= kSaw * x * x;
        }
    }
    if (kTri > 0.0f)
    {
        cur += kTri * (4.0f * __builtin_fabsf(p - 0.5f) - 1.0f);
        const float k = kTri * (8.0f / 6.0f) * d;
        if (wrap)
        {
            const float x = p / d, y = 1.0f - x;
            cur -= k * y * y * y;
            out -= k * x * x * x;
        }
        const bool h = p < 0.5f;
        if (!h && hi)
        {
            const float x = (p - 0.5f) / d, y = 1.0f - x;
            cur += k * y * y * y;
            out += k * x * x * x;
        }
        hi = h;
    }
    if (kSh == kShPulse)
    {
        const bool h = p < duty;
        cur = (h ? 1.0f : -1.0f) - (duty + duty - 1.0f);
        if (wrap)
        {
            const float x = p / d, y = 1.0f - x;
            cur -= y * y;
            out += x * x;
        }
        if (!h && (hi || wrap))
        {
            const float x = (p - duty) / d, y = 1.0f - x;
            cur += y * y;
            out -= x * x;
        }
        hi = h;
    }
    z = cur;
    return out;
}
template<int kSh>
inline bool vcoHi(float p, float duty)
{
    return p < (kSh == kShPulse ? duty : 0.5f);
}
// The plain VCO's sample at phase p (no band-limiting), for the sync step.
template<int kSh>
inline float vcoNaive(float p, float duty)
{
    if (kSh == kShPulse)
        return (p < duty ? 1.0f : -1.0f) - (duty + duty - 1.0f);
    if (kSh == kShSaw)
        return p + p - 1.0f;
    if (kSh == kShTri)
        return 4.0f * __builtin_fabsf(p - 0.5f) - 1.0f;
    if (kSh == kShTriSaw)
        return 0.5f * (p + p - 1.0f) + 0.5f * (4.0f * __builtin_fabsf(p - 0.5f) - 1.0f);
    return 0.0f;
}
// A VCO hard-synced to another: mark < 0, a plain band-limited frame; mark >= 0, the
// master wrapped `mark` frames ago: the phase restarts there, with a polyBLEP on the step.
template<int kSh>
inline float vcoSync(float& p, float& z, float d, float duty, float mark)
{
    bool hi = vcoHi<kSh>(p, duty);
    if (mark < 0.0f)
        return vcoFrame<kSh>(p, z, hi, d, duty);
    const float before = vcoNaive<kSh>(p + d * (1.0f - mark) - float(int(p + d * (1.0f - mark))), duty);
    p = mark * d;
    const float after = vcoNaive<kSh>(p, duty);
    const float h = after - before, x = mark, y = 1.0f - x;
    float out = z - 0.5f * h * x * x;
    z = after + 0.5f * h * y * y;
    return out;
}
// Saw and pulse off one phase (each with its gain), band-limited; `high` the pulse's state.
inline float vcoSawPulse(float& p, float& z, bool& high, float d, float duty, float gs, float gp)
{
    p += d;
    const bool wrap = p >= 1.0f;
    if (wrap)
        p -= 1.0f;
    float out = z;
    float cur = gs * (p + p - 1.0f);
    const bool h = p < duty;
    cur += gp * ((h ? 1.0f : -1.0f) - (duty + duty - 1.0f));
    if (wrap)
    {
        const float x = p / d, y = 1.0f - x;
        cur += gs * y * y - gp * y * y;
        out += -gs * x * x + gp * x * x;
    }
    if (!h && (high || wrap))
    {
        const float x = (p - duty) / d, y = 1.0f - x;
        cur += gp * y * y;
        out -= gp * x * x;
    }
    high = h;
    z = cur;
    return out;
}

// ---- the TPT state-variable filter ----------------------------------------------
struct Svf
{
    float s1, s2;
};
// g = tan(pi fc / fs), k = 2 x damping, h = 1 / (1 + g (g + k)).
struct SvfCoef
{
    float g, k, h;
};
inline float svfH(float g, float k) { return 1.0f / (1.0f + g * (g + k)); }
inline SvfCoef svfCoef(float g, float damping) { return SvfCoef{ g, damping + damping, svfH(g, damping + damping) }; }
// g of hz x 2^oct.
inline float svfG(float hz, float oct)
{
    const float f = clampf(hz * fastExp2(oct), 5.0f, 0.45f * kSampleRate);
    return tanPade(kPi * f * kInvFs);
}
inline float svfHNext(float, float g, float k) { return svfH(g, k); }
inline float svfHTrack(float, float g, float k) { return svfH(g, k); }
// The smallest damping that keeps the filter tame at this g.
inline float svfKMin(float g) { return clampf(0.02f + 0.01f * g, 0.02f, 0.2f); }
// Damping k raised with the state's level: the filter sings, bounded.
inline float svfLevelK(float k, float kmin, const Svf& f)
{
    const float lv = f.s1 * f.s1;
    return (k < kmin ? kmin : k) + 0.25f * (lv > 4.0f ? 4.0f : lv);
}
inline void svfStep(Svf& f, const SvfCoef& c, float x, float& lp, float& bp, float& hp)
{
    hp = (x - (c.k + c.g) * f.s1 - f.s2) * c.h;
    const float v1 = c.g * hp;
    bp = v1 + f.s1;
    f.s1 = bp + v1;
    const float v2 = c.g * bp;
    lp = v2 + f.s2;
    f.s2 = lp + v2;
}

// ---- the 4-pole ladder ------------------------------------------------------------
// G: each one-pole stage's gain g / (1 + g); k: the loop gain; r = 1 / (1 + k G^4).
// kSat: the loop input through the soft clip.
template<bool kSat>
inline float ladderStep(float x, float* s, float G, float k, float r)
{
    const float a = 1.0f - G;
    const float S = G * G * G * s[0] * a + G * G * s[1] * a + G * s[2] * a + s[3] * a;
    float u = (x - k * S) * r;
    if (kSat)
        u = tanhR(u);
    float y = u;
    for (int i = 0; i < 4; ++i)
    {
        const float v = (y - s[i]) * G;
        const float o = v + s[i];
        s[i] = o + v;
        y = o;
    }
    return y;
}

} // namespace va
} // namespace audio
} // namespace ot

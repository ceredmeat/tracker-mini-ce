#ifndef CE_LOOPER_DSP_H_
#define CE_LOOPER_DSP_H_

// tracker-mini-ce Looper: small DSP helpers shared by the engine, the grain player,
// the performance effects and the send effects. Single-precision only (the Mini's
// Cortex-M7 has a single-precision FPU).

#include <math.h>
#include <stdint.h>

#if defined(PLATFORM_IMX)
#include "cr_section_macros.h"
#define LP_FAST_BSS __BSS(SRAM_OC)   // the tightly-coupled RAM is full; the on-chip RAM has room
#else
#define LP_FAST_BSS
#endif

namespace lp
{

constexpr float kSampleRate = 44117.64706f;
constexpr int kBlock = 128;
constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.2831853f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

inline int16_t sat16(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

// soft clip, unity slope at 0, never above |1.5|
inline float softClip(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// 0..100 fader to gain: silent at 0, -12 dB at 40, 0 dB at 80, +3.9 dB at 100
inline float faderGain(int v)
{
    if (v <= 0) return 0.0f;
    float x = v * (1.0f / 80.0f);
    return x * x;
}

inline float dbToGain(float db) { return powf(10.0f, db / 20.0f); }

// fast sine for LFOs: phase 0..1
inline float sin01(float ph)
{
    float x = ph - floorf(ph);          // 0..1
    float y = x * 4.0f;                 // parabolic approximation
    float s = (x < 0.5f) ? (y * (2.0f - y)) : ((y - 2.0f) * (y - 4.0f));
    return s;
}

struct Rng
{
    uint32_t s = 0x1234567u;
    inline uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    inline float uni() { return (next() >> 8) * (1.0f / 16777216.0f); }   // 0..1
    inline float bi() { return uni() * 2.0f - 1.0f; }                        // -1..1
};

// one-pole low-pass
struct OnePole
{
    float z = 0.0f, a = 1.0f;
    void setHz(float hz) { a = 1.0f - expf(-kTwoPi * hz / kSampleRate); }
    inline float lp(float x) { z += a * (x - z); return z; }
    inline float hp(float x) { z += a * (x - z); return x - z; }
};

// state-variable filter (Simper / Cytomic), stable when modulated
struct Svf
{
    float ic1 = 0, ic2 = 0;
    float g = 0.1f, k = 1.4f, a1 = 0, a2 = 0, a3 = 0;
    void set(float hz, float q)
    {
        hz = clampf(hz, 10.0f, kSampleRate * 0.45f);
        g = tanf(kPi * hz / kSampleRate);
        k = 1.0f / clampf(q, 0.3f, 20.0f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    // returns low-pass, writes band/high
    inline void tick(float v0, float& lo, float& bd, float& hi)
    {
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lo = v2;
        bd = v1;
        hi = v0 - k * v1 - v2;
    }
    inline float lp(float x) { float l, b, h; tick(x, l, b, h); return l; }
    inline float hp(float x) { float l, b, h; tick(x, l, b, h); return h; }
    void reset() { ic1 = ic2 = 0; }
};

// Hann-ish window lookup (raised cosine), t in 0..1
inline float hann(float t)
{
    if (t <= 0.0f || t >= 1.0f) return 0.0f;
    return 0.5f - 0.5f * cosf(kTwoPi * t);
}

// window with variable shape: texture 0 = triangle-ish/box, 0.5 = hann, 1 = sharp attack
inline float shapedWindow(float t, float texture)
{
    if (t <= 0.0f || t >= 1.0f) return 0.0f;
    float h = 0.5f - 0.5f * cosf(kTwoPi * t);
    if (texture < 0.5f)
    {
        // blend from trapezoid (flat top) to hann
        float edge = 0.05f + 0.45f * (texture * 2.0f);
        float tr = (t < edge) ? t / edge : ((t > 1.0f - edge) ? (1.0f - t) / edge : 1.0f);
        return lerpf(tr, h, texture * 2.0f);
    }
    // percussive: fast attack, exponential-ish release
    float skew = (texture - 0.5f) * 2.0f;
    float pk = 0.5f - 0.45f * skew;
    float v = (t < pk) ? t / pk : (1.0f - t) / (1.0f - pk);
    return lerpf(h, v * v * (3.0f - 2.0f * v), skew);
}

} // namespace lp

#endif

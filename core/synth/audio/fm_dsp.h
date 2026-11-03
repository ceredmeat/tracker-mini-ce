// tracker-mini-ce: OpenTracker compatibility layer (see synth_engine.h). The 4-operator
// FM kernel shared by Arc-Type, rebuilt from FM4's own (which it was lifted from):
// four sine operators, eight algorithms, feedback on operator 1; the carriers' mean goes
// to the caller's per-sample hook.
#pragma once

#include "audio/synth_engine.h"

namespace ot {
namespace audio {
namespace fm {

constexpr int kOps = 4;

// For each operator, the operators modulating it (bit j = op j), and the carriers.
struct Alg
{
    uint8_t mod[kOps];
    uint8_t carriers;
};
constexpr Alg kAlgs[8] = {
    { { 0, 1, 2, 4 }, 8 },  // 1 > 2 > 3 > 4
    { { 0, 0, 3, 4 }, 8 },  // (1 + 2) > 3 > 4
    { { 0, 0, 2, 5 }, 8 },  // (1 + (2 > 3)) > 4
    { { 0, 1, 0, 6 }, 8 },  // ((1 > 2) + 3) > 4
    { { 0, 1, 0, 4 }, 10 }, // 1 > 2, 3 > 4
    { { 0, 1, 1, 1 }, 14 }, // 1 > (2, 3, 4)
    { { 0, 1, 0, 0 }, 14 }, // 1 > 2, 3, 4
    { { 0, 0, 0, 0 }, 15 }, // 1, 2, 3, 4
};

struct Ops
{
    float ph[kOps];
    float fbA, fbB; // operator 1, one and two samples back
};

struct OpRamp
{
    float inc[kOps], dinc[kOps];
    float amp[kOps], damp[kOps];
    float fb;
};

inline float fracf(float x)
{
    x += 8.0f;
    return x - float(int(x));
}
// sin(2 pi x), x in [0, 1): refined parabola (error < 0.1 %).
inline float sinCyc(float x)
{
    const float t = 2.0f * x - 1.0f;
    const float y = 4.0f * t * (1.0f - __builtin_fabsf(t));
    return -(y + 0.225f * (y * __builtin_fabsf(y) - y));
}

template<int kA, class Out>
inline void kernel(Ops& o, OpRamp& r, uint32_t n, Out& h)
{
    constexpr Alg a = kAlgs[kA];
    constexpr float kOut = 1.0f / float(((a.carriers >> 0) & 1) + ((a.carriers >> 1) & 1) +
                                        ((a.carriers >> 2) & 1) + ((a.carriers >> 3) & 1));
    float p0 = o.ph[0], p1 = o.ph[1], p2 = o.ph[2], p3 = o.ph[3];
    float fa = o.fbA, fb = o.fbB;
    for (uint32_t i = 0; i < n; ++i)
    {
        p0 += r.inc[0];
        p1 += r.inc[1];
        p2 += r.inc[2];
        p3 += r.inc[3];
        if (p0 >= 1.0f)
            p0 -= 1.0f;
        if (p1 >= 1.0f)
            p1 -= 1.0f;
        if (p2 >= 1.0f)
            p2 -= 1.0f;
        if (p3 >= 1.0f)
            p3 -= 1.0f;
        const float y0 = r.amp[0] * sinCyc(fracf(p0 + r.fb * (fa + fb)));
        fb = fa;
        fa = y0;
        float in1 = 0.0f, in2 = 0.0f, in3 = 0.0f;
        if (a.mod[1] & 1)
            in1 += y0;
        const float y1 = r.amp[1] * sinCyc(fracf(p1 + in1));
        if (a.mod[2] & 1)
            in2 += y0;
        if (a.mod[2] & 2)
            in2 += y1;
        const float y2 = r.amp[2] * sinCyc(fracf(p2 + in2));
        if (a.mod[3] & 1)
            in3 += y0;
        if (a.mod[3] & 2)
            in3 += y1;
        if (a.mod[3] & 4)
            in3 += y2;
        const float y3 = r.amp[3] * sinCyc(fracf(p3 + in3));
        float out = 0.0f;
        if (a.carriers & 1)
            out += y0;
        if (a.carriers & 2)
            out += y1;
        if (a.carriers & 4)
            out += y2;
        if (a.carriers & 8)
            out += y3;
        h(i, out * kOut);
        for (int k = 0; k < kOps; ++k)
        {
            r.inc[k] += r.dinc[k];
            r.amp[k] += r.damp[k];
        }
    }
    o.ph[0] = p0;
    o.ph[1] = p1;
    o.ph[2] = p2;
    o.ph[3] = p3;
    o.fbA = __builtin_fabsf(fa) < 1.0e-20f ? 0.0f : fa;
    o.fbB = __builtin_fabsf(fb) < 1.0e-20f ? 0.0f : fb;
}

} // namespace fm
} // namespace audio
} // namespace ot

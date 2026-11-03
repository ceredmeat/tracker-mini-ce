// tracker-mini-ce: OpenTracker compatibility layer (see synth_engine.h). A Juno-style
// stereo BBD chorus written for this port (OpenTracker's own is in its private core):
// a mono line, one triangle LFO, the two taps in opposite phase, a lowpass on the wet
// signal (the BBD's filters), dry + wet. Mode I: 0.51 Hz, Mode II: 0.86 Hz and deeper.
#pragma once

#include "audio/synth_engine.h"

namespace ot {
namespace audio {
namespace bbd {

constexpr uint32_t kLine = 512;                   // 11.6 ms
constexpr uint32_t kTailFrames = kLine + kBlock;

struct State
{
    uint32_t pos;
    float phase;
    float wet;    // 0..1, ramped towards the mode's mix
    float dl, dr; // the taps' delays at the last frame, frames
    float lpL, lpR;
};

inline void clear(State& s, int16_t* line)
{
    for (uint32_t i = 0; i < kLine; i++)
    {
        line[i] = 0;
    }
    s.pos = 0;
    s.lpL = s.lpR = 0.0f;
}

inline float tap(const int16_t* line, uint32_t pos, float d)
{
    float rp = float(pos) - d;
    while (rp < 0.0f)
    {
        rp += float(kLine);
    }
    uint32_t i = uint32_t(rp);
    float f = rp - float(i);
    float a = line[i & (kLine - 1)], b = line[(i + 1) & (kLine - 1)];
    return (a + (b - a) * f) * (1.0f / 32768.0f);
}

// Mono in L (R ignored), stereo out; true while the wet signal may still be heard.
inline bool process(State& s, int16_t* line, int mode, float, float, uint8_t tone, float* L, float* R,
                    uint32_t n)
{
    const float rate = mode == 2 ? 0.863f : 0.513f;
    const float centre = 3.5e-3f * kSampleRate, swing = (mode == 2 ? 1.85e-3f : 1.4e-3f) * kSampleRate;
    const float want = mode ? 0.5f : 0.0f;
    const float lpc = 0.3f + 0.6f * float(tone) / 100.0f;
    const float inc = rate / kSampleRate;
    for (uint32_t i = 0; i < n; i++)
    {
        float x = L[i];
        float xc = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
        line[s.pos] = int16_t(xc * 32767.0f);
        float tri = s.phase < 0.5f ? 4.0f * s.phase - 1.0f : 3.0f - 4.0f * s.phase;
        s.phase += inc;
        if (s.phase >= 1.0f)
        {
            s.phase -= 1.0f;
        }
        s.dl = centre + swing * tri;
        s.dr = centre - swing * tri;
        s.lpL += lpc * (tap(line, s.pos, s.dl) - s.lpL);
        s.lpR += lpc * (tap(line, s.pos, s.dr) - s.lpR);
        s.wet += (want - s.wet) * 0.0005f;
        if (!mode && s.wet < 1.0e-4f)
        {
            s.wet = 0.0f;
        }
        const float dry = 1.0f - 0.5f * s.wet;
        L[i] = dry * x + s.wet * s.lpL;
        R[i] = dry * x + s.wet * s.lpR;
        s.pos = (s.pos + 1) & (kLine - 1);
    }
    return s.wet > 0.0f;
}

} // namespace bbd
} // namespace audio
} // namespace ot

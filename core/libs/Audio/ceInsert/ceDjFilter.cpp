// tracker-mini-ce: the DJ Filter's audio (see ceDjFilter.h).
//
// A TPT state-variable filter per channel (Zavalishin's form). Lowpass cutoff goes from
// 20 kHz at -1 down to 80 Hz at -50, highpass from 20 Hz at +1 up to 8 kHz at +50, both
// along an exponential curve. The cutoff glides (per block, in octaves), and the filter
// fades in and out over about 20 ms, so moving the knob or switching it Off doesn't
// click. Off is untouched audio.

#include "ceDjFilter.h"
#include "../ceMasterInsert.h"

#include <math.h>
#include <stdio.h>

#include "AudioStream.h"
#include "ceModsExtra.h"

namespace
{
constexpr float kFs = 44100.0f;
constexpr float kPi = 3.14159265f;

volatile int target = 0;  // set from the screen
int mode = 0;             // -1 lowpass, +1 highpass, 0 off (the audio side's own)
float amount = 0.0f;      // wet amount 0..1
float logFc = 0.0f;       // log2 of the cutoff, smoothed
bool fcValid = false;
float ic1[2], ic2[2];

float targetLogFc(int pos)
{
    if (pos < 0)
    {
        float t = (float)(-pos - 1) / (ceDj::kMax - 1); // 0..1
        return log2f(20000.0f) + t * (log2f(80.0f) - log2f(20000.0f));
    }
    float t = (float)(pos - 1) / (ceDj::kMax - 1);
    return log2f(20.0f) + t * (log2f(8000.0f) - log2f(20.0f));
}

// k = 1/Q (Low Q 0.71, Mid Q 1.4, High Q 2.5) and a level trim for the resonant peak
void resonance(float& k, float& trim)
{
    switch (ceExtra::at(ceExtra::kDjResonance))
    {
        case 1: k = 1.0f / 1.4f; trim = 0.85f; break;
        case 2: k = 1.0f / 2.5f; trim = 0.65f; break;
        default: k = 1.41421356f; trim = 1.0f; break;
    }
}

inline int16_t sat16(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}
} // namespace

namespace ceDj
{
int position() { return target; }

void setPosition(int pos)
{
    if (pos > kMax) pos = kMax;
    if (pos < -kMax) pos = -kMax;
    target = pos;
}

void label(char* out, int len)
{
    int p = target;
    if (p == 0)
        snprintf(out, len, "Off");
    else
        snprintf(out, len, "%s %d%%", p < 0 ? "LP" : "HP", (p < 0 ? -p : p) * 2);
}
} // namespace ceDj

void ceDjFilterBlock(int16_t* left, int16_t* right)
{
    int pos = target;
    int want = pos < 0 ? -1 : (pos > 0 ? 1 : 0);

    if (mode == 0 && want == 0) return; // Off: untouched

    // fading out to switch side (or to Off), else fading in
    float goal;
    if (mode != want && mode != 0)
        goal = 0.0f;
    else
    {
        if (mode == 0)
        {
            mode = want;
            ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f;
            fcValid = false;
        }
        goal = 1.0f;
    }

    if (mode != 0 && pos != 0 && (pos < 0) == (mode < 0))
    {
        float t = targetLogFc(pos);
        if (!fcValid)
        {
            // start from the open end so it sweeps in
            logFc = mode < 0 ? log2f(20000.0f) : log2f(20.0f);
            fcValid = true;
        }
        float d = t - logFc;
        const float kStep = 0.25f; // at most a quarter octave per block
        if (d > kStep) d = kStep;
        if (d < -kStep) d = -kStep;
        logFc += d;
    }

    float fc = exp2f(logFc);
    if (fc > 20000.0f) fc = 20000.0f;
    float g = tanf(kPi * fc / kFs);
    float k, trim;
    resonance(k, trim);
    // near the open end the peak is above hearing: trim only once the cutoff moves in
    {
        int ap = pos < 0 ? -pos : pos;
        float depth = ap >= 10 ? 1.0f : ap / 10.0f;
        trim = 1.0f - (1.0f - trim) * depth;
    }
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;

    float start = amount;
    float step = (goal - start) / AUDIO_BLOCK_SAMPLES;
    if (step > 1.0f / 800.0f) step = 1.0f / 800.0f;
    if (step < -1.0f / 800.0f) step = -1.0f / 800.0f;

    int16_t* ch[2] = { left, right };
    for (int c = 0; c < 2; c++)
    {
        float s1 = ic1[c], s2 = ic2[c];
        float a = start;
        int16_t* d = ch[c];
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            float x = d[i];
            float v3 = x - s2;
            float v1 = a1 * s1 + a2 * v3;
            float v2 = s2 + a2 * s1 + a3 * v3;
            s1 = 2.0f * v1 - s1;
            s2 = 2.0f * v2 - s2;
            float y = (mode < 0 ? v2 : x - k * v1 - v2) * trim;
            a += step;
            if (a > 1.0f) a = 1.0f;
            if (a < 0.0f) a = 0.0f;
            d[i] = sat16(x + a * (y - x));
        }
        ic1[c] = s1;
        ic2[c] = s2;
        if (c == 1) amount = a;
    }

    if (amount <= 0.0f && goal == 0.0f)
    {
        amount = 0.0f;
        mode = 0; // next block picks the other side (or stays Off)
    }
}

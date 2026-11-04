// tracker-mini-ce: the FX Pack's audio (see ceFxPack.h). Our own DSP, written for this
// fork after the user's earlier stock-firmware FX Pack (fx2).
//
// Each effect fades in and out over about ten blocks (30 ms) when it's switched on or off,
// so switching doesn't click; Dirt and Tape keep the level about even as the drive goes up.
// The delay lines (Tape, Chorus, 16-bit) and the work buffer, 7 KB in all, come from the
// heap once at start-up (the external RAM is full on the Tracker+); without them the FX
// Pack stays off. A delay line is cleared when its effect switches on.

#include "ceFxPack.h"
#include "../ceMasterInsert.h"

#include <math.h>
#include <string.h>

#include "AudioStream.h"
#include "ceModsExtra.h"

namespace ceFx
{
uint8_t& param(Param p)
{
    return ceExtra::at((int)ceExtra::kFxPack + (int)p);
}
} // namespace ceFx

#ifdef PLATFORM_IMX

#include <stdlib.h>

using namespace ceFx;

namespace
{
constexpr float kFs = 44100.0f;
constexpr float kPi = 3.14159265f;
constexpr int kN = AUDIO_BLOCK_SAMPLES;
constexpr float kFade = 0.1f; // per block

constexpr int kTapeLen = 512;    // 11.6 ms
constexpr int kChorusLen = 1024; // 23 ms

struct Memory
{
    int16_t tape[2][kTapeLen];
    int16_t chorus[2][kChorusLen];
    float buf[2][AUDIO_BLOCK_SAMPLES];
};
Memory* const mem = (Memory*)calloc(1, sizeof(Memory)); // at start-up
constexpr float kLineScale = 32767.0f;

struct OnePole
{
    float z = 0.0f;
    inline float lp(float x, float a) { return z += a * (x - z); }
    inline float hp(float x, float a) { return x - lp(x, a); }
};

inline float lpCoeff(float fc)
{
    return 1.0f - expf(-2.0f * kPi * fc / kFs);
}

inline float softClip(float x) // tanh-like, exact at 0, ±1 beyond ±3
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// sin(x * pi / 2) for any x: folded into -1..1 (period 4), then a 5th-order polynomial
// (error below 0.0002; sinf per sample costs far more)
inline float fastSinQuarter(float x)
{
    float t = x + 1.0f;
    t -= 4.0f * floorf(t * 0.25f); // 0..4
    t = t < 2.0f ? t - 1.0f : 3.0f - t; // -1..1, a triangle
    float t2 = t * t;
    return t * (1.5704372f + t2 * (-0.6427093f + t2 * 0.0724327f));
}

inline float quant(float x, float levels)
{
    return floorf(x * levels + 0.5f) / levels;
}

inline int16_t toS16(float v)
{
    v *= 32768.0f;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

inline int16_t sat(float v) // to a 16-bit delay line
{
    v *= kLineScale;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

inline float ramp(float a, bool on)
{
    if (on) return a + kFade > 1.0f ? 1.0f : a + kFade;
    return a - kFade < 0.0f ? 0.0f : a - kFade;
}


//------------------------------------------------------------------------------ Dirt
struct Dirt
{
    float amount = 0.0f;
    OnePole tone[2], dc[2];
    float held[2] = { 0, 0 };
    int holdCount[2] = { 0, 0 };

    float shape(float x, int type)
    {
        switch (type)
        {
            case 1: return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);   // hard
            case 2: return fastSinQuarter(x);                          // fold
            case 3: return softClip(x + 0.35f) - softClip(0.35f);      // asym
            default: return softClip(x);                               // soft
        }
    }

    void run()
    {
        bool on = param(kDirtDrive) > 0;
        if (!on && amount == 0.0f) return;
        if (on && amount == 0.0f)
        {
            tone[0].z = tone[1].z = dc[0].z = dc[1].z = 0.0f;
            holdCount[0] = holdCount[1] = 0;
        }
        float target = ramp(amount, on);

        float d = param(kDirtDrive) / 100.0f;
        if (d > 1.0f) d = 1.0f;
        float pre = 1.0f + d * d * 40.0f;
        int type = param(kDirtShape) & 3;
        float ref = fabsf(shape(0.3f * pre, type));
        float makeup = type == 2 ? fmaxf(0.4f, 1.0f / (1.5708f * pre)) : (ref > 0.25f ? 0.3f / ref : 1.2f);
        if (type == 3) makeup *= 0.7f;
        float aDc = lpCoeff(10.0f);
        if (makeup > 1.2f) makeup = 1.2f;
        int bits = 16 - (param(kDirtBits) > 12 ? 12 : param(kDirtBits));
        float levels = (float)(1 << (bits - 1));
        int hold = 1 + (param(kDirtRate) > 15 ? 15 : param(kDirtRate));
        float dark = (param(kDirtDarken) > 100 ? 100 : param(kDirtDarken)) / 100.0f;
        float a = lpCoeff(18000.0f * powf(0.05f, dark));
        float dry = (param(kDirtDry) > 100 ? 100 : param(kDirtDry)) / 100.0f;

        for (int c = 0; c < 2; c++)
        {
            float g = amount, gs = (target - amount) / kN;
            for (int i = 0; i < kN; i++)
            {
                float x = mem->buf[c][i];
                float y = shape(x * pre, type) * makeup;
                if (type == 3) y = dc[c].hp(y, aDc);
                if (bits < 16) y = quant(y, levels);
                if (hold > 1)
                {
                    if (holdCount[c] <= 0)
                    {
                        held[c] = y;
                        holdCount[c] = hold;
                    }
                    holdCount[c]--;
                    y = held[c];
                }
                if (dark > 0.0f) y = tone[c].lp(y, a);
                y = dry * x + (1.0f - dry) * y;
                g += gs;
                mem->buf[c][i] = x + g * (y - x);
            }
        }
        amount = target;
    }
} dirt;

//------------------------------------------------------------------------------ LoFi
struct Lofi
{
    float amount = 0.0f;
    OnePole hp[2], lp1[2], lp2[2];
    float held[2] = { 0, 0 };
    int holdCount = 0;

    void run()
    {
        bool on = param(kLofiMix) > 0;
        if (!on && amount == 0.0f) return;
        if (on && amount == 0.0f)
        {
            for (int c = 0; c < 2; c++) hp[c].z = lp1[c].z = lp2[c].z = 0.0f;
            holdCount = 0;
        }
        float target = ramp(amount, on);

        float mix = (param(kLofiMix) > 100 ? 100 : param(kLofiMix)) / 100.0f;
        int bits = 16 - (param(kLofiBits) > 14 ? 14 : param(kLofiBits));
        float levels = (float)(1 << (bits - 1));
        int hold = 1 + (param(kLofiRate) > 31 ? 31 : param(kLofiRate));
        int speaker = param(kLofiSpeaker) > 4 ? 0 : param(kLofiSpeaker);
        bool mono = param(kLofiMono) != 0;

        //                       off     radio   phone   handheld tv
        static const float hpF[5] = { 0.0f, 300.0f, 500.0f, 800.0f, 150.0f };
        static const float lpF[5] = { 0.0f, 4000.0f, 3000.0f, 5000.0f, 6500.0f };
        static const float drv[5] = { 1.0f, 1.6f, 2.2f, 1.3f, 1.2f };
        float ah = speaker ? lpCoeff(hpF[speaker]) : 0.0f;
        float al = speaker ? lpCoeff(lpF[speaker]) : 0.0f;

        float g = amount, gs = (target - amount) / kN;
        for (int i = 0; i < kN; i++)
        {
            float x[2] = { mem->buf[0][i], mem->buf[1][i] };
            float in[2] = { x[0], x[1] };
            if (mono) in[0] = in[1] = 0.5f * (x[0] + x[1]);

            if (hold > 1)
            {
                if (holdCount <= 0)
                {
                    held[0] = in[0];
                    held[1] = in[1];
                    holdCount = hold;
                }
                holdCount--;
                in[0] = held[0];
                in[1] = held[1];
            }
            g += gs;
            for (int c = 0; c < 2; c++)
            {
                float y = in[c];
                if (bits < 16) y = quant(y, levels);
                if (speaker)
                {
                    y = hp[c].hp(y, ah);
                    y = lp2[c].lp(lp1[c].lp(y, al), al);
                    y = softClip(y * drv[speaker]) * (1.2f / drv[speaker]);
                }
                y = x[c] + mix * (y - x[c]);
                mem->buf[c][i] = x[c] + g * (y - x[c]);
            }
        }
        amount = target;
    }
} lofi;

//------------------------------------------------------------------------------ Tape
struct Tape
{
    float amount = 0.0f;
    OnePole tone[2], dc[2];
    int w = 0;
    float wowPh = 0.0f, flPh1 = 0.0f, flPh2 = 0.0f;

    void run()
    {
        bool on = param(kTapeDrive) > 0;
        if (!on && amount == 0.0f) return;
        if (on && amount == 0.0f)
        {
            memset(mem->tape, 0, sizeof(mem->tape));
            tone[0].z = tone[1].z = dc[0].z = dc[1].z = 0.0f;
            w = 0;
        }
        float target = ramp(amount, on);

        float d = (param(kTapeDrive) > 100 ? 100 : param(kTapeDrive)) / 100.0f;
        float pre = 1.0f + d * 6.0f;
        float makeup = 0.3f / softClip(0.3f * pre + 0.05f * d);
        float dark = (param(kTapeDarken) > 100 ? 100 : param(kTapeDarken)) / 100.0f;
        float a = lpCoeff(16000.0f * powf(0.125f, dark)); // 16 kHz .. 2 kHz
        float wow = (param(kTapeWow) > 100 ? 100 : param(kTapeWow)) / 100.0f * 120.0f; // samples
        float fl = (param(kTapeFlutter) > 100 ? 100 : param(kTapeFlutter)) / 100.0f * 10.0f;
        float dry = (param(kTapeDry) > 100 ? 100 : param(kTapeDry)) / 100.0f;
        float aDc = lpCoeff(10.0f);

        // the wobble (at most ~10 Hz) is worked out at the block's two ends and joined with a
        // straight line: 6 sines a block instead of 3 a sample
        auto delayAt = [&]() {
            return 140.0f + wow * (0.5f + 0.5f * sinf(2.0f * kPi * wowPh)) +
                   fl * (0.5f + 0.3f * sinf(2.0f * kPi * flPh1) + 0.2f * sinf(2.0f * kPi * flPh2));
        };
        float delay = delayAt();
        wowPh += 0.55f * kN / kFs;
        if (wowPh >= 1.0f) wowPh -= 1.0f;
        flPh1 += 6.3f * kN / kFs;
        if (flPh1 >= 1.0f) flPh1 -= 1.0f;
        flPh2 += 9.7f * kN / kFs;
        if (flPh2 >= 1.0f) flPh2 -= 1.0f;
        float delayStep = (delayAt() - delay) / kN;

        float g = amount, gs = (target - amount) / kN;
        for (int i = 0; i < kN; i++)
        {
            delay += delayStep;

            float rp = (float)w - delay;
            if (rp < 0.0f) rp += kTapeLen;
            int r0 = (int)rp;
            float fr = rp - r0;
            int r1 = r0 + 1 >= kTapeLen ? 0 : r0 + 1;
            g += gs;
            for (int c = 0; c < 2; c++)
            {
                float x = mem->buf[c][i];
                // saturation with a touch of even harmonics, DC removed
                float s = softClip(x * pre + 0.05f * d) * makeup;
                s = dc[c].hp(s, aDc);
                mem->tape[c][w] = sat(s);
                float y = (mem->tape[c][r0] + fr * (mem->tape[c][r1] - mem->tape[c][r0])) * (1.0f / kLineScale);
                y = tone[c].lp(y, a);
                y = dry * x + (1.0f - dry) * y;
                mem->buf[c][i] = x + g * (y - x);
            }
            if (++w >= kTapeLen) w = 0;
        }
        amount = target;
    }
} tape;

//------------------------------------------------------------------------------ Chorus
struct Chorus
{
    float amount = 0.0f;
    int w = 0;
    float ph = 0.0f;

    void run()
    {
        bool on = param(kChorusMix) > 0;
        if (!on && amount == 0.0f) return;
        if (on && amount == 0.0f)
        {
            memset(mem->chorus, 0, sizeof(mem->chorus));
            w = 0;
        }
        float target = ramp(amount, on);

        float mix = (param(kChorusMix) > 100 ? 100 : param(kChorusMix)) / 100.0f;
        float rate = 0.1f + (param(kChorusRate) > 100 ? 100 : param(kChorusRate)) / 100.0f * 2.9f;
        float depth = (0.2f + 0.8f * (param(kChorusDepth) > 100 ? 100 : param(kChorusDepth)) / 100.0f) * 220.0f;
        float width = 1.0f - (param(kChorusNarrow) > 100 ? 100 : param(kChorusNarrow)) / 100.0f;
        float base = 500.0f; // 11 ms

        // the LFO (at most 3 Hz) at the block's two ends, joined with a straight line
        float lfo[2], lfoStep[2];
        lfo[0] = sinf(2.0f * kPi * ph);
        lfo[1] = sinf(2.0f * kPi * (ph + 0.25f * width)); // quadrature at full width
        ph += rate * kN / kFs;
        if (ph >= 1.0f) ph -= 1.0f;
        lfoStep[0] = (sinf(2.0f * kPi * ph) - lfo[0]) / kN;
        lfoStep[1] = (sinf(2.0f * kPi * (ph + 0.25f * width)) - lfo[1]) / kN;

        float g = amount, gs = (target - amount) / kN;
        for (int i = 0; i < kN; i++)
        {
            lfo[0] += lfoStep[0];
            lfo[1] += lfoStep[1];
            g += gs;
            for (int c = 0; c < 2; c++)
            {
                float x = mem->buf[c][i];
                mem->chorus[c][w] = sat(x);
                float rp = (float)w - (base + depth * (0.5f + 0.5f * lfo[c]));
                if (rp < 0.0f) rp += kChorusLen;
                int r0 = (int)rp;
                float fr = rp - r0;
                int r1 = r0 + 1 >= kChorusLen ? 0 : r0 + 1;
                float dl = (mem->chorus[c][r0] + fr * (mem->chorus[c][r1] - mem->chorus[c][r0])) * (1.0f / kLineScale);
                float y = x * (1.0f - 0.5f * mix) + dl * (0.5f * mix) * 1.2f;
                mem->buf[c][i] = x + g * (y - x);
            }
            if (++w >= kChorusLen) w = 0;
        }
        amount = target;
    }
} chorus;
} // namespace

void ceFxPackBlock(int16_t* left, int16_t* right)
{
    bool any = param(kDirtDrive) || param(kLofiMix) || param(kTapeDrive) || param(kChorusMix) ||
               dirt.amount > 0.0f || lofi.amount > 0.0f || tape.amount > 0.0f ||
               chorus.amount > 0.0f;
    if (!any || mem == nullptr) return; // all Off: untouched

    for (int i = 0; i < kN; i++)
    {
        mem->buf[0][i] = left[i] * (1.0f / 32768.0f);
        mem->buf[1][i] = right[i] * (1.0f / 32768.0f);
    }
    dirt.run();
    lofi.run();
    tape.run();
    chorus.run();
    for (int i = 0; i < kN; i++)
    {
        left[i] = toS16(mem->buf[0][i]);
        right[i] = toS16(mem->buf[1][i]);
    }
}

#else

void ceFxPackBlock(int16_t*, int16_t*) {}

#endif

// tracker-mini-ce Looper: send effects. New code for this firmware.

#include "lp_fx.h"

#include <stdio.h>
#include <string.h>

namespace lp
{

const FxDesc kFxDesc[kFxTypes] = {
    {"Delay", 4, {"Time", "Feedback", "Tone", "Spread"}, {46, 40, 60, 0}},
    {"Ping-pong", 4, {"Time", "Feedback", "Tone", "Width"}, {46, 45, 60, 100}},
    {"Reverb", 4, {"Size", "Tone", "Width", "Pre-delay"}, {60, 50, 100, 0}},
    {"Chorus", 3, {"Rate", "Depth", "Width"}, {30, 50, 100}},
    {"Flanger", 4, {"Rate", "Depth", "Feedback", "Manual"}, {20, 60, 60, 30}},
    {"Clouds", 8, {"Position", "Size", "Pitch", "Density", "Texture", "Feedback", "Spread", "Freeze"}, {30, 40, 50, 65, 50, 0, 50, 0}},
    {"Phaser", 4, {"Rate", "Depth", "Feedback", "Width"}, {25, 70, 50, 100}},
    {"Shimmer", 4, {"Size", "Tone", "Shimmer", "Width"}, {70, 60, 50, 100}},
    {"Tape echo", 4, {"Time", "Feedback", "Wow", "Drive"}, {46, 45, 30, 40}},
    {"Reverse", 4, {"Time", "Feedback", "Tone", "Width"}, {65, 30, 60, 60}},
    {"Prime time", 4, {"Time", "Feedback", "Mod", "Tone"}, {46, 40, 30, 55}},
    {"Lo-fi", 4, {"Bits", "Rate", "Drive", "Tone"}, {50, 40, 30, 50}},
};

const float kDelayTimeBars[kDelayTimes] = {1.0f / 32,  1.0f / 24, 1.0f / 16, 1.0f / 12, 3.0f / 32,
                                           1.0f / 8,   1.0f / 6,  3.0f / 16, 1.0f / 4,  1.0f / 3,
                                           3.0f / 8,   1.0f / 2,  5.0f / 8,  3.0f / 4};
const char* const kDelayTimeNames[kDelayTimes] = {"1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8",  "1/4T",
                                                  "1/8D", "1/4",   "1/2T", "1/4D", "1/2",   "5/8",  "3/4"};

int delayTimeIndex(uint8_t knob)
{
    int i = knob * kDelayTimes / 101;
    return clampi(i, 0, kDelayTimes - 1);
}

static float expMap(uint8_t v, float lo, float hi)
{
    return lo * powf(hi / lo, v * 0.01f);
}

void fxKnobText(int type, int k, uint8_t v, char* out, int outLen)
{
    const bool timeKnob = (k == 0) && (type == kFxDelay || type == kFxPingPong || type == kFxTape ||
                                       type == kFxReverse || type == kFxPrime);
    if (timeKnob)
    {
        snprintf(out, outLen, "%s", kDelayTimeNames[delayTimeIndex(v)]);
        return;
    }
    switch (type)
    {
        case kFxReverb:
        case kFxShimmer:
            if (k == 0)
            {
                snprintf(out, outLen, "%.1f s", expMap(v, 0.3f, 9.0f));
                return;
            }
            if (type == kFxReverb && k == 3)
            {
                snprintf(out, outLen, "%d ms", v);
                return;
            }
            break;
        case kFxChorus:
        case kFxPhaser:
            if (k == 0)
            {
                snprintf(out, outLen, "%.2f Hz", expMap(v, 0.1f, 5.0f));
                return;
            }
            break;
        case kFxFlanger:
            if (k == 0)
            {
                snprintf(out, outLen, "%.2f Hz", expMap(v, 0.05f, 2.0f));
                return;
            }
            if (k == 3)
            {
                snprintf(out, outLen, "%.1f ms", expMap(v, 0.5f, 8.0f));
                return;
            }
            break;
        case kFxClouds:
            if (k == 1)
            {
                snprintf(out, outLen, "%d ms", (int)expMap(v, 20.0f, 1000.0f));
                return;
            }
            if (k == 2)
            {
                snprintf(out, outLen, "%+d st", (v - 50) * 24 / 50);
                return;
            }
            if (k == 7)
            {
                snprintf(out, outLen, "%s", v >= 50 ? "On" : "Off");
                return;
            }
            break;
        case kFxLofi:
            if (k == 0)
            {
                snprintf(out, outLen, "%d bit", 16 - v * 14 / 100);
                return;
            }
            if (k == 1)
            {
                snprintf(out, outLen, "1/%d", 1 + v * 31 / 100);
                return;
            }
            break;
    }
    snprintf(out, outLen, "%d", v);
}

//------------------------------------------------------------------------------

void FxSlot::init(void* mem, uint32_t bytes)
{
    mem_ = (uint8_t*)mem;
    bytes_ = bytes;
    line_ = (int16_t*)mem_;
    lineFrames_ = bytes_ / 4;
    memset(mem_, 0, bytes_);
    setType(type_);
    clearing_ = false;
}

void FxSlot::setType(int type)
{
    type_ = (uint8_t)clampi(type, 0, kFxTypes - 1);
    for (int k = 0; k < kFxKnobs; k++)
        knob[k] = kFxDesc[type_].def[k];
    clearPos_ = 0;
    clearing_ = true;
    wr_ = 0;
    curDelay_[0] = curDelay_[1] = 0;
    fbState_[0] = fbState_[1] = 0;
    memset(apz_, 0, sizeof(apz_));
    memset(cg_, 0, sizeof(cg_));
    revAge_ = 0;
    revStart_ = 0;
    visCount = 0;

    // carve the reverb lines
    if (type_ == kFxReverb || type_ == kFxShimmer)
    {
        static const int base[kFdn] = {1123, 1291, 1453, 1621, 1787, 1949, 2113, 2281};
        static const int apl[4] = {142, 107, 379, 277};
        float* f = (float*)mem_;
        for (int i = 0; i < kFdn; i++)
        {
            fdnLen_[i] = base[i] * 13 / 10;
            fdn_[i] = f;
            f += fdnLen_[i];
            fdnIdx_[i] = 0;
            fdnLp_[i] = 0;
        }
        for (int i = 0; i < 4; i++)
        {
            apLen_[i] = apl[i];
            ap_[i] = f;
            f += apl[i];
            apIdx_[i] = 0;
        }
        shift_ = f;      // 4096 floats for shimmer; pre-delay uses the int16 line after it
        f += 4096;
        line_ = (int16_t*)f;
        lineFrames_ = (uint32_t)((mem_ + bytes_) - (uint8_t*)f) / 4;
    }
    else
    {
        line_ = (int16_t*)mem_;
        lineFrames_ = bytes_ / 4;
    }
}

// clear 64 KB per block while switching (about 16 blocks for 1 MB)
void FxSlot::clearStep()
{
    uint32_t chunk = 65536;
    if (clearPos_ + chunk > bytes_) chunk = bytes_ - clearPos_;
    memset(mem_ + clearPos_, 0, chunk);
    clearPos_ += chunk;
    if (clearPos_ >= bytes_) clearing_ = false;
}

void FxSlot::process(const float* inL, const float* inR, float* outL, float* outR, int n, float barFrames)
{
    if (clearing_)
    {
        clearStep();
        for (int i = 0; i < n; i++)
            outL[i] = outR[i] = 0;
        return;
    }
    switch (type_)
    {
        case kFxDelay: procDelay(inL, inR, outL, outR, n, barFrames, false, false); break;
        case kFxPingPong: procDelay(inL, inR, outL, outR, n, barFrames, true, false); break;
        case kFxTape: procDelay(inL, inR, outL, outR, n, barFrames, false, true); break;
        case kFxReverb: procReverb(inL, inR, outL, outR, n, false); break;
        case kFxShimmer: procReverb(inL, inR, outL, outR, n, true); break;
        case kFxChorus: procChorus(inL, inR, outL, outR, n, false); break;
        case kFxFlanger: procChorus(inL, inR, outL, outR, n, true); break;
        case kFxClouds: procClouds(inL, inR, outL, outR, n); break;
        case kFxPhaser: procPhaser(inL, inR, outL, outR, n); break;
        case kFxReverse: procReverse(inL, inR, outL, outR, n, barFrames); break;
        case kFxPrime: procPrime(inL, inR, outL, outR, n, barFrames); break;
        case kFxLofi: procLofi(inL, inR, outL, outR, n); break;
        default:
            for (int i = 0; i < n; i++)
                outL[i] = outR[i] = 0;
    }
}

// linear-interpolated read from the int16 stereo line, d frames behind the write head
static inline float lineRead(const int16_t* line, uint32_t frames, uint32_t wr, float d, int c)
{
    float rp = (float)wr - d;
    while (rp < 0) rp += (float)frames;
    uint32_t i0 = (uint32_t)rp;
    float fr = rp - (float)i0;
    if (i0 >= frames) i0 -= frames;
    uint32_t i1 = i0 + 1;
    if (i1 >= frames) i1 = 0;
    float a = line[i0 * 2 + c], b = line[i1 * 2 + c];
    return a + (b - a) * fr;
}

void FxSlot::procDelay(const float* inL, const float* inR, float* outL, float* outR, int n, float barFrames, bool ping,
                       bool tape)
{
    float target = kDelayTimeBars[delayTimeIndex(knob[0])] * barFrames;
    float maxD = (float)lineFrames_ - 4.0f;
    float targetR = target;
    if (!ping && !tape) targetR = target * (1.0f + knob[3] * 0.005f);   // spread: right side later
    if (target > maxD) target = maxD;
    if (targetR > maxD) targetR = maxD;
    if (curDelay_[0] <= 0) curDelay_[0] = target, curDelay_[1] = targetR;

    float fb = knob[1] * 0.0098f;
    float toneHz = expMap(knob[2], 400.0f, 18000.0f);
    if (tape) toneHz = 5200.0f;
    tone_[0].setHz(toneHz);
    tone_[1].setHz(toneHz);
    float wow = tape ? knob[2] * 0.01f : 0.0f;
    float drive = tape ? 1.0f + knob[3] * 0.04f : 1.0f;
    float width = ping ? knob[3] * 0.01f : 1.0f;
    const float glide = 0.00025f;     // tape-like glide to a new time

    for (int i = 0; i < n; i++)
    {
        curDelay_[0] += (target - curDelay_[0]) * glide;
        curDelay_[1] += (targetR - curDelay_[1]) * glide;
        float dL = curDelay_[0], dR = curDelay_[1];
        if (tape)
        {
            ph_ += 0.6f / kSampleRate;
            ph2_ += 6.5f / kSampleRate;
            if (ph_ > 1) ph_ -= 1;
            if (ph2_ > 1) ph2_ -= 1;
            float m = (sin01(ph_) * 60.0f + sin01(ph2_) * 6.0f) * wow;
            dL += m;
            dR += m * 0.9f;
        }
        float yl = lineRead(line_, lineFrames_, wr_, dL, 0);
        float yr = lineRead(line_, lineFrames_, wr_, dR, 1);
        float tl = tone_[0].lp(yl), tr = tone_[1].lp(yr);
        float wl, wr;
        if (ping)
        {
            // mono in to the left line; the lines feed each other
            float mono = (inL[i] + inR[i]) * 0.5f;
            wl = mono + tr * fb;
            wr = tl * fb;
        }
        else
        {
            wl = inL[i] + tl * fb;
            wr = inR[i] + tr * fb;
        }
        if (tape)
        {
            wl = softClip(wl * drive * (1.0f / 32768.0f)) * 32768.0f / drive;
            wr = softClip(wr * drive * (1.0f / 32768.0f)) * 32768.0f / drive;
        }
        line_[wr_ * 2] = sat16(wl);
        line_[wr_ * 2 + 1] = sat16(wr);
        if (++wr_ >= lineFrames_) wr_ = 0;
        if (ping)
        {
            float m = (tl + tr) * 0.5f;
            outL[i] = m + (tl - m) * width;
            outR[i] = m + (tr - m) * width;
        }
        else
        {
            outL[i] = tl;
            outR[i] = tr;
        }
    }
    vis[0] = curDelay_[0];
    vis[1] = curDelay_[1];
    vis[2] = fb;
    visCount = 3;
    lfo = ph_;
}

void FxSlot::procReverb(const float* inL, const float* inR, float* outL, float* outR, int n, bool shimmer)
{
    float T = expMap(knob[0], 0.3f, 9.0f);
    float toneHz = expMap(knob[1], 1500.0f, 16000.0f);
    float lpA = 1.0f - expf(-kTwoPi * toneHz / kSampleRate);
    float width = (shimmer ? knob[3] : knob[2]) * 0.01f;
    float shimAmt = shimmer ? knob[2] * 0.006f : 0.0f;
    float g[kFdn];
    for (int k = 0; k < kFdn; k++)
        g[k] = powf(10.0f, -3.0f * fdnLen_[k] / (T * kSampleRate));
    uint32_t pre = shimmer ? 0 : (uint32_t)(knob[3] * kSampleRate / 1000.0f);
    if (pre >= lineFrames_) pre = lineFrames_ - 1;

    for (int i = 0; i < n; i++)
    {
        float xl = inL[i] * (1.0f / 32768.0f), xr = inR[i] * (1.0f / 32768.0f);
        if (!shimmer)
        {
            // pre-delay through the int16 line
            line_[wr_ * 2] = sat16(inL[i]);
            line_[wr_ * 2 + 1] = sat16(inR[i]);
            uint32_t rp = (wr_ + lineFrames_ - pre) % lineFrames_;
            xl = line_[rp * 2] * (1.0f / 32768.0f);
            xr = line_[rp * 2 + 1] * (1.0f / 32768.0f);
            if (++wr_ >= lineFrames_) wr_ = 0;
        }
        // input diffusion on each side
        float d[2] = {xl, xr};
        for (int a = 0; a < 4; a++)
        {
            int c = a & 1;
            float* buf = ap_[a];
            float v = buf[apIdx_[a]];
            float w = d[c] + 0.6f * v;
            buf[apIdx_[a]] = w;
            d[c] = v - 0.6f * w;
            if (++apIdx_[a] >= apLen_[a]) apIdx_[a] = 0;
        }
        // shimmer: octave-up of the tail back into the input
        float shimIn = 0;
        if (shimmer)
        {
            const int W = 2048;
            float mono = 0;
            for (int k = 0; k < kFdn; k++)
                mono += fdn_[k][fdnIdx_[k]];
            mono *= 0.25f;
            shift_[shiftIdx_] = mono;
            shiftPh_ += 1.0f / W;      // read pointer moves 2x: grain sweep
            if (shiftPh_ >= 1.0f) shiftPh_ -= 1.0f;
            float sum = 0;
            for (int gI = 0; gI < 2; gI++)
            {
                float p = shiftPh_ + gI * 0.5f;
                if (p >= 1.0f) p -= 1.0f;
                float back = (1.0f - p) * W;         // distance behind the write head shrinks: pitch up
                int idx = shiftIdx_ - (int)back;
                while (idx < 0) idx += 4096;
                sum += shift_[idx & 4095] * hann(p);
            }
            if (++shiftIdx_ >= 4096) shiftIdx_ = 0;
            shimIn = sum * shimAmt;
        }
        // FDN with Householder mixing
        float o[kFdn];
        float s = 0;
        for (int k = 0; k < kFdn; k++)
        {
            o[k] = fdn_[k][fdnIdx_[k]];
            s += o[k];
        }
        s *= 2.0f / kFdn;
        float outl = 0, outr = 0;
        for (int k = 0; k < kFdn; k++)
        {
            float v = (o[k] - s) * g[k];
            fdnLp_[k] += lpA * (v - fdnLp_[k]);
            float inj = (k < 4 ? d[0] : d[1]) * 0.35f + shimIn;
            fdn_[k][fdnIdx_[k]] = fdnLp_[k] + inj;
            if (++fdnIdx_[k] >= fdnLen_[k]) fdnIdx_[k] = 0;
            if (k & 1)
                outr += o[k];
            else
                outl += o[k];
        }
        float m = (outl + outr) * 0.5f;
        outl = m + (outl - m) * width;
        outr = m + (outr - m) * width;
        outL[i] = outl * 0.5f * 32768.0f;
        outR[i] = outr * 0.5f * 32768.0f;
    }
    vis[0] = T;
    vis[1] = toneHz;
    visCount = 2;
}

void FxSlot::procChorus(const float* inL, const float* inR, float* outL, float* outR, int n, bool flanger)
{
    float rate = flanger ? expMap(knob[0], 0.05f, 2.0f) : expMap(knob[0], 0.1f, 5.0f);
    float depth = knob[1] * 0.01f;
    float fb = flanger ? (knob[2] - 50) * 0.019f : 0.0f;
    float width = flanger ? 0.5f : knob[2] * 0.01f;
    float base = flanger ? expMap(knob[3], 0.5f, 8.0f) * kSampleRate / 1000.0f : 12.0f * kSampleRate / 1000.0f;
    float range = flanger ? base * 0.9f : 7.0f * kSampleRate / 1000.0f;
    uint32_t frames = 4096;
    if (frames > lineFrames_) frames = lineFrames_;
    for (int i = 0; i < n; i++)
    {
        ph_ += rate / kSampleRate;
        if (ph_ >= 1) ph_ -= 1;
        float pr = ph_ + width * 0.5f;
        float ml = sin01(ph_), mr = sin01(pr);
        float dl = base + range * depth * (flanger ? (ml * 0.5f + 0.5f) - 0.5f : ml) + 2.0f;
        float dr = base + range * depth * (flanger ? (mr * 0.5f + 0.5f) - 0.5f : mr) + 2.0f;
        if (dl < 2) dl = 2;
        if (dr < 2) dr = 2;
        float yl = lineRead(line_, frames, wr_, dl, 0);
        float yr = lineRead(line_, frames, wr_, dr, 1);
        line_[wr_ * 2] = sat16(inL[i] + yl * fb);
        line_[wr_ * 2 + 1] = sat16(inR[i] + yr * fb);
        if (++wr_ >= frames) wr_ = 0;
        outL[i] = yl;
        outR[i] = yr;
    }
    lfo = ph_;
}

void FxSlot::procPhaser(const float* inL, const float* inR, float* outL, float* outR, int n)
{
    float rate = expMap(knob[0], 0.1f, 5.0f);
    float depth = knob[1] * 0.01f;
    float fb = knob[2] * 0.0085f;
    float width = knob[3] * 0.01f;
    const int stages = 6;
    for (int i = 0; i < n; i++)
    {
        ph_ += rate / kSampleRate;
        if (ph_ >= 1) ph_ -= 1;
        for (int c = 0; c < 2; c++)
        {
            float p = ph_ + (c ? width * 0.5f : 0.0f);
            float m = sin01(p) * 0.5f + 0.5f;
            float hz = 200.0f * powf(20.0f, m * depth);
            float t = tanf(kPi * hz / kSampleRate);
            float a = (t - 1.0f) / (t + 1.0f);
            float x = (c ? inR[i] : inL[i]) * (1.0f / 32768.0f) + phFb_[c] * fb;
            for (int s = 0; s < stages; s++)
            {
                float y = a * x + apz_[c][s];
                apz_[c][s] = x - a * y;
                x = y;
            }
            phFb_[c] = x;
            (c ? outR : outL)[i] = x * 32768.0f;
        }
    }
    lfo = ph_;
}

void FxSlot::procClouds(const float* inL, const float* inR, float* outL, float* outR, int n)
{
    uint32_t frames = (uint32_t)(4.0f * kSampleRate);
    if (frames > lineFrames_) frames = lineFrames_;
    bool freeze = knob[7] >= 50;
    float size = expMap(knob[1], 20.0f, 1000.0f) * kSampleRate / 1000.0f;
    float pitch = powf(2.0f, ((knob[2] - 50) * 24.0f / 50.0f) / 12.0f);
    float texture = knob[4] * 0.01f;
    float fbk = knob[5] * 0.009f;
    float spread = knob[6] * 0.01f;
    int dens = knob[3] - 50;      // deadband at noon
    float back = knob[0] * 0.01f * ((float)frames - size * 2.0f - 64.0f);
    if (back < 0) back = 0;

    // spawn interval in samples
    float interval = 0;
    bool random = false;
    if (dens > 3)
    {
        random = true;
        float overl = 1.0f + (dens - 3) * (11.0f / 47.0f);      // up to 12 overlapping
        interval = size / overl;
    }
    else if (dens < -3)
    {
        float k = 1.0f + (-dens - 3) * (3.0f / 47.0f);          // a grain every 1..4 lengths
        interval = size * k;
    }

    for (int i = 0; i < n; i++)
    {
        float sl = 0, sr = 0;
        for (int g = 0; g < 12; g++)
        {
            CGrain& gr = cg_[g];
            if (!gr.on) continue;
            float t = (float)gr.age / gr.len;
            float w = shapedWindow(t, texture);
            float p = gr.pos;
            while (p < 0) p += frames;
            while (p >= frames) p -= frames;
            uint32_t i0 = (uint32_t)p;
            float fr = p - i0;
            uint32_t i1 = (i0 + 1 >= frames) ? 0 : i0 + 1;
            float a = line_[i0 * 2] + (line_[i1 * 2] - line_[i0 * 2]) * fr;
            float b = line_[i0 * 2 + 1] + (line_[i1 * 2 + 1] - line_[i0 * 2 + 1]) * fr;
            sl += a * w * gr.gl;
            sr += b * w * gr.gr;
            gr.pos += gr.step;
            if (++gr.age >= gr.len) gr.on = false;
        }
        if (!freeze)
        {
            line_[wr_ * 2] = sat16(inL[i] + cloudFb_[0] * fbk);
            line_[wr_ * 2 + 1] = sat16(inR[i] + cloudFb_[1] * fbk);
            if (++wr_ >= frames) wr_ = 0;
        }
        cloudFb_[0] = sl;
        cloudFb_[1] = sr;
        outL[i] = sl * 0.6f;
        outR[i] = sr * 0.6f;

        // spawn
        if (interval > 0)
        {
            spawnAcc_ -= 1.0f;
            if (spawnAcc_ <= 0)
            {
                spawnAcc_ += random ? interval * (0.2f + 1.6f * rng_.uni()) : interval;
                for (int g = 0; g < 12; g++)
                {
                    CGrain& gr = cg_[g];
                    if (gr.on) continue;
                    gr.on = true;
                    gr.len = (int)size;
                    gr.age = 0;
                    gr.step = pitch;
                    // start far enough behind the write head that a fast grain never overtakes it
                    float need = size * (pitch > 1.0f ? pitch - 1.0f : 0.0f) + 32.0f;
                    float jitter = random ? rng_.uni() * size * 0.5f : 0.0f;
                    float b = back + need + jitter;
                    gr.pos = (float)wr_ - b;
                    float pan = rng_.bi() * spread;
                    gr.gl = 1.0f - (pan > 0 ? pan : 0);
                    gr.gr = 1.0f + (pan < 0 ? pan : 0);
                    break;
                }
            }
        }
    }
    // picture: grain positions relative to the buffer
    visCount = 0;
    for (int g = 0; g < 12 && visCount < 16; g++)
        if (cg_[g].on)
        {
            float p = cg_[g].pos;
            while (p < 0) p += frames;
            float rel = (float)wr_ - p;
            while (rel < 0) rel += frames;
            vis[visCount++] = 1.0f - rel / frames;
        }
}

void FxSlot::procReverse(const float* inL, const float* inR, float* outL, float* outR, int n, float barFrames)
{
    uint32_t D = (uint32_t)(kDelayTimeBars[delayTimeIndex(knob[0])] * barFrames);
    if (D < 512) D = 512;
    if (D * 2 + 8 > lineFrames_) D = (lineFrames_ - 8) / 2;
    float fb = knob[1] * 0.009f;
    tone_[0].setHz(expMap(knob[2], 400.0f, 18000.0f));
    tone_[1].setHz(expMap(knob[2], 400.0f, 18000.0f));
    float width = knob[3] * 0.01f;
    const uint32_t fade = 441;
    for (int i = 0; i < n; i++)
    {
        if (revAge_ >= revChunk_ || revChunk_ == 0)
        {
            revChunk_ = D;
            revAge_ = 0;
            revStart_ = wr_;       // read the chunk just written, backwards from here
        }
        int32_t idx = (int32_t)revStart_ - 1 - (int32_t)revAge_;
        while (idx < 0) idx += lineFrames_;
        float w = 1.0f;
        if (revAge_ < fade) w = (float)revAge_ / fade;
        else if (revChunk_ - revAge_ < fade) w = (float)(revChunk_ - revAge_) / fade;
        // stereo offset: the right side reads half a chunk later
        int32_t idxR = idx - (int32_t)(width * revChunk_ * 0.25f);
        while (idxR < 0) idxR += lineFrames_;
        float yl = tone_[0].lp(line_[idx * 2] * w);
        float yr = tone_[1].lp(line_[idxR * 2 + 1] * w);
        line_[wr_ * 2] = sat16(inL[i] + yl * fb);
        line_[wr_ * 2 + 1] = sat16(inR[i] + yr * fb);
        if (++wr_ >= lineFrames_) wr_ = 0;
        revAge_++;
        outL[i] = yl;
        outR[i] = yr;
    }
    vis[0] = (float)revAge_ / (revChunk_ ? revChunk_ : 1);
    visCount = 1;
}

// two delays (A and A x 1.5) with slow modulation and a band-limited loop: the
// early-80s rack delay sound
void FxSlot::procPrime(const float* inL, const float* inR, float* outL, float* outR, int n, float barFrames)
{
    float A = kDelayTimeBars[delayTimeIndex(knob[0])] * barFrames;
    float B = A * 1.5f;
    float maxD = (float)lineFrames_ - 600.0f;
    if (B > maxD) B = maxD;
    if (A > maxD) A = maxD;
    float fb = knob[1] * 0.0092f;
    float mod = knob[2] * 0.01f * 0.004f * kSampleRate;     // up to 4 ms
    float toneHz = expMap(knob[3], 1200.0f, 12000.0f);
    tone_[0].setHz(toneHz);
    tone_[1].setHz(toneHz);
    if (curDelay_[0] <= 0) curDelay_[0] = A, curDelay_[1] = B;
    for (int i = 0; i < n; i++)
    {
        curDelay_[0] += (A - curDelay_[0]) * 0.0005f;
        curDelay_[1] += (B - curDelay_[1]) * 0.0005f;
        ph_ += 0.53f / kSampleRate;
        ph2_ += 0.37f / kSampleRate;
        if (ph_ >= 1) ph_ -= 1;
        if (ph2_ >= 1) ph2_ -= 1;
        float ya = lineRead(line_, lineFrames_, wr_, curDelay_[0] + mod * (sin01(ph_) + 1.0f) + 4, 0);
        float yb = lineRead(line_, lineFrames_, wr_, curDelay_[1] + mod * (sin01(ph2_) + 1.0f) + 4, 1);
        ya = tone_[0].lp(ya);
        yb = tone_[1].lp(yb);
        float mono = (inL[i] + inR[i]) * 0.5f;
        // 12-bit-ish converter grit in the loop
        float wa = mono + (ya + yb) * 0.5f * fb;
        float wb = mono + (ya + yb) * 0.5f * fb;
        line_[wr_ * 2] = sat16((float)((int)(wa) & ~7));
        line_[wr_ * 2 + 1] = sat16((float)((int)(wb) & ~7));
        if (++wr_ >= lineFrames_) wr_ = 0;
        outL[i] = ya;
        outR[i] = yb;
    }
    vis[0] = curDelay_[0];
    vis[1] = curDelay_[1];
    visCount = 2;
    lfo = ph_;
}

void FxSlot::procLofi(const float* inL, const float* inR, float* outL, float* outR, int n)
{
    int bits = 16 - knob[0] * 14 / 100;
    float stepQ = (float)(1 << (16 - bits));
    float rate = 1.0f / (1 + knob[1] * 31 / 100);
    float drive = 1.0f + knob[2] * 0.06f;
    float toneHz = expMap(knob[3], 800.0f, 18000.0f);
    tone_[0].setHz(toneHz);
    tone_[1].setHz(toneHz);
    for (int i = 0; i < n; i++)
    {
        holdAcc_ += rate;
        if (holdAcc_ >= 1.0f)
        {
            holdAcc_ -= 1.0f;
            hold_[0] = inL[i];
            hold_[1] = inR[i];
        }
        for (int c = 0; c < 2; c++)
        {
            float x = softClip(hold_[c] * drive * (1.0f / 32768.0f)) * 32768.0f;
            x = floorf(x / stepQ) * stepQ;
            (c ? outR : outL)[i] = tone_[c].lp(x);
        }
    }
}

} // namespace lp

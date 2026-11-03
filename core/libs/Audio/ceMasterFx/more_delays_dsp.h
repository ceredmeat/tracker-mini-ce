/*
 * more_delays_dsp.h: four more master delays for tracker-mini-ce (Prime, Reverse, Multi-tap,
 * Lo-fi). Include it once, in ceMasterFx.c, after tape_dsp.h (it uses tape_target, to16,
 * tape_sat and the DL_* fields of the delay parameter block).
 *
 * All of them use the stock delay's buffers (three 2^18-sample 16-bit lines) and the
 * stock knobs: Time (or Sync + Rate) in DL_TIME, Feedback (byte 0..252), Filter (the
 * one-pole lowpass pole) and PingPong. The send is mono; the return is stereo, wet only.
 *
 * Prime: a dual digital delay in the style of the early-80s studio units (Lexicon
 *   PrimeTime and friends). Two delays, A = Time on the left and B = 3/4 of it on the
 *   right; a slow "VCO sweep" moves them in opposite directions (0.3 Hz, about 0.5 %);
 *   the converters are band-limited (about 12 kHz) and gain-ranged, 12 bits with the
 *   step growing with the level, so loud repeats get a little grain. PingPong off: each
 *   delay feeds itself; on: they feed each other (A -> B -> A).
 * Reverse: each Time-long piece of the send comes back played backwards (two readers half
 *   a piece apart, crossfaded, so there are no clicks). Feedback sends the reversed sound
 *   round again. PingPong on: the two readers go to opposite sides.
 * Multi-tap: four taps at 1/4, 2/4, 3/4 and all of Time, getting louder towards the last
 *   one; the last feeds back. PingPong off: all in the middle; on: left, right, left, right.
 * Lo-fi: a cheap digital delay: 8-bit samples at a third of the sample rate (14.7 kHz),
 *   with the aliasing that brings, and the grit builds up with every repeat. PingPong as
 *   the stock delay (repeats alternate right and left).
 */
#ifndef MORE_DELAYS_DSP_H
#define MORE_DELAYS_DSP_H

#include <stdint.h>

#define MD_MAGIC        0x5359444Du         /* "MDYS": laid out for buf[] */
#define MD_FB_MAX       0.98f
#define MD_SWEEP_C      0.99999513f         /* per sample: cos, sin of 2 pi 0.3 Hz / 44100 */
#define MD_SWEEP_S      0.00004274f
#define MD_SWEEP_DEPTH  0.005f              /* of the delay */
#define MD_BAND         0.82f               /* one-pole lowpass, about 12 kHz */
#define MD_REV_MAX      (TAPE_LEN / 2u - 256u)

struct md_state {
    uint32_t magic;
    uint32_t buf[3];
    uint32_t wpos;
    uint32_t phase;                         /* Reverse: position in the piece */
    uint32_t hold;                          /* Lo-fi: samples left of the held value */
    float held[2];                          /* Lo-fi: the held value per line */
    float lfo[2];                           /* Prime: cos, sin */
    float band[2];                          /* Prime: converter lowpass per line */
    float lp_x[2], lp_y[2];                 /* the Filter knob's lowpass per output */
    float fb_x, fb_y;                       /* the Filter knob on the feedback (one line) */
};
static struct md_state md_state_ram;
#define MD (&md_state_ram)

__attribute__((unused)) static void md_init(uint32_t buf0, uint32_t buf1, uint32_t buf2)
{
    MD->buf[0] = buf0;
    MD->buf[1] = buf1;
    MD->buf[2] = buf2;
    MD->wpos = 0;
    MD->phase = 0;
    MD->hold = 0;
    MD->held[0] = MD->held[1] = 0.0f;
    MD->lfo[0] = 1.0f;
    MD->lfo[1] = 0.0f;
    MD->band[0] = MD->band[1] = 0.0f;
    MD->lp_x[0] = MD->lp_x[1] = MD->lp_y[0] = MD->lp_y[1] = 0.0f;
    MD->fb_x = MD->fb_y = 0.0f;
    MD->magic = MD_MAGIC;
}

static inline float md_gain(const uint8_t *obj)
{
    float g = (float)*(volatile uint8_t *)(obj + DL_FEEDBACK) * (MD_FB_MAX / 252.0f);
    return g > MD_FB_MAX ? MD_FB_MAX : g;
}

static inline float md_pole(const uint8_t *obj)
{
    float pole = -(float)*(volatile int32_t *)(obj + DL_FILTER_POLE) * (1.0f / 1023.0f);
    if (!(pole >= POLE_MIN))
        pole = POLE_MIN;
    if (pole > POLE_MAX)
        pole = POLE_MAX;
    return pole;
}

static inline float md_read(const int16_t *q, uint32_t pos, uint32_t back)
{
    return (float)q[(pos - back) & TAPE_MASK] * (1.0f / 32768.0f);
}

/* linear interpolation, `back` samples (fractional) behind pos */
static inline float md_read_frac(const int16_t *q, uint32_t pos, float back)
{
    uint32_t whole = (uint32_t)back;
    float frac = back - (float)whole;
    float a = q[(pos - whole) & TAPE_MASK], b = q[(pos - whole - 1u) & TAPE_MASK];
    return (a + (b - a) * frac) * (1.0f / 32768.0f);
}

static inline float md_lp(float x, float *px, float *py, float pole)
{
    *py = 0.5f * (1.0f - pole) * (x + *px) + pole * *py;
    *px = x;
    return *py;
}

/* gain-ranged 12-bit converter: the step grows with the level */
static inline float md_gainrange(float x)
{
    float a = x < 0.0f ? -x : x;
    float step = a > 0.5f ? (1.0f / 2048.0f) : a > 0.25f ? (1.0f / 4096.0f)
               : a > 0.125f ? (1.0f / 8192.0f) : (1.0f / 16384.0f);
    float q = x / step;
    int32_t r = (int32_t)(q + (q >= 0.0f ? 0.5f : -0.5f));
    return (float)r * step;
}

/* ---- Prime ------------------------------------------------------------------------ */
__attribute__((unused)) static void prime_process(const uint8_t *obj, const int16_t *in, int16_t *out_l, int16_t *out_r)
{
    int16_t *qa = (int16_t *)MD->buf[0], *qb = (int16_t *)MD->buf[1];
    int cross = *(volatile uint8_t *)(obj + DL_PINGPONG) != 0;
    float gain = md_gain(obj), pole = md_pole(obj);
    float ta = (float)tape_target(obj), tb = ta * 0.75f;
    uint32_t pos = MD->wpos;
    float c = MD->lfo[0], s = MD->lfo[1];
    for (uint32_t n = 0; n < BLOCK; n++) {
        float nc = c * MD_SWEEP_C - s * MD_SWEEP_S;
        s = s * MD_SWEEP_C + c * MD_SWEEP_S;
        c = nc;
        float ya = md_read_frac(qa, pos, ta * (1.0f + MD_SWEEP_DEPTH * s) + 2.0f);
        float yb = md_read_frac(qb, pos, tb * (1.0f - MD_SWEEP_DEPTH * s) + 2.0f);
        ya = md_lp(ya, &MD->lp_x[0], &MD->lp_y[0], pole);
        yb = md_lp(yb, &MD->lp_x[1], &MD->lp_y[1], pole);
        float x = (float)in[n] * (1.0f / 32768.0f);
        float ia = x + gain * (cross ? yb : ya);
        float ib = x + gain * (cross ? ya : yb);
        MD->band[0] += MD_BAND * (ia - MD->band[0]);
        MD->band[1] += MD_BAND * (ib - MD->band[1]);
        qa[pos] = to16(md_gainrange(tape_sat(MD->band[0])));
        qb[pos] = to16(md_gainrange(tape_sat(MD->band[1])));
        out_l[n] = to16(ya);
        out_r[n] = to16(yb);
        pos = (pos + 1u) & TAPE_MASK;
    }
    float g = 1.5f - 0.5f * (c * c + s * s);   /* keep the oscillator on the circle */
    MD->lfo[0] = c * g;
    MD->lfo[1] = s * g;
    MD->wpos = pos;
}

/* ---- Reverse ---------------------------------------------------------------------- */
__attribute__((unused)) static void reverse_process(const uint8_t *obj, const int16_t *in, int16_t *out_l, int16_t *out_r)
{
    int16_t *q = (int16_t *)MD->buf[0];
    int split = *(volatile uint8_t *)(obj + DL_PINGPONG) != 0;
    float gain = md_gain(obj), pole = md_pole(obj);
    uint32_t len = tape_target(obj);
    if (len > MD_REV_MAX)
        len = MD_REV_MAX;
    if (len < 64u)
        len = 64u;
    uint32_t half = len / 2u;
    uint32_t pos = MD->wpos, p = MD->phase;
    if (p >= len)
        p = 0;
    float inv_half = 1.0f / (float)half;
    for (uint32_t n = 0; n < BLOCK; n++) {
        uint32_t p2 = p + half;
        if (p2 >= len)
            p2 -= len;
        /* each reader goes back through the last piece at double speed: reversed */
        float r1 = md_read(q, pos, 2u * p + 1u);
        float r2 = md_read(q, pos, 2u * p2 + 1u);
        /* triangle windows, half a piece apart, sum to one */
        float w1 = p < half ? (float)p * inv_half : (float)(len - p) * inv_half;
        float w2 = 1.0f - w1;
        float a = md_lp(r1 * w1, &MD->lp_x[0], &MD->lp_y[0], pole);
        float b = md_lp(r2 * w2, &MD->lp_x[1], &MD->lp_y[1], pole);
        float x = (float)in[n] * (1.0f / 32768.0f);
        q[pos] = to16(tape_sat(x + gain * (a + b)));
        if (split) {
            out_l[n] = to16(a * 1.4f);
            out_r[n] = to16(b * 1.4f);
        } else {
            out_l[n] = out_r[n] = to16(a + b);
        }
        pos = (pos + 1u) & TAPE_MASK;
        if (++p >= len)
            p = 0;
    }
    MD->wpos = pos;
    MD->phase = p;
}

/* ---- Multi-tap -------------------------------------------------------------------- */
__attribute__((unused)) static void multitap_process(const uint8_t *obj, const int16_t *in, int16_t *out_l, int16_t *out_r)
{
    static const float tap_gain[4] = {0.35f, 0.5f, 0.7f, 1.0f};
    int16_t *q = (int16_t *)MD->buf[0];
    int spread = *(volatile uint8_t *)(obj + DL_PINGPONG) != 0;
    float gain = md_gain(obj), pole = md_pole(obj);
    uint32_t t = tape_target(obj);
    uint32_t tap[4] = {t / 4u, t / 2u, (3u * t) / 4u, t};
    for (uint32_t k = 0; k < 4; k++)
        if (tap[k] < 1u)
            tap[k] = 1u;
    uint32_t pos = MD->wpos;
    for (uint32_t n = 0; n < BLOCK; n++) {
        float y[4];
        for (uint32_t k = 0; k < 4; k++)
            y[k] = md_read(q, pos, tap[k]) * tap_gain[k];
        float l, r;
        if (spread) {
            l = y[0] + y[2];
            r = y[1] + y[3];
        } else {
            l = r = 0.6f * (y[0] + y[1] + y[2] + y[3]);
        }
        l = md_lp(l, &MD->lp_x[0], &MD->lp_y[0], pole);
        r = md_lp(r, &MD->lp_x[1], &MD->lp_y[1], pole);
        float fb = md_lp(y[3], &MD->fb_x, &MD->fb_y, pole);
        float x = (float)in[n] * (1.0f / 32768.0f);
        q[pos] = to16(tape_sat(x + gain * fb));
        out_l[n] = to16(l);
        out_r[n] = to16(r);
        pos = (pos + 1u) & TAPE_MASK;
    }
    MD->wpos = pos;
}

/* ---- Lo-fi ------------------------------------------------------------------------ */
static inline float md_crush(float x)       /* 8 bits */
{
    float q = x * 128.0f;
    int32_t r = (int32_t)(q + (q >= 0.0f ? 0.5f : -0.5f));
    if (r > 127)
        r = 127;
    if (r < -128)
        r = -128;
    return (float)r * (1.0f / 128.0f);
}

__attribute__((unused)) static void lofi_process(const uint8_t *obj, const int16_t *in, int16_t *out_l, int16_t *out_r)
{
    int16_t *qa = (int16_t *)MD->buf[0], *qb = (int16_t *)MD->buf[1];
    int pingpong = *(volatile uint8_t *)(obj + DL_PINGPONG) != 0;
    float gain = md_gain(obj), pole = md_pole(obj);
    uint32_t t = tape_target(obj);
    uint32_t pos = MD->wpos, hold = MD->hold;
    float ha = MD->held[0], hb = MD->held[1];
    for (uint32_t n = 0; n < BLOCK; n++) {
        float ya = md_lp(md_read(qa, pos, t), &MD->lp_x[0], &MD->lp_y[0], pole);
        float yb = md_lp(md_read(qb, pos, t), &MD->lp_x[1], &MD->lp_y[1], pole);
        float x = (float)in[n] * (1.0f / 32768.0f);
        if (hold == 0) {                    /* a new sample every third: 14.7 kHz */
            if (pingpong) {
                ha = md_crush(tape_sat(x + gain * yb));
                hb = md_crush(tape_sat(gain * ya));
            } else {
                ha = hb = md_crush(tape_sat(x + gain * ya));
            }
            hold = 3;
        }
        hold--;
        qa[pos] = to16(ha);
        qb[pos] = to16(hb);
        if (pingpong) {
            out_l[n] = to16(yb);
            out_r[n] = to16(ya);
        } else {
            out_l[n] = out_r[n] = to16(ya);
        }
        pos = (pos + 1u) & TAPE_MASK;
    }
    MD->wpos = pos;
    MD->hold = hold;
    MD->held[0] = ha;
    MD->held[1] = hb;
}

#endif

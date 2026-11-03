// tracker-mini-ce: Master FX glue. The effects (the *_dsp.h files) come from the
// masterfx patch of Modular-Trackular (MIT, Copyright (c) 2026 Modular-Trackular
// contributors), written for the Tracker+ firmware's reverb and delay objects. They read
// their knobs from fields of those objects; here two small parameter blocks laid out
// the same way stand in for the objects, filled from CE's own reverb and delay.

#include <stdint.h>
#include <string.h>

#include "ceMasterFx.h"

#include "freeverb_dsp.h"
#include "shimmer_dsp.h"
#include "plate_dsp.h"
#include "spring_dsp.h"
#include "tape_dsp.h"
#include "bbd_dsp.h"
#include "gated_dsp.h"
#include "more_delays_dsp.h"

#define CEMFX_RAM __attribute__((section(".audio_ram_functions"), used))

static uint8_t rv_obj[0x84] __attribute__((aligned(4)));
static uint8_t dl_obj[0x60] __attribute__((aligned(4)));

/* ---- the reverb ------------------------------------------------------------------- */
static struct
{
    uint8_t active, switching, target;
    uint32_t cleared;
} RVS;

static void rv_switch_to(uint8_t target)
{
    FV->magic = 0;
    PL->magic = 0;
    SP->magic = 0;
    SH->magic = 0;
    GT->magic = 0;
    RVS.cleared = 0;
    RVS.target = target;
    RVS.switching = 1;
}

static int rv_clear_step(uint16_t* l, uint16_t* r)
{
    uint32_t* wl = (uint32_t*)l;
    uint32_t* wr = (uint32_t*)r;
    uint32_t end = RVS.cleared + CLEAR_WORDS;
    if (end > BUFFER_SAMPLES / 2)
        end = BUFFER_SAMPLES / 2;
    for (uint32_t i = RVS.cleared; i < end; i++)
    {
        wl[i] = 0;
        wr[i] = 0;
    }
    RVS.cleared = end;
    return end == BUFFER_SAMPLES / 2;
}

static void rv_init(uint8_t which, uint32_t l, uint32_t r)
{
    if (which == CEMFX_REVERB_FREEVERB)
    {
        fv_init(l, r);
        FV->cleared = BUFFER_SAMPLES / 2;
    }
    else if (which == CEMFX_REVERB_PLATE)
        plate_init(l, r);
    else if (which == CEMFX_REVERB_SPRING)
        spring_init(l, r);
    else if (which == CEMFX_REVERB_SHIMMER)
        shimmer_init(l, r);
    else if (which == CEMFX_REVERB_GATED || which == CEMFX_REVERB_REVERSE)
        gated_init(l, r);
}

CEMFX_RAM int cemfx_reverb_block(uint8_t want, uint16_t* bufL, uint16_t* bufR,
                                 const cemfx_reverb_params* p, int16_t* left, int16_t* right)
{
    if (want >= CEMFX_REVERBS)
        want = CEMFX_REVERB_STOCK;
    if (!RVS.switching && want != RVS.active)
        rv_switch_to(want);
    if (RVS.switching)
    {
        if (RVS.target != want)
            rv_switch_to(want); /* changed again: start over */
        if (!rv_clear_step(bufL, bufR))
            return CEMFX_SILENT;
        rv_init(RVS.target, (uint32_t)bufL, (uint32_t)bufR);
        RVS.active = RVS.target;
        RVS.switching = 0;
        return RVS.active == CEMFX_REVERB_STOCK ? CEMFX_STOCK_RESET : CEMFX_SILENT;
    }
    if (RVS.active == CEMFX_REVERB_STOCK)
        return CEMFX_RUN_STOCK;

    *(float*)(rv_obj + RV_AMOUNT) = p->amount;
    *(float*)(rv_obj + RV_INPUT_GAIN) = p->inputGain;
    *(float*)(rv_obj + RV_SIZE) = p->size;
    *(float*)(rv_obj + RV_DIFFUSION) = p->diffusion;
    *(float*)(rv_obj + RV_LP) = p->lp;
    *(int32_t*)(rv_obj + RV_PREDELAY) = p->predelay;

    switch (RVS.active)
    {
        case CEMFX_REVERB_FREEVERB: fv_process(rv_obj, left, right, 0); break;
        case CEMFX_REVERB_PLATE: plate_process(rv_obj, left, right); break;
        case CEMFX_REVERB_SPRING: spring_process(rv_obj, left, right); break;
        case CEMFX_REVERB_SHIMMER: shimmer_process(rv_obj, left, right); break;
        case CEMFX_REVERB_GATED: gated_process(rv_obj, left, right, 0); break;
        case CEMFX_REVERB_REVERSE: gated_process(rv_obj, left, right, 1); break;
        default: return CEMFX_RUN_STOCK;
    }
    return CEMFX_DONE;
}

/* ---- the delay -------------------------------------------------------------------- */
#define DL_LINES 3u
#define DL_CLEAR_WORDS_PER_BLOCK 8192u /* 32 KB per block */

static struct
{
    uint8_t active, switching, target;
    uint32_t cleared;
} DLS;

static void dl_switch_to(uint8_t target)
{
    TP->magic = 0;
    BB->magic = 0;
    MD->magic = 0;
    DLS.cleared = 0;
    DLS.target = target;
    DLS.switching = 1;
}

static int dl_clear_step(int16_t* const* buf)
{
    const uint32_t words = TAPE_LEN / 2u;
    uint32_t total = DL_LINES * words;
    uint32_t left = DL_CLEAR_WORDS_PER_BLOCK;
    while (left && DLS.cleared < total)
    {
        uint32_t b = DLS.cleared / words, at = DLS.cleared % words;
        uint32_t n = words - at;
        if (n > left)
            n = left;
        memset((uint32_t*)buf[b] + at, 0, n * 4u);
        DLS.cleared += n;
        left -= n;
    }
    return DLS.cleared >= total;
}

CEMFX_RAM int cemfx_delay_block(uint8_t want, int16_t* buf0, int16_t* buf1, int16_t* buf2,
                                const cemfx_delay_params* p, const int16_t* in, int16_t* outL,
                                int16_t* outR)
{
    static const int16_t silence[BLOCK] = { 0 };
    int16_t* buf[DL_LINES] = { buf0, buf1, buf2 };

    if (want >= CEMFX_DELAYS)
        want = CEMFX_DELAY_STOCK;

    *(uint32_t*)(dl_obj + DL_TIME) = p->time;
    dl_obj[DL_FEEDBACK] = p->feedback;
    dl_obj[DL_BYPASS] = 0;
    dl_obj[DL_PINGPONG] = p->pingpong;
    *(int32_t*)(dl_obj + DL_FILTER_POLE) = p->filterB1;

    if (!DLS.switching && want != DLS.active)
        dl_switch_to(want);
    if (DLS.switching)
    {
        if (DLS.target != want)
            dl_switch_to(want);
        if (!dl_clear_step(buf))
            return CEMFX_SILENT;
        switch (DLS.target)
        {
            case CEMFX_DELAY_TAPE: tape_init(dl_obj, (uint32_t)buf0, (uint32_t)buf1); break;
            case CEMFX_DELAY_BBD: bbd_init(dl_obj, (uint32_t)buf0, (uint32_t)buf1); break;
            case CEMFX_DELAY_STOCK: break;
            default: md_init((uint32_t)buf0, (uint32_t)buf1, (uint32_t)buf2); break;
        }
        DLS.active = DLS.target;
        DLS.switching = 0;
        return DLS.active == CEMFX_DELAY_STOCK ? CEMFX_STOCK_RESET : CEMFX_SILENT;
    }
    if (DLS.active == CEMFX_DELAY_STOCK)
        return CEMFX_RUN_STOCK;

    if (!in)
        in = silence;
    switch (DLS.active)
    {
        case CEMFX_DELAY_TAPE: tape_process(dl_obj, in, outL, outR); break;
        case CEMFX_DELAY_BBD: bbd_process(dl_obj, in, outL, outR); break;
        case CEMFX_DELAY_PRIME: prime_process(dl_obj, in, outL, outR); break;
        case CEMFX_DELAY_REVERSE: reverse_process(dl_obj, in, outL, outR); break;
        case CEMFX_DELAY_MULTITAP: multitap_process(dl_obj, in, outL, outR); break;
        case CEMFX_DELAY_LOFI: lofi_process(dl_obj, in, outL, outR); break;
        default: return CEMFX_RUN_STOCK;
    }
    return CEMFX_DONE;
}

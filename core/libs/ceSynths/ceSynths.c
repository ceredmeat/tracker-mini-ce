// tracker-mini-ce: glue for the WAVE and PHASE engines (Modular-Trackular's synths patch,
// MIT, Copyright (c) 2026 Modular-Trackular contributors). The engines are included
// unchanged; CE's synth SDK voices call these functions.

#ifdef FEATURE_SYNTH

#include <stdint.h>
#include <string.h>

#include "ceSynths.h"

#include "wt_engine.h"
#include "wt_fx.h"
#include "ph_engine.h"

_Static_assert(WT_PARAMS == CESYN_ENGINE_PARAMS, "WAVE's parameter count");
_Static_assert(PH_PARAMS == CESYN_ENGINE_PARAMS, "PHASE's parameter count");

union cesyn_any {
    struct wt_voice wave;
    struct ph_voice phase;
};

size_t cesyn_voice_size(void)
{
    return sizeof(union cesyn_any);
}

void cesyn_voice_init(void* voice, int engine)
{
    memset(voice, 0, sizeof(union cesyn_any));
    if (engine == CESYN_PHASE)
        ph_voice_init((struct ph_voice*)voice, 0);
    else
        wt_voice_init((struct wt_voice*)voice, 0);
}

void cesyn_note_on(void* voice, int engine, const float* p, int note, float velocity)
{
    if (note < 0)
        note = 0;
    if (note > 127)
        note = 127;
    if (engine == CESYN_PHASE)
    {
        struct ph_voice* e = (struct ph_voice*)voice;
        e->volume = velocity;
        ph_note_on(e, p, (uint32_t)note);
    }
    else
    {
        struct wt_voice* e = (struct wt_voice*)voice;
        e->volume = velocity;
        wt_note_on(e, p, (uint32_t)note);
    }
}

void cesyn_note_off(void* voice, int engine)
{
    if (engine == CESYN_PHASE)
        ph_note_off((struct ph_voice*)voice);
    else
        wt_note_off((struct wt_voice*)voice);
}

void cesyn_set_bend(void* voice, int engine, float semitones)
{
    if (engine == CESYN_PHASE)
        ((struct ph_voice*)voice)->bend = semitones;
    else
        ((struct wt_voice*)voice)->bend = semitones;
}

int cesyn_sounding(const void* voice, int engine)
{
    if (engine == CESYN_PHASE)
        return ph_voice_sounding((const struct ph_voice*)voice);
    return wt_voice_sounding((const struct wt_voice*)voice);
}

void cesyn_render(void* voice, int engine, const float* p, float* out, float tempo)
{
    if (engine == CESYN_PHASE)
        ph_voice_render((struct ph_voice*)voice, p, out, tempo);
    else
        wt_voice_render((struct wt_voice*)voice, p, out, tempo);
}

size_t cesyn_fx_size(void)
{
    return sizeof(struct wt_fx);
}

void cesyn_fx_init(void* fx)
{
    memset(fx, 0, sizeof(struct wt_fx));
}

void cesyn_fx_run(void* fx, const float* fxParams, float* L, float* R)
{
    struct wt_fx* s = (struct wt_fx*)fx;
    for (uint32_t k = 0; k < FX_PARAMS; k++)
        s->p[k] = fxParams[k];
    wt_fx_run(s, L, R);
}

#endif

#ifndef CE_SYNTHS_H_
#define CE_SYNTHS_H_

// tracker-mini-ce: the WAVE (wavetable) and PHASE (phase distortion) synth engines.
// The DSP is C (wt_engine.h, ph_engine.h, wt_fx.h, from Modular-Trackular's synths
// patch, MIT); the voices for CE's synth SDK are in core/synth/engines/CeSynthVoices.cpp.
// Each engine reads a float array of CESYN_PARAMS values (see the engines' WP_* / PP_*
// indices) in the units the engines use.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CESYN_ENGINE_PARAMS 61 // the engines' own parameters (WT_PARAMS, PH_PARAMS)

enum
{
    CESYN_WAVE = 0,
    CESYN_PHASE = 1,
    CESYN_GRIT = 2, // WAVE's voice through a waveshaper; p[26] is the Shaper (no Digital),
                    // p[58] the Pre Gain, and grit[] the Routing, Bias, Grit Mix and Level
};

size_t cesyn_voice_size(void);
void cesyn_voice_init(void* voice, int engine);
void cesyn_note_on(void* voice, int engine, const float* p, int note, float velocity);
void cesyn_note_off(void* voice, int engine);
void cesyn_set_bend(void* voice, int engine, float semitones);
int cesyn_sounding(const void* voice, int engine);
void cesyn_render(void* voice, int engine, const float* p, float* out, float tempo,
                  const float* grit);

// the chorus (one per synth slot): fx[4] = effect (0 none, 1 chorus), rate, depth, mix;
// L is the mono input, L and R the output
size_t cesyn_fx_size(void);
void cesyn_fx_init(void* fx);
void cesyn_fx_run(void* fx, const float* fxParams, float* L, float* R);

#ifdef __cplusplus
}
#endif

#endif

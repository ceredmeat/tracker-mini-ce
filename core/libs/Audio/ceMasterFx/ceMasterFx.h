#ifndef CE_MASTER_FX_H_
#define CE_MASTER_FX_H_

// tracker-mini-ce: Master FX. More master reverbs and delays, chosen per project.
// The DSP is C (ceMasterFx.c and the *_dsp.h files, from Modular-Trackular's masterfx
// patch, MIT); the stock reverb and delay objects call it from their update().

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    CEMFX_REVERB_STOCK = 0,
    CEMFX_REVERB_FREEVERB,
    CEMFX_REVERB_PLATE,
    CEMFX_REVERB_SPRING,
    CEMFX_REVERB_SHIMMER,
    CEMFX_REVERB_GATED,
    CEMFX_REVERB_REVERSE,
    CEMFX_REVERBS
};

enum
{
    CEMFX_DELAY_STOCK = 0,
    CEMFX_DELAY_TAPE,
    CEMFX_DELAY_BBD,
    CEMFX_DELAY_PRIME,
    CEMFX_DELAY_REVERSE,
    CEMFX_DELAY_MULTITAP,
    CEMFX_DELAY_LOFI,
    CEMFX_DELAYS
};

// what the caller does after a block call
enum
{
    CEMFX_DONE = 0,        // the chosen effect processed the block: transmit it
    CEMFX_RUN_STOCK,       // run the stock effect as usual
    CEMFX_SILENT,          // switching (clearing memory): send nothing this block
    CEMFX_STOCK_RESET,     // switched back to stock: reset its state, send nothing
};

typedef struct
{
    float amount;     // return = send + (wet - send) * amount
    float inputGain;
    float size;       // Size (reverb time), 0..1
    float diffusion;
    float lp;         // 1 - Damping
    int32_t predelay; // samples, 0..5000
} cemfx_reverb_params;

typedef struct
{
    uint32_t time;     // samples
    uint8_t feedback;  // 0..252
    uint8_t pingpong;
    int32_t filterB1;  // the stock delay's Q10 filter coefficient b1 (-pole x 1023)
} cemfx_delay_params;

// Reverb: buffers are the stock reverb's two 16384-sample lines; left/right the send,
// processed in place.
int cemfx_reverb_block(uint8_t want, uint16_t* bufL, uint16_t* bufR,
                       const cemfx_reverb_params* p, int16_t* left, int16_t* right);

// Delay: buffers are the stock delay's three 2^18-sample lines; in is the mono send
// (may be NULL = silence), outL/outR the stereo return.
int cemfx_delay_block(uint8_t want, int16_t* buf0, int16_t* buf1, int16_t* buf2,
                      const cemfx_delay_params* p, const int16_t* in, int16_t* outL,
                      int16_t* outR);

#ifdef __cplusplus
}
#endif

#endif

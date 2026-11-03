#ifndef CE_LOOPER_FX_H_
#define CE_LOOPER_FX_H_

// tracker-mini-ce Looper: the send-effect slots. Each slot holds one of twelve effects
// and owns a block of the looper's memory for its delay lines.

#include "lp_dsp.h"

#include <stdint.h>

namespace lp
{

enum FxType : uint8_t
{
    kFxDelay,
    kFxPingPong,
    kFxReverb,
    kFxChorus,
    kFxFlanger,
    kFxGrains,      // our own light granular
    kFxPhaser,
    kFxShimmer,
    kFxTape,
    kFxReverse,
    kFxPrime,
    kFxLofi,
    kFxClouds,      // Mutable Instruments' Clouds (MIT), the original DSP
    kFxTypes
};

constexpr int kFxKnobs = 12;
constexpr int kFxReturnKnob = 15;   // the UI's index for the return level

struct FxDesc
{
    const char* name;
    uint8_t knobs;                 // 3 to 12
    const char* knobName[kFxKnobs];
    uint8_t def[kFxKnobs];
    uint8_t max[kFxKnobs];         // 0 = 100; else a choice (0 .. max)
};

extern const FxDesc kFxDesc[kFxTypes];

// delay time choices, in bars
constexpr int kDelayTimes = 14;
extern const float kDelayTimeBars[kDelayTimes];
extern const char* const kDelayTimeNames[kDelayTimes];
int delayTimeIndex(uint8_t knob);

// text for a knob value ("1/8", "2.4 s", "+12 st", "64 %")
void fxKnobText(int type, int knob, uint8_t value, char* out, int outLen);

class FxSlot
{
  public:
    void init(void* mem, uint32_t bytes);
    void setType(int type);            // UI: the audio side switches, clearing the memory first
    int type() const { return pendingType_; }
    void prepare();                    // main loop: Clouds' housekeeping (buffers, stretch search)
    static int knobMax(int type, int k) { return kFxDesc[type].max[k] ? kFxDesc[type].max[k] : 100; }

    uint8_t knob[kFxKnobs];

    // n samples; beatFrames = frames per beat at the current tempo
    void process(const float* inL, const float* inR, float* outL, float* outR, int n, float barFrames);

    // for the live picture
    float vis[16];
    int visCount = 0;
    float lfo = 0;
    bool frozen() const
    {
        return (type_ == kFxGrains && knob[7] >= 1) || (type_ == kFxClouds && knob[9] >= 1);
    }

  private:
    void applyType();
    void clearStep();
    void procMiClouds(const float* inL, const float* inR, float* outL, float* outR, int n);
    void procDelay(const float* inL, const float* inR, float* outL, float* outR, int n, float barFrames, bool ping, bool tape);
    void procReverb(const float* inL, const float* inR, float* outL, float* outR, int n, bool shimmer);
    void procChorus(const float* inL, const float* inR, float* outL, float* outR, int n, bool flanger);
    void procClouds(const float* inL, const float* inR, float* outL, float* outR, int n);
    void procPhaser(const float* inL, const float* inR, float* outL, float* outR, int n);
    void procReverse(const float* inL, const float* inR, float* outL, float* outR, int n, float barFrames);
    void procPrime(const float* inL, const float* inR, float* outL, float* outR, int n, float barFrames);
    void procLofi(const float* inL, const float* inR, float* outL, float* outR, int n);

    uint8_t type_ = kFxDelay;
    volatile uint8_t pendingType_ = kFxDelay;
    volatile bool preparing_ = false;
    void* clouds_ = nullptr;           // clouds::GranularProcessor, built in the slot's memory
    uint8_t cloudsMode_ = 255, cloudsQuality_ = 255;
    uint8_t* mem_ = nullptr;
    uint32_t bytes_ = 0;
    uint32_t clearPos_ = 0;
    bool clearing_ = false;

    // generic delay line (int16 stereo interleaved) over the whole memory
    int16_t* line_ = nullptr;
    uint32_t lineFrames_ = 0;
    uint32_t wr_ = 0;
    float curDelay_[2] = {0, 0};
    OnePole tone_[2];
    float fbState_[2] = {0, 0};
    float ph_ = 0, ph2_ = 0;
    Rng rng_;

    // reverb: float lines carved from the memory
    static constexpr int kFdn = 8;
    float* fdn_[kFdn];
    int fdnLen_[kFdn];
    int fdnIdx_[kFdn];
    float fdnLp_[kFdn];
    float* ap_[4];
    int apLen_[4], apIdx_[4];
    float* shift_ = nullptr;      // shimmer pitch buffer
    int shiftIdx_ = 0;
    float shiftPh_ = 0;

    // phaser
    float apz_[2][12];
    float phFb_[2] = {0, 0};

    // clouds
    struct CGrain
    {
        bool on;
        float pos, step;
        int age, len;
        float gl, gr;
    };
    CGrain cg_[12];
    float spawnAcc_ = 0;
    float cloudFb_[2] = {0, 0};

    // reverse
    uint32_t revChunk_ = 0, revAge_ = 0;
    uint32_t revStart_ = 0;

    // lofi
    float hold_[2] = {0, 0};
    float holdAcc_ = 0;
};

} // namespace lp

#endif

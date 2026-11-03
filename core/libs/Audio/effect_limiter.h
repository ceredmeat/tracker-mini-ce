#ifndef LIBS_AUDIO_EFFECT_LIMITER_H_
#define LIBS_AUDIO_EFFECT_LIMITER_H_

#include "AudioStream.h"

constexpr uint8_t LIMITER_DELAY = 5;

class AudioEffectLimiter : public AudioStream
{
  public:
    AudioEffectLimiter(void)
      : AudioStream(2, inputQueueArray)
    {
    }

    void begin();
    void setThreshold(uint16_t threshold);
    void setAttack(uint16_t attack);
    void setRelease(float release);
    void setSidechainEnable(bool isEnable);
    float getAverageGainChange();
    bool isAvailableGainToGet();
    virtual void update(void);

    void processSidechain();
    void processMainStream();
    void blockUpdate();
    void unblockUpdate();

    // tracker-mini-ce: the left limiter takes the right one's block too, so stereo master
    // effects (ceMasterInsert) see both channels of the same block before limiting
    void setStereoPartner(AudioEffectLimiter* right);

  private:
    audio_block_t* inputQueueArray[2];
    int16_t inputStorageBuffer[LIMITER_DELAY];
    int16_t outputStorageBuffer[LIMITER_DELAY];
    uint16_t threshold;
    float g = 1.0;
    uint16_t xpeak;
    float attackTime;
    float releaseTime;
    uint16_t a;
    float coeff;
    float f;
    float averageGain;
    float sumGain;
    bool isAverageGain;
    bool isSideChainEnable;
    bool isBlocked;

    AudioEffectLimiter* partner = nullptr;
    audio_block_t* heldBlock = nullptr;
    bool isRightOfPair = false;
    audio_block_t* takeInput();
};

#endif /* LIBS_AUDIO_EFFECT_LIMITER_H_ */

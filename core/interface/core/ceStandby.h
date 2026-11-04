#ifndef INTERFACE_CORE_CESTANDBY_H_
#define INTERFACE_CORE_CESTANDBY_H_

// tracker-mini-ce: Standby screen (Tracker+ and Mini). After a set time without a button,
// pad or knob, the screen shows a picture from /Images/Standby/ drifting slowly over black
// (or goes black and dark). The first press after that only wakes it up and does nothing
// else. Playback carries on. Config -> General -> Standby after (Off, 1, 2, 5, 10, 30 min)
// and Standby shows (Picture, Black), kept on the card in /Images/Standby/standby.cfg.
// Pictures: baseline JPEGs up to 800 x 480, one per standby, in name order. How big a
// picture fits depends on the screen it starts from (its controls use the same display
// memory): 640 x 352 fits on the usual screens, 480 x 272 everywhere; too big = black.

#include <stdint.h>

namespace ceStandby
{
// the Config menu's values (and their texts)
extern uint8_t timeoutSetting; // 0 Off, 1 = 1 min, 2 = 2, 3 = 5, 4 = 10, 5 = 30
extern uint8_t modeSetting;    // 0 Picture, 1 Black
void settingsChanged();

void update(); // main loop, once the tracker runs

// input: true = swallow this event (it woke the screen, or belongs to that press)
bool buttonEvent(uint8_t button, uint8_t state);
bool padEvent(uint8_t pad, uint8_t state);
bool knobEvent();
void activity(); // anything else that counts as use (the power button)

bool isActive();
} // namespace ceStandby

#endif

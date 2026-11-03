#ifndef INTERFACE_PATTERNEDITOR_CEVISUAL_H_
#define INTERFACE_PATTERNEDITOR_CEVISUAL_H_

// tracker-mini-ce: Visual. An animation over the pattern grid, driven by the notes that
// play. Pattern screen, More, More, button 7: tap = on / off, hold + turn = the style
// (Bars, Rain, Ripples, Wave, Scroll, Burst, Orbit, Tunnel). Muted tracks stay grey, the
// cursor track is brighter, a row of dots shows the step. Nothing is saved.

#include <stdint.h>

namespace ceVis
{
constexpr uint8_t kStyleCount = 8;

bool isOn();
void setOn(bool on);
uint8_t style();
void changeStyle(int dir);
const char* styleName(uint8_t style);

// called often from the Pattern screen's update; true when the canvas should be redrawn
bool tick();
// the canvas painter
void paint(int id, int x, int y, int w, int h);
} // namespace ceVis

#endif

#ifndef INTERFACE_CORE_SPLASHSCREEN_H_
#define INTERFACE_CORE_SPLASHSCREEN_H_

// tracker-mini-ce: a custom startup picture, /Images/Startup/splash.jpg, shown on the
// start screen instead of the Polyend logo (Tracker+ and Mini). A baseline JPEG of up to
// 800 x 416 pixels and 650 KB. Anything else, or any trouble while loading: the normal
// start screen.

#include <stdint.h>

namespace splash
{
// once the SD card is ready and the start project has begun loading
void begin();
// every main-loop pass while the start screen is up
void update();
// true while the picture is still loading: the start screen must stay until it is done
bool busy();
// the start screen is going away
void end();

// the same safety check for any JPEG (used by the standby screen): true if the FT812 can
// decode it for sure and its size in whole blocks fits maxW x maxH
bool checkJpeg(const char* path, uint16_t maxW, uint16_t maxH, uint16_t& w, uint16_t& h,
               uint16_t& padW, uint16_t& padH);
} // namespace splash

#endif

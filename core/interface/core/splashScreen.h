#ifndef INTERFACE_CORE_SPLASHSCREEN_H_
#define INTERFACE_CORE_SPLASHSCREEN_H_

// tracker-mini-ce: a custom startup picture, /Images/Startup/splash.jpg, shown on the
// start screen instead of the Polyend logo (Tracker+ and Mini). A baseline JPEG of up to
// 800 x 416 pixels and 650 KB. Anything else, or any trouble while loading: the normal
// start screen.

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
} // namespace splash

#endif

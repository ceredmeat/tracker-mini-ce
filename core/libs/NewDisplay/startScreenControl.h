#ifndef LIBS_STARTSCREEN_CONTROL_H_
#define LIBS_STARTSCREEN_CONTROL_H_

#include "displayControls.h"

// tracker-mini-ce: a picture from the SD card (/Images/Startup/splash.jpg) instead of the
// logo, decoded by the FT812 into RAM_G (see interface/core/splashScreen.cpp)
struct StartScreenSplash
{
    bool show = false;
    uint32_t address = 0; // RGB565 in RAM_G
    uint16_t width = 0;
    uint16_t height = 0;
};
extern StartScreenSplash startScreenSplash;

//--------------------------------------------------------------------
//--------------------------------------------------------------------
//--------------------------------------------------------------------
class cStartScreen : public cDisplayControl
{
  public:
    cStartScreen(strControlProperties* properties = nullptr);
    virtual ~cStartScreen();

    virtual void update();

    virtual void setStyle(uint32_t style);
    virtual void setText(const char* text);
    virtual void setValue(int value);
    virtual void setColors(const uint32_t* colors);
    virtual void setData(void* data);

    char* data;

    uint8_t dots;
    uint16_t textStyle;
    int16_t textFont;
};

#endif

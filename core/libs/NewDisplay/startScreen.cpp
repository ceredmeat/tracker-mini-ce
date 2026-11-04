
#include "FT812.h"

#include "display.h"
#include "displayControls.h"
#include "startScreenControl.h"

StartScreenSplash startScreenSplash;

// tracker-mini-ce: the splash look (dark teal panels, cream border and text)
static constexpr uint32_t kSplashPanel = 0x0F2630;
static constexpr uint32_t kSplashCream = 0xF1EAD0;
static constexpr uint16_t kSplashTop = 416; // the strip below the picture

// a rounded see-through panel: teal fill at ~70 %, thin cream border at ~45 %
static void splashPanel(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t r)
{
    API_COLOR(kSplashPanel);
    API_COLOR_A(178);
    API_LINE_WIDTH(r * 16); // rounded rectangle: the corners take the line width as radius
    API_BEGIN(RECTS);
    API_VERTEX2F(x0 + r, y0 + r);
    API_VERTEX2F(x1 - r, y1 - r);
    API_END();

    API_COLOR(kSplashCream);
    API_COLOR_A(115);
    API_LINE_WIDTH(12); // 0.75 px half-width: a 1.5 px line
    API_BEGIN(LINE_STRIP);
    // corner arcs from 8 points each (cos / sin of 0, 15 .. 90 degrees, x1000)
    static const int16_t c[7] = {1000, 966, 866, 707, 500, 259, 0};
    for (int k = 0; k < 7; k++) // top right
        API_VERTEX2F(x1 - r + r * c[6 - k] / 1000, y0 + r - r * c[k] / 1000);
    for (int k = 0; k < 7; k++) // bottom right
        API_VERTEX2F(x1 - r + r * c[k] / 1000, y1 - r + r * c[6 - k] / 1000);
    for (int k = 0; k < 7; k++) // bottom left
        API_VERTEX2F(x0 + r - r * c[6 - k] / 1000, y1 - r + r * c[k] / 1000);
    for (int k = 0; k < 7; k++) // top left
        API_VERTEX2F(x0 + r - r * c[k] / 1000, y0 + r - r * c[6 - k] / 1000);
    API_VERTEX2F(x1 - r, y0); // close
    API_END();
    API_COLOR_A(255);
}

//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
cStartScreen::cStartScreen(strControlProperties* properties)
  : cDisplayControl(controlIdStartScreen, 1)
{
    colorsCount = 2;
    colors = display.getTheme().startScreenColors;

    if (properties == nullptr)
    {
        posX = 0;
        posY = 0;
        text = nullptr;
        data = nullptr;
        value = 0;
        width = 0;
        height = 0;
        style = 0;
        return;
    }

    colors = (properties->colors == nullptr) ? colors : properties->colors;

    posX = properties->x;
    posY = properties->y;

    text = properties->text;
    value = properties->value;

    width = properties->w;
    height = properties->h;

    data = (char*)(properties->data);

    setStyle(properties->style);
}

cStartScreen::~cStartScreen()
{
}

//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
void cStartScreen::setStyle(uint32_t style)
{
    this->style = style;

    textStyle = (style & controlStyleCenterX ? OPT_CENTERX : 0) |
                (style & controlStyleCenterY ? OPT_CENTERY : 0) |
                (style & controlStyleRightX ? OPT_RIGHTX : 0);

    textFont = FONT_INDEX_FROM_STYLE;
    textFont = (textFont >= 0) ? textFont : 0;
    textFont = fonts[textFont].handle;
}

void cStartScreen::setText(const char* text)
{
    this->text = text;
}

void cStartScreen::setValue(int value)
{
    this->value = value;
}

void cStartScreen::setColors(const uint32_t* colors)
{
    this->colors = colors;
}

void cStartScreen::setData(void* data)
{
    data = (char*)(data);
}

//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
void cStartScreen::update()
{
#define bitmap_index 0

    API_SAVE_CONTEXT();

    const bool splash = startScreenSplash.show;
    if (splash)
    {
        // the picture, centred across the width, from the top; a strip below it
        API_BITMAP_HANDLE(13);
        API_CMD_SETBITMAP(
          startScreenSplash.address, RGB565, startScreenSplash.width, startScreenSplash.height);
        API_COLOR(0xFFFFFF);
        API_BEGIN(BITMAPS);
        API_VERTEX2F((800 - startScreenSplash.width) / 2, 0);
        API_END();
        API_BITMAP_HANDLE(0);

        API_COLOR(kSplashPanel);
        API_BEGIN(RECTS);
        API_VERTEX2F(0, startScreenSplash.height < kSplashTop ? startScreenSplash.height : kSplashTop);
        API_VERTEX2F(800, 480);
        API_END();

        // behind the text and bar, and behind the version number
        splashPanel(posX + 40, posY + 6, posX + width - 40, posY + 70, 12);
        splashPanel(4, 449, 150, 475, 8);
    }
    else
    {
        // API_BITMAP_HANDLE(0);
        API_BITMAP_SOURCE(bitmaps[bitmap_index].address);
        API_BITMAP_LAYOUT(bitmaps[bitmap_index].format,
                          bitmaps[bitmap_index].linestride,
                          bitmaps[bitmap_index].height);
        API_BITMAP_SIZE(
          NEAREST, BORDER, BORDER, bitmaps[bitmap_index].width, bitmaps[bitmap_index].height);

        API_BEGIN(BITMAPS);
        API_VERTEX2F((800 / 2) - (bitmaps[bitmap_index].width / 2), 100);
        API_END();
    }

    uint16_t barWidth = (5 * width) / 7;
    uint16_t barX = posX + (width / 7);

    uint16_t barHeight = 3;
    uint16_t barY = posY + ((3 * height) / 6);

    uint16_t textX = posX + (width / 2);
    uint16_t textY = posY + ((2 * height) / 7);

    if (value < 0)
        value = 0;
    else if (value > 100)
        value = 100;

    uint16_t barFillY = (barWidth * value) / 100;

    API_BLEND_FUNC(SRC_ALPHA, ZERO);

    API_COLOR(splash ? kSplashCream : colors[1]);

    API_LINE_WIDTH(16);
    API_BEGIN(RECTS);
    API_VERTEX2F(barX + 1, barY + 1);
    API_VERTEX2F(barX + barFillY - 1, barY + barHeight - 1);
    API_END();

    API_BLEND_FUNC(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);

    API_COLOR(splash ? kSplashCream : colors[0]);
    if (text != nullptr)
    {
        API_CMD_TEXT(textX, textY, textFont, OPT_CENTER, text);
    }
    if (data != nullptr)
    {
        API_CMD_TEXT(10, 457, textFont, 0, "v");
        API_CMD_TEXT(22, 457, textFont, 0, data);
    }

    API_RESTORE_CONTEXT();
}

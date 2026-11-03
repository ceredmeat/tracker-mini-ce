#include "ceCanvas.h"

#include "FT812.h"
#include "display.h"

cCeCanvas::cCeCanvas(strControlProperties* properties)
  : cDisplayControl(controlIdCeCanvas, 1)
{
    if (properties == nullptr) return;
    posX = properties->x;
    posY = properties->y;
    width = properties->w;
    height = properties->h;
    style = properties->style;
    value = properties->value;
    painter = (const Painter*)properties->data;
}

void cCeCanvas::update()
{
    if (painter == nullptr || painter->paint == nullptr) return;
    API_SAVE_CONTEXT();
    API_SCISSOR_XY(posX, posY);
    API_SCISSOR_SIZE(width, height);
    painter->paint(painter->id, posX, posY, width, height);
    API_RESTORE_CONTEXT();
}

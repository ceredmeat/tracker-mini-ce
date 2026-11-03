#ifndef LIBS_NEWDISPLAY_CECANVAS_H_
#define LIBS_NEWDISPLAY_CECANVAS_H_

// tracker-mini-ce: a control that draws whatever its owner's callback draws (inside its
// own rectangle). setData() takes a cCeCanvas::Painter.

#include "displayControls.h"

class cCeCanvas : public cDisplayControl
{
  public:
    struct Painter
    {
        void (*paint)(int id, int x, int y, int w, int h);
        int id;
    };

    cCeCanvas(strControlProperties* properties = nullptr);
    virtual ~cCeCanvas() {}

    virtual void update();
    virtual void setStyle(uint32_t style) { this->style = style; }
    virtual void setText(const char* text) { this->text = text; }
    virtual void setValue(int value) { this->value = value; }
    virtual void setColors(const uint32_t* colors) { this->colors = colors; }
    virtual void setData(void* data) { painter = (const Painter*)data; }

  private:
    const Painter* painter = nullptr;
};

#endif

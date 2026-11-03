#ifndef LIBS_NEWDISPLAY_LOOPERVIEW_H_
#define LIBS_NEWDISPLAY_LOOPERVIEW_H_

// tracker-mini-ce: a control that draws whatever its owner's callback draws (the Looper's
// lanes, key row, pages). setData() takes a cLooperView::Painter.

#include "displayControls.h"

class cLooperView : public cDisplayControl
{
  public:
    struct Painter
    {
        void (*paint)(int id, int x, int y, int w, int h);
        int id;
    };

    cLooperView(strControlProperties* properties = nullptr);
    virtual ~cLooperView() {}

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

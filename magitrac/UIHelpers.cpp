#include "UIHelpers.h"
#include <string.h>

void uiButton(EPD_PainterAdafruit& d,
              int x, int y, int w, int h,
              const char* label,
              uint8_t bg, uint8_t fg,
              int ts) {
    d.fillRect(x, y, w, h, bg);
    d.drawRect(x, y, w, h, fg);

    int charW  = ts * 6;
    int charH  = ts * 8;
    int labelW = (int)strlen(label) * charW;
    d.setTextSize(ts);
    d.setTextColor(fg);
    d.setCursor(x + (w - labelW) / 2, y + (h - charH) / 2);
    d.print(label);
}

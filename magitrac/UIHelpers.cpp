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

void uiButton2Line(EPD_PainterAdafruit& d,
                   int x, int y, int w, int h,
                   const char* line1, const char* line2,
                   uint8_t bg, uint8_t fg,
                   int ts) {
    d.fillRect(x, y, w, h, bg);
    d.drawRect(x, y, w, h, fg);

    int charW  = ts * 6;
    int charH  = ts * 8;
    int gap    = ts * 3;
    int blockH = charH * 2 + gap;
    int topY   = y + (h - blockH) / 2;

    d.setTextSize(ts);
    d.setTextColor(fg);

    int w1 = (int)strlen(line1) * charW;
    d.setCursor(x + (w - w1) / 2, topY);
    d.print(line1);

    int w2 = (int)strlen(line2) * charW;
    d.setCursor(x + (w - w2) / 2, topY + charH + gap);
    d.print(line2);
}

#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "UIHelpers.h"

// ── ConfirmDialog — reusable YES/NO overlay (960×540) ────────────────────────
//
// Draws a centred dialog box on top of whatever is already on screen.
// Call draw() once after open(), then poll() each loop until poll() returns true.
//
//   y=160  ┌─ dialog box (700×220) centred at x=130 ────────────────────────┐
//          │  [message — up to two lines of textSize 3]                       │
//          │                                                                   │
//          │  [YES  x=210,w=200,h=60]        [NO  x=550,w=200,h=60]          │
//   y=380  └───────────────────────────────────────────────────────────────────┘

static const int CD_BOX_X   = 130;
static const int CD_BOX_Y   = 160;
static const int CD_BOX_W   = 700;
static const int CD_BOX_H   = 220;
static const int CD_BTN_H   = 60;
static const int CD_BTN_Y   = CD_BOX_Y + CD_BOX_H - CD_BTN_H - 16;  // = 304
static const int CD_YES_X   = CD_BOX_X + 80;    // = 210
static const int CD_YES_W   = 200;
static const int CD_NO_X    = CD_BOX_X + 420;   // = 550
static const int CD_NO_W    = 200;

class ConfirmDialog {
public:
    ConfirmDialog(EPD_PainterAdafruit& display, GT911_Lite& touch);

    // Show dialog with a short message (keep it under ~28 chars for textSize 3).
    void open(const char* message);

    bool isOpen()     const { return _open; }
    bool confirmed()  const { return _confirmed; }

    // Full draw of the dialog overlay — call once after open().
    void draw();

    // Poll touch.  Returns true when YES or NO pressed.
    bool poll();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    char _message[64];
    bool _open;
    bool _confirmed;
    bool _wasDown;

    bool hitYes(int sx, int sy) const;
    bool hitNo (int sx, int sy) const;
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

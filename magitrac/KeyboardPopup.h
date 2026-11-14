#pragma once
#include <Arduino.h>
#include "Constants.h"
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"

// ── KeyboardPopup — full-screen QWERTY text entry (960×540) ──────────────────
//
// Layout:
//   y=  0  ┌─ Text field (75px) — black bg, white text; ["123"/"ABC"] layer
//          │  toggle button at its left ─────────────────────────────────────┐
//   y= 75  ├─ Row 1: Q W E R T Y U I O P (10 × 96px slots, key h=108) ──────┤
//   y=191  ├─ Row 2: A S D F G H J K L   (offset 48px) ─────────────────────┤
//   y=307  ├─ Row 3: [shift] Z X C V B N M  [BKSP: 2 slots] ────────────────┤
//   y=423  ├─ Row 4: [CANCEL] [SPACE] [DONE]  (320px each) ─────────────────┤
//   y=540  └──────────────────────────────────────────────────────────────────┘
//
// The symbol layer swaps the three letter rows for digits / punctuation and
// leaves the shift slot blank.

static const int KBD_W         = 960;
static const int KBD_H         = 540;
static const int KBD_FIELD_H   = 75;
static const int KBD_ROW_H     = 116;   // each keyboard row occupies this height
static const int KBD_KEY_H     = 108;   // drawn key height (8px gap below)
static const int KBD_SLOT_W    = 96;    // 960 / 10 columns = 96px slot
static const int KBD_KEY_W     = 88;    // drawn key width (8px gap right)

static const int KBD_ROW1_Y    = KBD_FIELD_H;                  // = 75
static const int KBD_ROW2_Y    = KBD_ROW1_Y + KBD_ROW_H;      // = 191
static const int KBD_ROW3_Y    = KBD_ROW2_Y + KBD_ROW_H;      // = 307
static const int KBD_ROW4_Y    = KBD_ROW3_Y + KBD_ROW_H;      // = 423

// ── KeyboardPopup ──────────────────────────────────────────────────────────────

class KeyboardPopup {
public:
    KeyboardPopup(EPD_PainterAdafruit& display, GT911_Lite& touch);

    // Open keyboard; edits buf in place (must be null-terminated).  maxLen is
    // the buffer size INCLUDING the terminator: typing stops at maxLen - 1
    // characters.  Caller owns buf; the keyboard writes into it directly as
    // keys are tapped.  CANCEL puts back the text buf held when open() was
    // called, so a cancelled edit leaves the caller's buffer untouched.
    void open(char* buf, uint8_t maxLen);

    bool isOpen() const { return _open; }

    // Full redraw — call once after open()
    void draw();

    // Poll touch.  Returns true when the user pressed DONE or CANCEL.
    // Check isDone() to distinguish.
    bool poll();
    bool isDone() const { return _done; }

    // Toggle between alpha and symbol/number layer (the "123"/"ABC" button in
    // the text field calls this).
    void toggleSymbolLayer();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    char*   _buf;
    uint8_t _maxLen;
    bool    _open;
    bool    _done;
    bool    _wasDown;
    bool    _symLayer;     // true = number/symbol layer
    bool    _upperCase;    // alpha layer case (true = QWERTY..., false = qwerty...)

    void drawTextField();
    void drawKeyRow(int y, const char* keys, int count, int xOffset);
    void drawKey(int x, int y, int w, const char* label);
    void toggleCase();

    // Map screen coords to a key.  Returns the char, or 0 for no key.
    // Sets bksp/done/cancel/shift for those special keys.
    char hitKey(int sx, int sy, bool& bksp, bool& done, bool& cancel, bool& shift) const;

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

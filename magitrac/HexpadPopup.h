#pragma once
#include <Arduino.h>
#include "Constants.h"
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"

// ── HexpadPopup — full-screen hex entry for effect/param (960×540) ──────────
//
// Layout:
//   y=  0  ┌─ Text field (90px) — black bg, white text, "EE PP" format ──────┐
//   y= 90  ├─ [0]  [1]  [2]  [3]   (4 × 240px, key h=72) ──────────────────┤
//   y=174  ├─ [4]  [5]  [6]  [7]   ─────────────────────────────────────────┤
//   y=258  ├─ [8]  [9]  [A]  [B]   ─────────────────────────────────────────┤
//   y=342  ├─ [C]  [D]  [E]  [F]   ─────────────────────────────────────────┤
//   y=426  ├─ [CANCEL]  [BKSP]  [CLR]  [SET]  (4 × 240px) ─────────────────┤
//   y=540  └──────────────────────────────────────────────────────────────────┘

static const int HP_W         = 960;
static const int HP_H         = 540;
static const int HP_FIELD_H   = 90;
static const int HP_KEY_H     = 72;
static const int HP_ROW_H     = 84;    // key + 12px gap
static const int HP_COL_W     = 240;   // 960 / 4
static const int HP_MAX_DIGITS = 4;    // Effect(2) + Param(2)

static const int HP_ROW1_Y    = HP_FIELD_H;                  // 90
static const int HP_ROW2_Y    = HP_ROW1_Y + HP_ROW_H;        // 174
static const int HP_ROW3_Y    = HP_ROW2_Y + HP_ROW_H;        // 258
static const int HP_ROW4_Y    = HP_ROW3_Y + HP_ROW_H;        // 342
static const int HP_ROW5_Y    = HP_ROW4_Y + HP_ROW_H;        // 426

class HexpadPopup {
public:
    HexpadPopup(EPD_PainterAdafruit& display, GT911_Lite& touch);

    // Open with initial effect/param values
    void open(uint8_t effect, uint8_t param);

    bool isOpen() const { return _open; }
    void draw();

    // Returns true when SET, CLR, or CANCEL pressed.
    bool poll();
    bool isDone()    const { return _done; }
    bool isCleared() const { return _cleared; }

    // Result — valid after isDone() returns true
    uint8_t effect() const { return _resultEffect; }
    uint8_t param()  const { return _resultParam; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    bool    _open;
    bool    _done;
    bool    _cleared;
    bool    _wasDown;
    bool    _swallowLift;   // ignore first lift after open (belongs to opening tap)

    char    _digits[HP_MAX_DIGITS + 1];
    int     _numDigits;

    uint8_t _resultEffect;
    uint8_t _resultParam;

    void drawTextField();
    void drawKey(int x, int y, int w, int h, const char* label);
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

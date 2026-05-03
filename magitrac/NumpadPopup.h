#pragma once
#include <Arduino.h>
#include "Constants.h"
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"

// ── NumpadPopup — full-screen numeric entry (960×540) ────────────────────────
//
// Layout:
//   y=  0  ┌─ Text field (90px) — black bg, white text, format hint ─────────┐
//   y= 90  ├─ [1]  [2]  [3]   (3 × 320px, key h=100, gap=12) ──────────────┤
//   y=202  ├─ [4]  [5]  [6]   ───────────────────────────────────────────────┤
//   y=314  ├─ [7]  [8]  [9]   ───────────────────────────────────────────────┤
//   y=426  ├─ [CANCEL] [0] [SET]  ───────────────────────────────────────────┤
//   y=540  └──────────────────────────────────────────────────────────────────┘

static const int NP_W         = 960;
static const int NP_H         = 540;
static const int NP_FIELD_H   = 90;
static const int NP_KEY_H     = 100;
static const int NP_ROW_H     = 112;   // key + 12px gap
static const int NP_COL_W     = 320;   // 960 / 3

static const int NP_ROW1_Y    = NP_FIELD_H;                  // 90
static const int NP_ROW2_Y    = NP_ROW1_Y + NP_ROW_H;       // 202
static const int NP_ROW3_Y    = NP_ROW2_Y + NP_ROW_H;       // 314
static const int NP_ROW4_Y    = NP_ROW3_Y + NP_ROW_H;       // 426

static const int NP_MAX_DIGITS = 8;  // enough for HHMMSS or DDMMYY

class NumpadPopup {
public:
    NumpadPopup(EPD_PainterAdafruit& display, GT911_Lite& touch);

    // Open numpad. formatHint is shown above the entry (e.g. "HH:MM:SS").
    // separators is a string of chars to auto-insert at positions
    // (e.g. ":" inserted at positions 2,4 for time).
    // maxDigits = number of raw digits expected (6 for both time and date).
    void open(const char* formatHint, const char* separators,
              const int* sepPositions, int numSeps, int maxDigits);

    bool isOpen() const { return _open; }
    void draw();

    // Returns true when SET or CANCEL pressed. Check isDone().
    bool poll();
    bool isDone() const { return _done; }

    // Get the entered digits as a raw string (no separators).
    const char* digits() const { return _digits; }
    int numDigits() const { return _numDigits; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    bool  _open;
    bool  _done;
    bool  _wasDown;

    char  _digits[NP_MAX_DIGITS + 1];
    int   _numDigits;
    int   _maxDigits;

    char  _formatHint[16];
    char  _separators[4];
    int   _sepPositions[4];
    int   _numSeps;

    void drawTextField();
    void drawKey(int x, int y, int w, int h, const char* label);
    void formatDisplay(char* out, int outLen) const;

    // Returns digit 0-9, or -1. Sets bksp/done/cancel.
    int hitKey(int sx, int sy, bool& bksp, bool& done, bool& cancel) const;
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

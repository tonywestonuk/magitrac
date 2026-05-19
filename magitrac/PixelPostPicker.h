#pragma once
#include <Arduino.h>
#include "Constants.h"
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"

// ── PixelPostPicker — full-screen picker mirroring pixel_post_controller ────
//
// Layout (960×540):
//
//   y=  0  ┌─ Title strip "PIXEL POST" + active page (50px) ─────────────────┐
//   y= 60  ├─ Effect 2×3 grid (left)  │ Slider │ Touchpad (right) ──────────┤
//   y=420  ├─ ──────────────────────────────────────────────────────────────┤
//   y=440  ├─ [1] [2] [3] [4] [5] [6] [7] [BACK]  (8 × 100w / 115step) ─────┤
//   y=540  └──────────────────────────────────────────────────────────────────┘
//
// Effect button touch:   sets _resultKind=NOTE, note = effect_idx + 1
//                        (effect_idx = button + page*6)
// Slider touch:          sets _resultKind=VELOCITY, scaled 0..127 (top = 127)
// Touchpad touch:        sets _resultKind=ATTR, _resultEffect=X, _resultParam=Y
//                        (top-left origin, both 0..255)
// Page button (1-7):     swaps effect-label set, stays open
// BACK:                  closes with _resultKind=NONE

static const int PPK_W           = 960;
static const int PPK_H           = 540;

static const int PPK_TITLE_Y     = 0;
static const int PPK_TITLE_H     = 50;

static const int PPK_BTN_W       = 190;
static const int PPK_BTN_H       = 110;
static const int PPK_BTN_COL1_X  = 5;
static const int PPK_BTN_COL2_X  = 205;
static const int PPK_BTN_ROW1_Y  = 60;
static const int PPK_BTN_ROW2_Y  = 190;
static const int PPK_BTN_ROW3_Y  = 320;

static const int PPK_SLIDER_X    = 410;
static const int PPK_SLIDER_W    = 70;
static const int PPK_SLIDER_Y    = 60;
static const int PPK_SLIDER_H    = 360;

static const int PPK_PAD_X       = 490;
static const int PPK_PAD_W       = 460;
static const int PPK_PAD_Y       = 60;
static const int PPK_PAD_H       = 360;

static const int PPK_PAGE_Y      = 440;
static const int PPK_PAGE_H      = 80;
static const int PPK_PAGE_BTN_W  = 100;
static const int PPK_PAGE_STEP   = 115;
static const int PPK_PAGE_X0     = 20;
static const int PPK_NUM_PAGES   = 7;

class PixelPostPicker {
public:
    enum ResultKind {
        RES_NONE     = 0,   // back pressed or no commit yet
        RES_NOTE     = 1,   // effect tapped → use resultSemitone/Octave
        RES_VELOCITY = 2,   // slider tapped → use resultVelocity
        RES_ATTR     = 3    // touchpad tapped → use resultEffect/Param
    };

    PixelPostPicker(EPD_PainterAdafruit& display, GT911_Lite& touch);

    // Open the picker.  fingerDown=true if a finger is still pressing
    // (caller opened on a rising edge); the picker then waits for a
    // clean lift before accepting the next touch.
    void open(bool fingerDown = true);

    bool isOpen() const { return _open; }
    void draw();

    // Returns true when picker closes (effect/slider/pad commit, or BACK).
    bool poll();

    ResultKind resultKind()      const { return _resultKind; }
    uint8_t    resultSemitone()  const { return _resultSemitone; }
    uint8_t    resultOctave()    const { return _resultOctave; }
    uint8_t    resultVelocity()  const { return _resultVelocity; }
    uint8_t    resultEffect()    const { return _resultEffect; }
    uint8_t    resultParam()     const { return _resultParam; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    bool       _open;
    bool       _wasDown;
    bool       _swallowDown;   // ignore touches until finger lifts after open
    uint8_t    _page;          // 0..PPK_NUM_PAGES-1

    ResultKind _resultKind;
    uint8_t    _resultSemitone;
    uint8_t    _resultOctave;
    uint8_t    _resultVelocity;
    uint8_t    _resultEffect;
    uint8_t    _resultParam;

    void drawTitle();
    void drawEffectButtons();
    void drawSlider();
    void drawTouchpad();
    void drawPageRow();
    void drawWrappedLabel(const char* text, int cx, int cy);
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

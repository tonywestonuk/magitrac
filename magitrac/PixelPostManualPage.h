#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "PixelPostSettingsPage.h"
#include "ConfirmDialog.h"

// ── PixelPostManualPage — live PixelPost control over MagiLink ─────────────
//
// Layout (960×540) — same shape as PixelPostPicker for muscle memory:
//
//   y=  0  ┌─ "PIXEL POST — MANUAL" + [PWR OFF] [BACK] (50px) ─────────────┐
//   y= 60  ├─ Effect 2×3 grid (left)  │ Slider │ Touchpad (right) ─────────┤
//   y=420  ├─ ──────────────────────────────────────────────────────────────┤
//   y=440  ├─ [1] [2] [3] [4] [5] [6] [7]   page selector (no BACK) ───────┤
//   y=540  └──────────────────────────────────────────────────────────────────┘
//
// Touch semantics:
//   - Effect tap   → MSG_PIXELPOST_SET_EFFECT(idx)
//   - Slider drag  → MSG_PIXELPOST_SET_SLIDER(value)    sent on value change
//   - Touchpad     → MSG_PIXELPOST_SET_TOUCHPAD(x,y,1)  while held
//                    MSG_PIXELPOST_SET_TOUCHPAD(x,y,0)  on release
//   - PWR OFF tap  → MSG_PIXELPOST_POWER_OFF(1)
//   - BACK tap     → close the page (returns to whoever opened it)
//
// All sends are fire-and-forget — the server's state cache + 2 s heartbeat
// covers any TCP loss.

static const int PPM_W           = 960;
static const int PPM_H           = 540;

static const int PPM_TITLE_Y     = 0;
static const int PPM_TITLE_H     = 50;
// Title-bar buttons (right edge): [FW] [PWR OFF] [BACK]
static const int PPM_FW_X        = 540;
static const int PPM_FW_W        = 90;
static const int PPM_PWR_X       = 640;
static const int PPM_PWR_W       = 180;
static const int PPM_BACK_X      = 830;
static const int PPM_BACK_W      = 130;

// 2×3 effect grid — left half of the centre band
static const int PPM_BTN_W       = 190;
static const int PPM_BTN_H       = 110;
static const int PPM_BTN_COL1_X  = 5;
static const int PPM_BTN_COL2_X  = 205;
static const int PPM_BTN_ROW1_Y  = 60;
static const int PPM_BTN_ROW2_Y  = 190;
static const int PPM_BTN_ROW3_Y  = 320;

// Slider — vertical, value derived from y (top = 255, bottom = 0)
static const int PPM_SLIDER_X    = 410;
static const int PPM_SLIDER_W    = 70;
static const int PPM_SLIDER_Y    = 60;
static const int PPM_SLIDER_H    = 360;

// X-Y touchpad
static const int PPM_PAD_X       = 490;
static const int PPM_PAD_W       = 460;
static const int PPM_PAD_Y       = 60;
static const int PPM_PAD_H       = 360;

// Page selector (no BACK button — that lives in the title bar)
static const int PPM_PAGE_Y      = 440;
static const int PPM_PAGE_H      = 80;
static const int PPM_PAGE_BTN_W  = 100;
static const int PPM_PAGE_STEP   = 115;
static const int PPM_PAGE_X0     = 20;
static const int PPM_NUM_PAGES   = 7;
// Settings button — sits just to the right of the last page button.
static const int PPM_SET_X       = PPM_PAGE_X0 + PPM_NUM_PAGES * PPM_PAGE_STEP;
static const int PPM_SET_W       = 100;

class PixelPostManualPage {
public:
    PixelPostManualPage(EPD_PainterAdafruit& display, GT911_Lite& touch);

    void open(bool fingerDown = true);
    bool isOpen() const { return _open; }
    void draw();

    // Returns true once when the user taps BACK — caller closes us out.
    bool poll();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    bool    _open;
    bool    _wasDown;
    bool    _swallowDown;       // ignore touches until finger lifts after open
    uint8_t _page;              // 0..PPM_NUM_PAGES-1
    uint8_t _selectedEffect;    // last effect we sent; used to highlight the grid

    // Active-widget tracking — set on rising edge so a drag stays inside the
    // widget it started in even if the finger crosses into another region.
    enum class ActiveWidget : uint8_t {
        NONE,
        SLIDER,
        TOUCHPAD,
    };
    ActiveWidget _active;

    // Last sent slider / touchpad values — used to suppress redundant sends.
    uint8_t _lastSliderSent;
    uint8_t _lastPadXSent;
    uint8_t _lastPadYSent;

    PixelPostSettingsPage _settings;
    ConfirmDialog         _confirmPwr;   // guards the all-posts PWR OFF

    void drawTitle();
    void drawEffectButtons();
    void drawSlider();
    void drawTouchpad();
    void drawPageRow();
    void drawWrappedLabel(const char* text, int cx, int cy);

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;

    int  hitEffect (int sx, int sy) const;   // -1, else 0..5 (index into 2×3 grid)
    int  hitPage   (int sx, int sy) const;   // -1, else 0..PPM_NUM_PAGES-1
    bool hitBack   (int sx, int sy) const;
    bool hitPwrOff (int sx, int sy) const;
    bool hitFw     (int sx, int sy) const;
    bool hitSet    (int sx, int sy) const;
    bool hitSlider (int sx, int sy) const;
    bool hitTouchpad(int sx, int sy) const;

    void sliderUpdateFromY(int sy);
    void touchpadUpdateFromXY(int sx, int sy, bool touched);
};

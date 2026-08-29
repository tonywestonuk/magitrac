#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "ScrollViewport.h"
#include "EffectCatalog.h"

// ── EffectPicker — full-screen scrollable list of output-column commands ──────
//
// Opened from the NoteEditor's CHOOSE button.  Lists the effect catalogue as
// "<name> - <HEX>" rows (drag to scroll); a tap selects one and closes.  The
// row matching the cell's current effect is highlighted on open.
//
// Layout (960×540):
//   y=  0  ┌─ Header: "CHOOSE EFFECT"        [BACK] (60px) ─────────────────┐
//   y= 60  ├─ Scrollable list of catalogue rows (96px each) ───────────────┤
//   y=540  └────────────────────────────────────────────────────────────────┘

static const int FXP_W      = 960;
static const int FXP_H      = 540;
static const int FXP_HDR_H  = 60;
static const int FXP_ROW_H  = 96;
static const int FXP_LIST_Y = FXP_HDR_H;
static const int FXP_LIST_H = FXP_H - FXP_HDR_H;
static const int FXP_BACK_W = 200;

class EffectPicker {
public:
    EffectPicker(EPD_PainterAdafruit& display, GT911_Lite& touch);

    // fingerDown=true when the caller opened us on a rising edge; we then
    // swallow touches until the finger lifts so the opening press can't
    // register as a pick.
    void open(uint8_t curEffect, uint8_t curParam, bool fingerDown = true);

    bool isOpen() const { return _open; }
    void draw();

    // Poll each loop.  Returns true once when the picker closes (a pick or
    // BACK).  On a pick tookChoice() is true and chosen*() carry the result.
    bool poll();

    bool    tookChoice()   const { return _took; }
    uint8_t chosenEffect() const { return _effect; }
    uint8_t chosenParam()  const { return _param; }
    int     chosenIndex()  const { return _idx; }   // catalogue index (-1 = none)

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    ScrollViewport       _view;

    bool    _open;
    bool    _wasDown;
    bool    _swallowDown;
    bool    _closing;
    bool    _took;
    uint8_t _effect;
    uint8_t _param;
    int     _idx;      // catalogue index chosen (-1 = none)
    int     _selIdx;   // row to highlight (matches the cell's current effect)

    void drawChrome();
    bool paintBand(int x, int y, int w, int h, int offsetY);
    void tap(int tx, int ty);
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    bool hitBack(int sx, int sy) const;
};

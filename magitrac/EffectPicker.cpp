#include "EffectPicker.h"
#include <string.h>
#include <stdio.h>

EffectPicker::EffectPicker(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _view(display)
    , _open(false)
    , _wasDown(false)
    , _swallowDown(false)
    , _closing(false)
    , _took(false)
    , _effect(0)
    , _param(0)
    , _selIdx(-1)
{
    _view.configure(0, FXP_LIST_Y, FXP_W, FXP_LIST_H,
        [this](int x, int y, int w, int h, int off) { return paintBand(x, y, w, h, off); },
        [this](int tx, int ty) { tap(tx, ty); });
    _view.setStep(FXP_ROW_H);
}

void EffectPicker::open(uint8_t curEffect, uint8_t curParam, bool fingerDown) {
    _open        = true;
    _wasDown     = _touch.isTouched;
    _swallowDown = fingerDown;
    _closing     = false;
    _took        = false;
    _effect      = curEffect;
    _param       = curParam;

    // Highlight the row matching the cell's current effect (if catalogued).
    _selIdx = -1;
    for (int i = 0; i < EFFECT_CATALOG_LEN; i++) {
        if (EFFECT_CATALOG[i].effect != curEffect) continue;
        if (EFFECT_CATALOG[i].paramFixed && EFFECT_CATALOG[i].fixedParam != curParam)
            continue;
        _selIdx = i;
        break;
    }
    _view.reset();
}

void EffectPicker::draw() {
    _d.fillScreen(0);   // white
    drawChrome();
    _view.redraw();
}

void EffectPicker::drawChrome() {
    _d.fillRect(0, 0, FXP_W, FXP_HDR_H, 3);   // black bar

    // [BACK] button (left)
    _d.fillRect(0, 8, FXP_BACK_W, FXP_HDR_H - 16, 0);
    _d.drawRect(0, 8, FXP_BACK_W, FXP_HDR_H - 16, 0);
    _d.setTextSize(3);
    _d.setTextColor(3);
    _d.setCursor((FXP_BACK_W - 4 * 18) / 2, (FXP_HDR_H - 24) / 2);
    _d.print("BACK");

    // Title
    _d.setTextColor(0);
    const char* title = "CHOOSE EFFECT";
    _d.setCursor(FXP_BACK_W + 30, (FXP_HDR_H - 24) / 2);
    _d.print(title);
}

bool EffectPicker::paintBand(int x, int y, int w, int h, int offsetY) {
    _d.fillRect(x, y, w, h, 0);   // white

    int contentH = EFFECT_CATALOG_LEN * FXP_ROW_H;
    int maxOff    = contentH - h;
    if (maxOff < 0) maxOff = 0;

    // Stop momentum at the content edges — the list never rubber-bands.
    bool stop = (offsetY <= 0) || (offsetY >= maxOff);

    for (int i = 0; i < EFFECT_CATALOG_LEN; i++) {
        int rowTop = y + i * FXP_ROW_H - offsetY;
        if (rowTop + FXP_ROW_H <= y || rowTop >= y + h) continue;

        bool sel = (i == _selIdx);
        _d.fillRect(x, rowTop, w, FXP_ROW_H, sel ? 3 : 0);
        _d.drawFastHLine(x, rowTop + FXP_ROW_H - 1, w, 2);

        char line[40];
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", EFFECT_CATALOG[i].effect);
        snprintf(line, sizeof(line), "%s  -  %s", EFFECT_CATALOG[i].name, hex);
        _d.setTextSize(4);
        _d.setTextColor(sel ? 0 : 3);
        _d.setCursor(x + 40, rowTop + (FXP_ROW_H - 32) / 2);
        _d.print(line);
    }
    return stop;
}

void EffectPicker::tap(int tx, int ty) {
    // ty is content-space (offset + touch-in-band).  Row = ty / rowH.
    if (ty < 0) return;
    int i = ty / FXP_ROW_H;
    if (i < 0 || i >= EFFECT_CATALOG_LEN) return;
    _effect  = EFFECT_CATALOG[i].effect;
    if (EFFECT_CATALOG[i].paramFixed) _param = EFFECT_CATALOG[i].fixedParam;
    _took    = true;
    _closing = true;
}

bool EffectPicker::poll() {
    _view.tick();
    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // Swallow the opening press until the finger lifts once.
    if (_swallowDown) {
        if (!down) _swallowDown = false;
        _wasDown = down;
        return false;
    }

    // Feed the scroll band (ignores presses that start in the header).
    _view.poll(down, sx, sy);   // may fire tap() → _closing

    // BACK on the falling edge (header is disjoint from the scroll band).
    if (!down && _wasDown && !_closing && hitBack(sx, sy)) {
        _took   = false;
        _closing = true;
    }
    _wasDown = down;

    if (_closing) {
        _open = false;
        return true;
    }
    return false;
}

bool EffectPicker::hitBack(int sx, int sy) const {
    return (sy < FXP_HDR_H && sx < FXP_BACK_W);
}

void EffectPicker::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = FXP_H - rx;
}

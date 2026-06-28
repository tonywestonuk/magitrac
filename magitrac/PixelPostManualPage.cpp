#include "PixelPostManualPage.h"
#include "UIHelpers.h"
#include "ServerPairing.h"
#include <pixelpost_proto.h>
#include <string.h>
#include <stdio.h>

extern ServerPairing gServerPairing;

PixelPostManualPage::PixelPostManualPage(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _open(false)
    , _wasDown(false)
    , _swallowDown(false)
    , _page(0)
    , _selectedEffect(0xFF)   // 0xFF = nothing highlighted yet
    , _active(ActiveWidget::NONE)
    , _lastSliderSent(0)
    , _lastPadXSent(0)
    , _lastPadYSent(0)
    , _settings(display, touch)
    , _confirmPwr(display, touch)
{}

void PixelPostManualPage::open(bool fingerDown) {
    _open           = true;
    _wasDown        = _touch.isTouched;
    _swallowDown    = fingerDown;
    _page           = 0;
    _selectedEffect = 0xFF;
    _active         = ActiveWidget::NONE;
    // Refresh server cache with the persisted flashCtrl — covers server reboot
    // since last time we visited.
    gServerPairing.sendPixelpostFlashCtrl(gPixelPostFlashCtrl);
}

void PixelPostManualPage::draw() {
    _d.fillScreen(COL_WHITE);
    drawTitle();
    drawEffectButtons();
    drawSlider();
    drawTouchpad();
    drawPageRow();
}

// ── poll() ─────────────────────────────────────────────────────────────────
bool PixelPostManualPage::poll() {
    if (!_open) return false;

    // While the settings sub-page is open it owns touch.
    if (_settings.isOpen()) {
        if (_settings.poll()) {
            // Settings closed — repaint the manual page.
            _d.clear();
            draw();
            _d.paintLater();
            _wasDown = _touch.isTouched;
        }
        return false;
    }

    // While the PWR OFF confirm dialog is up it owns touch.
    if (_confirmPwr.isOpen()) {
        if (_confirmPwr.poll()) {
            if (_confirmPwr.confirmed()) gServerPairing.sendPixelpostPowerOff(true);
            // Dialog closed — repaint the manual page underneath the overlay.
            _d.clear();
            draw();
            _d.paintLater();
            _wasDown = _touch.isTouched;
        }
        return false;
    }

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // Swallow the lingering touch that opened the page until the user lifts.
    if (_swallowDown) {
        if (!down) _swallowDown = false;
        _wasDown = down;
        return false;
    }

    bool rising  = ( down && !_wasDown);
    bool falling = (!down &&  _wasDown);
    _wasDown = down;

    // ── Rising edge: pick an active widget ─────────────────────────────────
    if (rising) {
        if (hitBack(sx, sy) || hitPwrOff(sx, sy) || hitFw(sx, sy) || hitSet(sx, sy)) {
            // Title-bar / settings buttons — wait until falling edge to act
            // so a slide off doesn't fire.
            _active = ActiveWidget::NONE;
        } else if (hitSlider(sx, sy)) {
            _active = ActiveWidget::SLIDER;
            sliderUpdateFromY(sy);
        } else if (hitTouchpad(sx, sy)) {
            _active = ActiveWidget::TOUCHPAD;
            touchpadUpdateFromXY(sx, sy, /*touched=*/true);
        } else {
            _active = ActiveWidget::NONE;
        }
    }

    // ── Move: stream updates for the active widget ─────────────────────────
    if (down && !rising) {
        if (_active == ActiveWidget::SLIDER) {
            sliderUpdateFromY(sy);
        } else if (_active == ActiveWidget::TOUCHPAD) {
            touchpadUpdateFromXY(sx, sy, /*touched=*/true);
        }
    }

    // ── Falling: commit tap-style actions / release touchpad ──────────────
    if (falling) {
        ActiveWidget was = _active;
        _active = ActiveWidget::NONE;

        if (was == ActiveWidget::TOUCHPAD) {
            // Send a final touched=false with the last (x,y) so the post
            // releases its finger but holds last position.
            gServerPairing.sendPixelpostTouchpad(_lastPadXSent, _lastPadYSent, false);
            return false;
        }

        if (was == ActiveWidget::SLIDER) {
            return false;  // already sent the latest value on the way down
        }

        // No active widget — it was a tap on the title bar / effect grid /
        // page row.  Decide what it hit at the release location.
        if (hitBack(sx, sy)) {
            _open = false;
            return true;
        }
        if (hitPwrOff(sx, sy)) {
            // Big red button — turning every post off by accident is a pain to
            // undo, so confirm first.  The dialog draws as an overlay on top of
            // the page (no clear), and is handled by the modal block above.
            _confirmPwr.open("Power OFF all posts?");
            _confirmPwr.draw();
            _d.paintLater();
            return false;
        }
        if (hitFw(sx, sy)) {
            gServerPairing.sendPixelpostFirmwareUpdate();
            return false;
        }
        if (hitSet(sx, sy)) {
            _settings.open(/*fingerDown=*/false);   // released the tap that opened it
            _d.clear();
            _settings.draw();
            _d.paintLater();
            return false;
        }
        int pageIdx = hitPage(sx, sy);
        if (pageIdx >= 0) {
            if (pageIdx != _page) {
                _page = (uint8_t)pageIdx;
                drawEffectButtons();
                drawPageRow();
                _d.paintLater();
            }
            return false;
        }
        int effIdx = hitEffect(sx, sy);
        if (effIdx >= 0) {
            uint8_t global = (uint8_t)(_page * 6 + effIdx);
            if (global < PIXELPOST_EFFECT_COUNT) {
                gServerPairing.sendPixelpostEffect(global);
                uint8_t prev = _selectedEffect;
                _selectedEffect = global;
                // Repaint just the affected buttons to flip the highlight.
                drawEffectButtons();
                _d.paintLater();
                (void)prev;
            }
            return false;
        }
    }

    return false;
}

// ── Update helpers ─────────────────────────────────────────────────────────
void PixelPostManualPage::sliderUpdateFromY(int sy) {
    // top = 255, bottom = 0
    int y = sy - PPM_SLIDER_Y;
    if (y < 0)             y = 0;
    if (y > PPM_SLIDER_H)  y = PPM_SLIDER_H;
    int v = 255 - (y * 255) / PPM_SLIDER_H;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    uint8_t value = (uint8_t)v;
    if (value == _lastSliderSent) return;
    _lastSliderSent = value;
    gServerPairing.sendPixelpostSlider(value);
}

void PixelPostManualPage::touchpadUpdateFromXY(int sx, int sy, bool touched) {
    int x = sx - PPM_PAD_X;
    int y = sy - PPM_PAD_Y;
    if (x < 0)             x = 0;
    if (x > PPM_PAD_W - 1) x = PPM_PAD_W - 1;
    if (y < 0)             y = 0;
    if (y > PPM_PAD_H - 1) y = PPM_PAD_H - 1;
    uint8_t bx = (uint8_t)((x * 255) / (PPM_PAD_W - 1));
    uint8_t by = (uint8_t)((y * 255) / (PPM_PAD_H - 1));
    if (bx == _lastPadXSent && by == _lastPadYSent) return;
    _lastPadXSent = bx;
    _lastPadYSent = by;
    gServerPairing.sendPixelpostTouchpad(bx, by, touched);
}

// ── Drawing ────────────────────────────────────────────────────────────────
void PixelPostManualPage::drawTitle() {
    _d.fillRect(0, PPM_TITLE_Y, PPM_W, PPM_TITLE_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    const char* title = "PIXEL POST - MANUAL";
    int tw = (int)strlen(title) * 18;
    _d.setCursor((PPM_FW_X - tw) / 2, (PPM_TITLE_H - 24) / 2);
    _d.print(title);

    uiButton(_d, PPM_FW_X,   PPM_TITLE_Y, PPM_FW_W,   PPM_TITLE_H, "FW",
             COL_BLACK, COL_WHITE, 3);
    uiButton(_d, PPM_PWR_X,  PPM_TITLE_Y, PPM_PWR_W,  PPM_TITLE_H, "PWR OFF",
             COL_BLACK, COL_WHITE, 3);
    uiButton(_d, PPM_BACK_X, PPM_TITLE_Y, PPM_BACK_W, PPM_TITLE_H, "BACK",
             COL_BLACK, COL_WHITE, 3);
}

void PixelPostManualPage::drawEffectButtons() {
    const int xs[2] = { PPM_BTN_COL1_X, PPM_BTN_COL2_X };
    const int ys[3] = { PPM_BTN_ROW1_Y, PPM_BTN_ROW2_Y, PPM_BTN_ROW3_Y };

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 2; col++) {
            int idx        = row * 2 + col;
            int globalIdx  = _page * 6 + idx;
            int x = xs[col];
            int y = ys[row];

            // Clear the button background first.
            _d.fillRect(x, y, PPM_BTN_W, PPM_BTN_H, COL_WHITE);

            const char* label = (globalIdx < (int)PIXELPOST_EFFECT_COUNT)
                                ? pixelPostEffectName((uint8_t)globalIdx)
                                : "--";

            bool highlighted = (globalIdx == _selectedEffect);
            uint8_t bg = highlighted ? COL_BLACK : COL_WHITE;
            uint8_t fg = highlighted ? COL_WHITE : COL_BLACK;
            // Disabled (no effect) — dim grey on white.
            if (globalIdx >= (int)PIXELPOST_EFFECT_COUNT) fg = COL_DKGREY;

            _d.fillRect(x, y, PPM_BTN_W, PPM_BTN_H, bg);
            _d.drawRect(x, y, PPM_BTN_W, PPM_BTN_H, fg);
            _d.drawRect(x + 1, y + 1, PPM_BTN_W - 2, PPM_BTN_H - 2, fg);

            _d.setTextColor(fg);
            drawWrappedLabel(label, x + PPM_BTN_W / 2, y + PPM_BTN_H / 2);
        }
    }
}

void PixelPostManualPage::drawSlider() {
    _d.fillRect(PPM_SLIDER_X, PPM_SLIDER_Y, PPM_SLIDER_W, PPM_SLIDER_H, COL_WHITE);
    _d.drawRect(PPM_SLIDER_X, PPM_SLIDER_Y, PPM_SLIDER_W, PPM_SLIDER_H, COL_BLACK);
    _d.drawRect(PPM_SLIDER_X + 1, PPM_SLIDER_Y + 1,
                PPM_SLIDER_W - 2, PPM_SLIDER_H - 2, COL_BLACK);
    // Centre tick marks for orientation.
    for (int i = 1; i < 8; i++) {
        int ty = PPM_SLIDER_Y + (PPM_SLIDER_H * i) / 8;
        _d.drawFastHLine(PPM_SLIDER_X + 6, ty, PPM_SLIDER_W - 12, COL_LTGREY);
    }
    _d.setTextColor(COL_BLACK);
    _d.setTextSize(2);
    _d.setCursor(PPM_SLIDER_X + 14, PPM_SLIDER_Y + 8);
    _d.print("HI");
    _d.setCursor(PPM_SLIDER_X + 14, PPM_SLIDER_Y + PPM_SLIDER_H - 24);
    _d.print("LO");
}

void PixelPostManualPage::drawTouchpad() {
    _d.fillRect(PPM_PAD_X, PPM_PAD_Y, PPM_PAD_W, PPM_PAD_H, COL_WHITE);
    _d.drawRect(PPM_PAD_X, PPM_PAD_Y, PPM_PAD_W, PPM_PAD_H, COL_BLACK);
    _d.drawRect(PPM_PAD_X + 1, PPM_PAD_Y + 1,
                PPM_PAD_W - 2, PPM_PAD_H - 2, COL_BLACK);
    // Quartile grid.
    int midX = PPM_PAD_X + PPM_PAD_W / 2;
    int midY = PPM_PAD_Y + PPM_PAD_H / 2;
    _d.drawFastVLine(midX, PPM_PAD_Y + 4, PPM_PAD_H - 8, COL_LTGREY);
    _d.drawFastHLine(PPM_PAD_X + 4, midY, PPM_PAD_W - 8, COL_LTGREY);
    _d.setTextColor(COL_DKGREY);
    _d.setTextSize(2);
    _d.setCursor(PPM_PAD_X + 10, PPM_PAD_Y + 10);
    _d.print("X-Y TOUCHPAD");
}

void PixelPostManualPage::drawPageRow() {
    _d.fillRect(0, PPM_PAGE_Y, PPM_W, PPM_PAGE_H + 20, COL_WHITE);
    for (int i = 0; i < PPM_NUM_PAGES; i++) {
        int x = PPM_PAGE_X0 + i * PPM_PAGE_STEP;
        bool active = (i == _page);
        uint8_t bg = active ? COL_BLACK : COL_WHITE;
        uint8_t fg = active ? COL_WHITE : COL_BLACK;
        char lab[4];
        snprintf(lab, sizeof(lab), "%d", i + 1);
        uiButton(_d, x, PPM_PAGE_Y, PPM_PAGE_BTN_W, PPM_PAGE_H, lab, bg, fg, 4);
    }
    uiButton(_d, PPM_SET_X, PPM_PAGE_Y, PPM_SET_W, PPM_PAGE_H, "SET",
             COL_WHITE, COL_BLACK, 4);
}

void PixelPostManualPage::drawWrappedLabel(const char* text, int cx, int cy) {
    _d.setTextSize(3);
    // 18 px advance, ~24 px line height at size 3.
    const int charW   = 18;
    const int lineDy  = 24;
    const char* sp = strchr(text, ' ');
    if (!sp || sp == text || *(sp + 1) == '\0') {
        int tw = (int)strlen(text) * charW;
        _d.setCursor(cx - tw / 2, cy - 12);
        _d.print(text);
        return;
    }
    int len1 = (int)(sp - text);
    const char* word2 = sp + 1;
    int len2 = (int)strlen(word2);
    int tw1 = len1 * charW;
    int tw2 = len2 * charW;
    _d.setCursor(cx - tw1 / 2, cy - lineDy / 2 - 12);
    for (int i = 0; i < len1; i++) _d.print(text[i]);
    _d.setCursor(cx - tw2 / 2, cy + lineDy / 2 - 12);
    _d.print(word2);
}

// ── Hit tests ─────────────────────────────────────────────────────────────
bool PixelPostManualPage::hitBack(int sx, int sy) const {
    return sx >= PPM_BACK_X && sx < PPM_BACK_X + PPM_BACK_W
        && sy >= PPM_TITLE_Y && sy < PPM_TITLE_Y + PPM_TITLE_H;
}

bool PixelPostManualPage::hitPwrOff(int sx, int sy) const {
    return sx >= PPM_PWR_X && sx < PPM_PWR_X + PPM_PWR_W
        && sy >= PPM_TITLE_Y && sy < PPM_TITLE_Y + PPM_TITLE_H;
}

bool PixelPostManualPage::hitFw(int sx, int sy) const {
    return sx >= PPM_FW_X && sx < PPM_FW_X + PPM_FW_W
        && sy >= PPM_TITLE_Y && sy < PPM_TITLE_Y + PPM_TITLE_H;
}

bool PixelPostManualPage::hitSet(int sx, int sy) const {
    return sx >= PPM_SET_X && sx < PPM_SET_X + PPM_SET_W
        && sy >= PPM_PAGE_Y && sy < PPM_PAGE_Y + PPM_PAGE_H;
}

bool PixelPostManualPage::hitSlider(int sx, int sy) const {
    return sx >= PPM_SLIDER_X && sx < PPM_SLIDER_X + PPM_SLIDER_W
        && sy >= PPM_SLIDER_Y && sy < PPM_SLIDER_Y + PPM_SLIDER_H;
}

bool PixelPostManualPage::hitTouchpad(int sx, int sy) const {
    return sx >= PPM_PAD_X && sx < PPM_PAD_X + PPM_PAD_W
        && sy >= PPM_PAD_Y && sy < PPM_PAD_Y + PPM_PAD_H;
}

int PixelPostManualPage::hitEffect(int sx, int sy) const {
    const int xs[2] = { PPM_BTN_COL1_X, PPM_BTN_COL2_X };
    const int ys[3] = { PPM_BTN_ROW1_Y, PPM_BTN_ROW2_Y, PPM_BTN_ROW3_Y };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 2; col++) {
            int x = xs[col];
            int y = ys[row];
            if (sx >= x && sx < x + PPM_BTN_W &&
                sy >= y && sy < y + PPM_BTN_H) {
                return row * 2 + col;
            }
        }
    }
    return -1;
}

int PixelPostManualPage::hitPage(int sx, int sy) const {
    if (sy < PPM_PAGE_Y || sy >= PPM_PAGE_Y + PPM_PAGE_H) return -1;
    for (int i = 0; i < PPM_NUM_PAGES; i++) {
        int x = PPM_PAGE_X0 + i * PPM_PAGE_STEP;
        if (sx >= x && sx < x + PPM_PAGE_BTN_W) return i;
    }
    return -1;
}

void PixelPostManualPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

#include "PixelPostPicker.h"
#include "ServerPairing.h"
#include "PixelPostSettingsPage.h"   // gPixelPostFlashCtrl
#include <pixelpost_proto.h>
#include <string.h>
#include <stdio.h>

extern ServerPairing gServerPairing;

// Effect labels are pulled from the shared pixelpost_proto catalogue per
// page/slot — single source of truth shared with pixel_post,
// pixel_post_controller, and magitrac_server.  Layout: row-major,
// 2-wide × 3-tall per page; effect_idx = page * 6 + slot.  Slots that
// don't map to a catalogue entry render as "--" (still tappable; the
// pixel_post just ignores unknown indices).

// Effect grid x/y per button index 0..5 (row-major)
static int effectButtonX(int idx) { return (idx & 1) ? PPK_BTN_COL2_X : PPK_BTN_COL1_X; }
static int effectButtonY(int idx) {
    int row = idx >> 1;
    return (row == 0) ? PPK_BTN_ROW1_Y : (row == 1) ? PPK_BTN_ROW2_Y : PPK_BTN_ROW3_Y;
}

PixelPostPicker::PixelPostPicker(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _open(false)
    , _wasDown(false)
    , _swallowDown(false)
    , _page(0)
    , _selectedEffect(0xFF)
    , _active(ActiveWidget::NONE)
    , _haveNote(false)
    , _haveVel(false)
    , _haveAttr(false)
    , _resultSemitone(0)
    , _resultOctave(0)
    , _resultVelocity(0)
    , _resultEffect(0)
    , _resultParam(0)
    , _lastPadX(0)
    , _lastPadY(0)
{}

void PixelPostPicker::open(bool fingerDown) {
    _open           = true;
    _wasDown        = fingerDown;
    _swallowDown    = fingerDown;
    _page           = 0;
    _selectedEffect = 0xFF;
    _active         = ActiveWidget::NONE;
    _haveNote = _haveVel = _haveAttr = false;
    _lastPadX = _lastPadY = 0;
    // Refresh the server's flash-ctrl cache so live preview actually flashes
    // (covers a server reboot since last visit).
    gServerPairing.sendPixelpostFlashCtrl(gPixelPostFlashCtrl);
}

// ── Draw ─────────────────────────────────────────────────────────────────────

void PixelPostPicker::draw() {
    _d.fillScreen(COL_WHITE);
    drawTitle();
    drawEffectButtons();
    drawSlider();
    drawTouchpad();
    drawPageRow();
}

void PixelPostPicker::drawTitle() {
    _d.fillRect(0, PPK_TITLE_Y, PPK_W, PPK_TITLE_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    const char* title = "PIXEL POST";
    int tw = (int)strlen(title) * 18;
    _d.setCursor((PPK_W - tw) / 2, PPK_TITLE_Y + (PPK_TITLE_H - 24) / 2);
    _d.print(title);

    // Page label on the right
    char pageLbl[16];
    snprintf(pageLbl, sizeof(pageLbl), "PAGE %d", (int)_page + 1);
    int pw = (int)strlen(pageLbl) * 18;
    _d.setCursor(PPK_W - pw - 20, PPK_TITLE_Y + (PPK_TITLE_H - 24) / 2);
    _d.print(pageLbl);
}

void PixelPostPicker::drawEffectButtons() {
    for (int i = 0; i < 6; i++) {
        int x = effectButtonX(i);
        int y = effectButtonY(i);
        uint8_t effectIdx = (uint8_t)((int)_page * 6 + i);
        bool valid = (effectIdx < (uint8_t)PIXELPOST_EFFECT_COUNT);
        bool hl    = valid && (effectIdx == _selectedEffect);
        uint8_t bg = hl ? COL_BLACK : COL_WHITE;
        uint8_t fg = hl ? COL_WHITE : (valid ? COL_BLACK : COL_DKGREY);
        _d.fillRect(x, y, PPK_BTN_W, PPK_BTN_H, bg);
        _d.drawRect(x, y, PPK_BTN_W, PPK_BTN_H, valid ? COL_BLACK : COL_DKGREY);
        _d.setTextColor(fg);
        drawWrappedLabel(pixelPostEffectName(effectIdx),
                         x + PPK_BTN_W / 2, y + PPK_BTN_H / 2);
    }
}

void PixelPostPicker::drawWrappedLabel(const char* text, int cx, int cy) {
    // Text colour is set by the caller (highlight-aware).
    _d.setTextSize(3);

    const char* space = strchr(text, ' ');
    if (!space) {
        int tw = (int)strlen(text) * 18;
        _d.setCursor(cx - tw / 2, cy - 12);
        _d.print(text);
        return;
    }
    char first[16];
    int n = (int)(space - text);
    if (n > 15) n = 15;
    memcpy(first, text, n);
    first[n] = '\0';
    const char* second = space + 1;

    int tw1 = (int)strlen(first) * 18;
    int tw2 = (int)strlen(second) * 18;
    _d.setCursor(cx - tw1 / 2, cy - 28);
    _d.print(first);
    _d.setCursor(cx - tw2 / 2, cy + 4);
    _d.print(second);
}

void PixelPostPicker::drawSlider() {
    _d.fillRect(PPK_SLIDER_X, PPK_SLIDER_Y, PPK_SLIDER_W, PPK_SLIDER_H, COL_WHITE);
    _d.drawRect(PPK_SLIDER_X, PPK_SLIDER_Y, PPK_SLIDER_W, PPK_SLIDER_H, COL_BLACK);

    // Wedge indicator (thin at top → wide at bottom = "high to low" visual,
    // matching pixel_post_controller's fillTriangle).  We flip semantics
    // when reading (top = high velocity) but keep the visual.
    int apexX  = PPK_SLIDER_X + PPK_SLIDER_W / 2;
    int apexY  = PPK_SLIDER_Y + 20;
    int baseY  = PPK_SLIDER_Y + PPK_SLIDER_H - 20;
    int baseHW = (PPK_SLIDER_W / 2) - 10;
    _d.fillTriangle(apexX,          apexY,
                    apexX - baseHW, baseY,
                    apexX + baseHW, baseY,
                    COL_LTGREY);

    // Label "VEL" along the top
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(PPK_SLIDER_X + 14, PPK_SLIDER_Y + 4);
    _d.print("VEL");
}

void PixelPostPicker::drawTouchpad() {
    _d.fillRect(PPK_PAD_X, PPK_PAD_Y, PPK_PAD_W, PPK_PAD_H, COL_WHITE);
    _d.drawRect(PPK_PAD_X, PPK_PAD_Y, PPK_PAD_W, PPK_PAD_H, COL_BLACK);

    // Crosshair / quadrant guides
    int cx = PPK_PAD_X + PPK_PAD_W / 2;
    int cy = PPK_PAD_Y + PPK_PAD_H / 2;
    _d.drawFastVLine(cx, PPK_PAD_Y + 4, PPK_PAD_H - 8, COL_LTGREY);
    _d.drawFastHLine(PPK_PAD_X + 4, cy, PPK_PAD_W - 8, COL_LTGREY);

    // Origin marker (top-left = 0,0)
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(PPK_PAD_X + 6, PPK_PAD_Y + 4);
    _d.print("0,0");
}

void PixelPostPicker::drawPageRow() {
    for (int i = 0; i < 8; i++) {
        int x = PPK_PAGE_X0 + i * PPK_PAGE_STEP;
        bool active = (i < PPK_NUM_PAGES) && ((int)_page == i);
        uint8_t bg = active ? COL_BLACK : COL_WHITE;
        uint8_t fg = active ? COL_WHITE : COL_BLACK;
        _d.fillRect(x, PPK_PAGE_Y, PPK_PAGE_BTN_W, PPK_PAGE_H, bg);
        _d.drawRect(x, PPK_PAGE_Y, PPK_PAGE_BTN_W, PPK_PAGE_H, COL_BLACK);

        char label[6];
        if (i < PPK_NUM_PAGES) snprintf(label, sizeof(label), "%d", i + 1);
        else                  snprintf(label, sizeof(label), "BACK");

        _d.setTextSize(3);
        _d.setTextColor(fg);
        int lw = (int)strlen(label) * 18;
        _d.setCursor(x + (PPK_PAGE_BTN_W - lw) / 2, PPK_PAGE_Y + (PPK_PAGE_H - 24) / 2);
        _d.print(label);
    }
}

void PixelPostPicker::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = PPK_H - rx;
}

// ── Poll ─────────────────────────────────────────────────────────────────────

bool PixelPostPicker::hitSlider(int sx, int sy) const {
    return sx >= PPK_SLIDER_X && sx < PPK_SLIDER_X + PPK_SLIDER_W &&
           sy >= PPK_SLIDER_Y && sy < PPK_SLIDER_Y + PPK_SLIDER_H;
}

bool PixelPostPicker::hitTouchpad(int sx, int sy) const {
    return sx >= PPK_PAD_X && sx < PPK_PAD_X + PPK_PAD_W &&
           sy >= PPK_PAD_Y && sy < PPK_PAD_Y + PPK_PAD_H;
}

void PixelPostPicker::sliderUpdateFromY(int sy) {
    // Top of slider = max velocity (127); bottom = 0.
    int fromBottom = (PPK_SLIDER_Y + PPK_SLIDER_H) - sy;
    int v = (fromBottom * 127) / PPK_SLIDER_H;
    if (v < 0)   v = 0;
    if (v > 127) v = 127;
    uint8_t vel = (uint8_t)v;
    if (_haveVel && vel == _resultVelocity) return;
    _resultVelocity = vel;
    _haveVel = true;
    // Live preview: the post's slider is 0..255 brightness; scale up from 0..127.
    gServerPairing.sendPixelpostSlider((uint8_t)((v * 255) / 127));
}

void PixelPostPicker::touchpadUpdateFromXY(int sx, int sy, bool touched) {
    int px = ((sx - PPK_PAD_X) * 255) / PPK_PAD_W;
    int py = ((sy - PPK_PAD_Y) * 255) / PPK_PAD_H;
    if (px < 0)   px = 0;
    if (px > 255) px = 255;
    if (py < 0)   py = 0;
    if (py > 255) py = 255;
    uint8_t bx = (uint8_t)px, by = (uint8_t)py;
    if (_haveAttr && touched && bx == _lastPadX && by == _lastPadY) return;
    _lastPadX = bx;
    _lastPadY = by;
    _resultEffect = bx;
    _resultParam  = by;
    _haveAttr = true;
    gServerPairing.sendPixelpostTouchpad(bx, by, touched);
}

bool PixelPostPicker::poll() {
    if (!_open || !_touch.read()) return false;

    bool down = _touch.isTouched;
    int  sx, sy;
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

    // ── Rising: pick an active widget for drag streaming ──────────────────────
    if (rising) {
        if (hitSlider(sx, sy)) {
            _active = ActiveWidget::SLIDER;
            sliderUpdateFromY(sy);
        } else if (hitTouchpad(sx, sy)) {
            _active = ActiveWidget::TOUCHPAD;
            touchpadUpdateFromXY(sx, sy, /*touched=*/true);
        } else {
            _active = ActiveWidget::NONE;   // tap-style — decided on falling edge
        }
    }

    // ── Move: stream the active widget ────────────────────────────────────────
    if (down && !rising) {
        if (_active == ActiveWidget::SLIDER)        sliderUpdateFromY(sy);
        else if (_active == ActiveWidget::TOUCHPAD) touchpadUpdateFromXY(sx, sy, true);
    }

    if (!falling) return false;

    // ── Falling: release the touchpad, or commit a tap ────────────────────────
    ActiveWidget was = _active;
    _active = ActiveWidget::NONE;

    if (was == ActiveWidget::TOUCHPAD) {
        // Release with last (x,y) so the post drops its finger but holds position.
        gServerPairing.sendPixelpostTouchpad(_lastPadX, _lastPadY, false);
        return false;
    }
    if (was == ActiveWidget::SLIDER) return false;  // latest value already sent

    // Bottom row: page selectors + BACK.
    if (sy >= PPK_PAGE_Y && sy < PPK_PAGE_Y + PPK_PAGE_H) {
        for (int i = 0; i < 8; i++) {
            int x = PPK_PAGE_X0 + i * PPK_PAGE_STEP;
            if (sx >= x && sx < x + PPK_PAGE_BTN_W) {
                if (i < PPK_NUM_PAGES) {
                    if ((int)_page != i) {
                        _page = (uint8_t)i;
                        drawTitle();
                        drawEffectButtons();
                        drawPageRow();
                        _d.paintLater();
                    }
                    return false;
                }
                // BACK — caller commits the touched axes.
                _open = false;
                return true;
            }
        }
        return false;
    }

    // Effect grid — send live, remember, stay open.
    for (int i = 0; i < 6; i++) {
        int bx = effectButtonX(i);
        int by = effectButtonY(i);
        if (sx >= bx && sx < bx + PPK_BTN_W &&
            sy >= by && sy < by + PPK_BTN_H) {
            uint8_t effectIdx = (uint8_t)((int)_page * 6 + i);
            if (effectIdx >= (uint8_t)PIXELPOST_EFFECT_COUNT) return false;  // "--"
            gServerPairing.sendPixelpostEffect(effectIdx);
            _resultSemitone = (uint8_t)(effectIdx % 12);
            _resultOctave   = (uint8_t)(effectIdx / 12);
            _haveNote       = true;
            _selectedEffect = effectIdx;
            drawEffectButtons();          // flip the highlight
            _d.paintLater();
            return false;
        }
    }

    return false;
}

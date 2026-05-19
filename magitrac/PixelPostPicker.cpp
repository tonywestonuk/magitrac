#include "PixelPostPicker.h"
#include <string.h>
#include <stdio.h>

// Effect labels — verbatim from pixel_post_controller.ino drawPageStrings.
// Layout matches the controller: row-major, 2-wide × 3-tall per page, so the
// table reads left-to-right top-to-bottom for each page.  "--" = empty slot
// (still tappable; sends the effect_idx anyway — pixel_post will just ignore
// unknown indices).  Expand as you add effects on the pixel_post side.
static const char* PIXEL_EFFECTS[PPK_NUM_PAGES][6] = {
    // page 0
    { "BLOCK-1", "SINE WAVE",
      "BLOCK-2", "FIRE",
      "POW!",    "COLOR WHEEL" },
    // page 1
    { "Rainbow", "Sparkle",
      "Strobe",  "Meteor",
      "Springs", "Circles 1" },
    // page 2
    { "Chaser-1",       "Sound Spectrum",
      "Blood",          "Firmware Update",
      "--",             "--" },
    // page 3..6 — empty for now
    { "--","--","--","--","--","--" },
    { "--","--","--","--","--","--" },
    { "--","--","--","--","--","--" },
    { "--","--","--","--","--","--" }
};

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
    , _resultKind(RES_NONE)
    , _resultSemitone(0)
    , _resultOctave(0)
    , _resultVelocity(0)
    , _resultEffect(0)
    , _resultParam(0)
{}

void PixelPostPicker::open(bool fingerDown) {
    _open        = true;
    _wasDown     = fingerDown;
    _swallowDown = fingerDown;
    _resultKind  = RES_NONE;
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
        _d.fillRect(x, y, PPK_BTN_W, PPK_BTN_H, COL_WHITE);
        _d.drawRect(x, y, PPK_BTN_W, PPK_BTN_H, COL_BLACK);
        drawWrappedLabel(PIXEL_EFFECTS[_page][i], x + PPK_BTN_W / 2, y + PPK_BTN_H / 2);
    }
}

void PixelPostPicker::drawWrappedLabel(const char* text, int cx, int cy) {
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);

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

bool PixelPostPicker::poll() {
    if (!_open || !_touch.read()) return false;

    bool down = _touch.isTouched;
    int  sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    bool rising  = (down  && !_wasDown);
    bool falling = (!down && _wasDown);
    _wasDown = down;

    // Wait for the opening finger to lift before accepting any input.
    if (_swallowDown) {
        if (falling) _swallowDown = false;
        return false;
    }

    if (!rising) return false;

    // ── Bottom row: page selectors + BACK ────────────────────────────────────
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
                // BACK
                _resultKind = RES_NONE;
                _open = false;
                return true;
            }
        }
        return false;
    }

    // ── Effect grid ──────────────────────────────────────────────────────────
    for (int i = 0; i < 6; i++) {
        int bx = effectButtonX(i);
        int by = effectButtonY(i);
        if (sx >= bx && sx < bx + PPK_BTN_W &&
            sy >= by && sy < by + PPK_BTN_H) {
            int effectIdx = i + (int)_page * 6;
            if (effectIdx < 0) effectIdx = 0;
            if (effectIdx > 95) effectIdx = 95;     // NOTE_MAX = 96 (B-7)
            _resultSemitone = (uint8_t)(effectIdx % 12);
            _resultOctave   = (uint8_t)(effectIdx / 12);
            _resultKind     = RES_NOTE;
            _open = false;
            return true;
        }
    }

    // ── Slider ───────────────────────────────────────────────────────────────
    if (sx >= PPK_SLIDER_X && sx < PPK_SLIDER_X + PPK_SLIDER_W &&
        sy >= PPK_SLIDER_Y && sy < PPK_SLIDER_Y + PPK_SLIDER_H) {
        // Top of slider = max velocity (127); bottom = 0.
        int fromBottom = (PPK_SLIDER_Y + PPK_SLIDER_H) - sy;
        int v = (fromBottom * 127) / PPK_SLIDER_H;
        if (v < 0)   v = 0;
        if (v > 127) v = 127;
        _resultVelocity = (uint8_t)v;
        _resultKind     = RES_VELOCITY;
        _open = false;
        return true;
    }

    // ── Touchpad ─────────────────────────────────────────────────────────────
    if (sx >= PPK_PAD_X && sx < PPK_PAD_X + PPK_PAD_W &&
        sy >= PPK_PAD_Y && sy < PPK_PAD_Y + PPK_PAD_H) {
        int px = ((sx - PPK_PAD_X) * 255) / PPK_PAD_W;
        int py = ((sy - PPK_PAD_Y) * 255) / PPK_PAD_H;
        if (px < 0)   px = 0;
        if (px > 255) px = 255;
        if (py < 0)   py = 0;
        if (py > 255) py = 255;
        _resultEffect = (uint8_t)px;
        _resultParam  = (uint8_t)py;
        _resultKind   = RES_ATTR;
        _open = false;
        return true;
    }

    return false;
}

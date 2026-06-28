#include "PixelPostSettingsPage.h"
#include "UIHelpers.h"
#include "ServerPairing.h"
#include "Constants.h"
#include <Preferences.h>
#include <string.h>
#include <stdio.h>

extern ServerPairing gServerPairing;

uint8_t gPixelPostFlashCtrl = 0;

static const char* PP_NVS_NS = "magitrac_pp";

void loadPixelPostFlashCtrl() {
    Preferences prefs;
    prefs.begin(PP_NVS_NS, true);
    gPixelPostFlashCtrl = prefs.getUChar("flashCtrl", 0);
    prefs.end();
}

void savePixelPostFlashCtrl() {
    Preferences prefs;
    prefs.begin(PP_NVS_NS, false);
    prefs.putUChar("flashCtrl", gPixelPostFlashCtrl);
    prefs.end();
}

// ── Layout ─────────────────────────────────────────────────────────────────
static const int PPS_W           = 960;
static const int PPS_H           = 540;

static const int PPS_TITLE_Y     = 0;
static const int PPS_TITLE_H     = 50;
static const int PPS_BACK_X      = 830;
static const int PPS_BACK_W      = 130;

// Two rows of [-] [VALUE] [+] with a label above each.
static const int PPS_LABEL_X     = 60;
static const int PPS_LABEL_W     = 360;
static const int PPS_LABEL_TS    = 4;

static const int PPS_BTN_W       = 100;
static const int PPS_BTN_H       = 100;
static const int PPS_VAL_W       = 160;
static const int PPS_MINUS_X     = 460;
static const int PPS_VAL_X       = PPS_MINUS_X + PPS_BTN_W + 20;
static const int PPS_PLUS_X      = PPS_VAL_X + PPS_VAL_W + 20;

static const int PPS_BRIGHT_Y    = 110;
static const int PPS_SMOOTH_Y    = 290;

// Help band along the bottom.
static const int PPS_HELP_Y      = 460;

// ── Helpers ────────────────────────────────────────────────────────────────
static uint8_t bright(uint8_t b)  { return (b >> 4) & 0x0F; }
static uint8_t smooth(uint8_t b)  { return  b       & 0x0F; }
static uint8_t pack(uint8_t br, uint8_t sm) {
    if (br > 15) br = 15;
    if (sm > 4)  sm = 4;
    return (uint8_t)((br << 4) | sm);
}

PixelPostSettingsPage::PixelPostSettingsPage(EPD_PainterAdafruit& display,
                                             GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _open(false)
    , _wasDown(false)
    , _swallowDown(false)
{}

void PixelPostSettingsPage::open(bool fingerDown) {
    _open        = true;
    _wasDown     = _touch.isTouched;
    _swallowDown = fingerDown;
}

void PixelPostSettingsPage::draw() {
    _d.fillScreen(COL_WHITE);
    drawTitle();
    drawRows();
}

void PixelPostSettingsPage::drawTitle() {
    _d.fillRect(0, PPS_TITLE_Y, PPS_W, PPS_TITLE_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    const char* title = "PIXEL POST - SETTINGS";
    int tw = (int)strlen(title) * 18;
    _d.setCursor((PPS_BACK_X - tw) / 2, (PPS_TITLE_H - 24) / 2);
    _d.print(title);

    uiButton(_d, PPS_BACK_X, PPS_TITLE_Y, PPS_BACK_W, PPS_TITLE_H, "BACK",
             COL_BLACK, COL_WHITE, 3);
}

void PixelPostSettingsPage::drawRows() {
    drawBrightnessRow();
    drawSmoothingRow();

    // Help line — when both nibbles are 0, the layer is fully disabled.
    _d.fillRect(0, PPS_HELP_Y, PPS_W, PPS_H - PPS_HELP_Y, COL_WHITE);
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(PPS_LABEL_X, PPS_HELP_Y);
    if (gPixelPostFlashCtrl == 0) {
        _d.print("Layer disabled - posts run at full brightness, no smoothing.");
    } else {
        _d.print("Smoothing 0=instant 4=~1s ramp. Brightness 15=full 0=black.");
    }
}

static void drawValueRow(EPD_PainterAdafruit& d, int y,
                         const char* label, int value, int maxValue) {
    // Clear strip.
    d.fillRect(0, y - 36, 960, PPS_BTN_H + 60, COL_WHITE);

    // Label above the buttons.
    d.setTextSize(3);
    d.setTextColor(COL_BLACK);
    d.setCursor(PPS_LABEL_X, y - 30);
    d.print(label);

    // [-] button.
    bool minusEnabled = value > 0;
    uint8_t mfg = minusEnabled ? COL_BLACK : COL_DKGREY;
    uiButton(d, PPS_MINUS_X, y, PPS_BTN_W, PPS_BTN_H, "-",
             COL_WHITE, mfg, 6);

    // Value box.
    d.drawRect(PPS_VAL_X, y, PPS_VAL_W, PPS_BTN_H, COL_BLACK);
    d.drawRect(PPS_VAL_X + 1, y + 1, PPS_VAL_W - 2, PPS_BTN_H - 2, COL_BLACK);
    char val[8];
    snprintf(val, sizeof(val), "%d/%d", value, maxValue);
    int vw = (int)strlen(val) * 24;     // textSize 4 → 24 px advance
    d.setTextSize(4);
    d.setTextColor(COL_BLACK);
    d.setCursor(PPS_VAL_X + (PPS_VAL_W - vw) / 2, y + (PPS_BTN_H - 32) / 2);
    d.print(val);

    // [+] button.
    bool plusEnabled = value < maxValue;
    uint8_t pfg = plusEnabled ? COL_BLACK : COL_DKGREY;
    uiButton(d, PPS_PLUS_X, y, PPS_BTN_W, PPS_BTN_H, "+",
             COL_WHITE, pfg, 6);
}

void PixelPostSettingsPage::drawBrightnessRow() {
    drawValueRow(_d, PPS_BRIGHT_Y, "Max Brightness", bright(gPixelPostFlashCtrl), 15);
}

void PixelPostSettingsPage::drawSmoothingRow() {
    drawValueRow(_d, PPS_SMOOTH_Y, "Flash Smoothing", smooth(gPixelPostFlashCtrl), 4);
}

void PixelPostSettingsPage::redrawAndSync() {
    drawBrightnessRow();
    drawSmoothingRow();
    // Re-render the help line so the "Layer disabled" text appears/clears.
    _d.fillRect(0, PPS_HELP_Y, PPS_W, PPS_H - PPS_HELP_Y, COL_WHITE);
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(PPS_LABEL_X, PPS_HELP_Y);
    if (gPixelPostFlashCtrl == 0) {
        _d.print("Layer disabled - posts run at full brightness, no smoothing.");
    } else {
        _d.print("Smoothing 0=instant 4=~1s ramp. Brightness 15=full 0=black.");
    }
    _d.paintLater();
    savePixelPostFlashCtrl();
    gServerPairing.sendPixelpostFlashCtrl(gPixelPostFlashCtrl);
}

bool PixelPostSettingsPage::poll() {
    if (!_open || !_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (_swallowDown) {
        if (!down) _swallowDown = false;
        _wasDown = down;
        return false;
    }

    bool rising  = ( down && !_wasDown);
    bool falling = (!down &&  _wasDown);
    _wasDown = down;

    if (!falling) return false;
    (void)rising;

    if (hitBack(sx, sy)) {
        _open = false;
        return true;
    }

    int hb = hitBrightness(sx, sy);
    if (hb >= 0) {
        uint8_t br = bright(gPixelPostFlashCtrl);
        uint8_t sm = smooth(gPixelPostFlashCtrl);
        if (hb == 0 && br > 0)  br--;
        if (hb == 1 && br < 15) br++;
        gPixelPostFlashCtrl = pack(br, sm);
        redrawAndSync();
        return false;
    }

    int hs = hitSmoothing(sx, sy);
    if (hs >= 0) {
        uint8_t br = bright(gPixelPostFlashCtrl);
        uint8_t sm = smooth(gPixelPostFlashCtrl);
        if (hs == 0 && sm > 0) sm--;
        if (hs == 1 && sm < 4) sm++;
        gPixelPostFlashCtrl = pack(br, sm);
        redrawAndSync();
        return false;
    }

    return false;
}

bool PixelPostSettingsPage::hitBack(int sx, int sy) const {
    return sx >= PPS_BACK_X && sx < PPS_BACK_X + PPS_BACK_W
        && sy >= PPS_TITLE_Y && sy < PPS_TITLE_Y + PPS_TITLE_H;
}

int PixelPostSettingsPage::hitBrightness(int sx, int sy) const {
    if (sy < PPS_BRIGHT_Y || sy >= PPS_BRIGHT_Y + PPS_BTN_H) return -1;
    if (sx >= PPS_MINUS_X && sx < PPS_MINUS_X + PPS_BTN_W) return 0;
    if (sx >= PPS_PLUS_X  && sx < PPS_PLUS_X  + PPS_BTN_W) return 1;
    return -1;
}

int PixelPostSettingsPage::hitSmoothing(int sx, int sy) const {
    if (sy < PPS_SMOOTH_Y || sy >= PPS_SMOOTH_Y + PPS_BTN_H) return -1;
    if (sx >= PPS_MINUS_X && sx < PPS_MINUS_X + PPS_BTN_W) return 0;
    if (sx >= PPS_PLUS_X  && sx < PPS_PLUS_X  + PPS_BTN_W) return 1;
    return -1;
}

void PixelPostSettingsPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

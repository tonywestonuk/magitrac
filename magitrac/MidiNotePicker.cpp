#include "MidiNotePicker.h"
#include "UIHelpers.h"
#include "Constants.h"
#include <string.h>
#include <stdio.h>

static const char* MNP_NAMES[12] = {
    "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
};

static const int MNP_W = 960;
static const int MNP_H = 540;

static const int MNP_HDR_H      = 60;
static const int MNP_HDR_BTN_W  = 180;

static const int MNP_VAL_Y      = MNP_HDR_H;
static const int MNP_VAL_H      = 80;

static const int MNP_NLBL_Y     = MNP_VAL_Y + MNP_VAL_H;     // 140
static const int MNP_NLBL_H     = 24;
static const int MNP_NBTN_Y     = MNP_NLBL_Y + MNP_NLBL_H;   // 164
static const int MNP_NBTN_H     = 100;
static const int MNP_NBTN_W     = MNP_W / 12;                 // 80

static const int MNP_OLBL_Y     = MNP_NBTN_Y + MNP_NBTN_H;   // 264
static const int MNP_OLBL_H     = 24;
static const int MNP_OBTN_Y     = MNP_OLBL_Y + MNP_OLBL_H;   // 288
static const int MNP_OBTN_H     = 100;
static const int MNP_OBTN_W     = 80;
static const int MNP_OBTN_GAP   = 8;    // 11 * 80 + 10 * 8 = 960

static const int MNP_HINT_Y     = MNP_OBTN_Y + MNP_OBTN_H + 12;  // 400

static const int MNP_ACT_Y      = 440;
static const int MNP_ACT_H      = 100;
static const int MNP_ACT_BTN_W  = 300;
static const int MNP_ACT_CANCEL_X = 80;
static const int MNP_ACT_OK_X     = MNP_W - MNP_ACT_BTN_W - 80;

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

MidiNotePicker::MidiNotePicker(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display), _touch(touch) {}

void MidiNotePicker::open(uint8_t initialNote, uint8_t minVal, uint8_t maxVal,
                          const char* title) {
    _min       = minVal;
    _max       = maxVal;
    _value     = (uint8_t)clampi(initialNote, _min, _max);
    _semitone  = _value % 12;
    _octave    = _value / 12;
    strncpy(_title, title ? title : "", sizeof(_title) - 1);
    _title[sizeof(_title) - 1] = '\0';
    _open      = true;
    _accepted  = false;
    _wasDown   = _touch.isTouched;
}

void MidiNotePicker::draw() {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    drawValue();
    drawNoteButtons();
    drawOctaveButtons();
    drawHint();
    drawActions();
}

void MidiNotePicker::drawHeader() {
    _d.fillRect(0, 0, MNP_W, MNP_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    int tw = (int)strlen(_title) * 18;
    _d.setCursor((MNP_W - tw) / 2, (MNP_HDR_H - 24) / 2);
    _d.print(_title);
}

void MidiNotePicker::drawValue() {
    _d.fillRect(0, MNP_VAL_Y, MNP_W, MNP_VAL_H, COL_WHITE);
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%d  (%d)", MNP_NAMES[_semitone], _octave, _value);
    _d.setTextSize(6);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(buf) * 36;
    _d.setCursor((MNP_W - tw) / 2, MNP_VAL_Y + (MNP_VAL_H - 48) / 2);
    _d.print(buf);
}

void MidiNotePicker::drawNoteButtons() {
    _d.fillRect(0, MNP_NLBL_Y, MNP_W, MNP_NLBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, MNP_NLBL_Y + (MNP_NLBL_H - 16) / 2);
    _d.print("NOTE");

    for (int i = 0; i < 12; i++) {
        int x = i * MNP_NBTN_W;
        bool selected = (i == _semitone);
        bool enabled  = (((uint8_t)(_octave * 12 + i)) >= _min &&
                          (uint8_t)(_octave * 12 + i) <= _max);
        uint8_t bg = selected ? COL_BLACK : COL_WHITE;
        uint8_t fg = selected ? COL_WHITE : (enabled ? COL_BLACK : COL_DKGREY);
        uiButton(_d, x, MNP_NBTN_Y, MNP_NBTN_W, MNP_NBTN_H,
                 MNP_NAMES[i], bg, fg, 4);
    }
}

void MidiNotePicker::drawOctaveButtons() {
    _d.fillRect(0, MNP_OLBL_Y, MNP_W, MNP_OLBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, MNP_OLBL_Y + (MNP_OLBL_H - 16) / 2);
    _d.print("OCTAVE");

    for (int o = 0; o < 11; o++) {
        int x = o * (MNP_OBTN_W + MNP_OBTN_GAP);
        bool selected = (o == _octave);
        bool enabled  = (((uint8_t)(o * 12 + _semitone)) >= _min &&
                          (uint8_t)(o * 12 + _semitone) <= _max);
        uint8_t bg = selected ? COL_BLACK : COL_WHITE;
        uint8_t fg = selected ? COL_WHITE : (enabled ? COL_BLACK : COL_DKGREY);
        char lbl[4];
        snprintf(lbl, sizeof(lbl), "%d", o);
        uiButton(_d, x, MNP_OBTN_Y, MNP_OBTN_W, MNP_OBTN_H, lbl, bg, fg, 4);
    }
}

void MidiNotePicker::drawHint() {
    _d.fillRect(0, MNP_HINT_Y, MNP_W, MNP_ACT_Y - MNP_HINT_Y, COL_WHITE);
    if (_min == 0 && _max == 127) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "Range %s%d..%s%d",
             MNP_NAMES[_min % 12], _min / 12,
             MNP_NAMES[_max % 12], _max / 12);
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    int tw = (int)strlen(buf) * 12;
    _d.setCursor((MNP_W - tw) / 2, MNP_HINT_Y);
    _d.print(buf);
}

void MidiNotePicker::drawActions() {
    uiButton(_d, MNP_ACT_CANCEL_X, MNP_ACT_Y, MNP_ACT_BTN_W, MNP_ACT_H,
             "CANCEL", COL_WHITE, COL_BLACK, 4);
    uiButton(_d, MNP_ACT_OK_X, MNP_ACT_Y, MNP_ACT_BTN_W, MNP_ACT_H,
             "OK", COL_BLACK, COL_WHITE, 4);
}

void MidiNotePicker::redrawValueAndButtons() {
    drawValue();
    drawNoteButtons();
    drawOctaveButtons();
    _d.paintLater();
}

int MidiNotePicker::hitSemitone(int sx, int sy) const {
    if (sy < MNP_NBTN_Y || sy >= MNP_NBTN_Y + MNP_NBTN_H) return -1;
    int i = sx / MNP_NBTN_W;
    return (i >= 0 && i < 12) ? i : -1;
}

int MidiNotePicker::hitOctave(int sx, int sy) const {
    if (sy < MNP_OBTN_Y || sy >= MNP_OBTN_Y + MNP_OBTN_H) return -1;
    int stride = MNP_OBTN_W + MNP_OBTN_GAP;
    int o = sx / stride;
    int xInCell = sx - o * stride;
    if (xInCell >= MNP_OBTN_W) return -1;  // tap landed in the gap
    return (o >= 0 && o < 11) ? o : -1;
}

void MidiNotePicker::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

bool MidiNotePicker::poll() {
    if (!_open) return true;
    if (!_touch.read()) return false;
    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (down && !_wasDown) { _wasDown = true; return false; }
    if (!down && _wasDown) {
        _wasDown = false;

        // CANCEL / OK in the action row
        if (sy >= MNP_ACT_Y && sy < MNP_ACT_Y + MNP_ACT_H) {
            if (sx >= MNP_ACT_CANCEL_X && sx < MNP_ACT_CANCEL_X + MNP_ACT_BTN_W) {
                _open = false; _accepted = false; return true;
            }
            if (sx >= MNP_ACT_OK_X && sx < MNP_ACT_OK_X + MNP_ACT_BTN_W) {
                _value = (uint8_t)clampi(_octave * 12 + _semitone, _min, _max);
                _open = false; _accepted = true; return true;
            }
        }

        int hi;
        if ((hi = hitSemitone(sx, sy)) >= 0) {
            int candidate = _octave * 12 + hi;
            if (candidate >= _min && candidate <= _max) {
                _semitone = (uint8_t)hi;
                _value    = (uint8_t)candidate;
                redrawValueAndButtons();
            }
            return false;
        }
        if ((hi = hitOctave(sx, sy)) >= 0) {
            int candidate = hi * 12 + _semitone;
            if (candidate >= _min && candidate <= _max) {
                _octave = (uint8_t)hi;
                _value  = (uint8_t)candidate;
                redrawValueAndButtons();
            }
            return false;
        }
    }
    return false;
}

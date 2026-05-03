#include "SettingsPage.h"
#include <Preferences.h>
#include <string.h>
#include <stdio.h>

// ── Global MIDI limits ───────────────────────────────────────────────────────
uint8_t gMaxBank    = 127;   // 0-based: 0..127 → displayed as 1..128
uint8_t gMaxProgram = 127;

static const char* MIDI_NVS_NS = "magitrac_midi";

void loadMidiLimits() {
    Preferences prefs;
    prefs.begin(MIDI_NVS_NS, true);
    gMaxBank    = prefs.getUChar("maxBank",    127);
    gMaxProgram = prefs.getUChar("maxProgram", 127);
    prefs.end();
}

void saveMidiLimits() {
    Preferences prefs;
    prefs.begin(MIDI_NVS_NS, false);
    prefs.putUChar("maxBank",    gMaxBank);
    prefs.putUChar("maxProgram", gMaxProgram);
    prefs.end();
}

SettingsPage::SettingsPage(EPD_PainterAdafruit& display, GT911_Lite& touch,
                           I2C_BM8563& rtc)
    : _d(display)
    , _touch(touch)
    , _rtc(rtc)
    , _numpad(display, touch)
    , _wasDown(false)
    , _editing(0)
    , _year(2025), _month(1), _day(1)
    , _hour(0), _minute(0), _second(0)
{}

// ── open / draw ───────────────────────────────────────────────────────────────

void SettingsPage::open() {
    _wasDown = _touch.isTouched;
    _editing = 0;
    readRTC();
}

void SettingsPage::draw() {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    drawSectionLabel();
    drawTimeRow();
    drawDateRow();
    drawMidiSection();
}

// ── RTC ───────────────────────────────────────────────────────────────────────

void SettingsPage::readRTC() {
    I2C_BM8563_DateTypeDef d;
    I2C_BM8563_TimeTypeDef t;
    _rtc.getDate(&d);
    _rtc.getTime(&t);
    _year   = d.year;
    _month  = d.month;
    _day    = d.date;
    _hour   = t.hours;
    _minute = t.minutes;
    _second = t.seconds;
}

// ── Header ────────────────────────────────────────────────────────────────────

void SettingsPage::drawHeader() {
    _d.fillRect(0, 0, SP_W, SP_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    const char* title = "SETTINGS";
    int tw = strlen(title) * 18;
    _d.setCursor((SP_W - tw) / 2, (SP_HDR_H - 24) / 2);
    _d.print(title);
    uiButton(_d, SP_HOME_X, 0, SP_HOME_W, SP_HDR_H, "HOME", COL_BLACK, COL_WHITE, 3);
}

void SettingsPage::drawSectionLabel() {
    _d.fillRect(0, SP_LBL_Y, SP_W, SP_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, SP_LBL_Y + (SP_LBL_H - 16) / 2);
    _d.print("DATE / TIME");
}

// ── Rows ──────────────────────────────────────────────────────────────────────

void SettingsPage::drawTimeRow() {
    _d.fillRect(0, SP_TIME_Y, SP_W, SP_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, SP_TIME_Y + SP_ROW_H - 1, SP_W, COL_LTGREY);

    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SP_LABEL_X, SP_TIME_Y + (SP_ROW_H - 24) / 2);
    _d.print("Time:");

    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", _hour, _minute, _second);
    _d.setTextSize(4);
    _d.setCursor(SP_VAL_X, SP_TIME_Y + (SP_ROW_H - 32) / 2);
    _d.print(buf);
}

void SettingsPage::drawDateRow() {
    _d.fillRect(0, SP_DATE_Y, SP_W, SP_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, SP_DATE_Y + SP_ROW_H - 1, SP_W, COL_LTGREY);

    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SP_LABEL_X, SP_DATE_Y + (SP_ROW_H - 24) / 2);
    _d.print("Date:");

    char buf[12];
    snprintf(buf, sizeof(buf), "%02d/%02d/%02d", _day, _month, _year % 100);
    _d.setTextSize(4);
    _d.setCursor(SP_VAL_X, SP_DATE_Y + (SP_ROW_H - 32) / 2);
    _d.print(buf);
}

// ── Numpad result → RTC ──────────────────────────────────────────────────────

void SettingsPage::applyNumpadResult() {
    const char* d = _numpad.digits();
    int n = _numpad.numDigits();

    if (_editing == 1 && n == 6) {
        // HHMMSS
        int hh = (d[0] - '0') * 10 + (d[1] - '0');
        int mm = (d[2] - '0') * 10 + (d[3] - '0');
        int ss = (d[4] - '0') * 10 + (d[5] - '0');
        if (hh > 23) hh = 23;
        if (mm > 59) mm = 59;
        if (ss > 59) ss = 59;
        _hour   = hh;
        _minute = mm;
        _second = ss;

        I2C_BM8563_TimeTypeDef t;
        t.hours   = _hour;
        t.minutes = _minute;
        t.seconds = _second;
        _rtc.setTime(&t);
        Serial.printf("[RTC] Set time %02d:%02d:%02d\n", _hour, _minute, _second);

    } else if (_editing == 2 && n == 6) {
        // DDMMYY
        int dd = (d[0] - '0') * 10 + (d[1] - '0');
        int mm = (d[2] - '0') * 10 + (d[3] - '0');
        int yy = (d[4] - '0') * 10 + (d[5] - '0');
        int year = 2000 + yy;
        if (mm < 1) mm = 1;
        if (mm > 12) mm = 12;
        if (dd < 1) dd = 1;
        if (dd > 31) dd = 31;
        _year  = year;
        _month = mm;
        _day   = dd;

        I2C_BM8563_DateTypeDef dt;
        dt.year    = _year;
        dt.month   = _month;
        dt.date    = _day;
        dt.weekDay = 0;
        _rtc.setDate(&dt);
        Serial.printf("[RTC] Set date %02d/%02d/%04d\n", _day, _month, _year);
    }
}

// ── MIDI section ──────────────────────────────────────────────────────────

void SettingsPage::drawMidiSection() {
    _d.fillRect(0, SP_MIDI_LBL_Y, SP_W, SP_MIDI_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, SP_MIDI_LBL_Y + (SP_MIDI_LBL_H - 16) / 2);
    _d.print("MIDI");
    for (int i = 0; i < SP_NUM_MIDI; i++) drawMidiRow(i);
}

void SettingsPage::drawMidiRow(int field) {
    int y    = SP_MIDI_Y + field * SP_MIDI_ROW_H;
    int btnY = y + (SP_MIDI_ROW_H - SP_MIDI_BTN_H) / 2;

    _d.fillRect(0, y, SP_W, SP_MIDI_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, y + SP_MIDI_ROW_H - 1, SP_W, COL_BLACK);

    const char* label = (field == 0) ? "MAX BANK" : "MAX PROGRAM";
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SP_LABEL_X, y + (SP_MIDI_ROW_H - 24) / 2);
    _d.print(label);

    char val[8];
    int v = (field == 0) ? (int)gMaxBank + 1 : (int)gMaxProgram + 1;
    snprintf(val, sizeof(val), "%d", v);
    _d.drawRect(SP_MIDI_VAL_X, btnY, SP_MIDI_VAL_W, SP_MIDI_BTN_H, COL_BLACK);
    int vw = (int)strlen(val) * 18;
    _d.setCursor(SP_MIDI_VAL_X + (SP_MIDI_VAL_W - vw) / 2, btnY + (SP_MIDI_BTN_H - 24) / 2);
    _d.print(val);

    uiButton(_d, SP_MIDI_MINUS_X, btnY, SP_MIDI_BTN_W, SP_MIDI_BTN_H, "-", COL_WHITE, COL_BLACK, 3);
    uiButton(_d, SP_MIDI_PLUS_X,  btnY, SP_MIDI_BTN_W, SP_MIDI_BTN_H, "+", COL_WHITE, COL_BLACK, 3);
}

void SettingsPage::adjustMidi(int field, int delta) {
    if (field == 0) {
        int v = (int)gMaxBank + delta;
        gMaxBank = (uint8_t)constrain(v, 0, 127);
    } else {
        int v = (int)gMaxProgram + delta;
        gMaxProgram = (uint8_t)constrain(v, 0, 127);
    }
    saveMidiLimits();
}

void SettingsPage::fireMidiHeld() {
    int f = _hold.active() ? _hold.field() : _hold.savedField();
    int d = _hold.active() ? _hold.delta() : _hold.savedDelta();
    if (f >= 0) {
        adjustMidi(f, d);
        drawMidiRow(f);
        _d.paintLater();
    }
}

// ── Hit tests — MIDI rows ─────────────────────────────────────────────────

int SettingsPage::hitMidiMinus(int sx, int sy) const {
    if (sy < SP_MIDI_Y || sy >= SP_MIDI_Y + SP_NUM_MIDI * SP_MIDI_ROW_H) return -1;
    int field = (sy - SP_MIDI_Y) / SP_MIDI_ROW_H;
    int btnY  = SP_MIDI_Y + field * SP_MIDI_ROW_H + (SP_MIDI_ROW_H - SP_MIDI_BTN_H) / 2;
    if (sy < btnY || sy >= btnY + SP_MIDI_BTN_H) return -1;
    return (sx >= SP_MIDI_MINUS_X && sx < SP_MIDI_MINUS_X + SP_MIDI_BTN_W) ? field : -1;
}

int SettingsPage::hitMidiPlus(int sx, int sy) const {
    if (sy < SP_MIDI_Y || sy >= SP_MIDI_Y + SP_NUM_MIDI * SP_MIDI_ROW_H) return -1;
    int field = (sy - SP_MIDI_Y) / SP_MIDI_ROW_H;
    int btnY  = SP_MIDI_Y + field * SP_MIDI_ROW_H + (SP_MIDI_ROW_H - SP_MIDI_BTN_H) / 2;
    if (sy < btnY || sy >= btnY + SP_MIDI_BTN_H) return -1;
    return (sx >= SP_MIDI_PLUS_X && sx < SP_MIDI_PLUS_X + SP_MIDI_BTN_W) ? field : -1;
}

// ── Hit tests ─────────────────────────────────────────────────────────────────

bool SettingsPage::hitHome(int sx, int sy) const {
    return sx >= SP_HOME_X && sx < SP_HOME_X + SP_HOME_W
        && sy >= 0 && sy < SP_HDR_H;
}

bool SettingsPage::hitTime(int sx, int sy) const {
    return sy >= SP_TIME_Y && sy < SP_TIME_Y + SP_ROW_H;
}

bool SettingsPage::hitDate(int sx, int sy) const {
    return sy >= SP_DATE_Y && sy < SP_DATE_Y + SP_ROW_H;
}

void SettingsPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

// ── poll ──────────────────────────────────────────────────────────────────────

bool SettingsPage::poll() {
    // Numpad is open — delegate to it
    if (_editing > 0) {
        if (_numpad.poll()) {
            if (_numpad.isDone()) applyNumpadResult();
            _editing = 0;
            _d.fillScreen(COL_WHITE);
            draw();
            _d.paintLater();
        }
        return false;
    }

    // Hold-repeat for MIDI rows
    if (_wasDown && _hold.active() && _hold.tickFast()) fireMidiHeld();

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    bool rising  = (down && !_wasDown);
    bool falling = (!down && _wasDown);

    if (rising) {
        _wasDown = true;

        // Check MIDI +/- on rising edge (start hold)
        int fi;
        if ((fi = hitMidiMinus(sx, sy)) >= 0) {
            _hold.start(fi, -1);
        } else if ((fi = hitMidiPlus(sx, sy)) >= 0) {
            _hold.start(fi, +1);
        }
        return false;
    }

    if (falling) {
        _wasDown = false;
        _hold.release();

        if (hitHome(sx, sy)) return true;

        if (hitTime(sx, sy)) {
            _editing = 1;
            static const char seps[] = { ':', ':' };
            static const int  sepPos[] = { 2, 4 };
            _numpad.open("HH:MM:SS", seps, sepPos, 2, 6);
            _d.fillScreen(COL_WHITE);
            _numpad.draw();
            _d.paint();
            return false;
        }

        if (hitDate(sx, sy)) {
            _editing = 2;
            static const char seps[] = { '/', '/' };
            static const int  sepPos[] = { 2, 4 };
            _numpad.open("DD/MM/YY", seps, sepPos, 2, 6);
            _d.fillScreen(COL_WHITE);
            _numpad.draw();
            _d.paint();
            return false;
        }

        // Single-tap MIDI +/- (no hold fired)
        if (!_hold.wasFired() && _hold.savedField() >= 0) {
            fireMidiHeld();
        }
    }

    return false;
}

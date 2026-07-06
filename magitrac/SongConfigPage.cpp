#include "SongConfigPage.h"
#include <string.h>
#include <stdio.h>

static const char* MIDI_NOTE_NAMES[12] = {
    "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
};

static void midiNoteStr(uint8_t note, char* buf) {
    snprintf(buf, 6, "%s%d", MIDI_NOTE_NAMES[note % 12], note / 12);
}

// ── Constructor ───────────────────────────────────────────────────────────────

SongConfigPage::SongConfigPage(EPD_PainterAdafruit& display, GT911_Lite& touch,
                                Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _wasDown(false)
    , _patchPending(false)
    , _picker(display, touch)
{}

// ── open / draw ───────────────────────────────────────────────────────────────

void SongConfigPage::open() {
    _patchPending = false;
    _wasDown      = _touch.isTouched;
}

void SongConfigPage::draw() {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    drawPage0();
}

// ── Header ────────────────────────────────────────────────────────────────────

void SongConfigPage::drawHeader() {
    _d.fillRect(0, 0, 960, SC_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);

    const char* title = "SONG SETTINGS";
    int tw = 13 * 18;  // "SONG SETTINGS"
    _d.setCursor((960 - tw) / 2, (SC_HDR_H - 24) / 2);
    _d.print(title);

    uiButton(_d, SC_HOME_X, 0, SC_HOME_W, SC_HDR_H, "HOME", COL_BLACK, COL_WHITE, 3);
}

// ── Sub-page 0: BPM + MIDI IN + SLOT ENABLE ──────────────────────────────────

void SongConfigPage::drawPage0() {
    drawBpmSection();
    drawMidiInSection();
    drawSlotSection();
    drawTransposeSection();
}

void SongConfigPage::drawBpmSection() {
    _d.fillRect(0, SC_BPM_LBL_Y, 960, SC_BPM_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, SC_BPM_LBL_Y + (SC_BPM_LBL_H - 16) / 2);
    _d.print("BPM");
    for (int i = 0; i < SC_NUM_BPM; i++) drawBpmRow(i);
}

void SongConfigPage::drawBpmRow(int field) {
    int y    = SC_BPM_Y + field * SC_BPM_ROW_H;
    int btnY = y + (SC_BPM_ROW_H - SC_ROW_BTN_H) / 2;

    _d.fillRect(0, y, 960, SC_BPM_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, y + SC_BPM_ROW_H - 1, 960, COL_BLACK);

    char label[16];
    bpmLabel(field, label);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SC_ROW_LABEL_X, y + (SC_BPM_ROW_H - 24) / 2);
    _d.print(label);

    char val[8];
    bpmValue(field, val);
    _d.drawRect(SC_ROW_VAL_X, btnY, SC_ROW_VAL_W, SC_ROW_BTN_H, COL_BLACK);
    int vw = (int)strlen(val) * 18;
    _d.setCursor(SC_ROW_VAL_X + (SC_ROW_VAL_W - vw) / 2, btnY + (SC_ROW_BTN_H - 24) / 2);
    _d.print(val);

    uiButton(_d, SC_ROW_MINUS_X, btnY, SC_ROW_BTN_W, SC_ROW_BTN_H, "-", COL_WHITE, COL_BLACK, 3);
    uiButton(_d, SC_ROW_PLUS_X,  btnY, SC_ROW_BTN_W, SC_ROW_BTN_H, "+", COL_WHITE, COL_BLACK, 3);
}

void SongConfigPage::drawMidiInSection() {
    _d.fillRect(0, SC_MI_LBL_Y, 960, SC_MI_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, SC_MI_LBL_Y + (SC_MI_LBL_H - 16) / 2);
    _d.print("MIDI IN");
    for (int i = 0; i < SC_NUM_MI; i++) drawMidiInRow(i);
}

void SongConfigPage::drawMidiInRow(int field) {
    int y    = SC_MI_Y + field * SC_MI_ROW_H;
    int btnY = y + (SC_MI_ROW_H - SC_ROW_BTN_H) / 2;

    _d.fillRect(0, y, 960, SC_MI_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, y + SC_MI_ROW_H - 1, 960, COL_BLACK);

    char label[16];
    midiInLabel(field, label);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SC_ROW_LABEL_X, y + (SC_MI_ROW_H - 24) / 2);
    _d.print(label);

    char val[8];
    midiInValue(field, val);

    if (field == 0) {
        // CHANNEL — keep the +/- stepper.
        _d.drawRect(SC_ROW_VAL_X, btnY, SC_ROW_VAL_W, SC_ROW_BTN_H, COL_BLACK);
        int vw = (int)strlen(val) * 18;
        _d.setCursor(SC_ROW_VAL_X + (SC_ROW_VAL_W - vw) / 2,
                     btnY + (SC_ROW_BTN_H - 24) / 2);
        _d.print(val);
        uiButton(_d, SC_ROW_MINUS_X, btnY, SC_ROW_BTN_W, SC_ROW_BTN_H,
                 "-", COL_WHITE, COL_BLACK, 3);
        uiButton(_d, SC_ROW_PLUS_X,  btnY, SC_ROW_BTN_W, SC_ROW_BTN_H,
                 "+", COL_WHITE, COL_BLACK, 3);
    } else {
        // NOTE LOW / NOTE HIGH — single wide tappable button opens picker.
        int x = SC_ROW_VAL_X;
        int w = SC_ROW_PLUS_X + SC_ROW_BTN_W - SC_ROW_VAL_X;
        uiButton(_d, x, btnY, w, SC_ROW_BTN_H, val, COL_WHITE, COL_BLACK, 3);
    }
}

// ── Slot Enable section ──────────────────────────────────────────────────────

void SongConfigPage::drawSlotSection() {
    _d.fillRect(0, SC_SL_LBL_Y, 960, SC_SL_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, SC_SL_LBL_Y + (SC_SL_LBL_H - 16) / 2);
    _d.print("SLOT ENABLE");

    _d.fillRect(0, SC_SL_Y, 960, SC_SL_H, COL_WHITE);

    static const char* SLOT_NAMES[4] = { "A", "B", "C", "D" };
    int btnY = SC_SL_Y + (SC_SL_H - SC_SL_BTN_H) / 2;

    for (int i = 0; i < SC_SL_COUNT; i++) {
        int x = SC_SL_X0 + i * (SC_SL_BTN_W + SC_SL_GAP);
        bool on = (_song.performerMask >> i) & 1;
        if (on) {
            uiButton(_d, x, btnY, SC_SL_BTN_W, SC_SL_BTN_H, SLOT_NAMES[i], COL_BLACK, COL_WHITE, 3);
        } else {
            uiButton(_d, x, btnY, SC_SL_BTN_W, SC_SL_BTN_H, SLOT_NAMES[i], COL_WHITE, COL_BLACK, 3);
        }
    }
}

// ── Transpose Channels section ───────────────────────────────────────────────

void SongConfigPage::drawTransposeSection() {
    _d.fillRect(0, SC_TR_LBL_Y, 960, SC_TR_LBL_H, COL_LTGREY);
    _d.setTextSize(1);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, SC_TR_LBL_Y + (SC_TR_LBL_H - 8) / 2);
    _d.print("TRANSPOSE CH");

    _d.fillRect(0, SC_TR_Y, 960, SC_TR_H, COL_WHITE);

    int btnY = SC_TR_Y + (SC_TR_H - SC_TR_BTN_H) / 2;
    char lbl[4];
    for (int i = 0; i < SC_TR_COUNT; i++) {
        int x = SC_TR_X0 + i * (SC_TR_BTN_W + SC_TR_GAP);
        snprintf(lbl, sizeof(lbl), "%d", i + 1);
        bool on = (_song.transposeChMask >> i) & 1;
        if (on) {
            uiButton(_d, x, btnY, SC_TR_BTN_W, SC_TR_BTN_H, lbl, COL_BLACK, COL_WHITE, 2);
        } else {
            uiButton(_d, x, btnY, SC_TR_BTN_W, SC_TR_BTN_H, lbl, COL_WHITE, COL_BLACK, 2);
        }
    }
}

// ── Value helpers — BPM ───────────────────────────────────────────────────────

static const char* SC_BPM_LABELS[3] = { "INITIAL BPM", "MIN BPM", "MAX BPM" };

void SongConfigPage::bpmLabel(int field, char* out) const {
    strncpy(out, SC_BPM_LABELS[field], 15);
    out[15] = '\0';
}

void SongConfigPage::bpmValue(int field, char* out) const {
    switch (field) {
        case 0: snprintf(out, 8, "%d", (int)_song.bpm);    break;
        case 1: snprintf(out, 8, "%d", (int)_song.minBPM); break;
        case 2: snprintf(out, 8, "%d", (int)_song.maxBPM); break;
        default: out[0] = '\0'; break;
    }
}

void SongConfigPage::adjustBpm(int field, int delta) {
    switch (field) {
        case 0: _song.bpm    = (uint16_t)constrain((int)_song.bpm    + delta, (int)_song.minBPM, (int)_song.maxBPM); break;
        case 1: _song.minBPM = (uint16_t)constrain((int)_song.minBPM + delta, 20,                (int)_song.bpm);    break;
        case 2: _song.maxBPM = (uint16_t)constrain((int)_song.maxBPM + delta, (int)_song.bpm,    400);               break;
    }
}

// ── Value helpers — MIDI IN ───────────────────────────────────────────────────

static const char* SC_MI_LABELS[3] = { "CHANNEL", "NOTE LOW", "NOTE HIGH" };

void SongConfigPage::midiInLabel(int field, char* out) const {
    strncpy(out, SC_MI_LABELS[field], 15);
    out[15] = '\0';
}

void SongConfigPage::midiInValue(int field, char* out) const {
    switch (field) {
        case 0:
            if (_song.midiInChannel == 0) strcpy(out, "ANY");
            else snprintf(out, 8, "%d", (int)_song.midiInChannel);
            break;
        case 1: midiNoteStr(_song.midiInNoteMin, out); break;
        case 2: midiNoteStr(_song.midiInNoteMax, out); break;
        default: out[0] = '\0'; break;
    }
}

void SongConfigPage::adjustMidiIn(int field, int delta) {
    switch (field) {
        case 0:
            _song.midiInChannel = (uint8_t)constrain((int)_song.midiInChannel + delta, 0, 16);
            break;
        case 1:
            _song.midiInNoteMin = (uint8_t)constrain((int)_song.midiInNoteMin + delta, 0, (int)_song.midiInNoteMax);
            break;
        case 2:
            _song.midiInNoteMax = (uint8_t)constrain((int)_song.midiInNoteMax + delta, (int)_song.midiInNoteMin, 127);
            break;
    }
}

// ── Poll ──────────────────────────────────────────────────────────────────────

void SongConfigPage::fireHeld() {
    int f = _hold.active() ? _hold.field()  : _hold.savedField();
    int d = _hold.active() ? _hold.delta()  : _hold.savedDelta();
    int t = _hold.active() ? _hold.type()   : _hold.savedType();
    switch (t) {
        case 1: adjustBpm   (f, d); drawBpmRow   (f); break;
        case 2: adjustMidiIn(f, d); drawMidiInRow(f); break;
    }
    _patchPending = true;
    _d.paintLater();
}

bool SongConfigPage::poll() {

    // ── Note picker overlay takes precedence ──────────────────────────────────
    if (_pickerField >= 0) {
        if (_picker.poll()) {
            if (_picker.accepted()) {
                uint8_t v = _picker.value();
                if (_pickerField == 1) {
                    if (v > _song.midiInNoteMax) v = _song.midiInNoteMax;
                    _song.midiInNoteMin = v;
                } else if (_pickerField == 2) {
                    if (v < _song.midiInNoteMin) v = _song.midiInNoteMin;
                    _song.midiInNoteMax = v;
                }
                _patchPending = true;
            }
            _pickerField = -1;
            _wasDown     = _touch.isTouched;
            draw();
            _d.paintLater();
        }
        return false;
    }

    // ── Hold-repeat ───────────────────────────────────────────────────────────
    if (_wasDown && _hold.active() && _hold.tickSlow()) fireHeld();

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // ── Rising edge — record held button ──────────────────────────────────────
    if (down && !_wasDown) {
        _wasDown = true;
        int fi;
        if      ((fi = hitRowMinus(SC_BPM_Y, SC_BPM_ROW_H, SC_NUM_BPM, sx, sy)) >= 0) _hold.start(fi, -1, 1);
        else if ((fi = hitRowPlus (SC_BPM_Y, SC_BPM_ROW_H, SC_NUM_BPM, sx, sy)) >= 0) _hold.start(fi, +1, 1);
        else if ((fi = hitRowMinus(SC_MI_Y,  SC_MI_ROW_H,  1,          sx, sy)) >= 0) _hold.start(fi, -1, 2);
        else if ((fi = hitRowPlus (SC_MI_Y,  SC_MI_ROW_H,  1,          sx, sy)) >= 0) _hold.start(fi, +1, 2);
        return false;
    }

    // ── Falling edge ──────────────────────────────────────────────────────────
    if (!down && _wasDown) {
        _wasDown = false;
        _hold.release();

        if (hitHome(sx, sy)) return true;

        // NOTE LOW / NOTE HIGH row tap → open picker
        for (int f = 1; f <= 2; f++) {
            int rowY = SC_MI_Y + f * SC_MI_ROW_H;
            if (sy >= rowY && sy < rowY + SC_MI_ROW_H) {
                _pickerField = f;
                uint8_t initial = (f == 1) ? _song.midiInNoteMin : _song.midiInNoteMax;
                const char* title = (f == 1) ? "MIDI-IN NOTE LOW" : "MIDI-IN NOTE HIGH";
                _picker.open(initial, 0, 127, title);
                _picker.draw();
                _d.paintLater();
                return false;
            }
        }

        // Slot toggle (falling edge only, no hold-repeat)
        int slot = hitSlot(sx, sy);
        if (slot >= 0) {
            _song.performerMask ^= (1 << slot);
            drawSlotSection();
            _patchPending = true;
            _d.paintLater();
            return false;
        }

        // Transpose-channel toggle (falling edge only)
        int trCh = hitTranspose(sx, sy);
        if (trCh >= 0) {
            _song.transposeChMask ^= (uint16_t)(1 << trCh);
            drawTransposeSection();
            _patchPending = true;
            _d.paintLater();
            return false;
        }

        // Only fire a quick-tap if the rising edge actually hit a +/- button
        // (savedField >= 0 means start() was called this press).
        if (!_hold.wasFired() && _hold.savedType() > 0 && _hold.savedField() >= 0) {
            fireHeld();
        }
    }

    return false;
}

// ── Hit tests ─────────────────────────────────────────────────────────────────

bool SongConfigPage::hitHome(int sx, int sy) const {
    return (sx >= SC_HOME_X && sx < SC_HOME_X + SC_HOME_W && sy >= 0 && sy < SC_HDR_H);
}

int SongConfigPage::hitRowMinus(int baseY, int rowH, int numRows, int sx, int sy) const {
    if (sy < baseY || sy >= baseY + numRows * rowH) return -1;
    int field = (sy - baseY) / rowH;
    int btnY  = baseY + field * rowH + (rowH - SC_ROW_BTN_H) / 2;
    if (sy < btnY || sy >= btnY + SC_ROW_BTN_H) return -1;
    return (sx >= SC_ROW_MINUS_X && sx < SC_ROW_MINUS_X + SC_ROW_BTN_W) ? field : -1;
}

int SongConfigPage::hitRowPlus(int baseY, int rowH, int numRows, int sx, int sy) const {
    if (sy < baseY || sy >= baseY + numRows * rowH) return -1;
    int field = (sy - baseY) / rowH;
    int btnY  = baseY + field * rowH + (rowH - SC_ROW_BTN_H) / 2;
    if (sy < btnY || sy >= btnY + SC_ROW_BTN_H) return -1;
    return (sx >= SC_ROW_PLUS_X && sx < SC_ROW_PLUS_X + SC_ROW_BTN_W) ? field : -1;
}

int SongConfigPage::hitSlot(int sx, int sy) const {
    int btnY = SC_SL_Y + (SC_SL_H - SC_SL_BTN_H) / 2;
    if (sy < btnY || sy >= btnY + SC_SL_BTN_H) return -1;
    for (int i = 0; i < SC_SL_COUNT; i++) {
        int x = SC_SL_X0 + i * (SC_SL_BTN_W + SC_SL_GAP);
        if (sx >= x && sx < x + SC_SL_BTN_W) return i;
    }
    return -1;
}

int SongConfigPage::hitTranspose(int sx, int sy) const {
    int btnY = SC_TR_Y + (SC_TR_H - SC_TR_BTN_H) / 2;
    if (sy < btnY || sy >= btnY + SC_TR_BTN_H) return -1;
    for (int i = 0; i < SC_TR_COUNT; i++) {
        int x = SC_TR_X0 + i * (SC_TR_BTN_W + SC_TR_GAP);
        if (sx >= x && sx < x + SC_TR_BTN_W) return i;
    }
    return -1;
}

void SongConfigPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

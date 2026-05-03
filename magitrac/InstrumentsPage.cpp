#include "InstrumentsPage.h"
#include <string.h>
#include <stdio.h>

// ── Constructor ───────────────────────────────────────────────────────────────

InstrumentsPage::InstrumentsPage(EPD_PainterAdafruit& display,
                                 GT911_Lite& touch, Instrument* instruments)
    : _d(display)
    , _touch(touch)
    , _instruments(instruments)
    , _keyboard(display, touch)
    , _state(State::LIST)
    , _listPage(0)
    , _editIdx(0)
    , _wasDown(false)
    , _anyEditMade(false)
    , _saveOnExit(true)
{}

// ── open() ────────────────────────────────────────────────────────────────────

void InstrumentsPage::open() {
    _state        = State::LIST;
    _listPage     = 0;
    _anyEditMade  = false;
    _saveOnExit   = true;
    _wasDown      = _touch.isTouched;
}

// ── draw() ────────────────────────────────────────────────────────────────────

void InstrumentsPage::draw() {
    _d.fillScreen(COL_WHITE);
    if (_state == State::LIST) {
        drawList();
    } else if (_state == State::CONFIRM) {
        drawConfirm();
    } else {
        drawEdit();
        if (_state == State::NAMING) _keyboard.draw();
    }
}

// ── poll() ────────────────────────────────────────────────────────────────────

bool InstrumentsPage::poll() {

    // ── NAMING — keyboard active ──────────────────────────────────────────────
    if (_state == State::NAMING) {
        if (_keyboard.poll()) {
            if (_keyboard.isDone()) _anyEditMade = true;
            _state = State::EDIT;
            _d.fillScreen(COL_WHITE);
            drawEdit();
            _d.paintLater();
        }
        return false;
    }

    // ── Hold-repeat ───────────────────────────────────────────────────────────
    if (_wasDown && _hold.active() && _hold.tickSlow()) {
        adjustField(_hold.field(), _hold.delta());
        drawEditField(_hold.field());
        _d.paintLater();
    }

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // ── Rising edge — record held button ──────────────────────────────────────
    if (down && !_wasDown) {
        _wasDown = true;
        if (_state == State::EDIT) {
            int fi = hitMinus(sx, sy);
            if (fi >= 0) {
                _hold.start(fi, -1);
            } else {
                fi = hitPlus(sx, sy);
                if (fi >= 0) _hold.start(fi, +1);
            }
        }
        return false;
    }

    // ── Falling edge ──────────────────────────────────────────────────────────
    if (!down && _wasDown) {
        _wasDown = false;
        _hold.release();

        // ── CONFIRM state ─────────────────────────────────────────────────────
        if (_state == State::CONFIRM) {
            if (sx >= 150 && sx < 400 && sy >= 310 && sy < 400) {
                // YES — save
                _saveOnExit = true;
                return true;
            }
            if (sx >= 560 && sx < 810 && sy >= 310 && sy < 400) {
                // NO — discard
                _saveOnExit = false;
                return true;
            }
            return false;
        }

        // ── LIST state ────────────────────────────────────────────────────────
        if (_state == State::LIST) {

            if (hitListHome(sx, sy)) {
                if (_anyEditMade) {
                    _state = State::CONFIRM;
                    _d.fillScreen(COL_WHITE);
                    drawConfirm();
                    _d.paintLater();
                } else {
                    return true;
                }
            }

            if (hitPrev(sx, sy) && _listPage > 0) {
                _listPage--;
                _d.fillScreen(COL_WHITE);
                drawList();
                _d.paintLater();
            } else if (hitNext(sx, sy) &&
                       (_listPage + 1) * IP_PER_PAGE < MAX_INSTRUMENTS) {
                _listPage++;
                _d.fillScreen(COL_WHITE);
                drawList();
                _d.paintLater();
            } else {
                int idx = hitRow(sx, sy);
                if (idx >= 0 && idx < MAX_INSTRUMENTS) {
                    _editIdx = (uint8_t)idx;
                    _state   = State::EDIT;
                    _d.fillScreen(COL_WHITE);
                    drawEdit();
                    _d.paintLater();
                }
            }

        // ── EDIT state ────────────────────────────────────────────────────────
        } else if (_state == State::EDIT) {

            if (hitEditHome(sx, sy)) {
                if (_anyEditMade) {
                    _state = State::CONFIRM;
                    _d.fillScreen(COL_WHITE);
                    drawConfirm();
                    _d.paintLater();
                } else {
                    return true;
                }
            }

            if (hitBack(sx, sy)) {
                _state = State::LIST;
                _d.fillScreen(COL_WHITE);
                drawList();
                _d.paintLater();
            } else if (hitName(sx, sy)) {
                _keyboard.open(_instruments[_editIdx].name,
                               INSTRUMENT_NAME_LEN);
                _state = State::NAMING;
                _d.fillScreen(COL_WHITE);
                drawEdit();
                _keyboard.draw();
                _d.paintLater();
            } else if (!_hold.wasFired() && _hold.savedField() >= 0) {
                // Quick tap — fire the single increment now
                adjustField(_hold.savedField(), _hold.savedDelta());
                drawEditField(_hold.savedField());
                _d.paintLater();
            }
        }
    }

    return false;
}

// ── List drawing ──────────────────────────────────────────────────────────────

void InstrumentsPage::drawList() {
    // Title bar (black) with centered title and HOME on the right
    _d.fillRect(0, 0, 960, IP_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    int tw = 11 * 18;  // "INSTRUMENTS"
    _d.setCursor((960 - tw) / 2, (IP_HDR_H - 24) / 2);
    _d.print("INSTRUMENTS");
    uiButton(_d, IP_LIST_HOME_X, 0, IP_LIST_HOME_W, IP_HDR_H,
             "HOME", COL_BLACK, COL_WHITE, 3);

    // Toolbar — pagination only
    _d.fillRect(0, IP_HDR_H, 960, IP_TOOL_H, COL_WHITE);
    bool hasPrev = (_listPage > 0);
    bool hasNext = ((_listPage + 1) * IP_PER_PAGE < MAX_INSTRUMENTS - 1);
    uiButton(_d, IP_PREV_X, IP_BTN_Y, IP_PREV_W, IP_BTN_H,
             "PREV", COL_WHITE, hasPrev ? COL_BLACK : COL_DKGREY, 3);
    uiButton(_d, IP_NEXT_X, IP_BTN_Y, IP_NEXT_W, IP_BTN_H,
             "NEXT", COL_WHITE, hasNext ? COL_BLACK : COL_DKGREY, 3);

    // Separator
    _d.drawFastHLine(0, IP_LIST_Y - 1, 960, COL_BLACK);

    // Instrument rows
    int first = _listPage * IP_PER_PAGE + 1;  // skip index 0 (reserved = no instrument)
    for (int i = 0; i < IP_PER_PAGE; i++) {
        drawListRow(first + i, IP_LIST_Y + i * IP_ROW_H);
    }
}

void InstrumentsPage::drawListRow(int idx, int y) {
    _d.fillRect(0, y, 960, IP_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, y + IP_ROW_H - 1, 960, COL_BLACK);

    // ID box — 2-digit hex
    char id[4];
    snprintf(id, sizeof(id), "%02X", idx);
    _d.fillRect(8, y + 6, 44, 42, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(10, y + 6 + (42 - 24) / 2);
    _d.print(id);

    Instrument& inst = _instruments[idx];

    // Name
    _d.setTextColor(COL_BLACK);
    _d.setCursor(62, y + (IP_ROW_H - 24) / 2);
    _d.print(inst.name);

    // BANK and PROG (right side, textSize 2 = 12px/char wide, 16px tall)
    char info[24];
    snprintf(info, sizeof(info), "B:%03d  P:%03d",
             inst.bankMSB + 1, inst.program + 1);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(680, y + (IP_ROW_H - 16) / 2);
    _d.print(info);
}

// ── Edit drawing ──────────────────────────────────────────────────────────────

void InstrumentsPage::drawEdit() {
    drawEditHeader();
    for (int i = 0; i < IP_NUM_FIELDS; i++) drawEditField(i);
}

void InstrumentsPage::drawEditHeader() {
    // Black title bar (matches other pages)
    _d.fillRect(0, 0, 960, IP_EDIT_HDR_H, COL_BLACK);

    // Left: BACK (returns to list — extended action). Right: HOME (exit page).
    uiButton(_d, IP_EDIT_BACK_X, 0, IP_EDIT_BACK_W, IP_EDIT_HDR_H,
             "BACK", COL_BLACK, COL_WHITE, 3);
    uiButton(_d, IP_EDIT_HOME_X, 0, IP_EDIT_HOME_W, IP_EDIT_HDR_H,
             "HOME", COL_BLACK, COL_WHITE, 3);

    // ID label (white on black)
    char id[4];
    snprintf(id, sizeof(id), "%02X", _editIdx);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(IP_EDIT_BACK_W + 12, (IP_EDIT_HDR_H - 24) / 2);
    _d.print(id);

    // Name — tappable input field (white box with black text on the black bar)
    Instrument& inst = _instruments[_editIdx];
    _d.fillRect(IP_EDIT_NAME_X, 6, IP_EDIT_NAME_W, IP_EDIT_HDR_H - 12, COL_WHITE);
    _d.drawRect(IP_EDIT_NAME_X, 6, IP_EDIT_NAME_W, IP_EDIT_HDR_H - 12, COL_BLACK);
    _d.setTextColor(COL_BLACK);
    int nw = (int)strlen(inst.name) * 18;
    _d.setCursor(IP_EDIT_NAME_X + (IP_EDIT_NAME_W - nw) / 2,
                 (IP_EDIT_HDR_H - 24) / 2);
    _d.print(inst.name);
}

void InstrumentsPage::drawEditField(int field) {
    int y    = IP_FIELD_Y + field * IP_FIELD_H;
    int btnY = y + (IP_FIELD_H - IP_FBTN_H) / 2;

    _d.fillRect(0, y, 960, IP_FIELD_H, COL_WHITE);
    _d.drawFastHLine(0, y + IP_FIELD_H - 1, 960, COL_BLACK);

    // Label
    char label[16];
    fieldLabel(field, label);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(IP_LABEL_X, y + (IP_FIELD_H - 24) / 2);
    _d.print(label);

    // Value box
    char val[12];
    fieldValue(field, val);
    _d.drawRect(IP_VAL_X, btnY, IP_VAL_W, IP_FBTN_H, COL_BLACK);
    int vw = (int)strlen(val) * 18;
    _d.setCursor(IP_VAL_X + (IP_VAL_W - vw) / 2, btnY + (IP_FBTN_H - 24) / 2);
    _d.print(val);

    // [-] and [+] buttons
    uiButton(_d, IP_MINUS_X, btnY, IP_FBTN_W, IP_FBTN_H, "-", COL_WHITE, COL_BLACK, 3);
    uiButton(_d, IP_PLUS_X,  btnY, IP_FBTN_W, IP_FBTN_H, "+", COL_WHITE, COL_BLACK, 3);
}

// ── Value helpers ─────────────────────────────────────────────────────────────

static const char* IP_FIELD_LABELS[4] = {
    "BANK", "PROGRAM", "VOLUME", "TRANSPOSE"
};

void InstrumentsPage::fieldLabel(int field, char* out) const {
    strncpy(out, IP_FIELD_LABELS[field], 15);
    out[15] = '\0';
}

void InstrumentsPage::fieldValue(int field, char* out) const {
    const Instrument& inst = _instruments[_editIdx];
    switch (field) {
        case 0: snprintf(out, 12, "%d",  inst.bankMSB + 1);           break;
        case 1: snprintf(out, 12, "%d",  inst.program + 1);          break;
        case 2: snprintf(out, 12, "%d",  inst.volume);               break;
        case 3: snprintf(out, 12, "%+d", (int)inst.transpose);       break;
        default: out[0] = '\0'; break;
    }
}

void InstrumentsPage::adjustField(int field, int delta) {
    _anyEditMade = true;
    Instrument& inst = _instruments[_editIdx];
    switch (field) {
        case 0: inst.bankMSB     = (uint8_t)constrain((int)inst.bankMSB     + delta, 0,  127);  break;
        case 1: inst.program     = (uint8_t)constrain((int)inst.program     + delta, 0,  127);  break;
        case 2: inst.volume      = (uint8_t)constrain((int)inst.volume      + delta, 0,  127);  break;
        case 3: inst.transpose   =  (int8_t)constrain((int)inst.transpose   + delta, -24, 24);  break;
    }
}

// ── Hit tests — list ──────────────────────────────────────────────────────────

int InstrumentsPage::hitRow(int sx, int sy) const {
    if (sy < IP_LIST_Y || sy >= IP_LIST_Y + IP_PER_PAGE * IP_ROW_H) return -1;
    int vi = (sy - IP_LIST_Y) / IP_ROW_H;
    return _listPage * IP_PER_PAGE + vi + 1;  // +1: index 0 is reserved
}
bool InstrumentsPage::hitPrev(int sx, int sy) const {
    return (sx >= IP_PREV_X && sx < IP_PREV_X + IP_PREV_W &&
            sy >= IP_BTN_Y  && sy < IP_BTN_Y  + IP_BTN_H);
}
bool InstrumentsPage::hitNext(int sx, int sy) const {
    return (sx >= IP_NEXT_X && sx < IP_NEXT_X + IP_NEXT_W &&
            sy >= IP_BTN_Y  && sy < IP_BTN_Y  + IP_BTN_H);
}
bool InstrumentsPage::hitListHome(int sx, int sy) const {
    return (sx >= IP_LIST_HOME_X && sx < IP_LIST_HOME_X + IP_LIST_HOME_W &&
            sy >= 0              && sy < IP_HDR_H);
}

// ── Hit tests — edit ──────────────────────────────────────────────────────────

bool InstrumentsPage::hitBack(int sx, int sy) const {
    return (sx >= IP_EDIT_BACK_X && sx < IP_EDIT_BACK_X + IP_EDIT_BACK_W &&
            sy >= 0              && sy < IP_EDIT_HDR_H);
}
bool InstrumentsPage::hitEditHome(int sx, int sy) const {
    return (sx >= IP_EDIT_HOME_X && sx < IP_EDIT_HOME_X + IP_EDIT_HOME_W &&
            sy >= 0              && sy < IP_EDIT_HDR_H);
}
bool InstrumentsPage::hitName(int sx, int sy) const {
    return (sx >= IP_EDIT_NAME_X && sx < IP_EDIT_NAME_X + IP_EDIT_NAME_W &&
            sy >= 0              && sy < IP_EDIT_HDR_H);
}
int InstrumentsPage::hitMinus(int sx, int sy) const {
    if (sy < IP_FIELD_Y) return -1;
    int field = (sy - IP_FIELD_Y) / IP_FIELD_H;
    if (field < 0 || field >= IP_NUM_FIELDS) return -1;
    int fy   = IP_FIELD_Y + field * IP_FIELD_H;
    int btnY = fy + (IP_FIELD_H - IP_FBTN_H) / 2;
    if (sy < btnY || sy >= btnY + IP_FBTN_H) return -1;
    return (sx >= IP_MINUS_X && sx < IP_MINUS_X + IP_FBTN_W) ? field : -1;
}
int InstrumentsPage::hitPlus(int sx, int sy) const {
    if (sy < IP_FIELD_Y) return -1;
    int field = (sy - IP_FIELD_Y) / IP_FIELD_H;
    if (field < 0 || field >= IP_NUM_FIELDS) return -1;
    int fy   = IP_FIELD_Y + field * IP_FIELD_H;
    int btnY = fy + (IP_FIELD_H - IP_FBTN_H) / 2;
    if (sy < btnY || sy >= btnY + IP_FBTN_H) return -1;
    return (sx >= IP_PLUS_X && sx < IP_PLUS_X + IP_FBTN_W) ? field : -1;
}

void InstrumentsPage::drawConfirm() {
    // Title
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    const char* msg = "SAVE INSTRUMENTS?";
    int tw = (int)strlen(msg) * 18;
    _d.setCursor((960 - tw) / 2, 200);
    _d.print(msg);

    // [YES] and [NO] buttons
    uiButton(_d, 150, 310, 250, 90, "YES", COL_WHITE, COL_BLACK, 3);
    uiButton(_d, 560, 310, 250, 90, "NO",  COL_WHITE, COL_BLACK, 3);
}

void InstrumentsPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

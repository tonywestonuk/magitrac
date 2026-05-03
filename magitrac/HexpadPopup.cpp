#include "HexpadPopup.h"
#include <string.h>
#include <stdio.h>

static const char HEX_KEYS[] = "0123456789ABCDEF";

HexpadPopup::HexpadPopup(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _open(false)
    , _done(false)
    , _cleared(false)
    , _wasDown(false)
    , _swallowLift(false)
    , _numDigits(0)
    , _resultEffect(0)
    , _resultParam(0)
{
    _digits[0] = '\0';
}

void HexpadPopup::open(uint8_t effect, uint8_t param) {
    _open    = true;
    _done    = false;
    _cleared = false;
    _wasDown     = true;   // assume finger is still down from opening tap
    _swallowLift = true;   // ignore the first lift (belongs to the opening tap)

    // Pre-fill with current values if non-zero
    if (effect != 0 || param != 0) {
        _digits[0] = HEX_KEYS[(effect >> 4) & 0xF];
        _digits[1] = HEX_KEYS[effect & 0xF];
        _digits[2] = HEX_KEYS[(param >> 4) & 0xF];
        _digits[3] = HEX_KEYS[param & 0xF];
        _digits[4] = '\0';
        _numDigits = 4;
    } else {
        _digits[0] = '\0';
        _numDigits = 0;
    }

    _resultEffect = effect;
    _resultParam  = param;
}

// ── Draw ─────────────────────────────────────────────────────────────────────

void HexpadPopup::draw() {
    _d.fillScreen(COL_WHITE);
    drawTextField();

    // Hex keys 0-F in 4 rows of 4
    for (int r = 0; r < 4; r++) {
        int y = HP_ROW1_Y + r * HP_ROW_H;
        for (int c = 0; c < 4; c++) {
            char label[2] = { HEX_KEYS[r * 4 + c], '\0' };
            drawKey(c * HP_COL_W + 4, y, HP_COL_W - 8, HP_KEY_H, label);
        }
    }

    // Bottom row: CANCEL | BKSP | CLR | SET  (4 × 240px)
    int bw = HP_W / 4;
    drawKey(4,        HP_ROW5_Y, bw - 8, HP_KEY_H, "CANCEL");
    drawKey(bw + 4,   HP_ROW5_Y, bw - 8, HP_KEY_H, "BKSP");
    drawKey(bw*2 + 4, HP_ROW5_Y, bw - 8, HP_KEY_H, "CLR");
    drawKey(bw*3 + 4, HP_ROW5_Y, bw - 8, HP_KEY_H, "SET");
}

void HexpadPopup::drawTextField() {
    _d.fillRect(0, 0, HP_W, HP_FIELD_H, COL_BLACK);

    // Format hint
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(20, 8);
    _d.print("EE PP");

    // Build display: EE PP with underscores for unfilled digits
    char display[6]; // "EE PP" + null
    for (int i = 0; i < HP_MAX_DIGITS; i++) {
        display[i] = (i < _numDigits) ? _digits[i] : '_';
    }
    display[HP_MAX_DIGITS] = '\0';

    // Insert space between effect and param for display
    char formatted[8];
    formatted[0] = display[0];
    formatted[1] = display[1];
    formatted[2] = ' ';
    formatted[3] = display[2];
    formatted[4] = display[3];
    formatted[5] = '\0';

    _d.setTextSize(4);
    _d.setTextColor(COL_WHITE);
    int tw = strlen(formatted) * 24;
    _d.setCursor((HP_W - tw) / 2, HP_FIELD_H / 2 + 4);
    _d.print(formatted);
}

void HexpadPopup::drawKey(int x, int y, int w, int h, const char* label) {
    _d.fillRect(x, y, w, h, COL_WHITE);
    _d.drawRect(x, y, w, h, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int lw = strlen(label) * 18;
    _d.setCursor(x + (w - lw) / 2, y + (h - 24) / 2);
    _d.print(label);
}

void HexpadPopup::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = HP_H - rx;
}

// ── Parse digits into effect/param ───────────────────────────────────────────

static uint8_t hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// ── Poll ─────────────────────────────────────────────────────────────────────

bool HexpadPopup::poll() {
    if (!_open || !_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // Fire on finger lift
    if (!down && _wasDown) {
        _wasDown = false;
        if (_swallowLift) { _swallowLift = false; return false; }

        // Bottom row: CANCEL | BKSP | CLR | SET
        if (sy >= HP_ROW5_Y && sy < HP_ROW5_Y + HP_KEY_H) {
            int bw = HP_W / 4;
            int col = sx / bw;
            if (col == 0) {
                // CANCEL
                _open = false;
                _done = false;
                return true;
            }
            if (col == 1) {
                // BKSP
                if (_numDigits > 0) {
                    _numDigits--;
                    _digits[_numDigits] = '\0';
                    drawTextField();
                    _d.paintLater();
                }
                return false;
            }
            if (col == 2) {
                // CLR — clear effect/param
                _resultEffect = 0;
                _resultParam  = 0;
                _cleared = true;
                _done    = true;
                _open    = false;
                return true;
            }
            if (col == 3) {
                // SET — parse digits into effect/param
                // Pad with zeros if incomplete
                char padded[5] = {'0','0','0','0','\0'};
                int offset = HP_MAX_DIGITS - _numDigits;
                for (int i = 0; i < _numDigits; i++)
                    padded[offset + i] = _digits[i];

                _resultEffect = (hexVal(padded[0]) << 4) | hexVal(padded[1]);
                _resultParam  = (hexVal(padded[2]) << 4) | hexVal(padded[3]);
                _cleared = false;
                _done    = true;
                _open    = false;
                return true;
            }
            return false;
        }

        // Hex key rows (rows 1-4)
        if (sy >= HP_ROW1_Y && sy < HP_ROW5_Y) {
            int row = (sy - HP_ROW1_Y) / HP_ROW_H;
            int col = sx / HP_COL_W;
            if (row >= 0 && row < 4 && col >= 0 && col < 4) {
                int keyIdx = row * 4 + col;
                if (_numDigits < HP_MAX_DIGITS) {
                    _digits[_numDigits] = HEX_KEYS[keyIdx];
                    _numDigits++;
                    _digits[_numDigits] = '\0';
                    drawTextField();
                    _d.paintLater();
                }
            }
            return false;
        }

        return false;
    }

    if (down && !_wasDown) _wasDown = true;
    return false;
}

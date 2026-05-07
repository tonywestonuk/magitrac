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
    , _maxDigits(HP_MAX_DIGITS)
    , _resultEffect(0)
    , _resultParam(0)
{
    _digits[0] = '\0';
    _title[0]  = '\0';
}

void HexpadPopup::open(uint8_t hi, uint8_t lo, uint8_t numDigits, const char* title,
                       bool fingerDown) {
    _open    = true;
    _done    = false;
    _cleared = false;
    _wasDown     = fingerDown;
    _swallowLift = fingerDown;
    _maxDigits = (numDigits == 2) ? 2 : 4;

    if (title) { strncpy(_title, title, sizeof(_title) - 1); _title[sizeof(_title) - 1] = '\0'; }
    else       { _title[0] = '\0'; }

    // Pre-fill the digit buffer.  In 2-digit mode `hi` carries the value; `lo`
    // is ignored.
    if (_maxDigits == 2) {
        if (hi != 0) {
            _digits[0] = HEX_KEYS[(hi >> 4) & 0xF];
            _digits[1] = HEX_KEYS[hi & 0xF];
            _digits[2] = '\0';
            _numDigits = 2;
        } else {
            _digits[0] = '\0';
            _numDigits = 0;
        }
        _resultEffect = hi;
        _resultParam  = 0;
    } else {
        if (hi != 0 || lo != 0) {
            _digits[0] = HEX_KEYS[(hi >> 4) & 0xF];
            _digits[1] = HEX_KEYS[hi & 0xF];
            _digits[2] = HEX_KEYS[(lo >> 4) & 0xF];
            _digits[3] = HEX_KEYS[lo & 0xF];
            _digits[4] = '\0';
            _numDigits = 4;
        } else {
            _digits[0] = '\0';
            _numDigits = 0;
        }
        _resultEffect = hi;
        _resultParam  = lo;
    }
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

    // Title (left-aligned, same size as the key labels)
    _d.setTextSize(3);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(20, 8);
    _d.print(_title[0] ? _title : (_maxDigits == 2 ? "VV" : "EE PP"));

    // Build the digit display: underscores for unfilled positions.
    char formatted[8];
    int pos = 0;
    for (int i = 0; i < _maxDigits; i++) {
        formatted[pos++] = (i < _numDigits) ? _digits[i] : '_';
        // Insert a space between effect and param in 4-digit mode.
        if (_maxDigits == 4 && i == 1) formatted[pos++] = ' ';
    }
    formatted[pos] = '\0';

    _d.setTextSize(4);
    _d.setTextColor(COL_WHITE);
    int tw = pos * 24;
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
                // SET — parse digits, right-justified within _maxDigits.
                char padded[5] = {'0','0','0','0','\0'};
                int offset = _maxDigits - _numDigits;
                for (int i = 0; i < _numDigits; i++)
                    padded[offset + i] = _digits[i];

                if (_maxDigits == 2) {
                    _resultEffect = (hexVal(padded[0]) << 4) | hexVal(padded[1]);
                    _resultParam  = 0;
                } else {
                    _resultEffect = (hexVal(padded[0]) << 4) | hexVal(padded[1]);
                    _resultParam  = (hexVal(padded[2]) << 4) | hexVal(padded[3]);
                }
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
                if (_numDigits < _maxDigits) {
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

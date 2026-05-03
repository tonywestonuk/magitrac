#include "NumpadPopup.h"
#include <string.h>
#include <stdio.h>

NumpadPopup::NumpadPopup(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _open(false)
    , _done(false)
    , _wasDown(false)
    , _numDigits(0)
    , _maxDigits(6)
    , _numSeps(0)
{
    _digits[0] = '\0';
    _formatHint[0] = '\0';
    _separators[0] = '\0';
    memset(_sepPositions, 0, sizeof(_sepPositions));
}

void NumpadPopup::open(const char* formatHint, const char* separators,
                        const int* sepPositions, int numSeps, int maxDigits) {
    _open      = true;
    _done      = false;
    _wasDown   = _touch.isTouched;
    _numDigits = 0;
    _digits[0] = '\0';
    _maxDigits = (maxDigits > NP_MAX_DIGITS) ? NP_MAX_DIGITS : maxDigits;

    strncpy(_formatHint, formatHint, sizeof(_formatHint) - 1);
    _formatHint[sizeof(_formatHint) - 1] = '\0';

    _numSeps = (numSeps > 4) ? 4 : numSeps;
    for (int i = 0; i < _numSeps; i++) {
        _separators[i]   = separators[i];
        _sepPositions[i] = sepPositions[i];
    }
}

// ── Format display string with separators ────────────────────────────────────

void NumpadPopup::formatDisplay(char* out, int outLen) const {
    // Build display string by interleaving digits and separators.
    // sepPositions[] gives the digit index AFTER which the separator appears.
    // E.g. for HH:MM:SS, sepPositions={2,4} means insert ':' after digit 2 and 4.
    int o = 0;
    for (int d = 0; d < _maxDigits && o < outLen - 1; d++) {
        out[o++] = (d < _numDigits) ? _digits[d] : '_';

        // Insert separator after this digit?
        for (int s = 0; s < _numSeps && o < outLen - 1; s++) {
            if (_sepPositions[s] == d + 1) {
                out[o++] = _separators[s];
                break;
            }
        }
    }
    out[o] = '\0';
}

void NumpadPopup::draw() {
    _d.fillScreen(COL_WHITE);
    drawTextField();

    // Number keys 1-9
    const char* labels[] = { "1","2","3","4","5","6","7","8","9" };
    for (int r = 0; r < 3; r++) {
        int y = NP_ROW1_Y + r * NP_ROW_H;
        for (int c = 0; c < 3; c++) {
            drawKey(c * NP_COL_W + 4, y, NP_COL_W - 8, NP_KEY_H,
                    labels[r * 3 + c]);
        }
    }

    // Bottom row: CANCEL | BKSP | 0 | SET  (4 × 240px)
    int bw = NP_W / 4;
    drawKey(4,        NP_ROW4_Y, bw - 8, NP_KEY_H, "CANCEL");
    drawKey(bw + 4,   NP_ROW4_Y, bw - 8, NP_KEY_H, "BKSP");
    drawKey(bw*2 + 4, NP_ROW4_Y, bw - 8, NP_KEY_H, "0");
    drawKey(bw*3 + 4, NP_ROW4_Y, bw - 8, NP_KEY_H, "SET");
}

void NumpadPopup::drawTextField() {
    _d.fillRect(0, 0, NP_W, NP_FIELD_H, COL_BLACK);

    // Format hint (small, top-left)
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(20, 8);
    _d.print(_formatHint);

    // Current entry with separators
    char display[20];
    formatDisplay(display, sizeof(display));

    _d.setTextSize(4);
    _d.setTextColor(COL_WHITE);
    int tw = strlen(display) * 24;  // textSize 4 = 24px wide
    _d.setCursor((NP_W - tw) / 2, NP_FIELD_H / 2 + 4);
    _d.print(display);
}

void NumpadPopup::drawKey(int x, int y, int w, int h, const char* label) {
    _d.fillRect(x, y, w, h, COL_WHITE);
    _d.drawRect(x, y, w, h, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int lw = strlen(label) * 18;
    _d.setCursor(x + (w - lw) / 2, y + (h - 24) / 2);
    _d.print(label);
}

bool NumpadPopup::poll() {
    if (!_open || !_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // Fire on finger lift
    if (!down && _wasDown) {
        _wasDown = false;

        bool bksp = false, done = false, cancel = false;
        int digit = hitKey(sx, sy, bksp, done, cancel);

        if (cancel) {
            _open = false;
            _done = false;
            return true;
        }
        if (done) {
            _open = false;
            _done = true;
            return true;
        }
        if (bksp) {
            if (_numDigits > 0) {
                _numDigits--;
                _digits[_numDigits] = '\0';
                drawTextField();
                _d.paintLater();
            }
        } else if (digit >= 0 && _numDigits < _maxDigits) {
            _digits[_numDigits] = '0' + digit;
            _numDigits++;
            _digits[_numDigits] = '\0';
            drawTextField();
            _d.paintLater();
        }
        return false;
    }

    if (down && !_wasDown) _wasDown = true;
    return false;
}

int NumpadPopup::hitKey(int sx, int sy, bool& bksp, bool& done, bool& cancel) const {
    bksp = done = cancel = false;

    if (sy < NP_ROW1_Y) return -1;

    // Rows 1-3: digits 1-9
    if (sy < NP_ROW4_Y) {
        int row = (sy - NP_ROW1_Y) / NP_ROW_H;
        int col = sx / NP_COL_W;
        if (row >= 0 && row < 3 && col >= 0 && col < 3) {
            return row * 3 + col + 1;  // 1-9
        }
        return -1;
    }

    // Row 4: CANCEL | BKSP | 0 | SET  (4 × 240px)
    if (sy < NP_ROW4_Y + NP_KEY_H) {
        int bw = NP_W / 4;
        int col = sx / bw;
        if (col == 0) { cancel = true; return -1; }
        if (col == 1) { bksp = true; return -1; }
        if (col == 2) return 0;
        if (col == 3) { done = true; return -1; }
    }

    return -1;
}

void NumpadPopup::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = NP_H - rx;
}

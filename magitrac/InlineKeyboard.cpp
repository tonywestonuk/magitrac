#include "InlineKeyboard.h"
#include "Constants.h"
#include <string.h>

static const int IK_SLOT_W = 96;
static const int IK_KEY_W  = 88;
static const int IK_KEY_H  = 70;

static const char* IK_ROWS[2][3] = {
    { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" },
    { "1234567890", "-'.,:;!?#", "()/&+=\"" },
};

// Bottom row: left edges, then the right edge of the last key.
enum { B_SYM, B_CLEAR, B_LEFT, B_SPACE, B_RIGHT, B_DONE, B_N };
static const int IK_BX[B_N + 1] = { 0, 112, 256, 352, 640, 736, 960 };

static char caseOf(char k, bool sym, bool shift) {
    if (!sym && !shift && k >= 'A' && k <= 'Z') return (char)(k - 'A' + 'a');
    return k;
}

void InlineKeyboard::drawKey(int x, int y, int w, const char* label, bool inverted) {
    uint8_t bg = inverted ? COL_BLACK : COL_WHITE;
    uint8_t fg = inverted ? COL_WHITE : COL_BLACK;
    _d.fillRect(x, y, w, IK_KEY_H, bg);
    _d.drawRect(x, y, w, IK_KEY_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(fg);
    _d.setCursor(x + (w - (int)strlen(label) * 18) / 2, y + (IK_KEY_H - 24) / 2);
    _d.print(label);
}

void InlineKeyboard::drawArrow(int x, int y, int w, bool left) {
    drawKey(x, y, w, "");
    int cx = x + w / 2, cy = y + IK_KEY_H / 2;
    if (left) _d.fillTriangle(cx + 10, cy - 14, cx + 10, cy + 14, cx - 12, cy, COL_BLACK);
    else      _d.fillTriangle(cx - 10, cy - 14, cx - 10, cy + 14, cx + 12, cy, COL_BLACK);
}

void InlineKeyboard::draw(int top) {
    _top = top;
    _d.fillRect(0, top, 960, HEIGHT, COL_WHITE);
    char label[2] = { 0, 0 };
    const int offs[3] = { 0, IK_SLOT_W / 2, IK_SLOT_W };
    for (int r = 0; r < 3; r++) {
        const char* keys = IK_ROWS[_sym][r];
        int y = top + r * ROW_H;
        for (int i = 0; keys[i]; i++) {
            label[0] = caseOf(keys[i], _sym, _shift);
            drawKey(offs[r] + i * IK_SLOT_W + 4, y, IK_KEY_W, label);
        }
    }
    int y3 = top + 2 * ROW_H, y4 = top + 3 * ROW_H;
    if (!_sym) {
        _d.setTextSize(2);   // "SHIFT" doesn't fit a letter key at size 3
        _d.fillRect(4, y3, IK_KEY_W, IK_KEY_H, _shift ? COL_BLACK : COL_WHITE);
        _d.drawRect(4, y3, IK_KEY_W, IK_KEY_H, COL_BLACK);
        _d.setTextColor(_shift ? COL_WHITE : COL_BLACK);
        _d.setCursor(4 + (IK_KEY_W - 5 * 12) / 2, y3 + (IK_KEY_H - 16) / 2);
        _d.print("SHIFT");
    }
    drawKey(8 * IK_SLOT_W + 4, y3, 2 * IK_SLOT_W - 8, "BKSP");

    auto w = [](int b) { return IK_BX[b + 1] - IK_BX[b] - 8; };
    drawKey(IK_BX[B_SYM] + 4,   y4, w(B_SYM),   _sym ? "ABC" : "123");
    drawKey(IK_BX[B_CLEAR] + 4, y4, w(B_CLEAR), "CLEAR");
    drawArrow(IK_BX[B_LEFT] + 4, y4, w(B_LEFT), true);
    drawKey(IK_BX[B_SPACE] + 4, y4, w(B_SPACE), "SPACE");
    drawArrow(IK_BX[B_RIGHT] + 4, y4, w(B_RIGHT), false);
    drawKey(IK_BX[B_DONE] + 4,  y4, w(B_DONE),  "DONE", true);
}

int InlineKeyboard::key(int sx, int sy) {
    if (sy < _top || sy >= _top + HEIGHT || sx < 0 || sx >= 960) return KEY_NONE;
    int row = (sy - _top) / ROW_H;
    if (row == 3) {
        int b = 0;
        while (b < B_N - 1 && sx >= IK_BX[b + 1]) b++;
        switch (b) {
            case B_SYM:   _sym = !_sym; draw(_top); _d.paintLater(); return KEY_NONE;
            case B_CLEAR: return KEY_CLEAR;
            case B_LEFT:  return KEY_LEFT;
            case B_SPACE: return ' ';
            case B_RIGHT: return KEY_RIGHT;
            default:      return KEY_DONE;
        }
    }
    if (row == 2) {
        if (sx >= 8 * IK_SLOT_W) return KEY_BKSP;
        if (sx < IK_SLOT_W) {
            if (_sym) return KEY_NONE;
            _shift = !_shift;
            draw(_top);
            _d.paintLater();
            return KEY_NONE;
        }
    }
    const int offs[3] = { 0, IK_SLOT_W / 2, IK_SLOT_W };
    const char* keys = IK_ROWS[_sym][row];
    if (sx < offs[row]) return KEY_NONE;
    int slot = (sx - offs[row]) / IK_SLOT_W;
    if (slot >= (int)strlen(keys)) return KEY_NONE;
    char c = caseOf(keys[slot], _sym, _shift);
    if (_shift && !_sym) {   // one-shot
        _shift = false;
        draw(_top);
    }
    return c;
}

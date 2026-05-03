#include "ConfirmDialog.h"
#include <string.h>

ConfirmDialog::ConfirmDialog(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _open(false)
    , _confirmed(false)
    , _wasDown(false)
{
    _message[0] = '\0';
}

void ConfirmDialog::open(const char* message) {
    strncpy(_message, message, sizeof(_message) - 1);
    _message[sizeof(_message) - 1] = '\0';
    _open      = true;
    _confirmed = false;
    _wasDown   = _touch.isTouched;
}

void ConfirmDialog::draw() {
    // Box background + border
    _d.fillRect(CD_BOX_X, CD_BOX_Y, CD_BOX_W, CD_BOX_H, COL_WHITE);
    _d.drawRect(CD_BOX_X, CD_BOX_Y, CD_BOX_W, CD_BOX_H, COL_BLACK);
    // Inner border for visual weight
    _d.drawRect(CD_BOX_X + 2, CD_BOX_Y + 2, CD_BOX_W - 4, CD_BOX_H - 4, COL_BLACK);

    // Message — centred horizontally, in upper portion of box
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(_message) * 18;  // textSize 3: 18px/char
    int tx = CD_BOX_X + (CD_BOX_W - tw) / 2;
    int ty = CD_BOX_Y + 40;
    _d.setCursor(tx, ty);
    _d.print(_message);

    // Buttons
    uiButton(_d, CD_YES_X, CD_BTN_Y, CD_YES_W, CD_BTN_H, "YES", COL_BLACK, COL_WHITE, 3);
    uiButton(_d, CD_NO_X,  CD_BTN_Y, CD_NO_W,  CD_BTN_H, "NO",  COL_WHITE, COL_BLACK, 3);
}

bool ConfirmDialog::poll() {
    if (!_open || !_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (!down && _wasDown) {
        _wasDown = false;
        if (hitYes(sx, sy)) {
            _confirmed = true;
            _open = false;
            return true;
        }
        if (hitNo(sx, sy)) {
            _confirmed = false;
            _open = false;
            return true;
        }
        return false;
    }
    if (down && !_wasDown) _wasDown = true;
    return false;
}

bool ConfirmDialog::hitYes(int sx, int sy) const {
    return (sx >= CD_YES_X && sx < CD_YES_X + CD_YES_W &&
            sy >= CD_BTN_Y && sy < CD_BTN_Y + CD_BTN_H);
}

bool ConfirmDialog::hitNo(int sx, int sy) const {
    return (sx >= CD_NO_X && sx < CD_NO_X + CD_NO_W &&
            sy >= CD_BTN_Y && sy < CD_BTN_Y + CD_BTN_H);
}

void ConfirmDialog::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

#include "BootMenu.h"

BootMenu::BootMenu(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _open(false)
    , _wasDown(false)
    , _noSong(false)
{}

void BootMenu::open(bool noSong) {
    _open    = true;
    _noSong  = noSong;
    _wasDown = _touch.isTouched;
    draw();
}

void BootMenu::draw() {
    // Box background + double border
    _d.fillRect(BM_BOX_X, BM_BOX_Y, BM_BOX_W, BM_BOX_H, COL_WHITE);
    _d.drawRect(BM_BOX_X,     BM_BOX_Y,     BM_BOX_W,     BM_BOX_H,     COL_BLACK);
    _d.drawRect(BM_BOX_X + 2, BM_BOX_Y + 2, BM_BOX_W - 4, BM_BOX_H - 4, COL_BLACK);

    // Row 1: navigation buttons
    if (_noSong)
        uiButton(_d, BM_SONG_X, BM_BTN_Y, BM_BTN_W, BM_BTN_H, "SONG", COL_WHITE, COL_DKGREY, 3);
    else
        uiButton(_d, BM_SONG_X, BM_BTN_Y, BM_BTN_W, BM_BTN_H, "SONG", COL_WHITE, COL_BLACK, 3);
    uiButton(_d, BM_INST_X, BM_BTN_Y, BM_BTN_W, BM_BTN_H, "INSTRUMENTS", COL_WHITE, COL_BLACK, 3);
    uiButton(_d, BM_SETT_X, BM_BTN_Y, BM_BTN_W, BM_BTN_H, "SETTINGS",    COL_WHITE, COL_BLACK, 3);

    // Row 2: backup + perform + pixelpost + pairing
    uiButton(_d, BM_BACKUP_X,    BM_BTN2_Y, BM_BTN2_W, BM_BTN2_H, "BACKUP",  COL_WHITE, COL_BLACK, 3);
    uiButton(_d, BM_PERFORM_X,   BM_BTN2_Y, BM_BTN2_W, BM_BTN2_H, "PERFORM", COL_WHITE, COL_BLACK, 3);
    uiButton(_d, BM_PIXELPOST_X, BM_BTN2_Y, BM_BTN2_W, BM_BTN2_H, "POSTS",   COL_WHITE, COL_BLACK, 3);
    uiButton(_d, BM_PAIR_X,      BM_BTN2_Y, BM_PAIR_W, BM_BTN2_H, "PAIR",    COL_WHITE, COL_BLACK, 3);

    // Row 3: drawbar organ
    uiButton(_d, BM_ORGAN_X, BM_BTN3_Y, BM_ORGAN_W, BM_BTN3_H, "ORGAN", COL_WHITE, COL_BLACK, 3);
}

BootMenuResult BootMenu::poll() {
    if (!_open || !_touch.read()) return BootMenuResult::NONE;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (!down && _wasDown) {
        _wasDown = false;
        _open    = false;

        if (!_noSong && hitSong(sx, sy)) return BootMenuResult::SONG;
        if (hitInstruments(sx, sy)) return BootMenuResult::INSTRUMENTS;
        if (hitSettings   (sx, sy)) return BootMenuResult::SETTINGS;
        if (hitBackup     (sx, sy)) return BootMenuResult::BACKUP;
        if (hitPerform    (sx, sy)) return BootMenuResult::PERFORM;
        if (hitPixelpost  (sx, sy)) return BootMenuResult::PIXELPOST;
        if (hitPair       (sx, sy)) return BootMenuResult::PAIR;
        if (hitOrgan      (sx, sy)) return BootMenuResult::DRAWBAR_ORGAN;
        return BootMenuResult::DISMISSED;
    }

    if (down && !_wasDown) _wasDown = true;
    return BootMenuResult::NONE;
}

bool BootMenu::hitSong(int sx, int sy) const {
    return (sx >= BM_SONG_X && sx < BM_SONG_X + BM_BTN_W &&
            sy >= BM_BTN_Y  && sy < BM_BTN_Y  + BM_BTN_H);
}

bool BootMenu::hitInstruments(int sx, int sy) const {
    return (sx >= BM_INST_X && sx < BM_INST_X + BM_BTN_W &&
            sy >= BM_BTN_Y  && sy < BM_BTN_Y  + BM_BTN_H);
}

bool BootMenu::hitSettings(int sx, int sy) const {
    return (sx >= BM_SETT_X && sx < BM_SETT_X + BM_BTN_W &&
            sy >= BM_BTN_Y  && sy < BM_BTN_Y  + BM_BTN_H);
}

bool BootMenu::hitBackup(int sx, int sy) const {
    return (sx >= BM_BACKUP_X && sx < BM_BACKUP_X + BM_BTN2_W &&
            sy >= BM_BTN2_Y  && sy < BM_BTN2_Y  + BM_BTN2_H);
}

bool BootMenu::hitPerform(int sx, int sy) const {
    return (sx >= BM_PERFORM_X && sx < BM_PERFORM_X + BM_BTN2_W &&
            sy >= BM_BTN2_Y   && sy < BM_BTN2_Y   + BM_BTN2_H);
}

bool BootMenu::hitPixelpost(int sx, int sy) const {
    return (sx >= BM_PIXELPOST_X && sx < BM_PIXELPOST_X + BM_BTN2_W &&
            sy >= BM_BTN2_Y      && sy < BM_BTN2_Y      + BM_BTN2_H);
}

bool BootMenu::hitPair(int sx, int sy) const {
    return (sx >= BM_PAIR_X && sx < BM_PAIR_X + BM_PAIR_W &&
            sy >= BM_BTN2_Y && sy < BM_BTN2_Y + BM_BTN2_H);
}

bool BootMenu::hitOrgan(int sx, int sy) const {
    return (sx >= BM_ORGAN_X && sx < BM_ORGAN_X + BM_ORGAN_W &&
            sy >= BM_BTN3_Y  && sy < BM_BTN3_Y  + BM_BTN3_H);
}

bool BootMenu::hitInsideBox(int sx, int sy) const {
    return (sx >= BM_BOX_X && sx < BM_BOX_X + BM_BOX_W &&
            sy >= BM_BOX_Y && sy < BM_BOX_Y + BM_BOX_H);
}

void BootMenu::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

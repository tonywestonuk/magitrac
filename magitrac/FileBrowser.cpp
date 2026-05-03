#include "FileBrowser.h"
#include <string.h>
#include <stdio.h>

// ── Constructor ───────────────────────────────────────────────────────────────

FileBrowser::FileBrowser(EPD_PainterAdafruit& d, GT911_Lite& touch)
    : _d(d), _touch(touch)
    , _hasPrev(false), _hasNext(false)
    , _altMode(false), _wasDown(false)
    , _itemCount(0)
{
    _title[0]      = '\0';
    _loadedName[0] = '\0';
    _statusText[0] = '\0';
}

// ── Configuration ─────────────────────────────────────────────────────────────

void FileBrowser::open() {
    _altMode = false;
    _wasDown = _touch.isTouched;
}

void FileBrowser::setTitle(const char* title) {
    strncpy(_title, title, sizeof(_title) - 1);
    _title[sizeof(_title) - 1] = '\0';
}

void FileBrowser::setLoadedName(const char* name) {
    strncpy(_loadedName, name, sizeof(_loadedName) - 1);
    _loadedName[sizeof(_loadedName) - 1] = '\0';
}

void FileBrowser::setHasPrev(bool v) { _hasPrev = v; }
void FileBrowser::setHasNext(bool v) { _hasNext = v; }

void FileBrowser::setStatusText(const char* text) {
    strncpy(_statusText, text, sizeof(_statusText) - 1);
    _statusText[sizeof(_statusText) - 1] = '\0';
}

void FileBrowser::clearItems() {
    _itemCount = 0;
}

void FileBrowser::addItem(const char* name) {
    if (_itemCount >= FB_PER_PAGE) return;
    strncpy(_items[_itemCount], name, STORAGE_FILENAME_MAX - 1);
    _items[_itemCount][STORAGE_FILENAME_MAX - 1] = '\0';
    _itemCount++;
}

void FileBrowser::onBootPress() {
    _altMode = !_altMode;
    _d.fillScreen(COL_WHITE);
    draw();
    _d.paintLater();
}

// ── Draw ──────────────────────────────────────────────────────────────────────

void FileBrowser::drawHeader() {
    const char* title = _altMode ? "DELETE MODE" : _title;
    _d.fillRect(0, 0, 960, FB_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    int tw = (int)strlen(title) * 18;
    _d.setCursor((960 - tw) / 2, (FB_HDR_H - 24) / 2);
    _d.print(title);

    // Right: HOME (exit). Left: DEL (delete-mode toggle).
    uiButton(_d, FB_HOME_X, 0, FB_HOME_W, FB_HDR_H, "HOME", COL_BLACK, COL_WHITE, 3);
    uiButton(_d, FB_DEL_X,  0, FB_DEL_W,  FB_HDR_H, "DEL",  COL_BLACK, COL_WHITE, 3);
}

void FileBrowser::drawToolbar() {
    _d.fillRect(0, FB_HDR_H, 960, FB_TOOL_H, COL_WHITE);
    _d.drawFastHLine(0, FB_HDR_H + FB_TOOL_H - 1, 960, COL_BLACK);
    uiButton(_d, FB_PREV_X, FB_BTN_Y, FB_PREV_W, FB_BTN_H,
             "PREV", COL_WHITE, _hasPrev ? COL_BLACK : COL_DKGREY, 3);
    uiButton(_d, FB_NEXT_X, FB_BTN_Y, FB_NEXT_W, FB_BTN_H,
             "NEXT", COL_WHITE, _hasNext ? COL_BLACK : COL_DKGREY, 3);
}

void FileBrowser::drawGrid() {
    if (_itemCount == 0 && _statusText[0] != '\0') {
        _d.fillRect(0, FB_LIST_Y, 960, FB_ROWS * FB_ROW_H, COL_WHITE);
        int y = FB_LIST_Y + (FB_ROWS / 2) * FB_ROW_H;
        _d.setTextSize(3);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(20, y + (FB_ROW_H - 24) / 2);
        _d.print(_statusText);
        return;
    }

    for (int vi = 0; vi < FB_ROWS; vi++) {
        int rowY = FB_LIST_Y + vi * FB_ROW_H;
        for (int ci = 0; ci < FB_COLS; ci++) {
            int idx = vi * FB_COLS + ci;
            if (idx < _itemCount) {
                bool isLoaded = (_loadedName[0] != '\0' &&
                                 strcmp(_items[idx], _loadedName) == 0);
                drawItem(ci, rowY, _items[idx], isLoaded);
            } else {
                _d.fillRect(ci * FB_COL_W, rowY, FB_COL_W, FB_ROW_H, COL_WHITE);
            }
        }
    }
}

void FileBrowser::drawItem(int col, int rowY, const char* name, bool isLoaded) {
    uint8_t cellBg = _altMode ? COL_BLACK : COL_WHITE;
    uint8_t border = _altMode ? COL_WHITE : COL_BLACK;
    uint8_t fg     = _altMode ? COL_WHITE : COL_BLACK;

    int cx = col * FB_COL_W;
    int bx = cx + 4;
    int by = rowY + 4;
    int bw = FB_COL_W - 8;
    int bh = FB_ROW_H - 8;

    _d.fillRect(cx, rowY, FB_COL_W, FB_ROW_H, cellBg);
    _d.fillRect(bx, by, bw, bh, cellBg);
    _d.drawRect(bx, by, bw, bh, border);

    int labelW = (int)strlen(name) * 18;   // textSize 3: 6*3 = 18px/char
    int tx = bx + (bw - labelW) / 2;
    int ty = by + (bh - 24) / 2;           // textSize 3: 8*3 = 24px/char

    _d.setTextSize(3);
    _d.setTextColor(fg);
    _d.setCursor(tx, ty);
    _d.print(name);

    if (isLoaded) {
        _d.setCursor(tx + 1, ty);           // bold simulation: offset second pass
        _d.print(name);
    }
}

void FileBrowser::drawFooter() {
    _d.drawFastHLine(0, FB_FOOT_Y, 960, COL_BLACK);

    bool saveEnabled = (_loadedName[0] != '\0');
    char saveLabel[32] = "SAVE";
    if (saveEnabled)
        snprintf(saveLabel, sizeof(saveLabel), "SAVE %s", _loadedName);

    uiButton(_d, FB_NEW_X,    FB_FBTN_Y, FB_NEW_W,    FB_FBTN_H,
             "NEW",     COL_WHITE, COL_BLACK, 3);
    uiButton(_d, FB_SAVE_X,   FB_FBTN_Y, FB_SAVE_W,   FB_FBTN_H,
             saveLabel, COL_WHITE, saveEnabled ? COL_BLACK : COL_DKGREY, 3);
    uiButton(_d, FB_SAVEAS_X, FB_FBTN_Y, FB_SAVEAS_W, FB_FBTN_H,
             "SAVE AS", COL_WHITE, COL_BLACK, 3);
}

void FileBrowser::draw() {
    drawHeader();
    drawToolbar();
    drawGrid();
    drawFooter();
}

// ── Poll ──────────────────────────────────────────────────────────────────────

FileBrowserResult FileBrowser::poll() {
    FileBrowserResult r { FileBrowserEvent::NONE, -1 };

    if (!_touch.read()) return r;
    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (!down && _wasDown) {
        _wasDown = false;

        if (hitHome(sx, sy)) {
            r.event = FileBrowserEvent::HOME;
        } else if (hitDel(sx, sy)) {
            onBootPress();   // toggles delete mode
        } else if (hitPrev(sx, sy) && _hasPrev) {
            r.event = FileBrowserEvent::PREV_PAGE;
        } else if (hitNext(sx, sy) && _hasNext) {
            r.event = FileBrowserEvent::NEXT_PAGE;
        } else if (hitNew(sx, sy)) {
            r.event = FileBrowserEvent::NEW;
        } else if (hitSave(sx, sy)) {
            r.event = FileBrowserEvent::SAVE;
        } else if (hitSaveAs(sx, sy)) {
            r.event = FileBrowserEvent::SAVE_AS;
        } else {
            int idx = hitItem(sx, sy);
            if (idx >= 0 && idx < _itemCount) {
                r.event = _altMode ? FileBrowserEvent::ITEM_DELETE
                                   : FileBrowserEvent::ITEM_TAP;
                r.index = idx;
            }
        }
    }

    if (down && !_wasDown) _wasDown = true;
    return r;
}

// ── Hit tests ─────────────────────────────────────────────────────────────────

bool FileBrowser::hitHome(int sx, int sy) const {
    return sx >= FB_HOME_X && sx < FB_HOME_X + FB_HOME_W &&
           sy >= 0         && sy < FB_HDR_H;
}
bool FileBrowser::hitDel(int sx, int sy) const {
    return sx >= FB_DEL_X && sx < FB_DEL_X + FB_DEL_W &&
           sy >= 0        && sy < FB_HDR_H;
}
bool FileBrowser::hitPrev(int sx, int sy) const {
    return sx >= FB_PREV_X && sx < FB_PREV_X + FB_PREV_W &&
           sy >= FB_BTN_Y  && sy < FB_BTN_Y  + FB_BTN_H;
}
bool FileBrowser::hitNext(int sx, int sy) const {
    return sx >= FB_NEXT_X && sx < FB_NEXT_X + FB_NEXT_W &&
           sy >= FB_BTN_Y  && sy < FB_BTN_Y  + FB_BTN_H;
}
int FileBrowser::hitItem(int sx, int sy) const {
    if (sy < FB_LIST_Y || sy >= FB_FOOT_Y) return -1;
    int vi = (sy - FB_LIST_Y) / FB_ROW_H;
    int ci = sx / FB_COL_W;
    if (vi < 0 || vi >= FB_ROWS || ci < 0 || ci >= FB_COLS) return -1;
    return vi * FB_COLS + ci;
}
bool FileBrowser::hitNew(int sx, int sy) const {
    return sx >= FB_NEW_X   && sx < FB_NEW_X   + FB_NEW_W &&
           sy >= FB_FBTN_Y  && sy < FB_FBTN_Y  + FB_FBTN_H;
}
bool FileBrowser::hitSave(int sx, int sy) const {
    if (_loadedName[0] == '\0') return false;
    return sx >= FB_SAVE_X  && sx < FB_SAVE_X  + FB_SAVE_W &&
           sy >= FB_FBTN_Y  && sy < FB_FBTN_Y  + FB_FBTN_H;
}
bool FileBrowser::hitSaveAs(int sx, int sy) const {
    return sx >= FB_SAVEAS_X && sx < FB_SAVEAS_X + FB_SAVEAS_W &&
           sy >= FB_FBTN_Y   && sy < FB_FBTN_Y   + FB_FBTN_H;
}

void FileBrowser::rawToScreen(int rx, int ry, int& sx, int& sy) {
    sx = ry;
    sy = 540 - rx;
}

#include "SetlistPage.h"
#include "UIHelpers.h"
#include <SD.h>
#include <string.h>
#include <stdio.h>

// Matches SONGS_DIR in SongPage.h (kept local to avoid a heavy include).
static const char* SETLIST_SONGS_DIR = "/songs";

SetlistPage::SetlistPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _keyboard(display, touch)
    , _dialog(display, touch)
    , _state(State::LIST)
    , _wasDown(false)
    , _slot(1)
    , _page(0)
    , _selectedIdx(-1)
    , _draftIsNew(false)
    , _kbdTarget(nullptr)
    , _fileCount(0)
    , _pickPage(0)
    , _srvPickPage(0)
    , _srvListDrawn(false)
    , _srvLoadStartMs(0)
{
    memset(&_list, 0, sizeof(_list));
    memset(&_draft, 0, sizeof(_draft));
    _kbdSnap[0]            = '\0';
    _loadedFilename[0]     = '\0';
    _loadedDisplayName[0]  = '\0';
}

void SetlistPage::open() {
    _state   = State::LIST;
    _wasDown = _touch.isTouched;
    _page    = 0;
    if (!loadSetlist(_slot, &_list)) {
        initSetlist(&_list, _slot);
    }
    _loadedFilename[0] = '\0';
}

void SetlistPage::draw() {
    _d.fillScreen(COL_WHITE);
    switch (_state) {
        case State::LIST:
            drawList();
            break;
        case State::INFO:
            drawList();
            drawInfo();
            break;
        case State::WAITING_SONG:
            drawWaitingSong();
            break;
        case State::EDIT:
            drawEdit();
            break;
        case State::KBD_NAME:
        case State::KBD_NOTES:
            _keyboard.draw();
            break;
        case State::PICK_FILE:
            drawPick();
            break;
        case State::PICK_FILE_SRV:
            drawPickSrv();
            break;
        case State::CONFIRM_DELETE:
            drawEdit();
            _dialog.draw();
            break;
    }
}

SetlistResult SetlistPage::poll() {
    switch (_state) {
        case State::LIST:
            return pollList();

        case State::INFO:
            return pollInfo();

        case State::WAITING_SONG:
            return pollWaitingSong();

        case State::EDIT:
            (void)pollEdit();
            return SetlistResult::NONE;

        case State::KBD_NAME:
        case State::KBD_NOTES:
            if (_keyboard.poll()) {
                size_t cap = (_state == State::KBD_NOTES)
                             ? SETLIST_NOTES_LEN : SETLIST_SONG_NAME_LEN;
                if (!_keyboard.isDone() && _kbdTarget) {
                    strncpy(_kbdTarget, _kbdSnap, cap - 1);
                    _kbdTarget[cap - 1] = '\0';
                }
                _kbdTarget = nullptr;
                _state = State::EDIT;
                _wasDown = _touch.isTouched;
                _d.fillScreen(COL_WHITE);
                drawEdit();
                _d.paint();
            }
            return SetlistResult::NONE;

        case State::PICK_FILE:
            (void)pollPick();
            return SetlistResult::NONE;

        case State::PICK_FILE_SRV:
            (void)pollPickSrv();
            return SetlistResult::NONE;

        case State::CONFIRM_DELETE:
            if (_dialog.poll()) {
                if (_dialog.confirmed() && _selectedIdx >= 0) {
                    deleteSong(_selectedIdx);
                    save();
                    _selectedIdx = -1;
                    _state = State::LIST;
                    _wasDown = _touch.isTouched;
                    _d.fillScreen(COL_WHITE);
                    drawList();
                    _d.paint();
                } else {
                    _state = State::EDIT;
                    _wasDown = _touch.isTouched;
                    _d.fillScreen(COL_WHITE);
                    drawEdit();
                    _d.paint();
                }
            }
            return SetlistResult::NONE;
    }
    return SetlistResult::NONE;
}

SetlistResult SetlistPage::pollInfo() {
    if (!_touch.read()) return SetlistResult::NONE;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);
    bool falling = (!down && _wasDown);
    _wasDown = down;
    if (!falling) return SetlistResult::NONE;

    if (hitInfoCancel(sx, sy)) {
        _state = State::LIST;
        _d.fillScreen(COL_WHITE);
        drawList();
        _d.paint();
        return SetlistResult::NONE;
    }
    if (hitInfoEdit(sx, sy)) {
        if (_selectedIdx >= 0 && _selectedIdx < (int)_list.count) {
            _draft       = _list.songs[_selectedIdx];
            _draftIsNew  = false;
            _state = State::EDIT;
            _d.fillScreen(COL_WHITE);
            drawEdit();
            _d.paint();
        }
        return SetlistResult::NONE;
    }
    if (hitInfoOk(sx, sy)) {
        if (_selectedIdx >= 0 && _selectedIdx < (int)_list.count) {
            const SetlistEntry& e = _list.songs[_selectedIdx];

            // No file attached — return to Performance with title set,
            // but leave the currently-loaded song alone (manual-play song).
            if (e.file[0] == '\0') {
                _loadedFilename[0] = '\0';
                strncpy(_loadedDisplayName, e.name, sizeof(_loadedDisplayName) - 1);
                _loadedDisplayName[sizeof(_loadedDisplayName) - 1] = '\0';
                return SetlistResult::TITLE_ONLY;
            }

            if (gServerPairing.isPaired()) {
                // Server load by bare name (no .mgt) — async; wait in WAITING_SONG.
                char bare[SETLIST_FILE_LEN];
                strncpy(bare, e.file, sizeof(bare) - 1);
                bare[sizeof(bare) - 1] = '\0';
                int blen = (int)strlen(bare);
                if (blen > 4 && bare[blen - 4] == '.' &&
                    (bare[blen - 3] == 'm' || bare[blen - 3] == 'M')) {
                    bare[blen - 4] = '\0';
                }
                gServerPairing.resetBrowse();
                _srvLoadStartMs = millis();
                gServerPairing.requestSongLoadByName(bare);
                _state = State::WAITING_SONG;
                _d.fillScreen(COL_WHITE);
                drawWaitingSong();
                _d.paint();
                return SetlistResult::NONE;
            }

            // Not paired — SD load.
            if (!fileExistsOnSd(e.file)) return SetlistResult::NONE;
            char path[64];
            snprintf(path, sizeof(path), "%s/%s%s",
                     SETLIST_SONGS_DIR, e.file,
                     (strstr(e.file, ".mgt") || strstr(e.file, ".MGT")) ? "" : ".mgt");
            if (loadSong(path, &_song)) {
                strncpy(_loadedFilename, e.file, sizeof(_loadedFilename) - 1);
                _loadedFilename[sizeof(_loadedFilename) - 1] = '\0';
                strncpy(_loadedDisplayName, e.name, sizeof(_loadedDisplayName) - 1);
                _loadedDisplayName[sizeof(_loadedDisplayName) - 1] = '\0';
                return SetlistResult::SONG_LOADED;
            }
            Serial.printf("[SetlistPage] load failed: %s\n", path);
        }
    }
    return SetlistResult::NONE;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── LIST view ───────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::drawList() {
    drawListHeader();
    drawListTabs();
    drawListRows();
    drawListBottomBar();
}

void SetlistPage::drawListHeader() {
    _d.fillRect(0, 0, 960, SL_HDR_H, COL_BLACK);

    uiButton(_d, SL_BACK_X, SL_BTN_Y, SL_BACK_W, SL_BTN_H, "BACK",
             COL_BLACK, COL_WHITE, 3);

    char title[32];
    snprintf(title, sizeof(title), "SETLIST %u", (unsigned)_slot);
    int tw = (int)strlen(title) * 18;
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor((960 - tw) / 2, (SL_HDR_H - 24) / 2);
    _d.print(title);

    bool full = (_list.count >= SETLIST_MAX_SONGS);
    uiButton(_d, SL_ADD_X, SL_BTN_Y, SL_ADD_W, SL_BTN_H, "ADD",
             COL_BLACK, full ? COL_DKGREY : COL_WHITE, 3);
}

void SetlistPage::drawListTabs() {
    _d.fillRect(0, SL_TAB_Y, 960, SL_TAB_H, COL_WHITE);
    for (int i = 0; i < SETLIST_COUNT; i++) {
        int x = SL_TAB_MARGIN + i * (SL_TAB_W + SL_TAB_GAP);
        uint8_t slot = (uint8_t)(i + 1);
        bool active = (slot == _slot);
        char label[12];
        snprintf(label, sizeof(label), "SET %u", (unsigned)slot);
        uiButton(_d, x, SL_TAB_Y, SL_TAB_W, SL_TAB_H, label,
                 active ? COL_BLACK : COL_WHITE,
                 active ? COL_WHITE : COL_BLACK, 3);
        if (!active) {
            _d.drawRect(x, SL_TAB_Y, SL_TAB_W, SL_TAB_H, COL_BLACK);
        }
    }
}

void SetlistPage::drawListRows() {
    _d.fillRect(0, SL_LIST_Y, 960, SL_ROWS_PER_PAGE * SL_ROW_H, COL_WHITE);
    for (int i = 0; i < SL_ROWS_PER_PAGE; i++) {
        drawListRow(i);
    }
}

void SetlistPage::drawListRow(int rowOnPage) {
    int y    = SL_LIST_Y + rowOnPage * SL_ROW_H;
    int abs  = _page * SL_ROWS_PER_PAGE + rowOnPage;
    bool has = (abs >= 0 && abs < (int)_list.count);

    _d.fillRect(0, y, 960, SL_ROW_H, COL_WHITE);
    _d.drawFastHLine(20, y + SL_ROW_H - 1, 920, COL_LTGREY);

    if (!has) return;

    char numStr[6];
    snprintf(numStr, sizeof(numStr), "%d.", abs + 1);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SL_NUM_X + 4, y + (SL_ROW_H - 24) / 2);
    _d.print(numStr);

    const char* name = _list.songs[abs].name;
    int maxChars = SL_NAME_W / 18;
    char trunc[SETLIST_SONG_NAME_LEN + 4];
    if ((int)strlen(name) > maxChars) {
        strncpy(trunc, name, maxChars);
        trunc[maxChars] = '\0';
    } else {
        strncpy(trunc, name, sizeof(trunc) - 1);
        trunc[sizeof(trunc) - 1] = '\0';
    }
    _d.setCursor(SL_NAME_X + 8, y + (SL_ROW_H - 24) / 2);
    _d.print(trunc);

    bool canUp   = (abs > 0);
    bool canDown = (abs + 1 < (int)_list.count);

    int btnY = y + (SL_ROW_H - SL_ARROW_H) / 2;
    uiButton(_d, SL_UP_X,   btnY, SL_UP_W,   SL_ARROW_H, "^",
             COL_LTGREY, canUp ? COL_BLACK : COL_DKGREY, 3);
    uiButton(_d, SL_DOWN_X, btnY, SL_DOWN_W, SL_ARROW_H, "v",
             COL_LTGREY, canDown ? COL_BLACK : COL_DKGREY, 3);
}

void SetlistPage::drawListBottomBar() {
    _d.fillRect(0, SL_BAR_Y, 960, SL_BAR_H, COL_WHITE);
    _d.drawFastHLine(0, SL_BAR_Y, 960, COL_BLACK);

    int total   = numPages();
    bool hasPrev = (_page > 0);
    bool hasNext = (_page + 1 < total);

    uiButton(_d, SL_PREV_X, SL_BAR_Y + 5, SL_PREV_W, SL_BAR_H - 10,
             "< PREV", COL_LTGREY, hasPrev ? COL_BLACK : COL_DKGREY, 3);
    uiButton(_d, SL_NEXT_X, SL_BAR_Y + 5, SL_NEXT_W, SL_BAR_H - 10,
             "NEXT >", COL_LTGREY, hasNext ? COL_BLACK : COL_DKGREY, 3);

    char status[24];
    snprintf(status, sizeof(status), "Page %d / %d", _page + 1, total);
    int tw = (int)strlen(status) * 18;
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor((960 - tw) / 2, SL_BAR_Y + (SL_BAR_H - 24) / 2);
    _d.print(status);
}

int SetlistPage::numPages() const {
    int n = ((int)_list.count + SL_ROWS_PER_PAGE - 1) / SL_ROWS_PER_PAGE;
    return (n < 1) ? 1 : n;
}

SetlistResult SetlistPage::pollList() {
    if (!_touch.read()) return SetlistResult::NONE;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);
    bool falling = (!down && _wasDown);
    _wasDown = down;
    if (!falling) return SetlistResult::NONE;

    if (hitBack(sx, sy)) {
        save();
        return SetlistResult::BACK;
    }

    int tab = hitTab(sx, sy);
    if (tab > 0) {
        if ((uint8_t)tab != _slot) switchToSlot((uint8_t)tab);
        _d.fillScreen(COL_WHITE);
        drawList();
        _d.paint();
        return SetlistResult::NONE;
    }

    if (hitPrevPage(sx, sy) && _page > 0) {
        _page--;
        drawListRows();
        drawListBottomBar();
        _d.paintLater();
        return SetlistResult::NONE;
    }
    if (hitNextPage(sx, sy) && _page + 1 < numPages()) {
        _page++;
        drawListRows();
        drawListBottomBar();
        _d.paintLater();
        return SetlistResult::NONE;
    }

    if (hitAdd(sx, sy)) {
        if (_list.count < SETLIST_MAX_SONGS) {
            memset(&_draft, 0, sizeof(_draft));
            strncpy(_draft.name, "Untitled", SETLIST_SONG_NAME_LEN - 1);
            _draftIsNew  = true;
            _selectedIdx = -1;
            _state = State::EDIT;
            _d.fillScreen(COL_WHITE);
            drawEdit();
            _d.paint();
        }
        return SetlistResult::NONE;
    }

    int upIdx = hitRowUp(sx, sy);
    if (upIdx >= 0) {
        moveSongUp(upIdx);
        save();
        if (upIdx - 1 >= 0) {
            int newPage = (upIdx - 1) / SL_ROWS_PER_PAGE;
            if (newPage != _page) _page = newPage;
        }
        drawListRows();
        drawListBottomBar();
        _d.paintLater();
        return SetlistResult::NONE;
    }

    int dnIdx = hitRowDown(sx, sy);
    if (dnIdx >= 0) {
        moveSongDown(dnIdx);
        save();
        int newPage = (dnIdx + 1) / SL_ROWS_PER_PAGE;
        if (newPage != _page) _page = newPage;
        drawListRows();
        drawListBottomBar();
        _d.paintLater();
        return SetlistResult::NONE;
    }

    int nameIdx = hitRowName(sx, sy);
    if (nameIdx >= 0) {
        _selectedIdx = nameIdx;
        _state = State::INFO;
        drawInfo();
        _d.paint();
        return SetlistResult::NONE;
    }

    return SetlistResult::NONE;
}

int SetlistPage::hitTab(int sx, int sy) const {
    if (sy < SL_TAB_Y || sy >= SL_TAB_Y + SL_TAB_H) return -1;
    for (int i = 0; i < SETLIST_COUNT; i++) {
        int x = SL_TAB_MARGIN + i * (SL_TAB_W + SL_TAB_GAP);
        if (sx >= x && sx < x + SL_TAB_W) return i + 1;
    }
    return -1;
}

int SetlistPage::hitRowName(int sx, int sy) const {
    if (sx < SL_NAME_X || sx >= SL_NAME_X + SL_NAME_W) return -1;
    if (sy < SL_LIST_Y || sy >= SL_LIST_Y + SL_ROWS_PER_PAGE * SL_ROW_H) return -1;
    int rowOnPage = (sy - SL_LIST_Y) / SL_ROW_H;
    int abs = _page * SL_ROWS_PER_PAGE + rowOnPage;
    if (abs < 0 || abs >= (int)_list.count) return -1;
    return abs;
}

int SetlistPage::hitRowUp(int sx, int sy) const {
    if (sx < SL_UP_X || sx >= SL_UP_X + SL_UP_W) return -1;
    if (sy < SL_LIST_Y || sy >= SL_LIST_Y + SL_ROWS_PER_PAGE * SL_ROW_H) return -1;
    int rowOnPage = (sy - SL_LIST_Y) / SL_ROW_H;
    int abs = _page * SL_ROWS_PER_PAGE + rowOnPage;
    if (abs <= 0 || abs >= (int)_list.count) return -1;
    return abs;
}

int SetlistPage::hitRowDown(int sx, int sy) const {
    if (sx < SL_DOWN_X || sx >= SL_DOWN_X + SL_DOWN_W) return -1;
    if (sy < SL_LIST_Y || sy >= SL_LIST_Y + SL_ROWS_PER_PAGE * SL_ROW_H) return -1;
    int rowOnPage = (sy - SL_LIST_Y) / SL_ROW_H;
    int abs = _page * SL_ROWS_PER_PAGE + rowOnPage;
    if (abs < 0 || abs + 1 >= (int)_list.count) return -1;
    return abs;
}

bool SetlistPage::hitBack(int sx, int sy) const {
    return sx >= SL_BACK_X && sx < SL_BACK_X + SL_BACK_W
        && sy >= SL_BTN_Y  && sy < SL_BTN_Y + SL_BTN_H;
}

bool SetlistPage::hitAdd(int sx, int sy) const {
    return sx >= SL_ADD_X && sx < SL_ADD_X + SL_ADD_W
        && sy >= SL_BTN_Y && sy < SL_BTN_Y + SL_BTN_H;
}

bool SetlistPage::hitPrevPage(int sx, int sy) const {
    return sx >= SL_PREV_X && sx < SL_PREV_X + SL_PREV_W
        && sy >= SL_BAR_Y + 5 && sy < SL_BAR_Y + SL_BAR_H - 5;
}

bool SetlistPage::hitNextPage(int sx, int sy) const {
    return sx >= SL_NEXT_X && sx < SL_NEXT_X + SL_NEXT_W
        && sy >= SL_BAR_Y + 5 && sy < SL_BAR_Y + SL_BAR_H - 5;
}

void SetlistPage::switchToSlot(uint8_t newSlot) {
    save();
    _slot = newSlot;
    _page = 0;
    if (!loadSetlist(_slot, &_list)) {
        initSetlist(&_list, _slot);
    }
}

void SetlistPage::moveSongUp(int idx) {
    if (idx <= 0 || idx >= (int)_list.count) return;
    SetlistEntry tmp = _list.songs[idx - 1];
    _list.songs[idx - 1] = _list.songs[idx];
    _list.songs[idx]     = tmp;
}

void SetlistPage::moveSongDown(int idx) {
    if (idx < 0 || idx + 1 >= (int)_list.count) return;
    SetlistEntry tmp = _list.songs[idx + 1];
    _list.songs[idx + 1] = _list.songs[idx];
    _list.songs[idx]     = tmp;
}

void SetlistPage::deleteSong(int idx) {
    if (idx < 0 || idx >= (int)_list.count) return;
    for (int i = idx; i + 1 < (int)_list.count; i++) {
        _list.songs[i] = _list.songs[i + 1];
    }
    _list.count--;
    memset(&_list.songs[_list.count], 0, sizeof(SetlistEntry));
    if (_page >= numPages()) _page = numPages() - 1;
    if (_page < 0) _page = 0;
}

bool SetlistPage::fileExistsOnSd(const char* file) const {
    if (!file || file[0] == '\0') return false;
    char path[64];
    if (strstr(file, ".mgt") || strstr(file, ".MGT")) {
        snprintf(path, sizeof(path), "%s/%s", SETLIST_SONGS_DIR, file);
    } else {
        snprintf(path, sizeof(path), "%s/%s.mgt", SETLIST_SONGS_DIR, file);
    }
    return SD.exists(path);
}

// ═════════════════════════════════════════════════════════════════════════════
// ── INFO popup ──────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::drawInfo() {
    _d.fillRect(SL_INFO_X, SL_INFO_Y, SL_INFO_W, SL_INFO_H, COL_WHITE);
    _d.drawRect(SL_INFO_X, SL_INFO_Y, SL_INFO_W, SL_INFO_H, COL_BLACK);
    _d.drawRect(SL_INFO_X + 1, SL_INFO_Y + 1, SL_INFO_W - 2, SL_INFO_H - 2, COL_BLACK);

    if (_selectedIdx < 0 || _selectedIdx >= (int)_list.count) return;
    const SetlistEntry& e = _list.songs[_selectedIdx];

    int ty = SL_INFO_Y + 16;
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SL_INFO_X + 20, ty);
    char title[40];
    int maxNameChars = (SL_INFO_W - 40) / 18;
    int cap = (maxNameChars < (int)sizeof(title)) ? maxNameChars : (int)sizeof(title) - 1;
    strncpy(title, e.name, cap);
    title[cap] = '\0';
    _d.print(title);

    ty += 36;
    _d.setTextSize(2);
    char src[64];
    bool paired  = gServerPairing.isPaired();
    bool missing = !paired && !fileExistsOnSd(e.file);
    if (e.file[0] == '\0') {
        snprintf(src, sizeof(src), "Source: (none)");
    } else {
        snprintf(src, sizeof(src), "Source: %s%s%s",
                 e.file,
                 paired ? "  [server]" : "",
                 missing ? "   (FILE MISSING)" : "");
    }
    _d.setCursor(SL_INFO_X + 20, ty);
    _d.print(src);

    ty += 28;
    _d.drawFastHLine(SL_INFO_X + 20, ty, SL_INFO_W - 40, COL_LTGREY);
    ty += 12;

    const int wrapW = SL_INFO_W - 40;
    const int charW = 12;
    const int perLine = wrapW / charW;
    const int maxLines = 5;
    const char* p = e.notes;
    int lineY = ty;
    for (int line = 0; line < maxLines && *p; line++) {
        int rem = (int)strlen(p);
        int take = (rem > perLine) ? perLine : rem;
        if (take < rem) {
            int br = take;
            for (int i = take; i > take / 2; i--) {
                if (p[i] == ' ') { br = i; break; }
            }
            take = br;
        }
        char buf[80];
        if (take > (int)sizeof(buf) - 1) take = sizeof(buf) - 1;
        memcpy(buf, p, take);
        buf[take] = '\0';
        _d.setCursor(SL_INFO_X + 20, lineY);
        _d.print(buf);
        lineY += 22;
        p += take;
        while (*p == ' ') p++;
    }

    // OK is only disabled when there's a file set but it can't actually be
    // loaded (unpaired + missing from SD).  Empty-file entries are valid
    // (manual-play songs) — OK simply returns to Performance with the title.
    bool fileSet  = (e.file[0] != '\0');
    bool canOk    = !(fileSet && !paired && missing);
    const char* okLabel = fileSet ? "OK - LOAD" : "OK";
    uiButton(_d, SL_INFO_OK_X,   SL_INFO_BTN_Y, SL_INFO_OK_W,   SL_INFO_BTN_H,
             okLabel, COL_BLACK,
             canOk ? COL_WHITE : COL_DKGREY, 3);
    uiButton(_d, SL_INFO_EDIT_X, SL_INFO_BTN_Y, SL_INFO_EDIT_W, SL_INFO_BTN_H,
             "EDIT", COL_LTGREY, COL_BLACK, 3);
    uiButton(_d, SL_INFO_CAN_X,  SL_INFO_BTN_Y, SL_INFO_CAN_W,  SL_INFO_BTN_H,
             "CANCEL", COL_LTGREY, COL_BLACK, 3);
}

bool SetlistPage::hitInfoOk(int sx, int sy) const {
    return sx >= SL_INFO_OK_X && sx < SL_INFO_OK_X + SL_INFO_OK_W
        && sy >= SL_INFO_BTN_Y && sy < SL_INFO_BTN_Y + SL_INFO_BTN_H;
}
bool SetlistPage::hitInfoEdit(int sx, int sy) const {
    return sx >= SL_INFO_EDIT_X && sx < SL_INFO_EDIT_X + SL_INFO_EDIT_W
        && sy >= SL_INFO_BTN_Y && sy < SL_INFO_BTN_Y + SL_INFO_BTN_H;
}
bool SetlistPage::hitInfoCancel(int sx, int sy) const {
    return sx >= SL_INFO_CAN_X && sx < SL_INFO_CAN_X + SL_INFO_CAN_W
        && sy >= SL_INFO_BTN_Y && sy < SL_INFO_BTN_Y + SL_INFO_BTN_H;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── EDIT screen ─────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::drawEdit() {
    _d.fillRect(0, 0, 960, SL_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    const char* title = _draftIsNew ? "ADD SONG ENTRY" : "EDIT SONG ENTRY";
    int tw = (int)strlen(title) * 18;
    _d.setCursor((960 - tw) / 2, (SL_HDR_H - 24) / 2);
    _d.print(title);

    drawEditFieldRow(SL_ED_NAME_Y, "Song Name:", _draft.name, SL_ED_FIELD_VAL_W, "EDIT");
    drawEditFileRow();
    drawEditNotesRow();
    drawEditActionBar();
}

void SetlistPage::drawEditFileRow() {
    int y = SL_ED_FILE_Y;

    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SL_ED_FIELD_LBL_X, y + 22);
    _d.print("Song File:");

    _d.fillRect(SL_ED_FIELD_VAL_X, y, SL_ED_FILE_VAL_W, 60, COL_WHITE);
    _d.drawRect(SL_ED_FIELD_VAL_X, y, SL_ED_FILE_VAL_W, 60, COL_BLACK);
    _d.setTextSize(3);
    int maxChars = (SL_ED_FILE_VAL_W - 20) / 18;
    char trunc[64];
    int n = (int)strlen(_draft.file);
    if (n > maxChars) n = maxChars;
    if (n > (int)sizeof(trunc) - 1) n = sizeof(trunc) - 1;
    memcpy(trunc, _draft.file, n);
    trunc[n] = '\0';
    if (trunc[0] == '\0') {
        _d.setTextColor(COL_DKGREY);
        _d.setCursor(SL_ED_FIELD_VAL_X + 10, y + (60 - 24) / 2);
        _d.print("(none)");
    } else {
        _d.setTextColor(COL_BLACK);
        _d.setCursor(SL_ED_FIELD_VAL_X + 10, y + (60 - 24) / 2);
        _d.print(trunc);
    }

    uiButton(_d, SL_ED_FILE_PICK_X, y, SL_ED_FILE_PICK_W, SL_ED_FIELD_BTN_H,
             "PICK", COL_LTGREY, COL_BLACK, 3);
    bool canClear = (_draft.file[0] != '\0');
    uiButton(_d, SL_ED_FILE_CLR_X, y, SL_ED_FILE_CLR_W, SL_ED_FIELD_BTN_H,
             "CLEAR", COL_LTGREY, canClear ? COL_BLACK : COL_DKGREY, 3);
}

void SetlistPage::drawEditFieldRow(int y, const char* label, const char* value,
                                   int valW, const char* btnLabel) {
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SL_ED_FIELD_LBL_X, y + 22);
    _d.print(label);

    _d.fillRect(SL_ED_FIELD_VAL_X, y, valW, 60, COL_WHITE);
    _d.drawRect(SL_ED_FIELD_VAL_X, y, valW, 60, COL_BLACK);
    _d.setTextSize(3);
    int maxChars = (valW - 20) / 18;
    char trunc[64];
    int n = (int)strlen(value);
    if (n > maxChars) n = maxChars;
    if (n > (int)sizeof(trunc) - 1) n = sizeof(trunc) - 1;
    memcpy(trunc, value, n);
    trunc[n] = '\0';
    if (trunc[0] == '\0') {
        _d.setTextColor(COL_DKGREY);
        _d.setCursor(SL_ED_FIELD_VAL_X + 10, y + (60 - 24) / 2);
        _d.print("(empty)");
    } else {
        _d.setTextColor(COL_BLACK);
        _d.setCursor(SL_ED_FIELD_VAL_X + 10, y + (60 - 24) / 2);
        _d.print(trunc);
    }

    uiButton(_d, SL_ED_FIELD_BTN_X, y, SL_ED_FIELD_BTN_W, SL_ED_FIELD_BTN_H,
             btnLabel, COL_LTGREY, COL_BLACK, 3);
}

void SetlistPage::drawEditNotesRow() {
    int y = SL_ED_NOTES_Y;
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SL_ED_FIELD_LBL_X, y + 8);
    _d.print("Notes:");

    int boxX = SL_ED_FIELD_VAL_X;
    int boxW = SL_ED_FIELD_VAL_W;
    int boxH = SL_ED_NOTES_H;
    _d.fillRect(boxX, y, boxW, boxH, COL_WHITE);
    _d.drawRect(boxX, y, boxW, boxH, COL_BLACK);

    const int charW = 12;
    int perLine = (boxW - 20) / charW;
    char l1[80], l2[80];
    wrapText(_draft.notes, perLine, l1, l2, sizeof(l1), sizeof(l2));
    _d.setTextSize(2);
    if (l1[0] == '\0') {
        _d.setTextColor(COL_DKGREY);
        _d.setCursor(boxX + 10, y + 10);
        _d.print("(empty)");
    } else {
        _d.setTextColor(COL_BLACK);
        _d.setCursor(boxX + 10, y + 10);
        _d.print(l1);
        if (l2[0]) {
            _d.setCursor(boxX + 10, y + 36);
            _d.print(l2);
        }
    }

    int btnY = y + (boxH - SL_ED_FIELD_BTN_H) / 2;
    uiButton(_d, SL_ED_FIELD_BTN_X, btnY, SL_ED_FIELD_BTN_W, SL_ED_FIELD_BTN_H,
             "EDIT", COL_LTGREY, COL_BLACK, 3);
}

void SetlistPage::drawEditActionBar() {
    if (!_draftIsNew) {
        uiButton(_d, SL_ED_DEL_X, SL_ED_ACT_Y, SL_ED_DEL_W, SL_ED_ACT_H,
                 "DELETE", COL_BLACK, COL_WHITE, 3);
    }
    uiButton(_d, SL_ED_CAN_X, SL_ED_ACT_Y, SL_ED_CAN_W, SL_ED_ACT_H,
             "CANCEL", COL_LTGREY, COL_BLACK, 3);
    uiButton(_d, SL_ED_OK_X, SL_ED_ACT_Y, SL_ED_OK_W, SL_ED_ACT_H,
             "OK", COL_BLACK, COL_WHITE, 3);
}

bool SetlistPage::pollEdit() {
    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);
    bool falling = (!down && _wasDown);
    _wasDown = down;
    if (!falling) return false;

    if (hitEditFieldBtn(SL_ED_NAME_Y, sx, sy)) {
        openKeyboardFor(_draft.name, SETLIST_SONG_NAME_LEN - 1, State::KBD_NAME);
        return true;
    }
    if (hitEditFilePick(sx, sy)) {
        if (gServerPairing.isPaired()) {
            startServerPick();
        } else {
            loadFileList();
            _pickPage = 0;
            _state = State::PICK_FILE;
            _d.fillScreen(COL_WHITE);
            drawPick();
            _d.paint();
        }
        return true;
    }
    if (hitEditFileClear(sx, sy) && _draft.file[0] != '\0') {
        _draft.file[0] = '\0';
        drawEditFileRow();
        _d.paintLater();
        return true;
    }
    {
        int notesBtnY = SL_ED_NOTES_Y + (SL_ED_NOTES_H - SL_ED_FIELD_BTN_H) / 2;
        if (sx >= SL_ED_FIELD_BTN_X && sx < SL_ED_FIELD_BTN_X + SL_ED_FIELD_BTN_W
         && sy >= notesBtnY && sy < notesBtnY + SL_ED_FIELD_BTN_H) {
            openKeyboardFor(_draft.notes, SETLIST_NOTES_LEN - 1, State::KBD_NOTES);
            return true;
        }
    }

    if (hitEditOk(sx, sy)) {
        commitDraft();
        save();
        _state = State::LIST;
        _d.fillScreen(COL_WHITE);
        drawList();
        _d.paint();
        return true;
    }
    if (hitEditCancel(sx, sy)) {
        _state = State::LIST;
        _d.fillScreen(COL_WHITE);
        drawList();
        _d.paint();
        return true;
    }
    if (!_draftIsNew && hitEditDelete(sx, sy)) {
        _dialog.open("DELETE THIS ENTRY?");
        _state = State::CONFIRM_DELETE;
        _d.fillScreen(COL_WHITE);
        drawEdit();
        _dialog.draw();
        _d.paint();
        return true;
    }

    return false;
}

bool SetlistPage::hitEditFieldBtn(int y, int sx, int sy) const {
    return sx >= SL_ED_FIELD_BTN_X && sx < SL_ED_FIELD_BTN_X + SL_ED_FIELD_BTN_W
        && sy >= y && sy < y + SL_ED_FIELD_BTN_H;
}

bool SetlistPage::hitEditFilePick(int sx, int sy) const {
    return sx >= SL_ED_FILE_PICK_X && sx < SL_ED_FILE_PICK_X + SL_ED_FILE_PICK_W
        && sy >= SL_ED_FILE_Y && sy < SL_ED_FILE_Y + SL_ED_FIELD_BTN_H;
}

bool SetlistPage::hitEditFileClear(int sx, int sy) const {
    return sx >= SL_ED_FILE_CLR_X && sx < SL_ED_FILE_CLR_X + SL_ED_FILE_CLR_W
        && sy >= SL_ED_FILE_Y && sy < SL_ED_FILE_Y + SL_ED_FIELD_BTN_H;
}

bool SetlistPage::hitEditOk(int sx, int sy) const {
    return sx >= SL_ED_OK_X && sx < SL_ED_OK_X + SL_ED_OK_W
        && sy >= SL_ED_ACT_Y && sy < SL_ED_ACT_Y + SL_ED_ACT_H;
}
bool SetlistPage::hitEditCancel(int sx, int sy) const {
    return sx >= SL_ED_CAN_X && sx < SL_ED_CAN_X + SL_ED_CAN_W
        && sy >= SL_ED_ACT_Y && sy < SL_ED_ACT_Y + SL_ED_ACT_H;
}
bool SetlistPage::hitEditDelete(int sx, int sy) const {
    return sx >= SL_ED_DEL_X && sx < SL_ED_DEL_X + SL_ED_DEL_W
        && sy >= SL_ED_ACT_Y && sy < SL_ED_ACT_Y + SL_ED_ACT_H;
}

void SetlistPage::commitDraft() {
    if (_draftIsNew) {
        if (_list.count >= SETLIST_MAX_SONGS) return;
        _list.songs[_list.count] = _draft;
        _list.count++;
    } else if (_selectedIdx >= 0 && _selectedIdx < (int)_list.count) {
        _list.songs[_selectedIdx] = _draft;
    }
}

void SetlistPage::openKeyboardFor(char* target, uint8_t maxLen, State afterState) {
    _kbdTarget = target;
    size_t cap = (afterState == State::KBD_NOTES) ? SETLIST_NOTES_LEN : SETLIST_SONG_NAME_LEN;
    strncpy(_kbdSnap, target, cap - 1);
    _kbdSnap[cap - 1] = '\0';
    _keyboard.open(target, maxLen);
    _state = afterState;
    _d.fillScreen(COL_WHITE);
    _keyboard.draw();
    _d.paint();
}

// ═════════════════════════════════════════════════════════════════════════════
// ── PICK_FILE screen ────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::loadFileList() {
    _fileCount = listSongs(SETLIST_SONGS_DIR, _files, STORAGE_MAX_FILES);
}

void SetlistPage::drawPick() {
    drawPickHeader("PICK SONG (SD)");
    drawPickRows();
    drawPickBottomBar(numPickPages());
}

void SetlistPage::drawPickHeader(const char* title) {
    _d.fillRect(0, 0, 960, SL_HDR_H, COL_BLACK);
    uiButton(_d, SL_BACK_X, SL_BTN_Y, SL_BACK_W, SL_BTN_H, "BACK",
             COL_BLACK, COL_WHITE, 3);
    int tw = (int)strlen(title) * 18;
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor((960 - tw) / 2, (SL_HDR_H - 24) / 2);
    _d.print(title);
}

void SetlistPage::drawPickRows() {
    int y0 = SL_HDR_H + 10;
    int rowH = (SL_BAR_Y - 10 - y0) / SL_PICK_ROWS_PER_PAGE;
    _d.fillRect(0, y0, 960, rowH * SL_PICK_ROWS_PER_PAGE, COL_WHITE);
    for (int i = 0; i < SL_PICK_ROWS_PER_PAGE; i++) {
        int y = y0 + i * rowH;
        int abs = _pickPage * SL_PICK_ROWS_PER_PAGE + i;
        if (abs < 0 || abs >= _fileCount) {
            _d.drawFastHLine(20, y + rowH - 1, 920, COL_LTGREY);
            continue;
        }
        char label[STORAGE_FILENAME_MAX];
        strncpy(label, _files[abs], sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
        int len = (int)strlen(label);
        if (len > 4 && label[len - 4] == '.' &&
            (label[len - 3] == 'm' || label[len - 3] == 'M')) {
            label[len - 4] = '\0';
        }
        _d.setTextSize(3);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(20, y + (rowH - 24) / 2);
        _d.print(label);
        _d.drawFastHLine(20, y + rowH - 1, 920, COL_LTGREY);
    }
}

void SetlistPage::drawPickBottomBar(int total) {
    _d.fillRect(0, SL_BAR_Y, 960, SL_BAR_H, COL_WHITE);
    _d.drawFastHLine(0, SL_BAR_Y, 960, COL_BLACK);
    int page = (_state == State::PICK_FILE_SRV) ? _srvPickPage : _pickPage;
    bool hasPrev = (page > 0);
    bool hasNext = (page + 1 < total);
    uiButton(_d, SL_PREV_X, SL_BAR_Y + 5, SL_PREV_W, SL_BAR_H - 10,
             "< PREV", COL_LTGREY, hasPrev ? COL_BLACK : COL_DKGREY, 3);
    uiButton(_d, SL_NEXT_X, SL_BAR_Y + 5, SL_NEXT_W, SL_BAR_H - 10,
             "NEXT >", COL_LTGREY, hasNext ? COL_BLACK : COL_DKGREY, 3);
    char status[24];
    snprintf(status, sizeof(status), "Page %d / %d", page + 1, total);
    int tw = (int)strlen(status) * 18;
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor((960 - tw) / 2, SL_BAR_Y + (SL_BAR_H - 24) / 2);
    _d.print(status);
}

int SetlistPage::numPickPages() const {
    int n = (_fileCount + SL_PICK_ROWS_PER_PAGE - 1) / SL_PICK_ROWS_PER_PAGE;
    return (n < 1) ? 1 : n;
}

bool SetlistPage::pollPick() {
    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);
    bool falling = (!down && _wasDown);
    _wasDown = down;
    if (!falling) return false;

    if (hitPickBack(sx, sy)) {
        _state = State::EDIT;
        _d.fillScreen(COL_WHITE);
        drawEdit();
        _d.paint();
        return true;
    }
    if (hitPickPrev(sx, sy) && _pickPage > 0) {
        _pickPage--;
        drawPickRows();
        drawPickBottomBar(numPickPages());
        _d.paintLater();
        return true;
    }
    if (hitPickNext(sx, sy) && _pickPage + 1 < numPickPages()) {
        _pickPage++;
        drawPickRows();
        drawPickBottomBar(numPickPages());
        _d.paintLater();
        return true;
    }
    int fileIdx = hitPickRow(sx, sy);
    if (fileIdx >= 0 && fileIdx < _fileCount) {
        char base[STORAGE_FILENAME_MAX];
        strncpy(base, _files[fileIdx], sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        int len = (int)strlen(base);
        if (len > 4 && base[len - 4] == '.' &&
            (base[len - 3] == 'm' || base[len - 3] == 'M')) {
            base[len - 4] = '\0';
        }
        strncpy(_draft.file, base, SETLIST_FILE_LEN - 1);
        _draft.file[SETLIST_FILE_LEN - 1] = '\0';
        if (_draftIsNew &&
            (_draft.name[0] == '\0' || strcmp(_draft.name, "Untitled") == 0)) {
            strncpy(_draft.name, base, SETLIST_SONG_NAME_LEN - 1);
            _draft.name[SETLIST_SONG_NAME_LEN - 1] = '\0';
        }
        _state = State::EDIT;
        _d.fillScreen(COL_WHITE);
        drawEdit();
        _d.paint();
        return true;
    }
    return false;
}

int SetlistPage::hitPickRow(int sx, int sy) const {
    int y0 = SL_HDR_H + 10;
    int rowH = (SL_BAR_Y - 10 - y0) / SL_PICK_ROWS_PER_PAGE;
    if (sy < y0 || sy >= y0 + rowH * SL_PICK_ROWS_PER_PAGE) return -1;
    if (sx < 20 || sx >= 940) return -1;
    int rowOnPage = (sy - y0) / rowH;
    return _pickPage * SL_PICK_ROWS_PER_PAGE + rowOnPage;
}
bool SetlistPage::hitPickBack(int sx, int sy) const {
    return sx >= SL_BACK_X && sx < SL_BACK_X + SL_BACK_W
        && sy >= SL_BTN_Y && sy < SL_BTN_Y + SL_BTN_H;
}
bool SetlistPage::hitPickPrev(int sx, int sy) const {
    return sx >= SL_PREV_X && sx < SL_PREV_X + SL_PREV_W
        && sy >= SL_BAR_Y + 5 && sy < SL_BAR_Y + SL_BAR_H - 5;
}
bool SetlistPage::hitPickNext(int sx, int sy) const {
    return sx >= SL_NEXT_X && sx < SL_NEXT_X + SL_NEXT_W
        && sy >= SL_BAR_Y + 5 && sy < SL_BAR_Y + SL_BAR_H - 5;
}

// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

void SetlistPage::save() {
    saveSetlist(_slot, &_list);
}

// ═════════════════════════════════════════════════════════════════════════════
// ── PICK_FILE_SRV (server) ──────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::startServerPick() {
    gServerPairing.resetBrowse();
    gServerPairing.requestSongList(0);
    _srvPickPage  = 0;
    _srvListDrawn = false;
    _state = State::PICK_FILE_SRV;
    _d.fillScreen(COL_WHITE);
    drawPickSrv();
    _d.paint();
}

void SetlistPage::drawPickSrv() {
    drawPickHeader("PICK SONG (SERVER)");
    drawPickSrvRows();
    drawPickBottomBar(gServerPairing.listTotalPages() > 0
                      ? gServerPairing.listTotalPages() : 1);
}

void SetlistPage::drawPickSrvRows() {
    int y0 = SL_HDR_H + 10;
    int rowH = (SL_BAR_Y - 10 - y0) / SL_PICK_ROWS_PER_PAGE;
    _d.fillRect(0, y0, 960, rowH * SL_PICK_ROWS_PER_PAGE, COL_WHITE);

    int count = gServerPairing.listCount();
    if (count == 0 && !_srvListDrawn) {
        // Loading state
        _d.setTextSize(3);
        _d.setTextColor(COL_DKGREY);
        const char* msg = "Loading from server...";
        int tw = (int)strlen(msg) * 18;
        _d.setCursor((960 - tw) / 2, y0 + 60);
        _d.print(msg);
        return;
    }
    for (int i = 0; i < SL_PICK_ROWS_PER_PAGE; i++) {
        int y = y0 + i * rowH;
        _d.drawFastHLine(20, y + rowH - 1, 920, COL_LTGREY);
        if (i >= count) continue;
        const char* name = gServerPairing.listName(i);
        if (!name || !*name) continue;
        _d.setTextSize(3);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(20, y + (rowH - 24) / 2);
        _d.print(name);
    }
}

void SetlistPage::drawPickSrvRow(int /*rowOnPage*/, const char* /*name*/) {
    // Unused — drawPickSrvRows draws all rows directly.
}

bool SetlistPage::pollPickSrv() {
    // Repopulate when server reports list ready.
    BrowseState bs = gServerPairing.browseState();
    if (bs == BrowseState::LIST_READY && !_srvListDrawn) {
        _srvListDrawn = true;
        drawPickSrvRows();
        drawPickBottomBar(gServerPairing.listTotalPages() > 0
                          ? gServerPairing.listTotalPages() : 1);
        _d.paintLater();
    }

    if (!_touch.read()) return false;
    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);
    bool falling = (!down && _wasDown);
    _wasDown = down;
    if (!falling) return false;

    if (hitPickBack(sx, sy)) {
        gServerPairing.resetBrowse();
        _state = State::EDIT;
        _d.fillScreen(COL_WHITE);
        drawEdit();
        _d.paint();
        return true;
    }
    if (hitPickPrev(sx, sy) && _srvPickPage > 0) {
        _srvPickPage--;
        _srvListDrawn = false;
        gServerPairing.requestSongList(_srvPickPage);
        drawPickSrvRows();
        drawPickBottomBar(gServerPairing.listTotalPages() > 0
                          ? gServerPairing.listTotalPages() : 1);
        _d.paintLater();
        return true;
    }
    if (hitPickNext(sx, sy) &&
        _srvPickPage + 1 < gServerPairing.listTotalPages()) {
        _srvPickPage++;
        _srvListDrawn = false;
        gServerPairing.requestSongList(_srvPickPage);
        drawPickSrvRows();
        drawPickBottomBar(gServerPairing.listTotalPages() > 0
                          ? gServerPairing.listTotalPages() : 1);
        _d.paintLater();
        return true;
    }
    int rowIdx = hitPickSrvRow(sx, sy);
    if (rowIdx >= 0 && rowIdx < gServerPairing.listCount()) {
        const char* name = gServerPairing.listName(rowIdx);
        if (name && *name) {
            strncpy(_draft.file, name, SETLIST_FILE_LEN - 1);
            _draft.file[SETLIST_FILE_LEN - 1] = '\0';
            if (_draftIsNew &&
                (_draft.name[0] == '\0' || strcmp(_draft.name, "Untitled") == 0)) {
                strncpy(_draft.name, name, SETLIST_SONG_NAME_LEN - 1);
                _draft.name[SETLIST_SONG_NAME_LEN - 1] = '\0';
            }
            gServerPairing.resetBrowse();
            _state = State::EDIT;
            _d.fillScreen(COL_WHITE);
            drawEdit();
            _d.paint();
        }
        return true;
    }
    return false;
}

int SetlistPage::hitPickSrvRow(int sx, int sy) const {
    int y0 = SL_HDR_H + 10;
    int rowH = (SL_BAR_Y - 10 - y0) / SL_PICK_ROWS_PER_PAGE;
    if (sy < y0 || sy >= y0 + rowH * SL_PICK_ROWS_PER_PAGE) return -1;
    if (sx < 20 || sx >= 940) return -1;
    return (sy - y0) / rowH;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── WAITING_SONG (server load) ──────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::drawWaitingSong() {
    _d.fillRect(0, 0, 960, SL_HDR_H, COL_BLACK);
    const char* title = "LOADING FROM SERVER";
    int tw = (int)strlen(title) * 18;
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor((960 - tw) / 2, (SL_HDR_H - 24) / 2);
    _d.print(title);

    const char* who = (_selectedIdx >= 0 && _selectedIdx < (int)_list.count)
                      ? _list.songs[_selectedIdx].file : "";
    char msg[64];
    snprintf(msg, sizeof(msg), "Loading '%s' ...", who);
    int mw = (int)strlen(msg) * 18;
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor((960 - mw) / 2, 240);
    _d.print(msg);
}

SetlistResult SetlistPage::pollWaitingSong() {
    BrowseState bs = gServerPairing.browseState();
    if (bs == BrowseState::SONG_READY) {
        if (gServerPairing.copySong(&_song)) {
            if (_selectedIdx >= 0 && _selectedIdx < (int)_list.count) {
                const SetlistEntry& e = _list.songs[_selectedIdx];
                strncpy(_loadedFilename, e.file, sizeof(_loadedFilename) - 1);
                _loadedFilename[sizeof(_loadedFilename) - 1] = '\0';
                strncpy(_loadedDisplayName, e.name, sizeof(_loadedDisplayName) - 1);
                _loadedDisplayName[sizeof(_loadedDisplayName) - 1] = '\0';
            }
            gServerPairing.resetBrowse();
            return SetlistResult::SONG_LOADED;
        }
        // Corrupt — fall through to error path
        gServerPairing.resetBrowse();
    }
    if (millis() - _srvLoadStartMs > 8000) {
        Serial.println("[SetlistPage] server load timeout");
        gServerPairing.resetBrowse();
        _state = State::INFO;
        _wasDown = _touch.isTouched;
        _d.fillScreen(COL_WHITE);
        drawList();
        drawInfo();
        _d.paint();
    }
    return SetlistResult::NONE;
}

void SetlistPage::wrapText(const char* in, int maxChars,
                           char line1[], char line2[],
                           int line1Cap, int line2Cap) {
    line1[0] = '\0';
    line2[0] = '\0';
    if (!in || !*in) return;
    int len = (int)strlen(in);
    if (len <= maxChars) {
        int n = (len < line1Cap - 1) ? len : line1Cap - 1;
        memcpy(line1, in, n);
        line1[n] = '\0';
        return;
    }
    int br = maxChars;
    for (int i = maxChars; i > maxChars / 2; i--) {
        if (in[i] == ' ') { br = i; break; }
    }
    int n1 = (br < line1Cap - 1) ? br : line1Cap - 1;
    memcpy(line1, in, n1);
    line1[n1] = '\0';
    const char* p = in + br;
    while (*p == ' ') p++;
    int rem = (int)strlen(p);
    if (rem > maxChars) rem = maxChars;
    int n2 = (rem < line2Cap - 1) ? rem : line2Cap - 1;
    memcpy(line2, p, n2);
    line2[n2] = '\0';
}

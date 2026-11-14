#include "SetlistPage.h"
#include "UIHelpers.h"
#include <string.h>
#include <stdio.h>

// The master catalog is large (~20 KB) and only one SetlistPage exists, so it
// lives at file scope rather than in the page object — and in PSRAM, because
// internal RAM is the radios' budget.  Allocated by the first open() and kept;
// every state except NOT_CONNECTED is reached only after that succeeded.
static MasterList* sMaster = nullptr;

// Serialise buffers and the scratch list used to patch the other slots — PSRAM
// too, allocated on first use and kept.
static char*    sMasterBuf  = nullptr;   // MASTER_SERIALIZE_MAX
static char*    sSetlistBuf = nullptr;   // SETLIST_SERIALIZE_MAX
static Setlist* sOtherList  = nullptr;

static bool ensureMaster() {
    if (sMaster) return true;
    sMaster = (MasterList*)ps_malloc(sizeof(MasterList));
    if (!sMaster) {
        Serial.println("[SetlistPage] no PSRAM for the master catalog");
        return false;
    }
    initMasterList(sMaster);
    return true;
}

// Drop a trailing ".mgt" (any case), in place.
static void stripMgt(char* s) {
    size_t n = strlen(s);
    if (n > 4 && strcasecmp(s + n - 4, ".mgt") == 0) s[n - 4] = '\0';
}

// Point every reference to oldName at newName, or drop it when newName is
// null.  Returns true if the list changed.
static bool patchSetlistName(Setlist* sl, const char* oldName, const char* newName) {
    bool changed = false;
    for (int i = 0; i < (int)sl->count; ) {
        if (strcasecmp(sl->names[i], oldName) != 0) { i++; continue; }
        changed = true;
        if (newName) {
            strncpy(sl->names[i], newName, SETLIST_SONG_NAME_LEN - 1);
            sl->names[i][SETLIST_SONG_NAME_LEN - 1] = '\0';
            i++;
        } else {
            setlistRemoveAt(sl, i);
        }
    }
    return changed;
}

// Floor division that rounds toward negative infinity (offsets may be negative).
static int floorDiv(int a, int b) {
    int q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

SetlistPage::SetlistPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _keyboard(display, touch)
    , _dialog(display, touch)
    , _msView(display)
    , _pickView(display)
    , _state(State::LIST)
    , _wasDown(false)
    , _slot(1)
    , _scrollOffset(0)
    , _dragStartY(0)
    , _dragStartScrollOffset(0)
    , _dragMoved(false)
    , _selectedIdx(-1)
    , _selectedMissing(false)
    , _moveSrcIdx(-1)
    , _draftIsNew(false)
    , _editIdx(-1)
    , _kbdTarget(nullptr)
    , _msEditMode(false)
    , _srvPickCount(0)
    , _srvPickReqPage(0)
    , _srvPickLoaded(false)
    , _srvListDrawn(false)
    , _srvLoadStartMs(0)
{
    initSetlist(&_list);
    memset(&_draft, 0, sizeof(_draft));
    _kbdSnap[0]           = '\0';
    _loadedFilename[0]    = '\0';
    _loadedDisplayName[0] = '\0';
    _currentLoadedFile[0] = '\0';

    // Both picker lists share the same band geometry; only their painter/tap
    // differ.  step = row height → line-by-line snap (one repaint per line).
    _msView.configure(0, SL_PICK_Y0, 960, SL_PICK_VIEW_H,
        [this](int x, int y, int w, int h, int off) { return paintMasterBand(x, y, w, h, off); },
        [this](int tx, int ty) { tapMaster(tx, ty); });
    _msView.setStep(SL_PICK_ROW_H);

    _pickView.configure(0, SL_PICK_Y0, 960, SL_PICK_VIEW_H,
        [this](int x, int y, int w, int h, int off) { return paintPickBand(x, y, w, h, off); },
        [this](int tx, int ty) { tapPick(tx, ty); });
    _pickView.setStep(SL_PICK_ROW_H);
}

const MasterEntry* SetlistPage::resolveRow(int setlistIdx) const {
    if (!sMaster) return nullptr;
    if (setlistIdx < 0 || setlistIdx >= (int)_list.count) return nullptr;
    int m = masterFindByName(sMaster, _list.names[setlistIdx]);
    return (m >= 0) ? &sMaster->entries[m] : nullptr;
}

void SetlistPage::open() {
    _wasDown      = _touch.isTouched;
    _scrollOffset = 0;
    _dragMoved    = false;
    _moveSrcIdx   = -1;
    _srvLoadLate  = false;      // a load from a previous visit is no longer ours
    _srvSongCache = SongCache::NONE;    // songs may have changed since last visit
    _srvSongCount = 0;
    _dupNameNote  = false;
    _noMemory     = false;

    if (!gServerPairing.isPaired()) {
        _state = State::NOT_CONNECTED;
        return;
    }
    if (!ensureMaster()) {
        _noMemory = true;
        _state    = State::NOT_CONNECTED;
        return;
    }

    loadMasterFromServer();             // fills sMaster (empty on miss)

    _state = State::LIST;
    if (!loadSlotFromServer(_slot, &_list)) initSetlist(&_list);
    _loadedFilename[0] = '\0';

    if (_currentLoadedFile[0]) {
        for (int i = 0; i < (int)_list.count; i++) {
            const MasterEntry* m = resolveRow(i);
            if (m && strcmp(m->file, _currentLoadedFile) == 0) { ensureRowVisible(i); break; }
        }
    }
}

void SetlistPage::draw() {
    _d.fillScreen(COL_WHITE);
    switch (_state) {
        case State::NOT_CONNECTED: drawNotConnected(); break;
        case State::LIST:          drawList(); drawLoadWarning(); break;
        case State::INFO:          drawList(); drawInfo(); break;
        case State::WAITING_SONG:  drawWaitingSong(); break;
        case State::MASTER_SELECT: drawMasterChrome(); _msView.redraw(); break;
        case State::EDIT:          drawEdit(); break;
        case State::KBD_NAME:
        case State::KBD_NOTES:     _keyboard.draw(); break;
        case State::PICK_FILE_SRV: drawPickChrome("PICK SONG (SERVER)"); _pickView.redraw(); break;
        case State::CONFIRM_DELETE: drawEdit(); _dialog.draw(); break;
    }
}

// A list that failed to load looks exactly like an empty one — say so, and
// that nothing done here will be saved (see _listLoaded).
void SetlistPage::drawLoadWarning() {
    if (_listLoaded && _masterLoaded) return;
    const char* msg = "COULD NOT LOAD FROM SERVER - CHANGES NOT SAVED";
    _d.fillRect(0, SL_HDR_H, 960, 34, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor((960 - (int)strlen(msg) * 18) / 2, SL_HDR_H + 5);
    _d.print(msg);
}

SetlistResult SetlistPage::poll() {
    // A load that outlived the WAITING_SONG timeout is still in flight — the
    // receive state was deliberately left intact.  Land it from wherever the
    // user is now on this page.
    if (_srvLoadLate && _state != State::WAITING_SONG &&
        gServerPairing.browseState() == BrowseState::SONG_READY) {
        _srvLoadLate = false;
        const bool retriedBefore = _pendingRetried;
        SetlistResult r = takeReadySong();
        if (r == SetlistResult::SONG_LOADED) {
            Serial.println("[SetlistPage] late server load landed");
            return r;
        }
        // The wrong song landed and the right one was asked for again — that
        // one is in flight now and still has to be landed from here.
        if (_pendingRetried && !retriedBefore) _srvLoadLate = true;
    }
    // The server answered the load with a failure.  It kept its previous song
    // and so do we — drop straight back to the INFO popup with the reason
    // instead of sitting out the slow-link timeout.
    if (gServerPairing.loadFailPending()) {
        bool waiting = (_state == State::WAITING_SONG);
        if (waiting || _state == State::INFO) {
            snprintf(_loadFailNote, sizeof(_loadFailNote), "   (LOAD FAILED - %s)",
                     gServerPairing.loadFailText());
        }
        gServerPairing.clearLoadFail();
        _srvLoadLate = false;
        if (waiting) {
            Serial.println("[SetlistPage] server load failed — back to list");
            _state   = State::INFO;
            _wasDown = _touch.isTouched;
            _d.fillScreen(COL_WHITE);
            drawList();
            drawInfo();
            _d.paint();
            return SetlistResult::NONE;
        }
        if (_state == State::INFO) {
            drawInfo();
            _d.paint();
        }
    }
    switch (_state) {
        case State::NOT_CONNECTED: return pollNotConnected();
        case State::LIST:          return pollList();
        case State::INFO:          return pollInfo();
        case State::WAITING_SONG:  return pollWaitingSong();
        case State::MASTER_SELECT: return pollMasterSelect();
        case State::EDIT:          (void)pollEdit(); return SetlistResult::NONE;

        case State::KBD_NAME:
        case State::KBD_NOTES:
            if (_keyboard.poll()) {
                size_t cap = (_state == State::KBD_NOTES) ? SETLIST_NOTES_LEN : SETLIST_SONG_NAME_LEN;
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

        case State::PICK_FILE_SRV: return pollPickSrv();

        case State::CONFIRM_DELETE:
            if (_dialog.poll()) {
                if (_dialog.confirmed() && _editIdx >= 0 && _editIdx < (int)sMaster->count) {
                    char gone[SETLIST_SONG_NAME_LEN];
                    strncpy(gone, sMaster->entries[_editIdx].name, sizeof(gone) - 1);
                    gone[sizeof(gone) - 1] = '\0';
                    for (int i = _editIdx; i + 1 < (int)sMaster->count; i++)
                        sMaster->entries[i] = sMaster->entries[i + 1];
                    sMaster->count--;
                    saveMaster();
                    // (A hand-edited catalog can hold the name twice; the
                    // references then still resolve, so leave them.)
                    if (masterFindByName(sMaster, gone) < 0)
                        propagateNameChange(gone, nullptr);
                    openMasterSelect();          // entry is gone — back to the list
                } else {
                    _state = State::EDIT;        // cancelled — keep editing
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

// ── NOT_CONNECTED ────────────────────────────────────────────────────────────

void SetlistPage::drawNotConnected() {
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    const char* l1 = _noMemory ? "Out of memory." : "Not connected.";
    const char* l2 = _noMemory ? "Could not allocate the setlist catalog."
                               : "Pair to a server to use setlists.";
    _d.setCursor((960 - (int)strlen(l1) * 18) / 2, 210);
    _d.print(l1);
    _d.setCursor((960 - (int)strlen(l2) * 18) / 2, 260);
    _d.print(l2);
    uiButton(_d, SL_BACK_X, SL_BAR_Y, SL_BACK_W, SL_BAR_H, "BACK", COL_BLACK, COL_WHITE, 3);
}

SetlistResult SetlistPage::pollNotConnected() {
    if (!_touch.read()) return SetlistResult::NONE;
    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);
    if (!down && _wasDown) {
        _wasDown = false;
        if (sx >= SL_BACK_X && sx < SL_BACK_X + SL_BACK_W && sy >= SL_BAR_Y)
            return SetlistResult::BACK;
    } else if (down && !_wasDown) {
        _wasDown = true;
    }
    return SetlistResult::NONE;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── LIST view ────────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::drawList() {
    drawListHeader();
    drawListTabs();
    drawListRows();
    drawListBottomBar();
}

void SetlistPage::drawListHeader() {
    _d.fillRect(0, 0, 960, SL_HDR_H, COL_BLACK);
    uiButton(_d, SL_BACK_X, SL_BTN_Y, SL_BACK_W, SL_BTN_H, "BACK", COL_BLACK, COL_WHITE, 3);

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
                 active ? COL_BLACK : COL_WHITE, active ? COL_WHITE : COL_BLACK, 3);
        if (!active) _d.drawRect(x, SL_TAB_Y, SL_TAB_W, SL_TAB_H, COL_BLACK);
    }
}

void SetlistPage::drawListRows() {
    _d.fillRect(0, SL_LIST_Y, 960, SL_ROWS_PER_PAGE * SL_ROW_H, COL_WHITE);
    for (int i = 0; i < SL_ROWS_PER_PAGE; i++) drawListRow(i);
}

void SetlistPage::drawListRow(int rowOnPage) {
    int y   = SL_LIST_Y + rowOnPage * SL_ROW_H;
    int abs = _scrollOffset + rowOnPage;
    bool has = (abs >= 0 && abs < (int)_list.count);

    _d.fillRect(0, y, 960, SL_ROW_H, COL_WHITE);
    _d.drawFastHLine(20, y + SL_ROW_H - 1, 920, COL_LTGREY);
    if (!has) return;

    const MasterEntry* m = resolveRow(abs);
    bool missing  = (m == nullptr);
    const char* file = m ? m->file : "";
    bool isCurrent = _currentLoadedFile[0] && file[0] && strcmp(file, _currentLoadedFile) == 0;
    if (isCurrent) _d.fillRect(0, y + 2, 6, SL_ROW_H - 4, COL_BLACK);

    char numStr[6];
    snprintf(numStr, sizeof(numStr), "%d.", abs + 1);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SL_NUM_X + 4, y + (SL_ROW_H - 24) / 2);
    _d.print(numStr);

    char nameBuf[SETLIST_SONG_NAME_LEN + 8];
    snprintf(nameBuf, sizeof(nameBuf), "%s%s", _list.names[abs], missing ? "  (?)" : "");
    char trunc[SETLIST_SONG_NAME_LEN + 8];
    int maxChars = SL_NAME_W / 18;
    if (maxChars > (int)sizeof(trunc) - 1) maxChars = (int)sizeof(trunc) - 1;
    if ((int)strlen(nameBuf) > maxChars) {
        strncpy(trunc, nameBuf, maxChars);
        trunc[maxChars] = '\0';
    } else {
        strncpy(trunc, nameBuf, sizeof(trunc) - 1);
        trunc[sizeof(trunc) - 1] = '\0';
    }
    _d.setTextColor(missing ? COL_DKGREY : COL_BLACK);
    _d.setCursor(SL_NAME_X + 8, y + (SL_ROW_H - 24) / 2);
    _d.print(trunc);

    int btnY = y + (SL_ROW_H - SL_MOVE_H) / 2;
    if (_moveSrcIdx < 0) {
        uiButton(_d, SL_MOVE_X, btnY, SL_MOVE_W, SL_MOVE_H, "Move", COL_LTGREY, COL_BLACK, 3);
    } else if (abs == _moveSrcIdx) {
        uiButton(_d, SL_MOVE_X, btnY, SL_MOVE_W, SL_MOVE_H, "Move", COL_BLACK, COL_WHITE, 3);
    } else {
        uiButton(_d, SL_MOVE_X, btnY, SL_MOVE_W, SL_MOVE_H, "After", COL_LTGREY, COL_BLACK, 3);
    }
}

void SetlistPage::drawListBottomBar() {
    _d.fillRect(0, SL_BAR_Y, 960, SL_BAR_H, COL_WHITE);
    _d.drawFastHLine(0, SL_BAR_Y, 960, COL_BLACK);
    char status[48];
    int total = (int)_list.count;
    if (total == 0) {
        snprintf(status, sizeof(status), "(empty)  -  ADD songs from the master list");
    } else {
        int firstVis = _scrollOffset + 1;
        int lastVis  = _scrollOffset + SL_ROWS_PER_PAGE;
        if (lastVis > total) lastVis = total;
        snprintf(status, sizeof(status), "%d-%d of %d  (drag to scroll)", firstVis, lastVis, total);
    }
    int tw = (int)strlen(status) * 18;
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor((960 - tw) / 2, SL_BAR_Y + (SL_BAR_H - 24) / 2);
    _d.print(status);
}

int SetlistPage::maxScrollOffset() const {
    int max = (int)_list.count - SL_ROWS_PER_PAGE;
    return max > 0 ? max : 0;
}

void SetlistPage::ensureRowVisible(int idx) {
    if (idx < 0) return;
    if (idx < _scrollOffset) _scrollOffset = idx;
    else if (idx >= _scrollOffset + SL_ROWS_PER_PAGE) _scrollOffset = idx - SL_ROWS_PER_PAGE + 1;
    int max = maxScrollOffset();
    if (_scrollOffset > max) _scrollOffset = max;
    if (_scrollOffset < 0)   _scrollOffset = 0;
}

SetlistResult SetlistPage::pollList() {
    if (!_touch.read()) return SetlistResult::NONE;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (down && !_wasDown) {
        _wasDown               = true;
        _dragStartY            = sy;
        _dragStartScrollOffset = _scrollOffset;
        _dragMoved             = false;
        return SetlistResult::NONE;
    }

    if (down && _wasDown) {
        int dy = sy - _dragStartY;
        if (!_dragMoved && (dy >= SL_DRAG_THRESH_PX || dy <= -SL_DRAG_THRESH_PX)) _dragMoved = true;
        if (_dragMoved) {
            int rowDelta = -dy / SL_ROW_H;
            int newOff   = _dragStartScrollOffset + rowDelta;
            int maxOff   = maxScrollOffset();
            if (newOff < 0)      newOff = 0;
            if (newOff > maxOff) newOff = maxOff;
            if (newOff != _scrollOffset) {
                _scrollOffset = newOff;
                drawListRows();
                drawListBottomBar();
                _d.paintLater();
            }
        }
        return SetlistResult::NONE;
    }

    if (!down && _wasDown) {
        _wasDown = false;
        if (_dragMoved) { _dragMoved = false; return SetlistResult::NONE; }
    } else {
        return SetlistResult::NONE;
    }

    // ── Tap ──
    if (hitBack(sx, sy)) { save(); return SetlistResult::BACK; }

    int tab = hitTab(sx, sy);
    if (tab > 0) {
        if ((uint8_t)tab != _slot) switchToSlot((uint8_t)tab);
        _d.fillScreen(COL_WHITE);
        drawList();
        _d.paint();
        return SetlistResult::NONE;
    }

    if (hitAdd(sx, sy)) {
        _moveSrcIdx = -1;
        if (_list.count < SETLIST_MAX_SONGS) openMasterSelect();
        return SetlistResult::NONE;
    }

    int mvIdx = hitRowMoveBtn(sx, sy);
    if (mvIdx >= 0) {
        if (_moveSrcIdx < 0)            _moveSrcIdx = mvIdx;
        else if (mvIdx == _moveSrcIdx)  _moveSrcIdx = -1;
        else { setlistMoveAfter(&_list, _moveSrcIdx, mvIdx); save(); ensureRowVisible(mvIdx); _moveSrcIdx = -1; }
        drawListRows();
        drawListBottomBar();
        _d.paintLater();
        return SetlistResult::NONE;
    }

    int nameIdx = hitRowName(sx, sy);
    if (nameIdx >= 0) {
        if (_moveSrcIdx >= 0) return SetlistResult::NONE;
        _selectedIdx = nameIdx;
        const MasterEntry* m = resolveRow(nameIdx);
        // Answered from the per-visit song-name cache (see fileExistsOnServer).
        // A file that really is missing is in any case still reported, by the
        // server's LOAD FAILED reply.
        _selectedMissing = (m == nullptr) ||
                           (m->file[0] != '\0' && !fileExistsOnServer(m->file));
        _loadFailNote[0] = '\0';
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
    int abs = _scrollOffset + (sy - SL_LIST_Y) / SL_ROW_H;
    if (abs < 0 || abs >= (int)_list.count) return -1;
    return abs;
}

int SetlistPage::hitRowMoveBtn(int sx, int sy) const {
    if (sx < SL_MOVE_X || sx >= SL_MOVE_X + SL_MOVE_W) return -1;
    if (sy < SL_LIST_Y || sy >= SL_LIST_Y + SL_ROWS_PER_PAGE * SL_ROW_H) return -1;
    int abs = _scrollOffset + (sy - SL_LIST_Y) / SL_ROW_H;
    if (abs < 0 || abs >= (int)_list.count) return -1;
    return abs;
}

bool SetlistPage::hitBack(int sx, int sy) const {
    return sx >= SL_BACK_X && sx < SL_BACK_X + SL_BACK_W && sy >= SL_BTN_Y && sy < SL_BTN_Y + SL_BTN_H;
}
bool SetlistPage::hitAdd(int sx, int sy) const {
    return sx >= SL_ADD_X && sx < SL_ADD_X + SL_ADD_W && sy >= SL_BTN_Y && sy < SL_BTN_Y + SL_BTN_H;
}

void SetlistPage::switchToSlot(uint8_t newSlot) {
    save();
    _slot         = newSlot;
    _scrollOffset = 0;
    _dragMoved    = false;
    _moveSrcIdx   = -1;
    if (!loadSlotFromServer(_slot, &_list)) initSetlist(&_list);
    if (_currentLoadedFile[0]) {
        for (int i = 0; i < (int)_list.count; i++) {
            const MasterEntry* m = resolveRow(i);
            if (m && strcmp(m->file, _currentLoadedFile) == 0) { ensureRowVisible(i); break; }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ── INFO popup ───────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::drawInfo() {
    _d.fillRect(SL_INFO_X, SL_INFO_Y, SL_INFO_W, SL_INFO_H, COL_WHITE);
    _d.drawRect(SL_INFO_X, SL_INFO_Y, SL_INFO_W, SL_INFO_H, COL_BLACK);
    _d.drawRect(SL_INFO_X + 1, SL_INFO_Y + 1, SL_INFO_W - 2, SL_INFO_H - 2, COL_BLACK);

    if (_selectedIdx < 0 || _selectedIdx >= (int)_list.count) return;
    const char* name = _list.names[_selectedIdx];
    const MasterEntry* m = resolveRow(_selectedIdx);

    int ty = SL_INFO_Y + 16;
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SL_INFO_X + 20, ty);
    char title[40];
    int maxNameChars = (SL_INFO_W - 40) / 18;
    int cap = (maxNameChars < (int)sizeof(title)) ? maxNameChars : (int)sizeof(title) - 1;
    strncpy(title, name, cap);
    title[cap] = '\0';
    _d.print(title);

    ty += 36;
    _d.setTextSize(2);
    char src[80];
    if (!m) {
        snprintf(src, sizeof(src), "Not in master list");
    } else if (m->file[0] == '\0') {
        snprintf(src, sizeof(src), "Source: (none)");
    } else {
        snprintf(src, sizeof(src), "Source: %s  [server]%s%s", m->file,
                 _selectedMissing ? "   (FILE MISSING)" : "", _loadFailNote);
    }
    _d.setCursor(SL_INFO_X + 20, ty);
    _d.print(src);

    ty += 28;
    _d.drawFastHLine(SL_INFO_X + 20, ty, SL_INFO_W - 40, COL_LTGREY);
    ty += 12;

    // Notes are drawn larger (size 3) — this is the cue the performer reads at
    // a glance just before loading, so it needs to be legible from arm's length.
    _d.setTextSize(3);
    const int wrapW = SL_INFO_W - 40;
    const int perLine = wrapW / 18;     // ~18 px per char at size 3
    const int maxLines = 4;
    const char* p = m ? m->notes : "";
    int lineY = ty + 4;
    for (int line = 0; line < maxLines && *p; line++) {
        int rem = (int)strlen(p);
        int take = (rem > perLine) ? perLine : rem;
        if (take < rem) {
            int br = take;
            for (int i = take; i > take / 2; i--) { if (p[i] == ' ') { br = i; break; } }
            take = br;
        }
        char buf[64];
        if (take > (int)sizeof(buf) - 1) take = sizeof(buf) - 1;
        memcpy(buf, p, take);
        buf[take] = '\0';
        _d.setCursor(SL_INFO_X + 20, lineY);
        _d.print(buf);
        lineY += 30;
        p += take;
        while (*p == ' ') p++;
    }

    // OK is enabled for a loadable song (file present, found on server) OR for
    // a no-file entry (title-only — e.g. a break or a manually-played song).
    bool hasFile = m && (m->file[0] != '\0');
    bool canOk   = m && (!hasFile || !_selectedMissing);
    uiButton(_d, SL_INFO_OK_X, SL_INFO_BTN_Y, SL_INFO_OK_W, SL_INFO_BTN_H,
             hasFile ? "OK - LOAD" : "OK", COL_BLACK, canOk ? COL_WHITE : COL_DKGREY, 3);
    uiButton(_d, SL_INFO_REM_X, SL_INFO_BTN_Y, SL_INFO_REM_W, SL_INFO_BTN_H,
             "REMOVE", COL_LTGREY, COL_BLACK, 3);
    uiButton(_d, SL_INFO_CAN_X, SL_INFO_BTN_Y, SL_INFO_CAN_W, SL_INFO_BTN_H,
             "CANCEL", COL_LTGREY, COL_BLACK, 3);
}

bool SetlistPage::hitInfoOk(int sx, int sy) const {
    return sx >= SL_INFO_OK_X && sx < SL_INFO_OK_X + SL_INFO_OK_W
        && sy >= SL_INFO_BTN_Y && sy < SL_INFO_BTN_Y + SL_INFO_BTN_H;
}
bool SetlistPage::hitInfoRemove(int sx, int sy) const {
    return sx >= SL_INFO_REM_X && sx < SL_INFO_REM_X + SL_INFO_REM_W
        && sy >= SL_INFO_BTN_Y && sy < SL_INFO_BTN_Y + SL_INFO_BTN_H;
}
bool SetlistPage::hitInfoCancel(int sx, int sy) const {
    return sx >= SL_INFO_CAN_X && sx < SL_INFO_CAN_X + SL_INFO_CAN_W
        && sy >= SL_INFO_BTN_Y && sy < SL_INFO_BTN_Y + SL_INFO_BTN_H;
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

    if (hitInfoRemove(sx, sy)) {
        if (_selectedIdx >= 0 && _selectedIdx < (int)_list.count) {
            setlistRemoveAt(&_list, _selectedIdx);
            save();
            int max = maxScrollOffset();
            if (_scrollOffset > max) _scrollOffset = max;
        }
        _selectedIdx = -1;
        _state = State::LIST;
        _d.fillScreen(COL_WHITE);
        drawList();
        _d.paint();
        return SetlistResult::NONE;
    }

    if (hitInfoOk(sx, sy)) {
        const MasterEntry* m = resolveRow(_selectedIdx);
        if (!m) return SetlistResult::NONE;                // name not in catalog — greyed

        // No file attached → title-only (manual-play song / break): return to
        // Performance with just the title.  The caller drops to OFF / No Song
        // for this result — it sends sendSetOff() and clears the local song.
        if (m->file[0] == '\0') {
            _loadedFilename[0] = '\0';
            strncpy(_loadedDisplayName, _list.names[_selectedIdx], sizeof(_loadedDisplayName) - 1);
            _loadedDisplayName[sizeof(_loadedDisplayName) - 1] = '\0';
            return SetlistResult::TITLE_ONLY;
        }
        if (_selectedMissing) return SetlistResult::NONE;  // file missing — greyed

        char bare[SETLIST_FILE_LEN];
        strncpy(bare, m->file, sizeof(bare) - 1);
        bare[sizeof(bare) - 1] = '\0';
        int blen = (int)strlen(bare);
        if (blen > 4 && bare[blen - 4] == '.' &&
            (bare[blen - 3] == 'm' || bare[blen - 3] == 'M')) bare[blen - 4] = '\0';

        strncpy(_pendingFile, m->file, sizeof(_pendingFile) - 1);
        _pendingFile[sizeof(_pendingFile) - 1] = '\0';
        strncpy(_pendingName, _list.names[_selectedIdx], sizeof(_pendingName) - 1);
        _pendingName[sizeof(_pendingName) - 1] = '\0';

        gServerPairing.resetBrowse();
        _srvLoadStartMs = millis();
        _srvLoadLate    = false;
        _pendingRetried = false;
        _loadFailNote[0] = '\0';
        gServerPairing.requestSongLoadByName(bare);
        _state = State::WAITING_SONG;
        _d.fillScreen(COL_WHITE);
        drawWaitingSong();
        _d.paint();
    }
    return SetlistResult::NONE;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── WAITING_SONG ─────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::drawWaitingSong() {
    _d.fillRect(0, 0, 960, SL_HDR_H, COL_BLACK);
    const char* title = "LOADING FROM SERVER";
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor((960 - (int)strlen(title) * 18) / 2, (SL_HDR_H - 24) / 2);
    _d.print(title);

    const MasterEntry* m = resolveRow(_selectedIdx);
    char msg[64];
    snprintf(msg, sizeof(msg), "Loading '%s' ...", m ? m->file : "");
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor((960 - (int)strlen(msg) * 18) / 2, 240);
    _d.print(msg);
}

SetlistResult SetlistPage::takeReadySong() {
    // The song that landed is not necessarily the one this page asked for:
    // the pull of the previously active song (sent on a reconnect) can arrive
    // ahead of the reply to the load request.  Compare file stems.
    const char* pushed = gServerPairing.pushedSongFile();   // "" = unknown
    char got[32];
    strncpy(got, pushed ? pushed : "", sizeof(got) - 1);
    got[sizeof(got) - 1] = '\0';
    stripMgt(got);
    char want[SETLIST_FILE_LEN];
    strncpy(want, _pendingFile, sizeof(want) - 1);
    want[sizeof(want) - 1] = '\0';
    stripMgt(want);
    const bool other = (got[0] != '\0' && strcasecmp(got, want) != 0);

    if (other && !_pendingRetried) {
        // Not ours — leave it, ask again (once) and keep waiting.
        _pendingRetried = true;
        Serial.printf("[SetlistPage] '%s' landed, wanted '%s' — asking again\n", got, want);
        gServerPairing.resetBrowse();
        _srvLoadStartMs = millis();
        gServerPairing.requestSongLoadByName(want);
        return SetlistResult::NONE;
    }

    if (gServerPairing.copySong(&_song)) {
        // Name the song from what was requested, not from the current
        // selection — a late load may land after the selection has moved.
        const char* file = _pendingFile;
        const char* name = _pendingName;
        if (other) {
            // Still not the requested song after asking twice: take it, but
            // name it as what it is — its catalog entry if it has one.
            Serial.printf("[SetlistPage] '%s' landed again, wanted '%s' — taking it as itself\n",
                          got, want);
            file = got;
            name = got;
            for (int i = 0; sMaster && i < (int)sMaster->count; i++) {
                char f[SETLIST_FILE_LEN];
                strncpy(f, sMaster->entries[i].file, sizeof(f) - 1);
                f[sizeof(f) - 1] = '\0';
                stripMgt(f);
                if (f[0] != '\0' && strcasecmp(f, got) == 0) {
                    file = sMaster->entries[i].file;
                    name = sMaster->entries[i].name;
                    break;
                }
            }
        }
        strncpy(_loadedFilename, file, sizeof(_loadedFilename) - 1);
        _loadedFilename[sizeof(_loadedFilename) - 1] = '\0';
        strncpy(_currentLoadedFile, file, sizeof(_currentLoadedFile) - 1);
        _currentLoadedFile[sizeof(_currentLoadedFile) - 1] = '\0';
        strncpy(_loadedDisplayName, name, sizeof(_loadedDisplayName) - 1);
        _loadedDisplayName[sizeof(_loadedDisplayName) - 1] = '\0';
        gServerPairing.resetBrowse();
        return SetlistResult::SONG_LOADED;
    }
    gServerPairing.resetBrowse();
    return SetlistResult::NONE;
}

SetlistResult SetlistPage::pollWaitingSong() {
    BrowseState bs = gServerPairing.browseState();
    if (bs == BrowseState::SONG_READY) {
        SetlistResult r = takeReadySong();
        if (r == SetlistResult::SONG_LOADED) return r;
    }
    if (millis() - _srvLoadStartMs > 8000) {
        // Slow, not dead: leave the receive state alone so the stream can
        // still complete (resetBrowse() here would zero the byte count under
        // it and the song could never finish).  poll() lands it later.
        Serial.println("[SetlistPage] server load slow — back to list, stream kept");
        _srvLoadLate = true;
        _state = State::INFO;
        _wasDown = _touch.isTouched;
        _d.fillScreen(COL_WHITE);
        drawList();
        drawInfo();
        _d.paint();
    }
    return SetlistResult::NONE;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── MASTER_SELECT (catalog picker — ScrollViewport) ──────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::openMasterSelect() {
    _msEditMode = false;
    _msView.reset();
    _wasDown = _touch.isTouched;
    _state = State::MASTER_SELECT;
    _d.fillScreen(COL_WHITE);
    drawMasterChrome();
    _msView.redraw();
}

void SetlistPage::drawMasterChrome() {
    drawMasterHeader();
    drawMasterHint();
}

void SetlistPage::drawMasterHeader() {
    _d.fillRect(0, 0, 960, SL_HDR_H, COL_BLACK);
    uiButton(_d, SL_BACK_X, SL_BTN_Y, SL_BACK_W, SL_BTN_H, "BACK", COL_BLACK, COL_WHITE, 3);

    const char* title = "MASTER LIST";
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(SL_BACK_W + 30, (SL_HDR_H - 24) / 2);
    _d.print(title);

    uiButton(_d, SL_MEDIT_X, SL_BTN_Y, SL_MEDIT_W, SL_BTN_H, "EDIT",
             _msEditMode ? COL_WHITE : COL_BLACK, _msEditMode ? COL_BLACK : COL_WHITE, 3);
    bool full = (sMaster->count >= MASTER_MAX_ENTRIES);
    uiButton(_d, SL_ADD_X, SL_BTN_Y, SL_ADD_W, SL_BTN_H, "NEW",
             COL_BLACK, full ? COL_DKGREY : COL_WHITE, 3);
}

void SetlistPage::drawMasterHint() {
    _d.fillRect(0, SL_BAR_Y, 960, SL_BAR_H, COL_WHITE);
    _d.drawFastHLine(0, SL_BAR_Y, 960, COL_BLACK);
    char status[64];
    if (_msEditMode)
        snprintf(status, sizeof(status), "EDIT MODE: tap a song to edit it");
    else
        snprintf(status, sizeof(status), "Tap to add / remove   (drag to scroll)");
    int tw = (int)strlen(status) * 18;
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor((960 - tw) / 2, SL_BAR_Y + (SL_BAR_H - 24) / 2);
    _d.print(status);
}

bool SetlistPage::paintMasterBand(int x, int y, int w, int h, int offsetY) {
    _d.fillRect(x, y, w, h, COL_WHITE);
    int rowH = SL_PICK_ROW_H;
    int count = (int)sMaster->count;

    if (count == 0) {
        _d.setTextSize(3);
        _d.setTextColor(COL_DKGREY);
        const char* msg = "Master list empty - NEW to add a song";
        _d.setCursor((960 - (int)strlen(msg) * 18) / 2, y + 60);
        _d.print(msg);
        return true;   // nothing to scroll
    }

    int first = floorDiv(offsetY, rowH);
    for (int idx = first; idx < count + 1; idx++) {
        int py = y + (idx * rowH - offsetY);
        if (py >= y + h) break;
        if (idx >= 0 && idx < count) drawMasterRow(idx, py, rowH);
    }

    int maxOff = count * rowH - h;
    if (maxOff < 0) maxOff = 0;
    return (offsetY <= 0) || (offsetY >= maxOff);
}

void SetlistPage::drawMasterRow(int idx, int py, int rowH) {
    _d.drawFastHLine(20, py + rowH - 1, 920, COL_LTGREY);
    const MasterEntry& e = sMaster->entries[idx];

    int bs = 32, bx = 20, by = py + (rowH - bs) / 2;
    int pos = setlistFind(&_list, e.name);
    _d.drawRect(bx, by, bs, bs, COL_BLACK);
    if (pos >= 0) {
        _d.fillRect(bx, by, bs, bs, COL_BLACK);
        char num[6];
        snprintf(num, sizeof(num), "%d", pos + 1);
        _d.setTextSize(2);
        _d.setTextColor(COL_WHITE);
        int nw = (int)strlen(num) * 12;
        _d.setCursor(bx + (bs - nw) / 2, by + (bs - 16) / 2);
        _d.print(num);
    }

    int tx = bx + bs + 15;
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int maxChars = (960 - tx - 20) / 18;
    char trunc[SETLIST_SONG_NAME_LEN + 4];
    int n = (int)strlen(e.name);
    if (n > maxChars) n = maxChars;
    if (n > (int)sizeof(trunc) - 1) n = sizeof(trunc) - 1;
    memcpy(trunc, e.name, n);
    trunc[n] = '\0';
    _d.setCursor(tx, py + (rowH - 24) / 2);
    _d.print(trunc);
}

void SetlistPage::tapMaster(int tx, int ty) {
    (void)tx;
    if (ty < 0) return;
    int idx = ty / SL_PICK_ROW_H;
    if (idx < 0 || idx >= (int)sMaster->count) return;

    if (_msEditMode) {
        _editIdx    = idx;
        _draft      = sMaster->entries[idx];
        _draftIsNew = false;
        _dupNameNote = false;
        _state = State::EDIT;
        _wasDown = _touch.isTouched;
        _d.fillScreen(COL_WHITE);
        drawEdit();
        _d.paint();
        return;
    }

    const char* name = sMaster->entries[idx].name;
    int p = setlistFind(&_list, name);
    if (p >= 0) {
        setlistRemoveAt(&_list, p);
    } else {
        if (!setlistAppend(&_list, name)) return;   // setlist full
    }
    save();
    _msView.redraw();   // refresh the membership markers
}

SetlistResult SetlistPage::pollMasterSelect() {
    _msView.tick();
    if (!_touch.read()) return SetlistResult::NONE;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // Body drag / tap (the viewport ignores presses that start outside its band).
    _msView.poll(down, sx, sy);

    // Header buttons on the falling edge (disjoint from the scroll band).
    if (!down && _wasDown) {
        _wasDown = false;
        if (_state != State::MASTER_SELECT) return SetlistResult::NONE;  // a tap opened the editor
        if (hitBack(sx, sy)) {
            _state = State::LIST;
            _d.fillScreen(COL_WHITE);
            drawList();
            _d.paint();
            return SetlistResult::NONE;
        }
        if (hitMasterEdit(sx, sy)) {
            _msEditMode = !_msEditMode;
            drawMasterHeader();
            drawMasterHint();
            _d.paintLater();
            return SetlistResult::NONE;
        }
        if (hitMasterNew(sx, sy)) {
            if (sMaster->count < MASTER_MAX_ENTRIES) {
                memset(&_draft, 0, sizeof(_draft));
                _draftIsNew = true;
                _editIdx    = -1;
                _dupNameNote = false;
                _state = State::EDIT;
                _wasDown = _touch.isTouched;
                _d.fillScreen(COL_WHITE);
                drawEdit();
                _d.paint();
            }
            return SetlistResult::NONE;
        }
    } else if (down && !_wasDown) {
        _wasDown = true;
    }
    return SetlistResult::NONE;
}

bool SetlistPage::hitMasterEdit(int sx, int sy) const {
    return sx >= SL_MEDIT_X && sx < SL_MEDIT_X + SL_MEDIT_W && sy >= SL_BTN_Y && sy < SL_BTN_Y + SL_BTN_H;
}
bool SetlistPage::hitMasterNew(int sx, int sy) const {
    return sx >= SL_ADD_X && sx < SL_ADD_X + SL_ADD_W && sy >= SL_BTN_Y && sy < SL_BTN_Y + SL_BTN_H;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── EDIT (catalog entry) ─────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::drawEdit() {
    _d.fillRect(0, 0, 960, SL_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    const char* title = _draftIsNew ? "NEW MASTER ENTRY" : "EDIT MASTER ENTRY";
    _d.setCursor((960 - (int)strlen(title) * 18) / 2, (SL_HDR_H - 24) / 2);
    _d.print(title);

    drawEditFieldRow(SL_ED_NAME_Y, "Song Name:", _draft.name, SL_ED_FIELD_VAL_W, "EDIT");
    drawEditFileRow();
    drawEditNotesRow();
    drawEditNote();
    drawEditActionBar();
}

// Same black strip as drawLoadWarning(), in the gap between the notes box and
// the action bar.
void SetlistPage::drawEditNote() {
    const int y = SL_ED_NOTES_Y + SL_ED_NOTES_H + 20;
    if (!_dupNameNote) {
        _d.fillRect(0, y, 960, 34, COL_WHITE);
        return;
    }
    const char* msg = "NAME ALREADY IN MASTER LIST";
    _d.fillRect(0, y, 960, 34, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor((960 - (int)strlen(msg) * 18) / 2, y + 5);
    _d.print(msg);
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

    uiButton(_d, SL_ED_FILE_PICK_X, y, SL_ED_FILE_PICK_W, SL_ED_FIELD_BTN_H, "PICK", COL_LTGREY, COL_BLACK, 3);
    bool canClear = (_draft.file[0] != '\0');
    uiButton(_d, SL_ED_FILE_CLR_X, y, SL_ED_FILE_CLR_W, SL_ED_FIELD_BTN_H, "CLEAR",
             COL_LTGREY, canClear ? COL_BLACK : COL_DKGREY, 3);
}

void SetlistPage::drawEditFieldRow(int y, const char* label, const char* value, int valW, const char* btnLabel) {
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
    uiButton(_d, SL_ED_FIELD_BTN_X, y, SL_ED_FIELD_BTN_W, SL_ED_FIELD_BTN_H, btnLabel, COL_LTGREY, COL_BLACK, 3);
}

void SetlistPage::drawEditNotesRow() {
    int y = SL_ED_NOTES_Y;
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SL_ED_FIELD_LBL_X, y + 8);
    _d.print("Notes:");

    int boxX = SL_ED_FIELD_VAL_X, boxW = SL_ED_FIELD_VAL_W, boxH = SL_ED_NOTES_H;
    _d.fillRect(boxX, y, boxW, boxH, COL_WHITE);
    _d.drawRect(boxX, y, boxW, boxH, COL_BLACK);

    int perLine = (boxW - 20) / 12;
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
        if (l2[0]) { _d.setCursor(boxX + 10, y + 36); _d.print(l2); }
    }

    int btnY = y + (boxH - SL_ED_FIELD_BTN_H) / 2;
    uiButton(_d, SL_ED_FIELD_BTN_X, btnY, SL_ED_FIELD_BTN_W, SL_ED_FIELD_BTN_H, "EDIT", COL_LTGREY, COL_BLACK, 3);
}

void SetlistPage::drawEditActionBar() {
    if (!_draftIsNew)
        uiButton(_d, SL_ED_DEL_X, SL_ED_ACT_Y, SL_ED_DEL_W, SL_ED_ACT_H, "DELETE", COL_BLACK, COL_WHITE, 3);
    uiButton(_d, SL_ED_CAN_X, SL_ED_ACT_Y, SL_ED_CAN_W, SL_ED_ACT_H, "CANCEL", COL_LTGREY, COL_BLACK, 3);
    uiButton(_d, SL_ED_OK_X, SL_ED_ACT_Y, SL_ED_OK_W, SL_ED_ACT_H, "OK", COL_BLACK, COL_WHITE, 3);
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
        _dupNameNote = false;       // the name is being changed
        openKeyboardFor(_draft.name, SETLIST_SONG_NAME_LEN - 1, State::KBD_NAME);
        return true;
    }
    if (hitEditFilePick(sx, sy)) { startServerPick(); return true; }
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
        if (commitDraft()) {
            openMasterSelect();
        } else {
            drawEditNote();         // refused — stay here and say why
            _d.paintLater();
        }
        return true;
    }
    if (hitEditCancel(sx, sy)) {
        openMasterSelect();
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
    return sx >= SL_ED_OK_X && sx < SL_ED_OK_X + SL_ED_OK_W && sy >= SL_ED_ACT_Y && sy < SL_ED_ACT_Y + SL_ED_ACT_H;
}
bool SetlistPage::hitEditCancel(int sx, int sy) const {
    return sx >= SL_ED_CAN_X && sx < SL_ED_CAN_X + SL_ED_CAN_W && sy >= SL_ED_ACT_Y && sy < SL_ED_ACT_Y + SL_ED_ACT_H;
}
bool SetlistPage::hitEditDelete(int sx, int sy) const {
    return sx >= SL_ED_DEL_X && sx < SL_ED_DEL_X + SL_ED_DEL_W && sy >= SL_ED_ACT_Y && sy < SL_ED_ACT_Y + SL_ED_ACT_H;
}

bool SetlistPage::commitDraft() {
    // ':' is the master.txt field separator — a name containing one would
    // split into name + file on the next load.
    for (char* c = _draft.name; *c; c++) { if (*c == ':') *c = '-'; }
    if (_draft.name[0] == '\0') return true;   // a nameless entry can't be referenced

    // Setlists refer to entries by name (case-insensitively), so a second
    // entry with the same name could never be reached.
    for (int i = 0; i < (int)sMaster->count; i++) {
        if (!_draftIsNew && i == _editIdx) continue;
        if (strcasecmp(sMaster->entries[i].name, _draft.name) == 0) {
            _dupNameNote = true;
            return false;
        }
    }
    _dupNameNote = false;

    if (_draftIsNew) {
        if (sMaster->count >= MASTER_MAX_ENTRIES) return true;
        sMaster->entries[sMaster->count] = _draft;
        sMaster->count++;
        saveMaster();
    } else if (_editIdx >= 0 && _editIdx < (int)sMaster->count) {
        char oldName[SETLIST_SONG_NAME_LEN];
        strncpy(oldName, sMaster->entries[_editIdx].name, sizeof(oldName) - 1);
        oldName[sizeof(oldName) - 1] = '\0';
        sMaster->entries[_editIdx] = _draft;
        saveMaster();
        // Keep every setlist's references in step if the name changed.
        // (Unless a hand-edited catalog still holds the old name elsewhere.)
        if (strcasecmp(oldName, _draft.name) != 0 &&
            masterFindByName(sMaster, oldName) < 0)
            propagateNameChange(oldName, _draft.name);
    }
    return true;
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
// ── PICK_FILE_SRV (server song picker — ScrollViewport) ──────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::startServerPick() {
    if (!_srvPickNames) {
        _srvPickNames = (char (*)[SETLIST_FILE_LEN])
                        ps_malloc((size_t)SRV_PICK_MAX * SETLIST_FILE_LEN);
        if (!_srvPickNames) {
            Serial.println("[SetlistPage] no PSRAM for the song picker");
            return;                 // stay on the edit screen
        }
    }
    gServerPairing.resetBrowse();
    gServerPairing.requestSongList(0);
    _srvPickCount   = 0;
    _srvPickReqPage = 0;
    _srvPickLoaded  = false;
    _srvListDrawn   = false;
    _pickView.reset();
    _wasDown = _touch.isTouched;
    _state = State::PICK_FILE_SRV;
    _d.fillScreen(COL_WHITE);
    drawPickChrome("PICK SONG (SERVER)");
    _pickView.redraw();
}

void SetlistPage::drawPickChrome(const char* title) {
    _d.fillRect(0, 0, 960, SL_HDR_H, COL_BLACK);
    uiButton(_d, SL_BACK_X, SL_BTN_Y, SL_BACK_W, SL_BTN_H, "BACK", COL_BLACK, COL_WHITE, 3);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor((960 - (int)strlen(title) * 18) / 2, (SL_HDR_H - 24) / 2);
    _d.print(title);

    _d.fillRect(0, SL_BAR_Y, 960, SL_BAR_H, COL_WHITE);
    _d.drawFastHLine(0, SL_BAR_Y, 960, COL_BLACK);
    char status[40];
    if (!_srvPickLoaded)          snprintf(status, sizeof(status), "Loading... (%d)", _srvPickCount);
    else if (_srvPickCount == 0)  snprintf(status, sizeof(status), "No songs on server");
    else                          snprintf(status, sizeof(status), "%d songs   (drag to scroll)", _srvPickCount);
    int tw = (int)strlen(status) * 18;
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor((960 - tw) / 2, SL_BAR_Y + (SL_BAR_H - 24) / 2);
    _d.print(status);
}

bool SetlistPage::paintPickBand(int x, int y, int w, int h, int offsetY) {
    _d.fillRect(x, y, w, h, COL_WHITE);
    int rowH = SL_PICK_ROW_H;
    int count = _srvPickCount;

    if (count == 0) {
        if (!_srvPickLoaded) {
            _d.setTextSize(3);
            _d.setTextColor(COL_DKGREY);
            const char* msg = "Loading from server...";
            _d.setCursor((960 - (int)strlen(msg) * 18) / 2, y + 60);
            _d.print(msg);
        }
        return true;
    }

    int first = floorDiv(offsetY, rowH);
    for (int idx = first; idx < count + 1; idx++) {
        int py = y + (idx * rowH - offsetY);
        if (py >= y + h) break;
        if (idx >= 0 && idx < count) {
            _d.drawFastHLine(20, py + rowH - 1, 920, COL_LTGREY);
            _d.setTextSize(3);
            _d.setTextColor(COL_BLACK);
            _d.setCursor(20, py + (rowH - 24) / 2);
            _d.print(_srvPickNames[idx]);
        }
    }

    int maxOff = count * rowH - h;
    if (maxOff < 0) maxOff = 0;
    bool atTop = (offsetY <= 0);
    bool atBot = (_srvPickLoaded && offsetY >= maxOff);   // not "done" until fully streamed
    return atTop || atBot;
}

void SetlistPage::tapPick(int tx, int ty) {
    (void)tx;
    if (ty < 0) return;
    int idx = ty / SL_PICK_ROW_H;
    if (idx < 0 || idx >= _srvPickCount) return;

    const char* name = _srvPickNames[idx];
    strncpy(_draft.file, name, SETLIST_FILE_LEN - 1);
    _draft.file[SETLIST_FILE_LEN - 1] = '\0';
    if (_draft.name[0] == '\0') {
        strncpy(_draft.name, name, SETLIST_SONG_NAME_LEN - 1);
        _draft.name[SETLIST_SONG_NAME_LEN - 1] = '\0';
    }
    _state = State::EDIT;
    _wasDown = _touch.isTouched;
    _d.fillScreen(COL_WHITE);
    drawEdit();
    _d.paint();
}

void SetlistPage::pumpServerPickPages() {
    if (_srvPickLoaded || !_srvPickNames) return;
    BrowseState bs = gServerPairing.browseState();
    if (bs != BrowseState::LIST_READY || _srvListDrawn) return;

    _srvListDrawn = true;
    int c = gServerPairing.listCount();
    for (int i = 0; i < c && _srvPickCount < SRV_PICK_MAX; i++) {
        const char* nm = gServerPairing.listName(i);
        if (!nm) continue;
        strncpy(_srvPickNames[_srvPickCount], nm, SETLIST_FILE_LEN - 1);
        _srvPickNames[_srvPickCount][SETLIST_FILE_LEN - 1] = '\0';
        _srvPickCount++;
    }
    if (_srvPickReqPage + 1 < gServerPairing.listTotalPages() && _srvPickCount < SRV_PICK_MAX) {
        _srvPickReqPage++;
        _srvListDrawn = false;
        gServerPairing.requestSongList(_srvPickReqPage);
    } else {
        _srvPickLoaded = true;
        gServerPairing.resetBrowse();
        drawPickChrome("PICK SONG (SERVER)");   // refresh the status line
        _pickView.redraw();                      // first real paint of the band
    }
}

SetlistResult SetlistPage::pollPickSrv() {
    pumpServerPickPages();
    _pickView.tick();
    if (!_touch.read()) return SetlistResult::NONE;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    _pickView.poll(down, sx, sy);

    if (!down && _wasDown) {
        _wasDown = false;
        if (_state != State::PICK_FILE_SRV) return SetlistResult::NONE;   // a tap chose a file
        if (hitPickBack(sx, sy)) {
            gServerPairing.resetBrowse();
            _state = State::EDIT;
            _d.fillScreen(COL_WHITE);
            drawEdit();
            _d.paint();
        }
    } else if (down && !_wasDown) {
        _wasDown = true;
    }
    return SetlistResult::NONE;
}

bool SetlistPage::hitPickBack(int sx, int sy) const {
    return sx >= SL_BACK_X && sx < SL_BACK_X + SL_BACK_W && sy >= SL_BTN_Y && sy < SL_BTN_Y + SL_BTN_H;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── helpers ──────────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

void SetlistPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

void SetlistPage::save() {
    if (!gServerPairing.isPaired()) return;
    if (!_listLoaded) {
        Serial.println("[SetlistPage] setlist never loaded — NOT saving over it");
        return;
    }
    saveSlot(_slot, &_list);
}

bool SetlistPage::saveSlot(uint8_t slot, const Setlist* sl) {
    if (!sSetlistBuf) {
        sSetlistBuf = (char*)ps_malloc(SETLIST_SERIALIZE_MAX);
        if (!sSetlistBuf) {
            Serial.println("[SetlistPage] no PSRAM for the setlist buffer — NOT saved");
            return false;
        }
    }
    size_t n = serializeSetlist(sl, sSetlistBuf, SETLIST_SERIALIZE_MAX);
    // An upload with no body never completes on the server (it finishes a
    // file when the last body byte arrives), so the old contents would stay.
    // A lone newline parses back as an empty list.
    if (n == 0) { sSetlistBuf[0] = '\n'; n = 1; }
    char fname[24];
    buildSetlistFilename(slot, fname, sizeof(fname));
    return gServerPairing.sendFileSave(FK_SETLISTS, fname, (const uint8_t*)sSetlistBuf, n);
}

void SetlistPage::propagateNameChange(const char* oldName, const char* newName) {
    const bool curChanged = patchSetlistName(&_list, oldName, newName);
    if (curChanged) {
        int max = maxScrollOffset();
        if (_scrollOffset > max) _scrollOffset = max;
    }
    if (!gServerPairing.isPaired()) return;

    if (!sOtherList) {
        sOtherList = (Setlist*)ps_malloc(sizeof(Setlist));
        if (!sOtherList)
            Serial.println("[SetlistPage] no PSRAM — other setlists NOT updated");
    }

    // The server finishes an upload (temp file → rename) on its main loop,
    // and the header of a following upload discards one not yet finished.  A
    // file-load reply comes from that same loop, after the finish — so a
    // completed load between two saves keeps them apart.  Each other slot's
    // load does that job for the save before it.
    const bool curLoaded = _listLoaded;     // loadSlotFromServer() overwrites it
    bool saveInFlight = true;               // the caller has just sent master.txt
    bool linkOk       = (sOtherList != nullptr);

    for (uint8_t slot = 1; slot <= SETLIST_COUNT && linkOk; slot++) {
        if (slot == _slot) continue;
        bool ok       = loadSlotFromServer(slot, sOtherList);
        bool answered = _listLoaded;        // contents, or "no such file"
        _listLoaded   = curLoaded;
        if (!answered) {
            // No reply — don't sit out a timeout per remaining slot.  Those
            // setlists keep the old name and show it as "(?)".
            Serial.printf("[SetlistPage] setlist %u did not load — references not updated\n",
                          (unsigned)slot);
            linkOk = false;
            break;
        }
        saveInFlight = false;
        if (ok && patchSetlistName(sOtherList, oldName, newName)) {
            saveSlot(slot, sOtherList);
            saveInFlight = true;
        }
    }

    if (curChanged) {
        if (saveInFlight && linkOk) {       // load only for the ordering above
            loadSlotFromServer(_slot, sOtherList);
            _listLoaded = curLoaded;
        }
        save();
    }
}

void SetlistPage::saveMaster() {
    if (!gServerPairing.isPaired()) return;
    if (!_masterLoaded) {
        Serial.println("[SetlistPage] master catalog never loaded — NOT saving over it");
        return;
    }
    if (!sMaster) return;
    if (!sMasterBuf) {
        sMasterBuf = (char*)ps_malloc(MASTER_SERIALIZE_MAX);
        if (!sMasterBuf) {
            Serial.println("[SetlistPage] no PSRAM for the catalog buffer — NOT saved");
            return;
        }
    }
    size_t n = serializeMasterList(sMaster, sMasterBuf, MASTER_SERIALIZE_MAX);
    if (n == 0) { sMasterBuf[0] = '\n'; n = 1; }   // see saveSlot(): no empty uploads
    gServerPairing.sendFileSave(FK_SETLISTS, masterListFilename(),
                                (const uint8_t*)sMasterBuf, n);
}

bool SetlistPage::loadSlotFromServer(uint8_t slot, Setlist* sl) {
    _listLoaded = false;
    if (!gServerPairing.isPaired()) { initSetlist(sl); return false; }
    char fname[24];
    buildSetlistFilename(slot, fname, sizeof(fname));
    gServerPairing.requestFileLoad(FK_SETLISTS, fname);

    uint32_t start = millis();
    while (millis() - start < 2500) {
        FileLoadState st = gServerPairing.fileLoadState();
        if (st == FileLoadState::READY) {
            bool ok = parseSetlist((const char*)gServerPairing.fileLoadData(),
                                   gServerPairing.fileLoadLen(), sl);
            gServerPairing.resetFileLoad();
            _listLoaded = ok;           // unparseable → leave the file alone
            return ok;
        }
        if (st == FileLoadState::NOT_FOUND) {      // no file yet: a new, empty list
            gServerPairing.resetFileLoad();
            initSetlist(sl);
            _listLoaded = true;
            return false;
        }
        if (st == FileLoadState::ERROR) break;
        delay(5);
    }
    Serial.printf("[SetlistPage] '%s' did not load — saving disabled for it\n", fname);
    gServerPairing.resetFileLoad();
    initSetlist(sl);
    return false;
}

bool SetlistPage::loadMasterFromServer() {
    _masterLoaded = false;
    if (!sMaster) return false;
    if (!gServerPairing.isPaired()) { initMasterList(sMaster); return false; }
    gServerPairing.requestFileLoad(FK_SETLISTS, masterListFilename());

    uint32_t start = millis();
    while (millis() - start < 2500) {
        FileLoadState st = gServerPairing.fileLoadState();
        if (st == FileLoadState::READY) {
            parseMasterList((const char*)gServerPairing.fileLoadData(),
                            gServerPairing.fileLoadLen(), sMaster);
            gServerPairing.resetFileLoad();
            _masterLoaded = true;
            return true;
        }
        if (st == FileLoadState::NOT_FOUND) {      // no catalog yet: start one
            gServerPairing.resetFileLoad();
            initMasterList(sMaster);
            _masterLoaded = true;
            return false;
        }
        if (st == FileLoadState::ERROR) break;
        delay(5);
    }
    Serial.println("[SetlistPage] master catalog did not load — saving disabled for it");
    gServerPairing.resetFileLoad();
    initMasterList(sMaster);
    return false;
}

// Pull the server's whole song-name list into the PSRAM cache.  Blocking — a
// request per 7-name page — which is why it runs once per open() and not on
// every row tap.  False if the list could not be had.
bool SetlistPage::fetchServerSongNames() {
    if (!_srvSongNames) {
        _srvSongNames = (char (*)[SL_NAME_LEN])
                        ps_malloc((size_t)SRV_MAX_SONGS * SL_NAME_LEN);
        if (!_srvSongNames) {
            Serial.println("[SetlistPage] no PSRAM for the song-name cache");
            return false;
        }
    }
    _srvSongCount = 0;

    int totalPages = 1;
    for (int page = 0; page < totalPages && page < 64 && _srvSongCount < SRV_MAX_SONGS; page++) {
        gServerPairing.resetBrowse();
        gServerPairing.requestSongList((uint8_t)page);
        uint32_t start = millis();
        bool got = false;
        while (millis() - start < 1500) {
            if (gServerPairing.browseState() == BrowseState::LIST_READY) { got = true; break; }
            delay(5);
        }
        if (!got) { gServerPairing.resetBrowse(); return false; }
        totalPages = gServerPairing.listTotalPages();
        int n = gServerPairing.listCount();
        for (int i = 0; i < n && _srvSongCount < SRV_MAX_SONGS; i++) {
            const char* nm = gServerPairing.listName(i);
            if (!nm || nm[0] == '\0') continue;
            strncpy(_srvSongNames[_srvSongCount], nm, SL_NAME_LEN - 1);
            _srvSongNames[_srvSongCount][SL_NAME_LEN - 1] = '\0';
            _srvSongCount++;
        }
    }
    gServerPairing.resetBrowse();
    return true;
}

bool SetlistPage::fileExistsOnServer(const char* file) {
    if (!file || file[0] == '\0') return false;
    if (!gServerPairing.isPaired())  return false;

    if (_srvSongCache == SongCache::NONE) {
        // The fetch calls resetBrowse(), which would zero the receive count
        // under a slow load still streaming in, so that song could never
        // land.  Leave it for a later tap.
        if (_srvLoadLate) return true;
        _srvSongCache = fetchServerSongNames() ? SongCache::VALID : SongCache::FAILED;
    }
    if (_srvSongCache != SongCache::VALID) return true;     // couldn't ask

    char want[SETLIST_FILE_LEN];
    strncpy(want, file, sizeof(want) - 1);
    want[sizeof(want) - 1] = '\0';
    stripMgt(want);

    // Listed names carry at most SL_NAME_LEN - 1 characters.
    for (int i = 0; i < _srvSongCount; i++) {
        if (strncasecmp(_srvSongNames[i], want, SL_NAME_LEN - 1) == 0) return true;
    }
    // A full cache may not be the whole card — don't call a song missing then.
    return _srvSongCount >= SRV_MAX_SONGS;
}

void SetlistPage::wrapText(const char* in, int maxChars, char line1[], char line2[], int line1Cap, int line2Cap) {
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
    for (int i = maxChars; i > maxChars / 2; i--) { if (in[i] == ' ') { br = i; break; } }
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

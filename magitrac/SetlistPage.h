#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "KeyboardPopup.h"
#include "ConfirmDialog.h"
#include "SetlistStorage.h"
#include "SongStorage.h"
#include "ServerPairing.h"

// ── SetlistPage — performer setlists, client-only ────────────────────────────
//
// Opened from PerformancePage's SETLIST button.  Owns 4 setlists, one
// visible at a time.  Each entry = display name + .mgt file ref + free notes.
//
// LIST view (960×540):
//   y=  0  [BACK 130]   "SETLIST 1"                       [ADD 130]   (50px)
//   y= 60  [SET 1] [SET 2] [SET 3] [SET 4]                            (60px)
//   y=130  ┌─ 7 song rows × 50px ────────────────────────────────────────┐
//          │  1. Tainted Love                       [↑] [↓]            │
//          │  2. Enola Gay                          [↑] [↓]            │
//          │  ...                                                       │
//   y=490  [PREV PAGE 130]    "Page 1 / 8"     [NEXT PAGE 130]         (50px)
//
// INFO popup (overlay on LIST):
//   "Song name"
//   Source: FILE.mgt   (or "FILE MISSING")
//   Notes (wrapped, up to ~5 lines)
//   [OK — LOAD]   [EDIT ENTRY]   [CANCEL]
//
// EDIT screen (full screen):
//   "EDIT SONG ENTRY"
//   Song Name:  <value>                          [EDIT]
//   Song File:  <value>                          [PICK]
//   Notes:      <wrapped value>                  [EDIT]
//   [DELETE]                          [CANCEL]   [OK]
//
// PICK_FILE screen:  list of /songs/*.mgt for the user to tap.

enum class SetlistResult : uint8_t {
    NONE,
    BACK,         // BACK pressed — return to PerformancePage
    SONG_LOADED,  // OK pressed in INFO popup, song restored into Song&
    TITLE_ONLY,   // OK pressed with no file attached — return to PerformancePage,
                  //   update title only, leave Song& untouched
};

// ── Layout constants ─────────────────────────────────────────────────────────

static const int SL_HDR_H        = 50;
static const int SL_BACK_X       = 0;
static const int SL_BACK_W       = 130;
static const int SL_ADD_X        = 830;
static const int SL_ADD_W        = 130;
static const int SL_BTN_Y        = 5;
static const int SL_BTN_H        = 40;

static const int SL_TAB_Y        = 60;
static const int SL_TAB_H        = 50;
static const int SL_TAB_MARGIN   = 5;
static const int SL_TAB_GAP      = 10;
static const int SL_TAB_W        = (960 - 2 * SL_TAB_MARGIN - 3 * SL_TAB_GAP) / 4;  // 230

static const int SL_LIST_Y       = 130;
static const int SL_ROW_H        = 50;
static const int SL_ROWS_PER_PAGE = 7;

static const int SL_NUM_X        = 10;
static const int SL_NUM_W        = 50;
static const int SL_NAME_X       = 70;
static const int SL_NAME_W       = 720;
static const int SL_UP_X         = 800;
static const int SL_UP_W         = 60;
static const int SL_DOWN_X       = 870;
static const int SL_DOWN_W       = 60;
static const int SL_ARROW_H      = 40;

static const int SL_BAR_Y        = 490;
static const int SL_BAR_H        = 50;
// Prev/next page buttons — only used by the PICK_FILE (SD song picker)
// view; the main setlist LIST view scrolls by touch-drag instead.
static const int SL_PREV_X       = 0;
static const int SL_PREV_W       = 130;
static const int SL_NEXT_X       = 830;
static const int SL_NEXT_W       = 130;

// Drag-scroll: any vertical movement past this threshold during a touch
// suppresses the falling-edge tap and instead pans the list.
static const int SL_DRAG_THRESH_PX = 12;

// INFO popup
static const int SL_INFO_X       = 80;
static const int SL_INFO_Y       = 110;
static const int SL_INFO_W       = 800;
static const int SL_INFO_H       = 320;
static const int SL_INFO_BTN_Y   = SL_INFO_Y + SL_INFO_H - 60 - 16;
static const int SL_INFO_BTN_H   = 60;
static const int SL_INFO_OK_X    = SL_INFO_X + 30;     // 110
static const int SL_INFO_OK_W    = 220;
static const int SL_INFO_EDIT_X  = SL_INFO_X + 290;    // 370
static const int SL_INFO_EDIT_W  = 220;
static const int SL_INFO_CAN_X   = SL_INFO_X + 550;    // 630
static const int SL_INFO_CAN_W   = 220;

// EDIT screen
static const int SL_ED_FIELD_LBL_X   = 20;
static const int SL_ED_FIELD_VAL_X   = 210;
static const int SL_ED_FIELD_VAL_W   = 600;
static const int SL_ED_FIELD_BTN_X   = 830;
static const int SL_ED_FIELD_BTN_W   = 120;
static const int SL_ED_FIELD_BTN_H   = 60;

// File row has two buttons (PICK + CLEAR) instead of one EDIT.
static const int SL_ED_FILE_VAL_W    = 500;
static const int SL_ED_FILE_PICK_X   = 720;
static const int SL_ED_FILE_PICK_W   = 110;
static const int SL_ED_FILE_CLR_X    = 840;
static const int SL_ED_FILE_CLR_W    = 110;

static const int SL_ED_NAME_Y   = 70;
static const int SL_ED_FILE_Y   = 160;
static const int SL_ED_NOTES_Y  = 250;
static const int SL_ED_NOTES_H  = 130;

static const int SL_ED_ACT_Y      = 470;
static const int SL_ED_ACT_H      = 60;
static const int SL_ED_DEL_X      = 20;
static const int SL_ED_DEL_W      = 180;
static const int SL_ED_CAN_X      = 540;
static const int SL_ED_CAN_W      = 180;
static const int SL_ED_OK_X       = 750;
static const int SL_ED_OK_W       = 190;

// PICK_FILE screen — reuse list layout from LIST view, with full-width rows.
static const int SL_PICK_ROWS_PER_PAGE = 8;

class SetlistPage {
public:
    SetlistPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    void open();
    void draw();
    SetlistResult poll();

    // Filled in when poll() returns SONG_LOADED — caller reads these so it
    // knows what was loaded.  loadedFilename() matches SongPage's behaviour;
    // loadedDisplayName() is the setlist entry's NAME (used as the
    // PerformancePage title override).
    const char* loadedFilename()    const { return _loadedFilename; }
    const char* loadedDisplayName() const { return _loadedDisplayName; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;
    KeyboardPopup        _keyboard;
    ConfirmDialog        _dialog;

    enum class State : uint8_t {
        LIST,
        INFO,
        WAITING_SONG,      // OK pressed in INFO: waiting for server SONG_READY
        EDIT,
        KBD_NAME,
        KBD_NOTES,
        PICK_FILE,         // SD-card file picker
        PICK_FILE_SRV,     // server-side song picker (paired)
        CONFIRM_DELETE,
    };

    State    _state;
    bool     _wasDown;

    uint8_t  _slot;          // 1..SETLIST_COUNT (currently visible)
    Setlist  _list;          // currently loaded setlist
    int      _scrollOffset;  // top visible row in the song list (0-based)
    int      _selectedIdx;   // song selected in INFO / being EDITed

    // Drag-scroll state — populated on touch-down inside the list view,
    // cleared on touch-up.  `_dragMoved` flips to true once vertical
    // movement exceeds SL_DRAG_THRESH_PX, which suppresses the
    // would-be tap so panning doesn't accidentally open a row.
    int      _dragStartY;
    int      _dragStartScrollOffset;
    bool     _dragMoved;

    // Bare filename (no .mgt) of the most recently loaded setlist entry —
    // used to draw a "currently playing" marker.  Survives close/reopen
    // since this object is long-lived.  Stale if the user loads via
    // SongPage in the interim; tracking that path is out of scope.
    char     _currentLoadedFile[SETLIST_FILE_LEN];

    // EDIT draft (so CANCEL is non-destructive).
    SetlistEntry _draft;
    bool         _draftIsNew;

    // Keyboard snapshot for non-destructive cancel (keyboard mutates buf in place).
    char     _kbdSnap[SETLIST_NOTES_LEN];
    char*    _kbdTarget;     // points into _draft

    // PICK_FILE state
    char     _files[STORAGE_MAX_FILES][STORAGE_FILENAME_MAX];
    int      _fileCount;
    int      _pickPage;

    // PICK_FILE_SRV state
    int      _srvPickPage;       // current server list page
    bool     _srvListDrawn;      // false = browser needs repopulating on next poll

    // WAITING_SONG state (OK-LOAD from server)
    uint32_t _srvLoadStartMs;

    // Result handover
    char     _loadedFilename[STORAGE_FILENAME_MAX];
    char     _loadedDisplayName[SETLIST_SONG_NAME_LEN];

    // ── LIST ─────────────────────────────────────────────────────────────────
    void drawList();
    void drawListHeader();
    void drawListTabs();
    void drawListRows();
    void drawListRow(int rowOnPage);
    void drawListBottomBar();
    SetlistResult pollList();   // can emit BACK

    int  maxScrollOffset() const;
    void ensureRowVisible(int idx);
    int  hitTab(int sx, int sy) const;          // -1 or 1..SETLIST_COUNT
    int  hitRowName(int sx, int sy) const;      // -1 or absolute song idx
    int  hitRowUp(int sx, int sy) const;        // -1 or absolute song idx
    int  hitRowDown(int sx, int sy) const;      // -1 or absolute song idx
    bool hitBack(int sx, int sy) const;
    bool hitAdd(int sx, int sy) const;

    void switchToSlot(uint8_t newSlot);
    void moveSongUp(int idx);
    void moveSongDown(int idx);
    void deleteSong(int idx);
    bool fileExistsOnSd(const char* file) const;

    // ── INFO ─────────────────────────────────────────────────────────────────
    void drawInfo();
    SetlistResult pollInfo();   // can emit SONG_LOADED
    bool hitInfoOk    (int sx, int sy) const;
    bool hitInfoEdit  (int sx, int sy) const;
    bool hitInfoCancel(int sx, int sy) const;

    // ── EDIT ─────────────────────────────────────────────────────────────────
    void drawEdit();
    void drawEditFieldRow(int y, const char* label, const char* value,
                          int valW, const char* btnLabel);
    void drawEditFileRow();
    void drawEditNotesRow();
    void drawEditActionBar();
    bool pollEdit();   // returns true if state changed
    bool hitEditFieldBtn(int y, int sx, int sy) const;
    bool hitEditFileClear(int sx, int sy) const;
    bool hitEditFilePick(int sx, int sy) const;
    bool hitEditOk    (int sx, int sy) const;
    bool hitEditCancel(int sx, int sy) const;
    bool hitEditDelete(int sx, int sy) const;

    void commitDraft();        // OK in EDIT — copy _draft into _list
    void openKeyboardFor(char* target, uint8_t maxLen, State afterState);

    // ── PICK_FILE (SD) ───────────────────────────────────────────────────────
    void loadFileList();
    void drawPick();
    void drawPickHeader(const char* title);
    void drawPickRows();
    void drawPickRow(int rowOnPage);
    void drawPickBottomBar(int total);
    bool pollPick();
    int  numPickPages() const;
    int  hitPickRow(int sx, int sy) const;
    bool hitPickBack(int sx, int sy) const;
    bool hitPickPrev(int sx, int sy) const;
    bool hitPickNext(int sx, int sy) const;

    // ── PICK_FILE_SRV (server) ───────────────────────────────────────────────
    void startServerPick();
    void drawPickSrv();
    void drawPickSrvRows();
    void drawPickSrvRow(int rowOnPage, const char* name);
    bool pollPickSrv();
    int  hitPickSrvRow(int sx, int sy) const;

    // ── WAITING_SONG ─────────────────────────────────────────────────────────
    void drawWaitingSong();
    SetlistResult pollWaitingSong();

    // ── helpers ──────────────────────────────────────────────────────────────
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    void save();   // persist _list to SD
    static void wrapText(const char* in, int maxChars,
                         char line1[], char line2[],
                         int line1Cap, int line2Cap);
};

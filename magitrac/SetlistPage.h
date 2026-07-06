#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "KeyboardPopup.h"
#include "ConfirmDialog.h"
#include "SetlistStorage.h"
#include "ServerPairing.h"
#include "ScrollViewport.h"

// ── SetlistPage — performer setlists, server-backed ──────────────────────────
//
// Opened from PerformancePage's SETLIST button.  Two data layers, both on the
// server SD (see SetlistStorage.h):
//   • the MASTER catalog — every song that could be played (name/file/notes).
//   • 4 SETLISTS — ordered lists of names that reference the catalog.
//
// LIST view (the current setlist):
//   [BACK] "SETLIST n" [ADD]                            ADD → master picker
//   [SET 1][SET 2][SET 3][SET 4]
//   1. Tainted Love                              [Move]
//   2. Enola Gay                                 [Move]
//   ...                                          (drag to scroll)
//
// MASTER_SELECT (ADD): the catalog as a drag-scroll list with inertia.  Tap a
//   song to add it to the current setlist (in tap order) — a numbered marker
//   shows it's in, tap again to remove.  [NEW] makes a catalog entry; [EDIT]
//   flips taps to "open the entry editor".
//
// INFO popup (tap a setlist row): name / file / notes (resolved from the
//   catalog).  [OK-LOAD] [REMOVE] [CANCEL].
//
// EDIT: catalog-entry editor — Name / File (server picker) / Notes, DELETE.

enum class SetlistResult : uint8_t {
    NONE,
    BACK,
    SONG_LOADED,
    TITLE_ONLY,
};

// ── Layout ────────────────────────────────────────────────────────────────────
static const int SL_HDR_H   = 50;
static const int SL_BACK_X   = 0;
static const int SL_BACK_W   = 130;
static const int SL_ADD_X    = 830;     // ADD / NEW (rightmost header button)
static const int SL_ADD_W    = 130;
static const int SL_MEDIT_X  = 690;     // EDIT-mode toggle in MASTER_SELECT
static const int SL_MEDIT_W  = 130;
static const int SL_BTN_Y    = 5;
static const int SL_BTN_H    = 40;

static const int SL_TAB_Y      = 60;
static const int SL_TAB_H      = 50;
static const int SL_TAB_MARGIN = 5;
static const int SL_TAB_GAP    = 10;
static const int SL_TAB_W      = (960 - 2 * SL_TAB_MARGIN - 3 * SL_TAB_GAP) / 4;

static const int SL_LIST_Y        = 130;
static const int SL_ROW_H         = 50;
static const int SL_ROWS_PER_PAGE = 7;

static const int SL_NUM_X  = 10;
static const int SL_NUM_W  = 50;
static const int SL_NAME_X = 70;
static const int SL_NAME_W = 720;
static const int SL_MOVE_X = 800;
static const int SL_MOVE_W = 130;
static const int SL_MOVE_H = 40;

static const int SL_BAR_Y = 490;
static const int SL_BAR_H = 50;

// LIST drag-scroll threshold (the picker lists use the ScrollViewport instead).
static const int SL_DRAG_THRESH_PX = 12;

// INFO popup
static const int SL_INFO_X      = 80;
static const int SL_INFO_Y      = 110;
static const int SL_INFO_W      = 800;
static const int SL_INFO_H      = 320;
static const int SL_INFO_BTN_Y  = SL_INFO_Y + SL_INFO_H - 60 - 16;
static const int SL_INFO_BTN_H  = 60;
static const int SL_INFO_OK_X   = SL_INFO_X + 30;
static const int SL_INFO_OK_W   = 220;
static const int SL_INFO_REM_X  = SL_INFO_X + 290;
static const int SL_INFO_REM_W  = 220;
static const int SL_INFO_CAN_X  = SL_INFO_X + 550;
static const int SL_INFO_CAN_W  = 220;

// EDIT screen
static const int SL_ED_FIELD_LBL_X = 20;
static const int SL_ED_FIELD_VAL_X = 210;
static const int SL_ED_FIELD_VAL_W = 600;
static const int SL_ED_FIELD_BTN_X = 830;
static const int SL_ED_FIELD_BTN_W = 120;
static const int SL_ED_FIELD_BTN_H = 60;
static const int SL_ED_FILE_VAL_W  = 500;
static const int SL_ED_FILE_PICK_X = 720;
static const int SL_ED_FILE_PICK_W = 110;
static const int SL_ED_FILE_CLR_X  = 840;
static const int SL_ED_FILE_CLR_W  = 110;
static const int SL_ED_NAME_Y  = 70;
static const int SL_ED_FILE_Y  = 160;
static const int SL_ED_NOTES_Y = 250;
static const int SL_ED_NOTES_H = 130;
static const int SL_ED_ACT_Y = 470;
static const int SL_ED_ACT_H = 60;
static const int SL_ED_DEL_X = 20;
static const int SL_ED_DEL_W = 180;
static const int SL_ED_CAN_X = 540;
static const int SL_ED_CAN_W = 180;
static const int SL_ED_OK_X  = 750;
static const int SL_ED_OK_W  = 190;

// Picker lists (MASTER_SELECT, server song picker) — share one ScrollViewport
// geometry: a band below the header, above the bottom status bar.
static const int SL_PICK_Y0    = SL_HDR_H + 10;     // 60
static const int SL_PICK_ROW_H = 52;
static const int SL_PICK_VIS   = 8;
static const int SL_PICK_VIEW_H = SL_PICK_VIS * SL_PICK_ROW_H;   // 416

class SetlistPage {
public:
    SetlistPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    void open();
    void draw();
    SetlistResult poll();

    const char* loadedFilename()    const { return _loadedFilename; }
    const char* loadedDisplayName() const { return _loadedDisplayName; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;
    KeyboardPopup        _keyboard;
    ConfirmDialog        _dialog;
    ScrollViewport       _msView;     // master-select picker
    ScrollViewport       _pickView;   // server song-file picker

    enum class State : uint8_t {
        NOT_CONNECTED,
        LIST,
        INFO,
        WAITING_SONG,
        MASTER_SELECT,
        EDIT,
        KBD_NAME,
        KBD_NOTES,
        PICK_FILE_SRV,
        CONFIRM_DELETE,
    };

    State   _state;
    bool    _wasDown;

    uint8_t _slot;
    Setlist _list;

    // LIST view scroll (manual drag — rows carry Move buttons).
    int     _scrollOffset;
    int     _dragStartY;
    int     _dragStartScrollOffset;
    bool    _dragMoved;

    int     _selectedIdx;       // setlist row in INFO / REMOVE
    bool    _selectedMissing;   // resolved file absent on server (computed at INFO)
    int     _moveSrcIdx;        // -1 = not reordering
    char    _currentLoadedFile[SETLIST_FILE_LEN];

    // Catalog-entry editor.
    MasterEntry _draft;
    bool        _draftIsNew;
    int         _editIdx;       // index into the master catalog, or -1 for new

    // Keyboard snapshot for non-destructive cancel.
    char    _kbdSnap[SETLIST_NOTES_LEN];
    char*   _kbdTarget;

    // MASTER_SELECT.
    bool    _msEditMode;        // taps open the editor instead of toggling

    // Server song-file picker (streamed in, scrolled via _pickView).
    static const int SRV_PICK_MAX = FILE_LIST_CACHE_MAX;
    char    _srvPickNames[SRV_PICK_MAX][SETLIST_FILE_LEN];
    int     _srvPickCount;
    int     _srvPickReqPage;
    bool    _srvPickLoaded;
    bool    _srvListDrawn;

    uint32_t _srvLoadStartMs;   // WAITING_SONG

    char    _loadedFilename[32];
    char    _loadedDisplayName[SETLIST_SONG_NAME_LEN];

    // ── NOT_CONNECTED ──────────────────────────────────────────────────────────
    void drawNotConnected();
    SetlistResult pollNotConnected();

    // ── LIST ─────────────────────────────────────────────────────────────────
    void drawList();
    void drawListHeader();
    void drawListTabs();
    void drawListRows();
    void drawListRow(int rowOnPage);
    void drawListBottomBar();
    SetlistResult pollList();
    int  maxScrollOffset() const;
    void ensureRowVisible(int idx);
    int  hitTab(int sx, int sy) const;
    int  hitRowName(int sx, int sy) const;
    int  hitRowMoveBtn(int sx, int sy) const;
    bool hitBack(int sx, int sy) const;
    bool hitAdd(int sx, int sy) const;
    void switchToSlot(uint8_t newSlot);

    // Resolve a setlist row's name to its catalog entry (or nullptr if the name
    // is no longer in the master list).
    const MasterEntry* resolveRow(int setlistIdx) const;

    // ── INFO ─────────────────────────────────────────────────────────────────
    void drawInfo();
    SetlistResult pollInfo();
    bool hitInfoOk(int sx, int sy) const;
    bool hitInfoRemove(int sx, int sy) const;
    bool hitInfoCancel(int sx, int sy) const;

    // ── WAITING_SONG ───────────────────────────────────────────────────────────
    void drawWaitingSong();
    SetlistResult pollWaitingSong();

    // ── MASTER_SELECT ──────────────────────────────────────────────────────────
    void openMasterSelect();
    void drawMasterHeader();
    void drawMasterHint();
    void drawMasterChrome();                  // header + hint (no band)
    bool paintMasterBand(int x, int y, int w, int h, int offsetY);  // ScrollViewport PaintFn
    void tapMaster(int tx, int ty);                                 // ScrollViewport TapFn
    void drawMasterRow(int idx, int py, int rowH);
    SetlistResult pollMasterSelect();
    bool hitMasterEdit(int sx, int sy) const;
    bool hitMasterNew(int sx, int sy) const;

    // ── EDIT (catalog entry) ───────────────────────────────────────────────────
    void drawEdit();
    void drawEditFieldRow(int y, const char* label, const char* value,
                          int valW, const char* btnLabel);
    void drawEditFileRow();
    void drawEditNotesRow();
    void drawEditActionBar();
    bool pollEdit();
    bool hitEditFieldBtn(int y, int sx, int sy) const;
    bool hitEditFileClear(int sx, int sy) const;
    bool hitEditFilePick(int sx, int sy) const;
    bool hitEditOk(int sx, int sy) const;
    bool hitEditCancel(int sx, int sy) const;
    bool hitEditDelete(int sx, int sy) const;
    void commitDraft();
    void openKeyboardFor(char* target, uint8_t maxLen, State afterState);

    // ── PICK_FILE_SRV (server song picker) ─────────────────────────────────────
    void startServerPick();
    void drawPickChrome(const char* title);   // header + hint (no band)
    bool paintPickBand(int x, int y, int w, int h, int offsetY);    // ScrollViewport PaintFn
    void tapPick(int tx, int ty);                                   // ScrollViewport TapFn
    void pumpServerPickPages();                // accumulate streamed pages
    SetlistResult pollPickSrv();
    bool hitPickBack(int sx, int sy) const;

    // ── helpers ────────────────────────────────────────────────────────────────
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    void save();         // serialise + upload the current setlist (FK_SETLISTS)
    void saveMaster();   // serialise + upload the master catalog (master.txt)
    bool loadSlotFromServer(uint8_t slot, Setlist* sl);
    bool loadMasterFromServer();
    bool fileExistsOnServer(const char* file);
    static void wrapText(const char* in, int maxChars,
                         char line1[], char line2[], int line1Cap, int line2Cap);
};

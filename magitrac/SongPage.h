#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "SongStorage.h"
#include "KeyboardPopup.h"
#include "ConfirmDialog.h"
#include "UIHelpers.h"
#include "ServerPairing.h"
#include "FileBrowser.h"

// ── Layout (960×540) ──────────────────────────────────────────────────────────
//
// SERVER BROWSER — FileBrowser (see FileBrowser.h for constants):
//   y=  0  ┌─ Header (50px): title / status banner ........ [HOME x=830,w=130]┐
//   y= 50  ├─ Toolbar (55px): "1-7 of N (drag scroll - swipe left to delete)" ┤
//   y=105  ├─ 7 full-width song rows (378px), drag-scroll with inertia;
//          │  swipe a row left to reveal its DEL button ──────────────────────┤
//   y=483  ├─ [NEW][SAVE name][SAVE AS] footer (57px) ─────────────────────────┤
//   y=540  └──────────────────────────────────────────────────────────────────┘
//
// NOT CONNECTED (unpaired): header "SONGS", a two-line notice, [BACK] at
// bottom-left.  Name entry (SAVE AS / NEW) uses the full-screen KeyboardPopup;
// confirms use the ConfirmDialog overlay.

// ── Result ────────────────────────────────────────────────────────────────────

enum class SongPageResult : uint8_t {
    NONE,
    HOME,
    SONG_LOADED,
};

// ── SongPage ──────────────────────────────────────────────────────────────────

class SongPage {
public:
    SongPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    void open();
    void draw();
    SongPageResult poll();

    // Still used by the setlist's (legacy) local-load path; harmless server-side.
    void clearLoadedFile() { _loadedFilename[0] = '\0'; }
    void setServerLoadedName(const char* name);   // mark song as loaded from server (enables save)
    // Name the loaded song after the push the server just completed: the file
    // it was read from (gServerPairing.pushedSongFile(), ".mgt" stripped), or
    // the song's own name field when the server reports no file.  This is the
    // name SAVE and autosave write to.
    void adoptPushedSongName();
    // The server reported that an explicit save failed: put "SAVE FAILED" in
    // the header bar (repainting now if the list is what's on screen).
    void showSaveFailed();
    const char* srvLoadedName() const { return _srvLoadedName; }     // empty if no server file loaded

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;
    KeyboardPopup        _keyboard;
    ConfirmDialog        _dialog;
    FileBrowser          _browser;

    enum class State : uint8_t {
        SERVER_BROWSE,         // browsing server song list
        SAVING_AS_SRV,         // keyboard open for save-as to server
        CONFIRM_DELETE_SRV,    // confirm dialog before deleting server song
        CONFIRM_SAVE_LOAD,     // "save changes to X?" before loading another song / NEW
        NAMING_NEW,            // keyboard: name a freshly-created (NEW) song
        CONFIRM_NEW_EXISTS,    // NEW name clashes an existing server song — rename / cancel
        NOT_CONNECTED,         // unpaired — client is server-only; show "pair" notice
    };

    State    _state;
    bool     _wasDown;

    char _loadedFilename[STORAGE_FILENAME_MAX];   // legacy: kept for setlist clearLoadedFile()
    char _srvLoadedName[SRV_NAME_MAX];            // loaded server file (no extension)
    char _pendingSaveName[32];
    int      _srvPage;          // next server list page to request while accumulating
    bool     _srvListDrawn;     // per-page latch: false = next list page not yet appended
    char     _srvPendingDeleteName[SRV_NAME_MAX];  // server song pending delete confirm
    uint32_t _srvLoadStartMs = 0;    // millis() when startServerLoad() last requested a song
    bool     _srvLoadSlow = false;   // slow-link notice already shown for this load
    // The load this page has in flight (startServerLoad → song / failure / link drop).
    bool     _srvAwaitingSong   = false;  // requested, no result yet
    bool     _srvReceivingShown = false;  // "Receiving song..." is standing in for the list
    bool     _srvRerequested    = false;  // already re-issued once after a wrong song landed
    char     _srvRequestedName[SRV_NAME_MAX];  // name asked for; "" = asked by list index
    int      _pendingLoadAbsIdx = 0; // server-list index tapped, awaiting save-changes confirm
    bool     _pendingNew = false; // true = the save-changes confirm precedes a NEW song

    // Drawing
    void drawHeader(const char* title, int h);

    // Restart the server list: clear accumulated items and re-request from
    // page 0, showing `status` until the list arrives.
    void restartServerList(const char* status);
    // Request the server song at absolute list index, showing "Receiving...".
    // With no link it requests nothing and shows NOT CONNECTED over the list.
    void startServerLoad(int absIdx);
    // Repaint the browser with `title` in the header bar (the banner slot).
    void showListBanner(const char* title);
    // Open the keyboard to name a new (blank) song before creating it.
    void beginNewSong();
    // True if `name` (case-insensitive) is already in the accumulated server list.
    bool nameExistsOnServer(const char* name) const;

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

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
#include "SongSource.h"
#include "FileBrowser.h"

// ── Layout (960×540) ──────────────────────────────────────────────────────────
//
// FILE BROWSER (see FileBrowser.h for layout constants):
//   y=  0  ┌─ Header (50px) ──────────────────────────────────────────────────┐
//   y= 50  ├─ [BACK x=0,w=160] .............. [PREV x=680] [NEXT x=818] (55px)┤
//   y=105  ├─ 4-column × 7-row file grid (378px) ───────────────────────────┤
//   y=483  ├─ [NEW][SAVE NAME][SAVE AS] footer (57px) ─────────────────────────┤
//   y=540  └──────────────────────────────────────────────────────────────────┘

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

    // Boot-button short press — forwards to the keyboard's symbol layer when a
    // name-entry keyboard is open; otherwise toggles the browser's delete mode.
    void onBootPress();

    // Still used by the setlist's (legacy) local-load path; harmless server-side.
    void clearLoadedFile() { _loadedFilename[0] = '\0'; }
    void setServerLoadedName(const char* name);   // mark song as loaded from server (enables save)
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
    uint32_t _srvLoadStartMs;   // millis() when requestSongLoad was last called
    int      _pendingLoadAbsIdx; // server-list index tapped, awaiting save-changes confirm
    bool     _pendingNew = false; // true = the save-changes confirm precedes a NEW song

    // Drawing
    void drawHeader(const char* title, int h);

    // Restart the server list: clear accumulated items and re-request from
    // page 0, showing `status` until the list arrives.
    void restartServerList(const char* status);
    // Request the server song at absolute list index, showing "Receiving...".
    void startServerLoad(int absIdx);
    // Open the keyboard to name a new (blank) song before creating it.
    void beginNewSong();
    // True if `name` (case-insensitive) is already in the accumulated server list.
    bool nameExistsOnServer(const char* name) const;

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

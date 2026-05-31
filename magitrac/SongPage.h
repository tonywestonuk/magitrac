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

// ── Layout constants ──────────────────────────────────────────────────────────

static const char* SONGS_DIR  = "/songs";

// ── SongPage ──────────────────────────────────────────────────────────────────

class SongPage {
public:
    SongPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    void open(bool sdAvailable);
    void draw();
    SongPageResult poll();

    // Called on boot button short press — toggles delete mode in file browser,
    // or forwards to the keyboard's symbol layer when keyboard is open.
    void onBootPress();

    void clearLoadedFile() { _loadedFilename[0] = '\0'; }
    void setServerLoadedName(const char* name);   // mark song as loaded from server (enables save)
    const char* loadedFilename() const { return _loadedFilename; }   // empty if no SD file loaded
    const char* srvLoadedName() const { return _srvLoadedName; }     // empty if no server file loaded

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;
    KeyboardPopup        _keyboard;
    ConfirmDialog        _dialog;
    FileBrowser          _browser;

    enum class State : uint8_t {
        FILE_BROWSE,
        SAVING_AS,
        CONFIRM_OVERWRITE,
        CONFIRM_DELETE,
        SERVER_BROWSE,         // browsing server song list
        SAVING_AS_SRV,         // keyboard open for save-as to server
        CONFIRM_DELETE_SRV,    // confirm dialog before deleting server song
    };

    State    _state;
    bool     _sdAvailable;
    uint32_t _sdCheckMs = 0;   // last time SD presence was probed
    bool     _wasDown;
    int      _pendingDeleteIdx;    // absolute SD file index awaiting delete confirm

    char _files[STORAGE_MAX_FILES][STORAGE_FILENAME_MAX];
    int  _fileCount;
    int  _filePage;   // current page index (0-based, each page = FB_PER_PAGE items)

    char _loadedFilename[STORAGE_FILENAME_MAX];   // loaded SD file (with .mgt extension)
    char _srvLoadedName[SRV_NAME_MAX];            // loaded server file (no extension)
    char _pendingSaveName[32];
    int      _srvPage;          // current server song list page
    bool     _srvListDrawn;     // false = browser needs repopulating on next poll
    int      _srvPendingDeleteIdx;  // page-relative index in server list pending delete confirm
    uint32_t _srvLoadStartMs;   // millis() when requestSongLoad was last called

    // Drawing
    void drawHeader(const char* title, int h);

    // Browser population helpers
    void populateBrowserFromSD();
    void populateBrowserFromServer();

    // Helpers
    void loadFileList();
    void buildPath(const char* name, char* out, int outLen) const;
    bool fileExists(const char* name) const;
    void doSave(const char* name);
    void doDelete(int fileIdx);

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

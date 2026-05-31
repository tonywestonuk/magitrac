#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "DrumTrackParser.h"
#include "DrumGmMap.h"

// ── DrumTrackImportPage ──────────────────────────────────────────────────────
// Picker for /drumtracks/*.txt on the server, then block-chooser dialog,
// then applies the chosen block to a contiguous column range starting at
// the cursor's current column in the current pattern.
//
// State machine:
//   LOADING_LIST → FILE_PICKER → LOADING_FILE → LOADING_MAP → BLOCK_CHOOSE
//                                                                  ↓
//                                                              APPLYING
//                                                                  ↓
//                                                                DONE (closes)
// (ERROR state shown on any failure; user dismisses to close.)
//
// gm_map.txt is fetched once per device session and cached in a static
// inside the .cpp.

class ServerPairing;

class DrumTrackImportPage {
public:
    DrumTrackImportPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    void open(uint8_t patternIdx, uint8_t startCol);
    void draw();
    // Returns true when the page wants to close (user tapped HOME, CANCEL,
    // or import completed).
    bool poll();

    // After a successful import: bitmask of columns that need full resync
    // to the server (settings unchanged but notes mutated).  Drained by the
    // caller via clearResync().
    uint32_t resyncMask() const { return _resyncMask; }
    void     clearResync()      { _resyncMask = 0; }

    bool importDidComplete() const { return _imported; }

private:
    enum class State : uint8_t {
        LOADING_LIST,
        FILE_PICKER,
        LOADING_FILE,
        LOADING_MAP,
        BLOCK_CHOOSE,
        APPLYING,
        ERROR_VIEW,
        DONE,
    };

    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;

    State    _state          = State::DONE;
    uint8_t  _patIdx         = 0;
    uint8_t  _startCol       = 1;
    uint32_t _resyncMask     = 0;
    bool     _imported       = false;
    bool     _wasDown        = false;
    char     _errMsg[64]     = {};

    // File picker
    int      _scroll         = 0;
    int      _dragStartY     = 0;
    int      _dragStartScroll= 0;
    bool     _dragMoved      = false;

    // Loaded parsed file + chosen block
    DrumPatternFile _file;
    int      _selectedBlock  = 0;     // 0-based block index

    // Audition (drum block playback through server MIDI) — only active on
    // the BLOCK_CHOOSE screen.  Stopped on tap STOP, block change, or any
    // state transition out of BLOCK_CHOOSE.
    bool     _audPlaying     = false;
    int      _audStep        = 0;     // 0..DRUM_PATTERN_STEPS-1
    uint32_t _audNextMs      = 0;
    uint16_t _audStepMs      = 125;   // recomputed from file tempo on play
    void auditionStart();
    void auditionStop();
    void auditionTick();              // call every poll; fires due steps

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    // Returns true on any touch state change (press OR release).  `down`
    // reflects whether the finger is currently held; coords are valid
    // either way (last reported position).  Returns false on idle frames
    // (no new event) — caller should treat that as "no work to do".
    bool readTouch(int& sx, int& sy, bool& down);

    void enterLoadingList();
    void enterFilePicker();
    void enterLoadingFile(const char* name);
    void enterLoadingMap();
    void enterBlockChoose();
    void enterError(const char* msg);

    void drawHeader(const char* title, bool withBack = false);
    void drawLoading(const char* what);
    void drawFilePicker();
    void drawBlockChoose();
    void drawError();

    bool hitHomeOrBack(int sx, int sy) const;

    void pollLoadingList();
    void pollFilePicker();
    void pollLoadingFile();
    void pollLoadingMap();
    void pollBlockChoose();
    void pollError();

    // Performs the import.  Returns true on success; sets _resyncMask.
    bool applyImport();
};

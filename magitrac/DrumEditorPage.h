#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"

class DrumPatternFile;   // DrumTrackParser.h — fwd-declared for importBlock()

// ── DrumEditorPage ────────────────────────────────────────────────────────────
// Step-grid view over a block's channel-10 columns.  Lanes derived from the
// distinct drum notes present in those columns; placing a step writes through
// to whichever ch10 column has a free slot (preferring one that already
// carries the same drum).  Pages: blocks ≤32 rows = 1 page; 48 → 2×24; 64 →
// 2×32.  Adding a lane opens an inline GM-drum picker.

static const int DEP_W       = 960;
static const int DEP_H       = 540;

static const int DEP_HDR_H   = 48;
static const int DEP_HOME_X  = 830;
static const int DEP_HOME_W  = 130;
static const int DEP_ADD_X   = 690;
static const int DEP_ADD_W   = 130;

// Page-nav buttons (only drawn for multi-page blocks)
static const int DEP_PREV_X  = 10;
static const int DEP_PREV_W  = 50;
static const int DEP_PAGEL_X = 60;
static const int DEP_PAGEL_W = 80;
static const int DEP_NEXT_X  = 140;
static const int DEP_NEXT_W  = 50;

// Transport strip (PLAY + BPM control), sits between header and grid
static const int DEP_TRANS_Y    = DEP_HDR_H;           // 48
static const int DEP_TRANS_H    = 48;
static const int DEP_PLAY_X     = 10;
static const int DEP_PLAY_W     = 130;
static const int DEP_BPM_LBL_X  = 170;
static const int DEP_BPM_LBL_W  = 70;
static const int DEP_BPM_MINUS_X = 250;
static const int DEP_BPM_VAL_X   = 310;
static const int DEP_BPM_VAL_W   = 100;
static const int DEP_BPM_PLUS_X  = 420;
static const int DEP_BPM_BTN_W   = 50;
static const int DEP_BPM_BTN_H   = 38;
static const int DEP_BPM_STEP    = 5;
static const int DEP_BPM_MIN     = 30;
static const int DEP_BPM_MAX     = 300;

// IMPORT button — opens the drum-pattern-file picker in drum-editor mode.
static const int DEP_IMPORT_X    = 750;
static const int DEP_IMPORT_W    = 200;

// Grid geometry (pushed down to make room for the transport strip)
static const int DEP_LBL_W   = 60;
static const int DEP_GRID_X  = DEP_LBL_W;                              // 60
static const int DEP_GRID_Y  = DEP_HDR_H + DEP_TRANS_H + 4;            // 100
static const int DEP_GRID_W  = DEP_W - DEP_LBL_W - 4;                  // 896
static const int DEP_GRID_H  = DEP_H - DEP_GRID_Y - 4;                 // 436

// Lane sizing
static const int DEP_LANE_H_MIN = 27;
static const int DEP_LANE_H_MAX = 48;
static const int DEP_MAX_LANES  = 16;   // 16 * 27 = 432, fits in 436

// Active-note marker radius
static const int DEP_DOT_R   = 12;
static const int DEP_IDLE_R  = 2;

// Picker (overlay)
static const int DEP_PCK_ROW_H   = 60;
static const int DEP_PCK_ROWS    = 7;
static const int DEP_PCK_Y       = DEP_HDR_H + 8;
static const int DEP_PCK_X       = 30;
static const int DEP_PCK_W       = DEP_W - 60;
static const int DEP_PCK_DRAG_THRESH = 12;

// Per-row audition button (right edge of each picker row).
static const int DEP_PCK_PLAY_W      = 70;
static const int DEP_PCK_PLAY_H      = 44;
static const int DEP_PCK_PLAY_MARGIN = 12;
static const int DEP_PCK_PLAY_OFF_MS = 200;
// Hit-area for the audition button extends LEFT of the visible triangle so
// the user can't accidentally tap-add a drum when aiming for play.  The
// boundary lines up with the left edge of the "MIDI NN" label:
//   strlen("MIDI NN")*18 + 16 padding = 142.
static const int DEP_PCK_PLAY_HIT_EXTRA_LEFT = 142;

class DrumEditorPage {
public:
    DrumEditorPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    void open(uint8_t patIdx);
    void draw();
    bool poll();        // returns true on HOME exit

    uint8_t patIdx() const { return _patIdx; }

    // Caller polls this after closing the drum-track-import overlay; true
    // means "user tapped IMPORT and chose a block + kit — perform the
    // import now".  Drained by clearImportRequest().
    bool importRequested() const { return _importRequested; }
    void clearImportRequest()    { _importRequested = false; }

    // Re-sync touch tracking after another page (e.g. drum-track-import
    // overlay) handed control back.  Without this, a stale _wasDown from
    // before the overlay could trigger a phantom falling-edge tap.
    void resumeTouch() { _wasDown = _touch.isTouched; }

    // Import a pre-parsed drum-pattern block + GS kit into this pattern's
    // ch10 columns.  Clears existing ch10 notes on this pattern first, then
    // writes the imported hits using the editor's same-drum-first allocation
    // rules.  Sets a per-column resync mask, drained via resyncMask().
    void importBlock(const DrumPatternFile& file, int blockIdx, int kitIdx);

    uint32_t resyncMask() const { return _resyncMask; }
    void     clearResync()      { _resyncMask = 0; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;

    uint8_t  _patIdx;
    bool     _wasDown;
    bool     _importRequested;
    uint32_t _resyncMask;

    // Derived lane list — tracker note values, sorted ascending.
    uint8_t _laneNotes[DEP_MAX_LANES];
    int     _laneCount;

    // Pagination state
    int _page;          // 0-based
    int _pageCount;     // 1 or 2
    int _pageCols;      // 16, 24, or 32
    int _pageStartRow;

    // Cached geometry
    int _laneH;
    int _cellW;

    // Cached cell state for current page: _cellOn[lane][step]
    bool _cellOn[DEP_MAX_LANES][32];

    // Audition state
    bool     _audPlaying;
    uint8_t  _audStep;       // global row index 0..pat.length-1
    uint32_t _audNextMs;
    uint16_t _audStepMs;
    uint16_t _tempo;         // BPM 30..300

    // GM-drum picker overlay state
    bool _pickerActive;
    int  _pickerScroll;
    bool _pickerWasDown;
    int  _pickerDragStartY;
    int  _pickerDragStartScroll;
    bool _pickerDragMoved;
    bool _pickerDragActive;  // true between a picker-area rising edge and its release

    // Per-row audition (play-icon tap): note-on now, scheduled note-off later.
    bool     _pickerAudActive;
    uint8_t  _pickerAudMidi;
    uint32_t _pickerAudOffMs;

    // Picker drag inertia (pixel-based; _pickerScroll stays integer-row).
    int      _pickerScrollPx;
    int      _pickerDragStartPx;
    int      _pickerVelTrackY;
    uint32_t _pickerVelTrackMs;
    float    _pickerVelPxPerMs;
    bool     _pickerInertiaActive;
    uint32_t _pickerInertiaLastTickMs;

    Pattern& pat() { return _song.patterns[_patIdx]; }

    // ── Setup ─────────────────────────────────────────────────────────────────
    void rebuildLanes();
    void rebuildCellOn();
    void recomputeGeometry();   // sets _pageCount, _pageCols, _pageStartRow, _laneH, _cellW
    void setPage(int p);        // updates _pageStartRow + rebuilds cellOn

    // ── Drawing ───────────────────────────────────────────────────────────────
    void drawHeader();
    void drawTransport();
    void drawBpmValue();
    void drawPlayButton();
    void drawLaneLabels();
    void drawGrid();
    void drawCell(int lane, int step);
    void drawStepColumn(int step, bool highlighted);
    void overlayPlayhead();   // draws marker for _audStep if visible on current page

    // ── Hit tests ─────────────────────────────────────────────────────────────
    bool hitHome    (int sx, int sy) const;
    bool hitAddDrum (int sx, int sy) const;
    bool hitPrevPage(int sx, int sy) const;
    bool hitNextPage(int sx, int sy) const;
    bool hitGridCell(int sx, int sy, int& lane, int& step) const;
    bool hitPlay    (int sx, int sy) const;
    bool hitBpmMinus(int sx, int sy) const;
    bool hitBpmPlus (int sx, int sy) const;
    bool hitImport  (int sx, int sy) const;

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;

    // ── Edit ops ──────────────────────────────────────────────────────────────
    // Returns the column index to write/clear at, or -1 if no slot available.
    int  findColumnForPlace(uint8_t row, uint8_t trackerNote);
    int  findColumnHoldingNote(uint8_t row, uint8_t trackerNote) const;
    int  allocateNewDrumColumn();   // returns new col idx or -1 if all 20 used
    void placeStep(int lane, int step);
    void clearStep(int lane, int step);
    void addLane(uint8_t trackerNote);
    bool laneHasNote(uint8_t trackerNote) const;

    // ── Picker ────────────────────────────────────────────────────────────────
    void openPicker();
    void closePicker(bool redraw);
    void drawPicker();
    void drawPickerRow(int visIdx, int globalIdx);
    bool pollPicker();
    bool hitPickerPlay(int sx, int sy, int& visIdx) const;
    void pickerAuditionStart(uint8_t midiNote);
    void pickerAuditionTick();
    void pickerInertiaTick();

    // ── Audition ──────────────────────────────────────────────────────────────
    void auditionStart();
    void auditionStop();
    void auditionTick();
    void recomputeStepMs();
};

// GM drum-kit label lookup (defined in DrumEditorPage.cpp).
// trackerNote: 1..96.  Returns 2-char abbreviation, or null-terminates buf
// with the raw note name if outside the GM drum-kit range.
//   buf must hold ≥ 4 bytes.
void gmDrumLabel(uint8_t trackerNote, char* buf);

// Full GM drum name for the picker (e.g. "Bass Drum 1").  Returns nullptr
// if outside the GM drum range.
const char* gmDrumName(uint8_t trackerNote);

#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "NoteGrid.h"
#include "HexpadPopup.h"

// ── Layout (960×540 EPD) ─────────────────────────────────────────────────────
//
//   y= 0  ┌─ "Column Note Editor"  [HOME x=830..960] ────────────────────────┐
//   y=40  │  ┌─ selector strip (28 high) ───────┐  ┌─ Right info panel ──┐  │
//   y=72  │  ├─ 13 pitch rows × 16 row-cells     │  │ [<] 02.1 / 3 [>]    │  │
//         │  │   each cell = CNE_CELL × CNE_CELL │  │ COL: {name}         │  │
//         │  │                                    │  │ ROW: 07             │  │
//         │  │                                    │  │ VEL: [6A]           │  │
//         │  │                                    │  │ ATTR: [E01C]  *     │  │
//         │  │                                    │  │ [OFF] | [ANY/W/S]   │  │
//         │  │                                    │  │ [COPY] [PASTE]      │  │
//   y=531 │  └────────────────────────────────────┘  └─────────────────────┘  │
//         └────────────────────────────────────────────────────────────────────┘
//
// * ATTR button only shown for output cols (1..8).  Col 0 swaps the OFF
//   action button for ANY/WAIT/SYNC, and hides ATTR (param=0 always).

static const int CNE_W           = 960;
static const int CNE_H           = 540;

// Header
static const int CNE_HDR_Y       = 0;
static const int CNE_HDR_H       = 40;
static const int CNE_HOME_X      = 830;
static const int CNE_HOME_W      = 130;

// Grid block
static const int CNE_LBL_W       = 50;     // pitch label column
static const int CNE_GRID_X      = CNE_LBL_W;
static const int CNE_SEL_Y       = 54;     // top selector strip — flatter than note cells
static const int CNE_SEL_H       = 18;
static const int CNE_GRID_Y      = 76;
static const int CNE_CELL        = 35;     // square cell size
static const int CNE_COLS        = 16;     // segment width
static const int CNE_VIS_PITCHES = 13;     // visible rows in the pitch axis
static const int CNE_GRID_W      = CNE_CELL * CNE_COLS;     // 560
static const int CNE_GRID_H      = CNE_CELL * CNE_VIS_PITCHES; // 455

// Right info panel
static const int CNE_RP_X        = 625;
static const int CNE_RP_W        = 330;    // 955 - 625
static const int CNE_RP_NAV_Y    = 50;
static const int CNE_RP_NAV_H    = 50;
static const int CNE_RP_PREV_W   = 65;
static const int CNE_RP_NEXT_W   = 65;
static const int CNE_RP_COL_Y    = 110;
static const int CNE_RP_ROW_Y    = 150;
static const int CNE_RP_VEL_Y    = 195;
static const int CNE_RP_ATTR_Y   = 245;
static const int CNE_RP_LBL_W    = 90;     // "VEL:" / "ATTR:" column
static const int CNE_RP_FIELD_H  = 40;
static const int CNE_RP_ACT_Y    = 305;
static const int CNE_RP_ACT_H    = 60;
static const int CNE_RP_CP_H     = 60;
static const int CNE_RP_CP_Y     = CNE_H - CNE_RP_CP_H - 6;   // anchored to bottom

// Hold threshold to fire COLUMN_HEADER_HOLD (set in TouchHandler, mirrored
// here only for documentation): 500 ms.

enum class CnePopup { NONE, VEL, ATTR };
enum class CneDrag  { NONE, GRID_MAYBE, GRID_PITCH, SELECTOR };

class ColumnNoteEditorPage {
public:
    ColumnNoteEditorPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    void open(uint8_t patIdx, uint8_t col, uint8_t row);
    void draw();
    bool poll();   // returns true on HOME exit

    // Server sync — caller drains these between polls.
    bool    notePending() const { return _dirtyRow != 0xFF; }
    uint8_t pendingPat() const  { return _patIdx; }
    uint8_t pendingRow() const  { return _dirtyRow; }
    uint8_t pendingCol() const  { return _col; }
    void    clearPending()      { _dirtyRow = 0xFF; }

    bool    bulkPending() const { return _dirtyAll; }
    void    clearBulkPending()  { _dirtyAll = false; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;
    HexpadPopup          _hexpad;

    uint8_t  _patIdx;
    uint8_t  _col;          // 0 = input track, 1..8 = MIDI out
    uint8_t  _segment;      // 0..3 — which 16-row window of the pattern
    uint8_t  _selRow;       // currently selected pattern row (0..63)
    uint8_t  _topPitch;     // pitch at the TOP of the visible grid (high pitch first)
    bool     _wasDown;

    // Selection / clipboard
    uint8_t  _selStartCol;
    uint8_t  _selEndCol;
    Note     _clip[CNE_COLS];
    uint8_t  _clipLen;

    // Drag state
    CneDrag  _drag;
    int      _dragStartX, _dragStartY;
    uint8_t  _dragStartTopPitch;
    int8_t   _dragStartCellCol;     // initial selector or grid cell column
    int8_t   _dragStartCellPitchIdx;// row index within visible pitches at touch-down

    // Popup state
    CnePopup _popup;

    // Server-sync flags
    uint8_t  _dirtyRow;     // 0xFF = clean; else last edited row in _patIdx, col=_col
    bool     _dirtyAll;     // bulk edit (paste) — caller does full song resync

    // Drawing
    void drawHeader();
    void drawSelectorStrip();
    void drawGrid();
    void drawRightPanel();
    void drawNavRow();
    void drawRowLine();
    void drawVelAttrRow();
    void drawActionButton();
    void drawCopyPasteRow(int pressed = -1);   // pressed: -1 none, 0 COPY, 1 PASTE

    // Helpers
    NoteGrid    grid();
    Pattern&    pat()  { return _song.patterns[_patIdx]; }
    uint8_t     numSegments() const;          // ceil(pat().length / 16)
    uint8_t     segmentRow(uint8_t i) const;  // _segment * 16 + i, or 0xFF
    bool        isInputCol() const { return _col == 0; }
    void        rawToScreen(int rx, int ry, int& sx, int& sy) const;

    // Hit tests
    bool hitHome(int sx, int sy) const;
    bool hitNavPrev(int sx, int sy) const;
    bool hitNavNext(int sx, int sy) const;
    bool hitVelField(int sx, int sy) const;
    bool hitAttrField(int sx, int sy) const;
    bool hitAction(int sx, int sy, uint8_t& which) const;  // which: 0=OFF/single, 0=ANY,1=WAIT,2=SYNC
    bool hitCopy(int sx, int sy) const;
    bool hitPaste(int sx, int sy) const;
    bool hitSelector(int sx, int sy, int8_t& col) const;
    bool hitGridCell(int sx, int sy, int8_t& col, int8_t& pitchIdx) const;

    // Edit operations
    void applyCellTap(uint8_t segCol, uint8_t pitch);
    void applyAction(uint8_t which);    // index into action button group
    void applyCopy();
    void applyPaste();
    void stepSegment(int delta);        // +1 / -1 with pattern wrap
    void clampTopPitch();
    void markDirty(uint8_t row);
};

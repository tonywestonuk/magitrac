#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "UIHelpers.h"
#include "KeyboardPopup.h"
#include "HoldRepeat.h"

// ── Column Editor — edit per-block column MIDI settings ──────────────────────
//
//   y=  0  ┌─ "COL N: name" header (black bar, 48px)  [HOME x=830,w=130] ────┐
//   y= 48  ├─ "MIDI SETTINGS" label strip (20px) ────────────────────────────┤
//   y= 68  ├─ MIDI CH row (60px)  [−] value [+] ─────────────────────────────┤
//   y=128  ├─ BANK row (60px) ────────────────────────────────────────────────┤
//   y=188  ├─ PROG row (60px) ────────────────────────────────────────────────┤
//   y=248  ├─ VOL row (60px) ─────────────────────────────────────────────────┤
//   y=308  ├─ TRANS row (60px) ───────────────────────────────────────────────┤
//   y=368  ├─ NAME row (60px) — name value at left, [PICK INSTR] in right gap ┤
//   y=428  ├─ Action bar: [CLEAR COL] [COPY TO COL...] [SWAP WITH COL...] ───┤
//   y=488  └─ (gap / overlays) ──────────────────────────────────────────────┘

static const int CE_W = 960;
static const int CE_H = 540;

// Header
static const int CE_HDR_H    = 48;
static const int CE_HOME_X   = 830;
static const int CE_HOME_W   = 130;

// Label strip
static const int CE_LBL_Y    = 48;
static const int CE_LBL_H    = 20;

// Field rows
static const int CE_ROW_Y    = 68;
static const int CE_ROW_H    = 60;
static const int CE_NUM_FIELDS = 6;   // CH, BANK, PROG, VOL, TRANS, NAME

// Field layout within each row
static const int CE_LABEL_X  = 30;
static const int CE_VAL_X    = 400;
static const int CE_VAL_W    = 200;
static const int CE_MINUS_X  = 620;
static const int CE_PLUS_X   = 770;
static const int CE_BTN_W    = 130;
static const int CE_BTN_H    = 44;

// Pick instrument button — sits in the right gap of the NAME row (field index 5)
static const int CE_PICK_X   = 620;
static const int CE_PICK_W   = 280;
static const int CE_PICK_H   = CE_BTN_H;
static const int CE_PICK_Y   = CE_ROW_Y + 5 * CE_ROW_H + (CE_ROW_H - CE_BTN_H) / 2;

// Instrument list overlay (when picking)
static const int CE_LIST_Y     = CE_LBL_Y + CE_LBL_H;
static const int CE_LIST_ROW_H = 48;
static const int CE_LIST_ROWS  = 8;
static const int CE_LIST_PREV_X = 0;
static const int CE_LIST_NEXT_X = CE_W - 130;
static const int CE_LIST_NAV_W  = 130;

// Action bar (bottom): CLEAR / COPY / SWAP buttons
static const int CE_ACT_Y       = 428;
static const int CE_ACT_H       = 60;
static const int CE_ACT_BTN_H   = 50;
static const int CE_CLEAR_X     = 10;
static const int CE_CLEAR_W     = 290;
static const int CE_COPY_X      = 320;
static const int CE_COPY_W      = 300;
static const int CE_SWAP_X      = 640;
static const int CE_SWAP_W      = 310;

// Column-picker overlay (for COPY/SWAP destination)
// Buttons for cols 1..MAX_COLUMNS-1 in a 4×N grid (col 0 is INPUT, excluded)
static const int CE_PICKCOL_TITLE_Y  = 80;
static const int CE_PICKCOL_TITLE_H  = 50;
static const int CE_PICKCOL_GRID_Y   = 150;
static const int CE_PICKCOL_BTN_W    = 220;
static const int CE_PICKCOL_BTN_H    = 100;
static const int CE_PICKCOL_GAP      = 10;
static const int CE_PICKCOL_COLS     = 4;     // buttons per row in grid
static const int CE_PICKCOL_X0       = 25;    // left margin: 25 + 4*(220+10) = 945
static const int CE_PICKCOL_CANCEL_Y = 410;
static const int CE_PICKCOL_CANCEL_W = 240;
static const int CE_PICKCOL_CANCEL_H = 70;

// Copy confirm dialog
static const int CE_CONF_TITLE_Y = 120;
static const int CE_CONF_BODY_Y  = 220;
static const int CE_CONF_BTN_Y   = 380;
static const int CE_CONF_BTN_W   = 220;
static const int CE_CONF_BTN_H   = 90;
static const int CE_CONF_YES_X   = 200;
static const int CE_CONF_NO_X    = 540;

class ColumnEditor {
public:
    ColumnEditor(EPD_PainterAdafruit& display, GT911_Lite& touch,
                 Song& song, Instrument* instruments);

    void open(uint8_t patternIdx, uint8_t col);
    void draw();
    bool poll();        // returns true when HOME tapped (close)

    bool patchPending() const { return _patchPending; }
    void clearPatch()         { _patchPending = false; }

    uint8_t editPattern() const { return _patIdx; }
    uint8_t editCol()     const { return _col; }

    // After COPY/SWAP, returns a bitmask of column indices that need full
    // (settings + notes across all patterns) resync to the server.  Caller
    // dispatches the syncing and then calls clearResync().
    uint16_t resyncMask() const { return _resyncMask; }
    void     clearResync()      { _resyncMask = 0; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;
    Instrument*          _instruments;

    uint8_t _patIdx;
    uint8_t _col;
    bool    _wasDown;
    bool    _patchPending;
    bool    _picking;       // true when instrument list overlay is shown
    bool    _pickingSample; // true when sample-picker overlay is shown
    bool    _naming;        // true when keyboard popup is open for NAME
    int     _pickPage;      // 0-based page of instrument / sample list
    KeyboardPopup _keyboard;

    // COPY / SWAP destination-picker + confirm overlay
    enum class Action : uint8_t {
        NONE,
        PICK_COPY_DST,
        PICK_SWAP_DST,
        CONFIRM_COPY,
        CONFIRM_CLEAR,
        PICK_SAMPLE,
    };
    Action   _action;
    uint8_t  _actionDst;    // remembered destination column for CONFIRM_COPY
    uint16_t _resyncMask;   // bits of columns whose settings + notes need resync
    bool     _pressedOnName; // true if last touch-down landed in the NAME row
                             // (gates the falling-edge keyboard popup so other
                             //  overlays' YES/NO buttons don't trigger it)
    uint8_t  _sampleListSeenState; // last observed sample list state — used to
                                   // trigger a repaint when WAITING → READY
                                   // while the picker overlay is open

    HoldRepeat _hold;

    ColumnSettings& cs();              // shortcut to current column settings
    void drawHeader();
    void drawFieldRow(int field);
    void drawAllFields();
    void drawPickButton();
    void drawPickList();               // instrument list overlay
    void drawPickSampleList();         // sample list overlay (SFX columns)
    void drawActionBar();              // COPY / SWAP buttons (bottom of normal view)
    void drawColumnPicker();           // overlay: pick destination column
    void drawCopyConfirm();            // overlay: confirm destructive copy
    void drawClearConfirm();           // overlay: confirm destructive clear

    // Returns target col (1..MAX_COLUMNS-1) for a tap inside the column
    // picker grid, or -1 if the tap missed (or hit the current column).
    int  hitColumnPicker(int sx, int sy) const;

    // Apply COPY src → dst (overwrites dst settings + notes in every pattern).
    void doCopyColumn(uint8_t srcCol, uint8_t dstCol);

    // Apply SWAP between two columns (settings + notes across patterns).
    void doSwapColumn(uint8_t a, uint8_t b);

    // Reset col to default settings and delete all its notes (every pattern).
    void doClearColumn(uint8_t col);

    void fieldLabel(int field, char* out) const;
    void fieldValue(int field, char* out) const;
    void adjustField(int field, int delta);
    void fireHeld();

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "UIHelpers.h"
#include "KeyboardPopup.h"
#include "HoldRepeat.h"

// ── Column Editor — edit a column's MIDI settings ────────────────────────────
//
// ColumnSettings are per-SONG (one per column, shared by every block).
// COPY / PASTE also carry the column's notes for the block the editor was
// opened on; CLEAR resets the settings and deletes the column's notes in
// every block.
//
//   y=  0  ┌─ "COL N: name" header (black bar, 48px)  [HOME x=830,w=130] ────┐
//   y= 48  ├─ "MIDI SETTINGS" label strip (20px) ────────────────────────────┤
//   y= 68  ├─ MIDI CH row (60px)  [−] value [+] ─────────────────────────────┤
//   y=128  ├─ BANK row (60px) — TUNE on SFX columns ──────────────────────────┤
//   y=188  ├─ PROGRAM row (60px) — KIT on drums, VOICE on ORGAN ──────────────┤
//   y=248  ├─ VOLUME row (60px) ──────────────────────────────────────────────┤
//   y=308  ├─ TRANSPOSE row (60px) ───────────────────────────────────────────┤
//   y=368  ├─ NAME row (60px) — name at left; right gap: [PICK INSTR], or
//          │  [PICK][EDIT] on SFX (none on PXL / drums / ORGAN) ──────────────┤
//   y=428  ├─ Action bar: [CLEAR n] [COPY] [PASTE] [IMPORT DRUMS] ───────────┤
//   y=488  └─ (gap — "COLUMN COPIED" banner) ─────────────────────────────────┘

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

// Instrument list overlay (when picking) — starts under the label strip
static const int CE_LIST_ROW_H = 48;
static const int CE_LIST_ROWS  = 8;
static const int CE_LIST_PREV_X = 0;
static const int CE_LIST_NEXT_X = CE_W - 130;
static const int CE_LIST_NAV_W  = 130;

// Action bar (bottom): CLEAR / COPY / PASTE / IMPORT buttons (4 across)
static const int CE_ACT_Y       = 428;
static const int CE_ACT_H       = 60;
static const int CE_ACT_BTN_H   = 50;
static const int CE_CLEAR_X     = 10;
static const int CE_CLEAR_W     = 220;
static const int CE_COPY_X      = 240;
static const int CE_COPY_W      = 220;
static const int CE_PASTE_X     = 470;
static const int CE_PASTE_W     = 220;
static const int CE_IMPORT_X    = 700;
static const int CE_IMPORT_W    = 250;

// PASTE / CLEAR confirm overlay
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

    // Other columns whose bank + program were just changed to follow the
    // edited column (they share its MIDI channel, so share its preset).  The
    // caller patches each one's bankMSB/program and calls clearPresetSync().
    uint32_t presetSyncMask() const { return _presetSyncMask; }
    void     clearPresetSync()      { _presetSyncMask = 0; }

    uint8_t editPattern() const { return _patIdx; }
    uint8_t editCol()     const { return _col; }

    // After PASTE, the current (pattern, col)'s notes changed — the caller
    // resyncs just that block's cells (settings ride patchPending()).
    bool     pasteNotesPending() const { return _pasteNotesPending; }
    void     clearPasteNotes()         { _pasteNotesPending = false; }

    // After CLEAR, returns a bitmask of column indices that need full
    // (settings + notes across all patterns) resync to the server.  Caller
    // dispatches the syncing and then calls clearResync().
    uint32_t resyncMask() const { return _resyncMask; }
    void     clearResync()      { _resyncMask = 0; }

    // Set when the user taps IMPORT DRUMS in the action bar.  The page
    // closes (poll() returns true); the caller checks this flag and opens
    // DrumTrackImportPage with the current pattern + cursor column.
    bool importRequested() const { return _importRequested; }
    void clearImportRequest()    { _importRequested = false; }

    // Set when the user taps EDIT on an SFX column's NAME row.  The page
    // closes; the caller opens SampleEditPage for the column's sample.
    bool sampleEditRequested() const { return _sampleEditRequested; }
    void clearSampleEditRequest()    { _sampleEditRequested = false; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;
    Instrument*          _instruments;

    uint8_t _patIdx;
    uint8_t _col;
    bool    _wasDown;
    bool    _patchPending;
    uint32_t _presetSyncMask = 0;
    void    syncSharedPreset();
    bool    _picking;       // true when instrument list overlay is shown
    bool    _pickingSample; // true when sample-picker overlay is shown
    bool    _naming;        // true when keyboard popup is open for NAME
    int     _pickPage;      // 0-based page of instrument / sample list
    KeyboardPopup _keyboard;

    // Confirm overlays (destructive PASTE / CLEAR).
    enum class Action : uint8_t {
        NONE,
        CONFIRM_PASTE,
        CONFIRM_CLEAR,
    };
    Action   _action;
    bool     _pasteNotesPending = false;  // paste changed this block's notes
    uint32_t _resyncMask;   // bits of columns whose settings + notes need resync
    bool     _importRequested = false;
    bool     _sampleEditRequested = false;
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
    void drawPickList();               // instrument list overlay
    void drawPickSampleList();         // sample list overlay (SFX columns)
    void drawActionBar();              // CLEAR / COPY / PASTE / IMPORT DRUMS (bottom of normal view)
    void drawPasteConfirm();           // overlay: confirm destructive paste
    void drawClearConfirm();           // overlay: confirm destructive clear

    // COPY: capture the current column's settings + this block's notes into a
    // static clipboard that survives closing/reopening the editor.
    void copyToClip();
    // PASTE: overwrite the current column's settings + this block's notes from
    // the clipboard, and flag the block's notes for resync.
    void pasteFromClip();

    // Reset col to default settings and delete all its notes (every pattern).
    void doClearColumn(uint8_t col);

    void fieldLabel(int field, char* out) const;
    void fieldValue(int field, char* out) const;
    void adjustField(int field, int delta);
    void fireHeld();

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

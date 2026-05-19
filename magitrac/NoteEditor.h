#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "HexpadPopup.h"
#include "HoldRepeat.h"
#include "PixelPostPicker.h"

// ── Full-screen popup layout (960×540) ───────────────────────────────────────
//
//   y=  0  ┌─ Header: "COL2 ROW07 / 15"  [<] [>] nav  (48px) ────────────────┐
//   y= 48  ├─ "NOTE" label strip (22px) ─────────────────────────────────────┤
//   y= 70  ├─ 12 note-name buttons × 80px wide × 86px tall ─────────────────┤
//   y=156  ├─ "OCTAVE" label strip (22px) ──────────────────────────────────┤
//   y=178  ├─ 8 octave buttons × 120px wide × 78px tall ───────────────────┤
//   y=256  ├─ "NOTE OFF / VELOCITY" label strip (22px) ────────────────────┤
//   y=278  ├─ [OFF](480px) | [-] VV [+] velocity (480px)  (60px) ──────────┤
//   y=338  ├─ [COPY]  [PASTE]  [OK]  action buttons (202px) ──────────────┤
//   y=540  └──────────────────────────────────────────────────────────────────┘
//  Sum: 48+22+86+22+78+22+60+202 = 540 ✓

static const int PE_W = 960;
static const int PE_H = 540;

static const int PE_HDR_H   = 48;

static const int PE_NLBL_Y  = PE_HDR_H;                    // = 48
static const int PE_NLBL_H  = 22;
static const int PE_NBTN_Y  = PE_NLBL_Y + PE_NLBL_H;       // = 70
static const int PE_NBTN_H  = 86;
static const int PE_NBTN_W  = PE_W / 12;                    // = 80

static const int PE_OLBL_Y  = PE_NBTN_Y + PE_NBTN_H;       // = 156
static const int PE_OLBL_H  = 22;
static const int PE_OBTN_Y  = PE_OLBL_Y + PE_OLBL_H;       // = 178
static const int PE_OBTN_H  = 78;
static const int PE_OBTN_W  = PE_W / 8;                     // = 120

static const int PE_ILBL_Y  = PE_OBTN_Y + PE_OBTN_H;       // = 256
static const int PE_ILBL_H  = 22;
static const int PE_IBTN_Y  = PE_ILBL_Y + PE_ILBL_H;       // = 278
static const int PE_IBTN_H  = 60;
static const int PE_IARROW_W = 200;                          // inst prev/next width

// Velocity control (output columns) — right half of PE_IBTN row
static const int PE_VEL_X     = PE_W / 2;                    // = 480
static const int PE_VEL_W     = PE_W / 2;                    // = 480
static const int PE_VEL_ARR_W = 120;                          // [-] and [+] width
static const int PE_VEL_VAL_W = PE_VEL_W - 2 * PE_VEL_ARR_W; // = 240

static const int PE_ACT_Y   = PE_IBTN_Y + PE_IBTN_H;       // = 338
static const int PE_ACT_H   = PE_H - PE_ACT_Y;              // = 202

// Action row — 3 buttons at 320px each: COPY | PASTE | OK
static const int PE_ACT_BTN_W  = 320;
static const int PE_ACT_COPY_X  = 0;
static const int PE_ACT_PASTE_X = 320;
static const int PE_ACT_OK_X    = 640;

// Pixel-post variant — 4 buttons at 240px each: PXL | COPY | PASTE | OK
static const int PE_ACT_PXL_BTN_W  = 240;
static const int PE_ACT_PXL_X      = 0;
static const int PE_ACT_PXL_COPY_X  = 240;
static const int PE_ACT_PXL_PASTE_X = 480;
static const int PE_ACT_PXL_OK_X    = 720;

// Row navigation buttons sit inside the header bar
static const int PE_HDR_NAVBTN_W = 100;  // width of [<] and [>] in header

// ── NoteEditor ────────────────────────────────────────────────────────────────

class NoteEditor {
public:
    NoteEditor(EPD_PainterAdafruit& display, GT911_Lite& touch);

    // Open popup for the given cell — caller should display.clear() first.
    // undoBuf is written before each commit so the caller can restore on undo.
    void open(Song* song, uint8_t patternIdx, uint8_t row, uint8_t col,
              UndoEntry* undoBuf, Instrument* instruments);

    bool isOpen() const { return _open; }

    // Force-close without committing (e.g. page change while editor is open)
    void close() { _open = false; }

    // Cell that was last committed (valid after pollTouch() returns true, or after pendingSync())
    uint8_t editPattern() const { return _syncPattern; }
    uint8_t editRow()     const { return _syncRow; }
    uint8_t editCol()     const { return _syncCol; }

    // True whenever commit() was called since the last clearPendingSync().
    // Use this to detect row-navigation commits, not just the final OK.
    bool pendingSync()        const { return _pendingSync; }
    void clearPendingSync()         { _pendingSync = false; }

    // Full popup draw — call once after open()
    void draw();

    // Poll touch. Returns true when popup should close (OK / CANCEL / CLEAR).
    // Caller should display.clear() + ui.drawAll() + display.paint() after.
    bool pollTouch();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    Song*       _song;
    UndoEntry*  _undoBuf;
    Instrument* _instruments;
    uint8_t    _patternIdx;
    uint8_t    _row;
    uint8_t    _col;
    bool       _open;
    bool       _wasDown;

    // Working copy
    bool    _hasNote;    // false = "---"
    bool    _offNote;    // true = NOTE_OFF (note-off cell; overrides _hasNote)
    uint8_t _semitone;   // 0–11
    uint8_t _octave;     // 0–7
    uint8_t _velocity;   // 0–127 explicit, or VEL_DEFAULT
    uint8_t _effect;     // 0x00–0xFF
    uint8_t _param;      // 0x00–0xFF
    // col-0 working state (mutually exclusive: at most one of these is set)
    bool    _waitSet;    // col0: effect == EFFECT_WAIT
    bool    _syncSet;    // col0: effect == EFFECT_SYNC

    // Hex editor for attributes
    HexpadPopup _hexpad;

    // Pixel-post picker (only opened for PIXELPOST_CHANNEL columns)
    PixelPostPicker _picker;

    // Hold-repeat for velocity +/-
    HoldRepeat _hold;

    // Pending sync — set on every commit(), cleared by caller via clearPendingSync()
    bool    _pendingSync;
    uint8_t _syncPattern;
    uint8_t _syncRow;
    uint8_t _syncCol;

    void drawHeader();
    void drawNoteButtons();
    void drawOctaveButtons();
    void drawInstControl();
    void drawVelocity();
    void drawAttrButton();
    void drawInputOptions();
    void drawActionButtons();

    void sectionLabel(int y, int h, const char* text);
    void popupBtn(int x, int y, int w, int h, const char* label, bool highlighted);

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    void commit();
    void loadRow();   // load song row → working copy (call after changing _row)

    // Session clipboard — static so it survives row navigation and re-opens
    static bool _clipHasNote;
    static uint8_t _clipSemitone;
    static uint8_t _clipOctave;
    static uint8_t _clipVelocity;
    static uint8_t _clipEffect;
    static uint8_t _clipParam;
    static bool    _clipValid;  // true once something has been copied
};

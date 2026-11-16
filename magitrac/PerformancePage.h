#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "UIHelpers.h"
#include "KeyboardPopup.h"

// ── PerformancePage — 8 pads for live block switching ────────────────────────
//
//  PERFORM VIEW:
//  y=  0  ┌─ Header: title  [SETLIST] [EDIT] [HOME]            (50px)
//  y= 60  ├─ 4 pads top row (blocks 1–4)      │ [organ icon]    (200px)
//  y=270  ├─ 4 pads bottom row (blocks 5–8)   │ [sample icon]   (200px)
//  y=478  ├─ Light strip: [< FX] [FX >] [WHITE] [RELEASE]       (54px)
//  y=532  └─
//
//  The two icon buttons sit in a column directly under HOME (same x, same
//  width) and open the organ page and the samples page; the sketch brings
//  this page back when their BACK is tapped (see resume()).
//
//  PAD EDIT VIEW:
//  y=  0  ┌─ Header: "PAD SETUP"  [BACK]           (50px)
//  y= 55  ├─ 8 rows × 60px: [#] [NAME field] [IMM/QUE toggle]
//  y=535  └─
//
//  SETLIST, EDIT, HOME and the two icon buttons require a 500ms hold to
//  activate (anti-fumble during performance).  BACK in the pad-edit view is a
//  plain tap and returns to the pads.

// Header
static const int PP_HDR_H      = 50;
static const int PP_SETLIST_X  = 540;
static const int PP_SETLIST_W  = 140;
static const int PP_EDIT_X     = 700;
static const int PP_EDIT_W     = 120;
static const int PP_HOME_X     = 830;
static const int PP_HOME_W     = 130;
static const int PP_BTN_Y      = 5;
static const int PP_BTN_H      = 40;

// Side column under HOME: organ (beside the top pad row) + samples (bottom row).
static const int PP_SIDE_X     = PP_HOME_X;   // 830
static const int PP_SIDE_W     = PP_HOME_W;   // 130

// Pad grid — 4 columns × 2 rows, left of the side column
static const int PP_PAD_COLS   = 4;
static const int PP_PAD_COUNT  = 8;
static const int PP_PAD_MARGIN = 20;   // outer (left) margin
static const int PP_PAD_GAP    = 10;   // gap between pads, and before the side column
static const int PP_PAD_Y0     = 60;   // first row top
static const int PP_PAD_W      = (PP_SIDE_X - PP_PAD_GAP - PP_PAD_MARGIN
                                  - (PP_PAD_COLS - 1) * PP_PAD_GAP) / PP_PAD_COLS;  // 192
static const int PP_PAD_H      = 200;  // shortened from 225 to free a light-control strip
static const int PP_PAD_ROW_GAP = 10;

// Light-control strip (PixelPost) — short button row below the pads.
// Row 2 bottom = 60 + (200+10) + 200 = 470, so the strip sits at 478.
static const int PP_LIGHT_Y      = 478;
static const int PP_LIGHT_H      = 54;
static const int PP_LIGHT_MARGIN = 20;
static const int PP_LIGHT_GAP    = 10;
static const int PP_LIGHT_COUNT  = 4;
static const int PP_LIGHT_W      = (960 - 2 * PP_LIGHT_MARGIN - (PP_LIGHT_COUNT - 1) * PP_LIGHT_GAP) / PP_LIGHT_COUNT;  // 222

// Hold duration for header buttons (anti-fumble)
static const uint32_t PP_HOLD_MS = 500;
// Hold duration on a pad → STOP playback + clear pad lights
static const uint32_t PP_PAD_HOLD_MS = 1000;

// Pad edit list layout
static const int PE_ROW_Y0     = 55;
static const int PE_ROW_H      = 60;
static const int PE_NUM_X      = 20;    // pad number column
static const int PE_NUM_W      = 60;
static const int PE_NAME_X     = 90;    // name field
static const int PE_NAME_W     = 560;
static const int PE_MODE_X     = 670;   // mode toggle button
static const int PE_MODE_W     = 270;

class PerformancePage {
public:
    PerformancePage(EPD_PainterAdafruit& display, GT911_Lite& touch,
                    Song& song);

    enum class Result : uint8_t {
        NONE,
        HOME,       // HOME held — exit performance page
        SETLIST,    // SETLIST held — open setlist page
        ORGAN,      // organ icon held — open the organ page, come back here after
        SAMPLES,    // sample icon held — open the samples page, come back here after
    };

    void open(uint8_t currentPattern, bool stopped = false);
    // Back from a page opened off this one (ORGAN / SAMPLES): same song, same
    // title, pad edits kept.  The caller redraws.  `stopped` darkens the pads
    // if the song stopped meanwhile; a playing song's position arrives with
    // the next server update.
    void resume(bool stopped);
    void draw();
    Result poll();

    // Called by main loop to update playing pattern from server position
    void setPlayingPattern(uint8_t pat);

    // Override the header title (e.g. with the setlist entry's display name).
    // Pass nullptr or "" to revert to "PERFORMANCE".  Cleared on every open().
    void setTitleOverride(const char* name);

    // True if any pad config changed (caller should sync to server)
    bool patchPending() const { return _patchPending; }
    void clearPatch()         { _patchPending = false; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;

    bool     _wasDown;
    int8_t   _playingPattern;   // currently playing block (white on black); -1 = none lit
    int8_t   _queuedPattern;    // queued block (-1 = none)
    bool     _flashState;       // toggled for queued pad animation
    uint32_t _lastFlashMs;      // millis() of last flash toggle
    bool     _patchPending;     // perfPad config changed, needs server sync

    // Hold tracking for header buttons + pads
    enum class HoldTarget : uint8_t { NONE, HOME, EDIT, SETLIST, PAD, ORGAN, SAMPLES };
    HoldTarget _holdTarget;
    uint32_t   _holdStartMs;
    bool       _holdFired;
    int8_t     _holdPadIdx;     // pad under finger when HoldTarget::PAD is armed
    // Anti-phantom validation: on-screen count of touches the driver rejected.
    uint32_t   _lastRejShown  = 0;
    uint32_t   _lastRejDrawMs = 0;

    // Light-control strip state.
    int8_t   _lightArmedBtn = -1;    // strip button pressed on finger-down (-1 = none)
    bool     _lightManual   = false; // we've grabbed PixelPost control → RELEASE enabled

    // Sub-views
    bool     _editing;          // true = pad edit list, false = performance pads
    KeyboardPopup _keyboard;
    int8_t   _kbdPadIdx;        // which pad is being renamed (-1 = none)

    char     _titleOverride[40] = {};  // empty = use "PERFORMANCE"

    // ── Performance pad view ─────────────────────────────────────────────────
    void drawPerfHeader();
    void drawRej();          // anti-phantom: small rejected-touch counter in header
    void drawPads();
    void drawPad(int idx);
    int  padX(int idx) const;
    int  padY(int idx) const;
    bool hitHome(int sx, int sy) const;
    bool hitEdit(int sx, int sy) const;
    bool hitSetlist(int sx, int sy) const;
    int  hitPad(int sx, int sy) const;
    int  hitSide(int sx, int sy) const;      // 0 = organ, 1 = samples; -1 none
    void drawSideButtons();
    void drawOrganIcon(int cx, int cy);      // centred on (cx, cy)
    void drawSampleIcon(int cx, int cy);
    Result pollPerf();

    // ── Light-control strip (PixelPost) ──────────────────────────────────────
    void drawLightStrip();
    int  hitLightBtn(int sx, int sy) const;   // 0=PREV 1=NEXT 2=WHITE 3=RELEASE; -1 none
    void fireLightButton(int btn);            // send the override + refresh strip

    // ── Pad edit view ────────────────────────────────────────────────────────
    void drawEditView();
    void drawEditHeader();
    void drawEditRow(int idx);
    bool hitBack(int sx, int sy) const;
    int  hitEditName(int sx, int sy) const;  // -1 or pad index
    int  hitEditMode(int sx, int sy) const;  // -1 or pad index
    bool pollEdit();    // returns true = back to perf view

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

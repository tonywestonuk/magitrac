#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"

// ── Layout ────────────────────────────────────────────────────────────────────
//
//   y=  0  ┌─ "BLOCK SETTINGS" header (black bar, 48px)  [HOME x=830,w=130] ─┐
//   y= 48  ├─ "BLOCKS" subheading strip (lt grey, 20px) ────────────────────┤
//   y= 68  ├─ [<] 01/03 [>]   [NEW]   [DUPLICATE]   [DELETE]   (60px) ──────┤
//   y=128  ├─ LEN [16][24][32][48][64] (52px) ───────────────────────────────┤
//   y=180  ├─ "INPUT NOTES" label (22px) ────────────────────────────────────┤
//   y=202  ├─ 12 note buttons × 80px (74px) ────────────────────────────────┤
//   y=276  ├─ BLOCK switch row (72px)  ← per note ──────────────────────────┤
//   y=348  ├─ TRANSPOSE row (72px)     ← per note ──────────────────────────┤
//   y=420  ├─ KEY-CHG row (52px)       ← per block ─────────────────────────┤
//   y=472  ├─ NEXT BLK row (68px)      ← per block ─────────────────────────┤
//   y=540  └─────────────────────────────────────────────────────────────────┘

static const int BSP_W = 960;
static const int BSP_H = 540;

// Header bar
static const int BSP_HDR_Y  = 0;
static const int BSP_HDR_H  = 48;

// Subheading strip (lt grey, label "BLOCKS") — acts as visual buffer below header
static const int BSP_SUB_Y  = 48;
static const int BSP_SUB_H  = 20;

// Combined nav + action row — fat buttons, full row height
static const int BSP_NAV_Y  = 68;
static const int BSP_NAV_H  = 60;   // touch target height for all buttons in this row

// Nav section (left side): [<]  01/03  [>]
static const int BSP_PREV_W   = 90;             // x=0
// label occupies x=90..270 (180px)
static const int BSP_NEXT_X   = 270;  static const int BSP_NEXT_W  = 90;

// Action buttons (right side): [NEW] [DUPL] [SPLT] [DEL]
static const int BSP_NEW_X    = 360;  static const int BSP_NEW_W   = 150;
static const int BSP_DUP_X    = 510;  static const int BSP_DUP_W   = 150;
static const int BSP_SPL_X    = 660;  static const int BSP_SPL_W   = 150;
static const int BSP_DEL_X    = 810;  static const int BSP_DEL_W   = 150;

// Length row
static const int BSP_LEN_Y    = 128;
static const int BSP_LEN_H    = 52;
static const int BSP_LEN_LBL_W = 90;
static const int BSP_LEN_BTN_W = 130;
static const int BSP_LEN_COUNT = 5;
static const uint8_t BSP_LENGTHS[BSP_LEN_COUNT] = { 16, 24, 32, 48, 64 };

// "INPUT NOTES" section label
static const int BSP_LBL_Y    = 180;
static const int BSP_LBL_H    = 22;

// Note buttons row
static const int BSP_NOT_Y    = 202;
static const int BSP_NOT_H    = 74;
static const int BSP_NOT_W    = 80;   // BSP_W / 12 = 80

// Block switch row
static const int BSP_BLK_Y    = 276;
static const int BSP_BLK_H    = 72;

// Transpose row
static const int BSP_TRP_Y    = 348;
static const int BSP_TRP_H    = 72;

// HOME button in header bar (top-right, matching SongConfigPage style)
static const int BSP_HOME_X   = 830;
static const int BSP_HOME_W   = 130;

// Button height for internal rows (LEN, BLK, TRP) — smaller than nav row
static const int BSP_BTN_H    = 44;

// Block switch row button positions
static const int BSP_BLK_STAY_X  = 130; static const int BSP_BLK_STAY_W  = 120;
static const int BSP_BLK_SPOS_X  = 260; static const int BSP_BLK_SPOS_W  = 170;
static const int BSP_BLK_TOP_X   = 440; static const int BSP_BLK_TOP_W   = 100;
static const int BSP_BLK_MINUS_X = 600; static const int BSP_BLK_ARROW_W = 70;
static const int BSP_BLK_VAL_X   = 670; static const int BSP_BLK_VAL_W   = 90;
static const int BSP_BLK_PLUS_X  = 760;

// Transpose row button positions
static const int BSP_TRP_KEEP_X  = 140; static const int BSP_TRP_KEEP_W  = 100;
static const int BSP_TRP_NOTE_X  = 250; static const int BSP_TRP_NOTE_W  = 100;
static const int BSP_TRP_CUST_X  = 360; static const int BSP_TRP_CUST_W  = 130;
static const int BSP_TRP_MINUS_X = 600;
static const int BSP_TRP_VAL_X   = 670; static const int BSP_TRP_VAL_W   = 90;
static const int BSP_TRP_PLUS_X  = 760;

// Key-change row (shrunk to 52px to make room for end-nav row)
static const int BSP_KCH_Y       = 420;
static const int BSP_KCH_H       = 52;
static const int BSP_KCH_SPOS_X  = 160; static const int BSP_KCH_SPOS_W  = 170;
static const int BSP_KCH_TOP_X   = 340; static const int BSP_KCH_TOP_W   = 100;

// End-navigation row (block-end behaviour: loop/fwd/back/abs/rnt)
static const int BSP_END_Y       = 472;
static const int BSP_END_H       = 68;
static const int BSP_END_LOOP_X  = 160; static const int BSP_END_LOOP_W  = 80;
static const int BSP_END_FWD_X   = 240; static const int BSP_END_FWD_W   = 80;
static const int BSP_END_BACK_X  = 320; static const int BSP_END_BACK_W  = 80;
static const int BSP_END_ABS_X   = 400; static const int BSP_END_ABS_W   = 80;
static const int BSP_END_RNT_X   = 480; static const int BSP_END_RNT_W   = 80;
static const int BSP_END_MINUS_X = 600; static const int BSP_END_ARROW_W = 70;
static const int BSP_END_VAL_X   = 670; static const int BSP_END_VAL_W   = 90;
static const int BSP_END_PLUS_X  = 760;

// ── BlockSettingsPage ─────────────────────────────────────────────────────────

class BlockSettingsPage {
public:
    BlockSettingsPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    void open(uint8_t patIdx);
    void draw();
    bool poll();   // returns true on HOME exit

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;

    uint8_t _patIdx;
    uint8_t _selNote;
    bool    _wasDown;
    bool    _confirmDelete;    // true while waiting for delete confirmation
    bool    _confirmSplit;     // true while waiting for split confirmation
    bool    _didDuplicate;    // set when a duplicate was performed this session
    bool    _didSplit;        // set when a split was performed this session
    bool    _keyChangePending; // set when keyChangeMode is changed — caller should patch server
    uint8_t _keyChangePat;    // pattern index that was changed
    bool    _navChangePending; // set when blockEndNav is changed
    uint8_t _navChangePat;     // pattern index that was changed

    // Hold-to-set-all state for transpose buttons
    uint8_t  _holdTrpBtn;     // 0=none, 1=KEEP held, 2=NOTE held
    uint32_t _holdTrpStart;
    bool     _confirmSetAll;  // true when "set all?" overlay is showing
    uint8_t  _setAllAction;   // TransposeAction value to apply if confirmed

    void drawHeader();
    void drawNavRow();          // nav arrows + action buttons (or confirm overlay)
    void drawLengthRow();
    void drawSectionLabel(int y, int h, const char* text);
    void drawNoteButtons();
    void drawBlockRow();
    void drawTransposeRow();
    void drawSetAllOverlay();
    void drawKeyChangeRow();
    void drawEndNavRow();

    void btn(int x, int y, int w, int h, const char* label,
             bool highlighted, bool greyed = false);

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;

    Pattern&        pat()   { return _song.patterns[_patIdx]; }
    InputNoteEntry& entry() { return pat().inputNotes[_selNote]; }

    void doDelete();
    void doSplit();
    bool canSplit() const;

public:
    // True if a duplicate or split was performed since open() — caller should sendSongToServer
    bool didDuplicate() const { return _didDuplicate; }
    bool didSplit()     const { return _didSplit; }
    void clearSplit()         { _didSplit = false; }

    // True if keyChangeMode was changed — caller should patch server immediately
    bool    keyChangePending() const { return _keyChangePending; }
    uint8_t keyChangePat()     const { return _keyChangePat; }
    void    clearKeyChange()         { _keyChangePending = false; }

    // True if blockEndNav was changed — caller should patch server immediately
    bool    navChangePending() const { return _navChangePending; }
    uint8_t navChangePat()     const { return _navChangePat; }
    void    clearNavChange()         { _navChangePending = false; }
};

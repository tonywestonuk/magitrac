#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "UIHelpers.h"

// ── OrganPage — drawbar organ control (960×540) ───────────────────────────────
//
// Nine Hammond drawbars drawn on the e-paper.  Drag a bar to set 0..8; every
// change is pushed to the server (MSG_ORGAN) which runs the additive synth and
// sounds it from the live MIDI-in keyboard.  open() also flips the server into
// organ mode (ENTER) and syncs the full registration; HOME leaves it (EXIT).
//
// Values are quantised 0..8, so a drag only repaints/sends on a step change —
// keeps the slow e-paper redraws (and the wire) to a minimum.

static const int OR_N        = 9;
static const int OR_HDR_H    = 50;
static const int OR_HOME_X   = 820;
static const int OR_HOME_W   = 125;
static const int OR_BTN_Y    = 5;
static const int OR_BTN_H    = 40;

// Top-bar tap-to-cycle buttons: [TYPE] [VIB/CHORUS] [LESLIE] [DRIVE]  ...  [HOME]
static const int OR_FX_N     = 4;
static const int OR_FX_X0    = 12;
static const int OR_FX_W     = 192;
static const int OR_FX_GAP   = 10;
// Setting counts (must match the server's ranges).
static const int OR_TYPE_N   = 5;     // DRAWBAR, TONEWHEEL, CLAUDE, NEBULA, SAMPLE
static const int OR_VC_N     = 7;     // off, V1-3, C1-3
static const int OR_LES_N    = 3;     // stop, slow, fast
static const int OR_DRV_N    = 2;     // off, on

// Preset buttons run down the left edge; the drawbar panel sits to their right.
static const int OR_PRESET_N   = 5;
static const int OR_PRESET_X   = 15;
static const int OR_PRESET_W   = 160;
static const int OR_PRESET_Y0  = 70;
static const int OR_PRESET_H   = 74;
static const int OR_PRESET_GAP = 13;

static const int OR_MARGIN   = 185;    // left edge of the drawbar panel
static const int OR_RMARGIN  = 163;    // wide right margin → room for the knob column
static const int OR_COL_W    = (960 - OR_MARGIN - OR_RMARGIN) / OR_N;   // 68
static const int OR_KNOB_W   = 50;     // drawbar tab width
static const int OR_KNOB_H   = 46;
static const int OR_TOP      = 120;    // knob centre at value 0
static const int OR_BOT      = 470;    // knob centre at value 8
static const int OR_VAL_Y    = 62;     // value number row
static const int OR_LABEL_Y  = 484;    // footage label row

// Per-type knob column on the right edge (vertical sliders 0..8).
static const int OR_NKNOB    = 3;      // max knobs a type can show
static const int OR_KCOL_X0  = 812;
static const int OR_KCOL_W   = 40;
static const int OR_KCOL_GAP = 5;

class OrganPage {
public:
    OrganPage(EPD_PainterAdafruit& display, GT911_Lite& touch);

    enum class Result : uint8_t { NONE, HOME };

    void   open();      // draw + tell the server to enter organ mode
    void   draw();
    Result poll();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    uint8_t _bars[OR_N];   // 0..8 per drawbar
    bool    _open;
    bool    _wasDown;
    int     _dragCol;      // drawbar column locked on finger-down (-1 = none)
    int     _dragKnob;     // knob locked on finger-down (-1 = none)
    int     _activePreset; // index of the last-applied preset, -1 if hand-edited
    int     _type;         // organ voice model (0..OR_TYPE_N-1)
    int     _vibChorus;    // 0=off, 1-3 V1-3, 4-6 C1-3
    int     _leslie;       // 0=stop, 1=slow, 2=fast
    int     _drive;        // 0=off, 1=on
    uint8_t _knob[OR_TYPE_N][OR_NKNOB];   // per-type knob values 0..8

    void drawHeader();
    void drawFooter();
    void drawBar(int i);
    void drawPresets();
    void drawKnobs();
    void drawKnob(int k);
    void syncKnobs();              // send the active type's knob values to the server
    int  hitKnob(int sx, int sy) const;   // knob index for the active type, or -1
    void applyPreset(int idx);
    bool hitHome(int sx, int sy) const;
    int  hitPreset(int sx, int sy) const;   // returns preset index or -1
    int  hitFx(int sx, int sy) const;       // top-bar control index 0..3, or -1
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "UIHelpers.h"
#include "TrackerData.h"
#include "OrganSynth.h"
#include "SynthEditor.h"

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

// Top bar (6 tappable slots): [TYPE] [fx] [fx] [fx] [FX n/N page] [HOME].
// Slot 0 is the voice TYPE (always shown); slots 1..3 show one PAGE of effects;
// slot 4 cycles the effect page.  Effects are paged so the bar doesn't get
// crowded as more are added.
static const int OR_FX_N        = 5;   // tappable header slots left of HOME (0..4)
static const int OR_FX_PER_PAGE = 3;   // effects shown per page (slots 1..3)
static const int OR_FX_X0    = 12;
static const int OR_FX_W     = 155;
static const int OR_FX_GAP   = 6;
// Setting counts (must match the server's ranges).
static const int OR_TYPE_N   = 5;     // DRAWBAR, TONEWHEEL, CLAUDE, NEBULA, PROC
static const int OR_VC_N     = 7;     // off, V1-3, C1-3
static const int OR_LES_N    = 3;     // stop, slow, fast
static const int OR_DRV_N    = 2;     // off, on
static const int OR_RVB_N    = 3;     // off, room, hall
static const int OR_TYPE_PROC = 4;    // the procedural-sounds type

// PROC type — the sound list replaces presets + drawbars.  Count and order
// must mirror PROC_SOUNDS in the server's proc_sounds.h.
static const int OR_PROC_N     = 4;
static const int OR_PROC_REED  = 0;    // the REED sound (server: PROC_IDX_REED)
static const int OR_PROC_VOC   = 2;    // the VOCODER sound (server: PROC_IDX_VOC)
static const int OR_PROC_SYNTH = ORGAN_PROC_SYNTH;   // edited on its own screen (SynthEditor)
// The sound buttons are a grid, two to a row, filled left to right then down
// (OR_PRESET_N rows → room for 10).  Together they span the old single
// column's 420 px, so the list still ends at 435.
static const int OR_PROC_COLS  = 2;
static const int OR_PROC_GAP   = 10;
static const int OR_PROCBTN_W  = (420 - OR_PROC_GAP) / OR_PROC_COLS;   // 205

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
// PROC mode has no drawbars, so its column starts further left and fits 5.
static const int OR_NKNOB        = 5;    // max sliders a type/sound can show
static const int OR_KCOL_X0      = 812;  // drawbar types (3 sliders max)
static const int OR_KCOL_X0_PROC = 720;
static const int OR_KCOL_W       = 40;
static const int OR_KCOL_GAP     = 5;

// Drag tooltip — the slider labels under the column are tiny (size 1, all the
// width there is), so while a finger is down on a slider its name + value are
// also shown big in a box to the LEFT of the whole slider column.  Transient:
// wiped on release, and whatever it covered is redrawn.
// Width is bounded by PROC mode: its slider column starts at 720 and the sound
// list ends at OR_PRESET_X + 420 (435), so 270 is the widest that
// still clears the buttons.
static const int OR_TIP_W = 270;
static const int OR_TIP_H = 62;
static const int OR_TIP_Y = 264;      // roughly centred on the groove (120..470)

class OrganPage {
public:
    OrganPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    enum class Result : uint8_t { NONE, HOME };

    // draw + tell the server to enter organ mode.  backLabel: the corner
    // button reads BACK instead of HOME (opened from the performance page —
    // the sketch decides where Result::HOME leads).
    void   open(bool backLabel = false);
    void   draw();
    Result poll();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;

    uint8_t _bars[OR_N];   // 0..8 per drawbar
    bool    _open;
    bool    _backLabel = false;   // corner button says BACK, not HOME
    bool    _wasDown;
    int     _dragCol;      // drawbar column locked on finger-down (-1 = none)
    int     _dragKnob;     // knob locked on finger-down (-1 = none)
    bool    _tipShown;     // drag tooltip is on screen (needs wiping on release)
    int     _activePreset; // index of the last-applied preset, -1 if hand-edited
    int     _type;         // organ voice model (0..OR_TYPE_N-1)
    int     _vibChorus;    // 0=off, 1-3 V1-3, 4-6 C1-3
    int     _leslie;       // 0=stop, 1=slow, 2=fast
    int     _drive;        // 0=off, 1=on
    int     _reverb;       // 0=off, 1=room, 2=hall
    int     _fxPage;       // which page of effects the top bar shows
    uint8_t _knob[OR_TYPE_N][OR_NKNOB];   // per-type knob values 0..8
    int     _procSel;                     // selected procedural sound (PROC type)
    uint8_t _procKnob[OR_PROC_N][OR_NKNOB];   // per-sound slider values 0..8
    uint8_t _synth[ORGAN_SYNTH_PARAMS];   // the SYNTH sound's settings
    bool    _synthOpen = false;           // SynthEditor has the screen
    SynthEditor _synthEd;

    bool isSynth() const { return _type == OR_TYPE_PROC && _procSel == OR_PROC_SYNTH; }
    void openSynth();
    bool hitSynthEdit(int sx, int sy) const;

    void drawHeader();
    void drawFxButton(int slot, int id);   // one effect button on the top bar
    void cycleFx(int id);                  // advance + push one effect
    void drawFooter();
    void drawBar(int i);
    void drawPresets();
    void drawProcList();
    void selectProc(int idx);
    int  hitProc(int sx, int sy) const;   // procedural-sound button index, or -1
    void drawKnobs();
    void drawKnob(int k);
    void drawKnobTip(int k);   // big "NAME  v" box left of the slider column
    void hideKnobTip();        // wipe it + repaint what it covered
    void syncKnobs();              // send the active type's knob values to the server
    uint8_t& knobRef(int k);       // active slider store (per-type, or per-proc-sound)
    void applyToSong();            // write the page state into Song::organ (OrganPatch) + patch the
                                   // server — only if the song has an ORGAN column
    void adoptFromSong();          // load the page state from Song::organ, if it is marked present
    int  hitKnob(int sx, int sy) const;   // knob index for the active type, or -1
    void applyPreset(int idx);
    bool hitHome(int sx, int sy) const;
    int  hitPreset(int sx, int sy) const;   // returns preset index or -1
    int  hitFx(int sx, int sy) const;       // top-bar slot index 0..OR_FX_N-1, or -1
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

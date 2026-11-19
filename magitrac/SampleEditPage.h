#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "MagiMsg.h"
#include "HoldRepeat.h"

// ── SampleEditPage — non-destructive sample trim/loop/tune editor ────────────
//
// Shows a waveform overview fetched from the server (MSG_SAMPLE_INFO);
// START/END markers are touch-and-drag (grab near the line, drag, release to
// store); LOOP makes a held key wrap the region; TUNE −/+ shifts the sample's
// own pitch a cent at a time and −ST/+ST a semitone at a time (TRANSPOSE —
// the same stored value, which is in cents); VEL ON/OFF says whether MIDI keys
// play it louder/softer with velocity or always at its own level.  Metadata
// lives on the server (/samples/trim.txt) — the WAV never changes — and
// belongs to the SAMPLE, so it applies in every song.
//
// Two ways in:
//   • SamplesPage (hold a square): no column — col = SE_NO_COL.  PLAY sounds
//     the bare sample, and while the page is open the MIDI-in keyboard plays
//     it at the played pitch (the server does that itself; this page only
//     keeps the SAMPLE_EDIT_KEYS lease alive).
//   • ColumnEditor's EDIT button on an SFX column: PLAY auditions through the
//     column (its TUNE / TRANSPOSE / VOLUME on top); the sketch relays MIDI
//     keys the same way.
//
// ZOOM ± halves/doubles the visible window; every zoom or scroll re-requests
// the overview for just the visible window, so the server re-scans that slice
// at full 400-point resolution — real detail, not stretched pixels.  The
// scrollbar under the wave drags the window when zoomed (disabled when the
// whole file fits).

// Layout (960×540)
static const int SE_HDR_H    = 70;
static const int SE_BTN_H    = 54;
static const int SE_BTN_Y    = 8;
static const int SE_VEL_X    = 340;
static const int SE_VEL_W    = 140;
static const int SE_NAME_MAXW = SE_VEL_X - 12 - 12;   // name area left of VEL
static const int SE_LOOP_X   = 500;
static const int SE_LOOP_W   = 140;
static const int SE_PLAY_X   = 660;
static const int SE_PLAY_W   = 140;
static const int SE_BACK_X   = 820;
static const int SE_BACK_W   = 125;

static const int SE_WAVE_X   = 80;                    // 2 px per overview column
static const int SE_WAVE_W   = SAMPLE_OVERVIEW_N * 2;
static const int SE_WAVE_Y   = 100;
static const int SE_WAVE_H   = 250;

static const int SE_TIME_Y   = 360;                   // view-window readout line
static const int SE_TIME_W   = 580;                   // readout area (left of KEEP LENGTH)

// [x] KEEP LENGTH checkbox, right of the readout line: other notes change the
// sample's pitch, not its length.  Its tap beats the wave's marker grab zone.
static const int SE_KEEP_X   = 680;
static const int SE_KEEP_Y   = 358;
static const int SE_KEEP_BOX = 28;
static const int SE_KEEP_HIT_X0 = 670, SE_KEEP_HIT_X1 = 900;
static const int SE_KEEP_HIT_Y0 = 352, SE_KEEP_HIT_Y1 = 392;

static const int SE_SCROLL_Y = 392;                   // drag scrollbar
static const int SE_SCROLL_H = 44;

static const int SE_ZOOM_Y   = 462;                   // bottom controls row
static const int SE_ZOOM_H   = 60;
static const int SE_ZOUT_X   = 20;                    // [ZOOM −]
static const int SE_ZIN_X    = 130;                   // [ZOOM +]
static const int SE_ZBTN_W   = 100;
// [TUNE +7.05] on the bottom row opens the TUNE dialog (full screen):
// the value, [−ST] [−] [+] [+ST] [ZERO], and [AUTO TUNE].
static const int SE_TUNE_X     = 330;
static const int SE_TUNE_W     = 380;
static const int SE_TD_VAL_Y   = 110;                 // big value
static const int SE_TD_BTN_Y   = 250;                 // −ST − + +ST ZERO
static const int SE_TD_BTN_H   = 80;
static const int SE_TD_BTN_W   = 150;
static const int SE_TD_BTN_X0  = 65;                  // then every 170
static const int SE_TD_BTN_STEP = 170;
static const int SE_TD_AUTO_X  = 280;
static const int SE_TD_AUTO_Y  = 370;
static const int SE_TD_AUTO_W  = 400;
static const int SE_TD_AUTO_H  = 80;
static const int SE_TD_MSG_Y   = 478;                 // AUTO TUNE's result line
static const uint32_t SE_AUTO_TIMEOUT_MS = 12000;

static const uint8_t SE_NO_COL = 0xFF;                // opened without a column

// Stop zooming in once the window is this small (≈2 frames per pixel).
static const uint32_t SE_MIN_VIEW_FRAMES = 1600;

class SampleEditPage {
public:
    SampleEditPage(EPD_PainterAdafruit& d, GT911_Lite& touch)
        : _d(d), _touch(touch) {}

    // col: the SFX column the sample is being edited through, or SE_NO_COL.
    // tuneCents: that column's TUNE — playback speed shifts the audition's
    // real duration, which drives the PLAY button's auto-reset.
    void open(uint8_t sampleId, const char* name, uint8_t col = SE_NO_COL,
              int16_t tuneCents = 0);
    bool poll();        // returns true when BACK tapped (caller picks where to)

private:
    void draw();
    void drawWave();
    void drawScrollbar();
    void drawKeepLen();
    void drawZoomRow();
    void drawTune();           // the TUNE button (zoom row) / the dialog's value
    void drawTuneDialog();     // the whole TUNE dialog
    void drawAutoStatus();     // the dialog's AUTO TUNE button + result line
    bool pollTuneDialog(bool down, int tx, int ty);   // its touches
    void startAudition();      // PLAY
    void stepTune(int cents);  // adjust + repaint; stored on release
    bool standalone() const { return _col == SE_NO_COL; }
    void drawHeader();
    void adopt();              // copy meta out of the arrived MsgSampleInfo
    void sendMeta();           // live-apply (SAMPLE_EDIT_SET)
    void fetchView();          // request peaks for the current window
    void zoom(int dir);        // ±1: halve/double the window, centre kept
    void scrollTo(int tx);     // centre the window on a track position
    void auditionStop();
    void drawSpectrogram();    // Fairlight Page-D waterfall (display only)
    uint32_t frameForX(int sx) const;
    int  markerX(int which) const;        // screen x of START(0)/END(1) line
    void formatTime(uint32_t frame, char* out, int outLen) const;
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    uint32_t viewLen() const { return _viewEnd - _viewStart; }

    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    uint8_t  _sampleId = 0;
    uint8_t  _col      = 0;               // column context (audition routing)
    char     _name[24] = {};
    bool     _loading  = true;
    bool     _playing  = false;

    MsgSampleInfo _info = {};             // last arrived overview + meta
    uint32_t _start = 0, _end = 0;        // trim, frames; _end 0 = end of file
    bool     _loop  = false;
    bool     _velOff = false;             // keys play it at its own level
    bool     _keepLen = false;            // KEEP LENGTH: pitch moves, length stays
    uint32_t _viewStart = 0, _viewEnd = 0;   // visible window, frames

    bool     _wasDown    = false;
    int      _dragMarker = -1;            // 0=START, 1=END while finger holds it
    bool     _dragScroll = false;
    bool     _dragWave   = false;         // panning the view by dragging the wave
    int      _dragAnchorX  = 0;           // finger x at grab
    uint32_t _dragAnchorVs = 0;           // viewStart at grab
    int16_t  _tuneCents  = 0;             // column TUNE (audition speed)
    int16_t  _sampleTune = 0;             // the sample's own TUNE, ± cents
    HoldRepeat _tuneHold;                 // TUNE −/+ held: field 0 = cents,
                                          // 1 = semitones; delta = direction
    int      _tuneStep   = 1;             // cents per repeat — grows while held
    uint32_t _keysLastMs = 0;             // last SAMPLE_EDIT_KEYS lease renewal
    uint32_t _playEndMs  = 0;             // when a non-looping audition ends
    bool     _backArmed  = false;         // BACK pressed; fires on RELEASE so the
                                          // reopened ColumnEditor (HOME shares the
                                          // corner) never sees this gesture
    int      _specMode   = 0;             // 0=off, 1=waiting for data, 2=showing
    bool     _tuneMode   = false;         // the TUNE dialog is up
    bool     _doneArmed  = false;         // its DONE pressed; fires on release
    uint8_t  _autoState  = 0;             // AUTO TUNE: 0 idle, 1 asking, 2 found,
                                          // 3 no clear pitch, 4 not ready, 5 no answer
    uint32_t _autoSentMs = 0;
    uint8_t  _autoNote   = 0;             // found: nearest note + its cents
    int8_t   _autoCents  = 0;
};

// Spectrogram popup: [SPECTRUM] button position (in the zoom row).
static const int SE_SPEC_X = 750;
static const int SE_SPEC_W = 180;

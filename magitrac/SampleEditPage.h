#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "MagiMsg.h"

// ── SampleEditPage — non-destructive sample trim/loop editor ─────────────────
//
// Opened from the ColumnEditor's EDIT button on an SFX column.  Shows a
// waveform overview fetched from the server (MSG_SAMPLE_INFO); START/END
// markers are touch-and-drag (grab near the line, drag, release to store);
// LOOP makes a held key wrap the region.  Metadata lives on the server
// (/samples/trim.txt) — the WAV never changes.
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

static const int SE_SCROLL_Y = 392;                   // drag scrollbar
static const int SE_SCROLL_H = 44;

static const int SE_ZOOM_Y   = 462;                   // bottom controls row
static const int SE_ZOOM_H   = 60;
static const int SE_ZOUT_X   = 80;                    // [ZOOM −]
static const int SE_ZIN_X    = 240;                   // [ZOOM +]
static const int SE_ZBTN_W   = 140;

// Stop zooming in once the window is this small (≈2 frames per pixel).
static const uint32_t SE_MIN_VIEW_FRAMES = 1600;

class SampleEditPage {
public:
    SampleEditPage(EPD_PainterAdafruit& d, GT911_Lite& touch)
        : _d(d), _touch(touch) {}

    void open(uint8_t sampleId, const char* name, uint8_t col);
    bool poll();        // returns true when BACK tapped (return to ColumnEditor)

private:
    void draw();
    void drawWave();
    void drawScrollbar();
    void drawZoomRow();
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
    uint32_t _viewStart = 0, _viewEnd = 0;   // visible window, frames

    bool     _wasDown    = false;
    int      _dragMarker = -1;            // 0=START, 1=END while finger holds it
    bool     _dragScroll = false;
    bool     _backArmed  = false;         // BACK pressed; fires on RELEASE so the
                                          // reopened ColumnEditor (HOME shares the
                                          // corner) never sees this gesture
    int      _specMode   = 0;             // 0=off, 1=waiting for data, 2=showing
};

// Spectrogram popup: [SPECTRUM] button position (in the zoom row).
static const int SE_SPEC_X = 700;
static const int SE_SPEC_W = 220;

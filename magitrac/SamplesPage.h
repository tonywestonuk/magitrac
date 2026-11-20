#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "MagiMsg.h"

// ── SamplesPage — the server's samples as tappable squares (960×540) ─────────
//
// One square per sample in the server's /samples folder (the same list the
// SFX column picker uses), 6 × 3 to a page.
//
//   • Tap a square    → that sample goes ON THE KEYS: the MIDI keyboard plays
//                       it at the played pitch (and, with a song running,
//                       still cues the song).  Its square stays black with
//                       white text.  Tapping it again takes it off; tapping
//                       another square moves it.  A tap makes no sound itself.
//   • Hold a square   → after SMP_HOLD_MS…
//       song stopped: poll() returns EDIT — the sketch opens SampleEditPage
//                     for that sample (trim, loop, tune, MIDI keys), and
//                     calls reopen() when its BACK is tapped.
//       song playing: the sample PLAYS, once, at its own pitch.
//   • STOP            → silences everything (the only way to end a sample
//                       whose LOOP is on).  While a song is playing it stops
//                       only what this page started.
//   • ADD             → poll() returns ADD: the sketch opens SampleRecordPage
//                       (record a new sample through the server's mic), and
//                       calls reopen() when it's done — which fetches the
//                       list again, so a new sample shows up.
//
// Nothing here touches the song: the page plays samples directly
// (SAMPLE_EDIT_PLAY / SAMPLE_EDIT_KEYS), with no column involved.
//
// While the page is open it keeps a lease with the server (SAMPLE_EDIT_PADS)
// that has every sample that fits loaded into memory, so the squares are
// polyphonic; a sample too long for that plays from the SD card, one at a
// time.  The sample on the keys is a second lease (SAMPLE_EDIT_KEYS).

static const int SMP_HDR_H    = 60;
static const int SMP_BTN_Y    = 8;
static const int SMP_BTN_H    = 44;
static const int SMP_STOP_X   = 12;
static const int SMP_STOP_W   = 150;
static const int SMP_ADD_X    = 180;
static const int SMP_ADD_W    = 150;
static const int SMP_PREV_X   = 560;
static const int SMP_PREV_W   = 80;
static const int SMP_PAGE_X   = 640;    // "2/3" readout between PREV and NEXT
static const int SMP_PAGE_W   = 95;
static const int SMP_NEXT_X   = 735;
static const int SMP_NEXT_W   = 75;
static const int SMP_HOME_X   = 820;
static const int SMP_HOME_W   = 125;

static const int SMP_COLS     = 6;
static const int SMP_ROWS     = 3;
static const int SMP_PER_PAGE = SMP_COLS * SMP_ROWS;
static const int SMP_CELL     = 144;    // square side
static const int SMP_GAP      = 12;
static const int SMP_X0       = (960 - (SMP_COLS * SMP_CELL + (SMP_COLS - 1) * SMP_GAP)) / 2;  // 18
static const int SMP_Y0       = SMP_HDR_H + SMP_GAP;                                         // 72

static const uint32_t SMP_HOLD_MS = 500;   // hold a square this long → edit it

class SamplesPage {
public:
    SamplesPage(EPD_PainterAdafruit& display, GT911_Lite& touch)
        : _d(display), _touch(touch) {}

    enum class Result : uint8_t { NONE, HOME, EDIT, ADD };

    // re-fetch the sample list + draw.  backLabel: the corner button reads
    // BACK instead of HOME (opened from the performance page — the sketch
    // decides where Result::HOME leads).  reopen() keeps it.
    void   open(bool backLabel = false);
    void   reopen();    // back from the editor or ADD: same page (list re-fetched after ADD)
    void   refetchOnReopen() { _refetch = true; }   // the editor deleted a sample
    Result poll();

    // Valid after poll() returned EDIT.
    uint8_t     editId()   const { return _editId; }
    const char* editName() const { return _editName; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    int      _page      = 0;
    bool     _backLabel = false;   // corner button says BACK, not HOME
    bool     _wasDown   = false;
    bool     _homeArmed = false;   // HOME pressed; fires on release
    int      _pressIdx  = -1;      // list index of the square under the finger
    uint32_t _pressMs   = 0;
    uint8_t  _seenState = 0xFF;    // sample-list state last drawn
    uint8_t  _keysId    = 0;       // manifest id of the sample on the keys; 0 = none
    uint32_t _leaseMs   = 0;       // last lease renewal
    uint8_t  _editId    = 0;
    bool     _refetch   = false;   // ADD / DELETE was used: fetch the list again on reopen()
    char     _editName[SAMPLE_NAME_LEN] = {};

    void draw();
    void drawHeader();
    void drawCell(int cell, bool pressed);
    void redrawCellOf(uint8_t sampleId);  // if that sample's square is on this page
    void sendLeases();                    // PADS always; KEYS if a sample is on the keys
    void resetTouch();
    int  pageCount() const;
    bool listReady() const;
    int  hitCell(int sx, int sy) const;   // 0..SMP_PER_PAGE-1, or -1
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

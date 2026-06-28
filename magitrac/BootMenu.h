#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "UIHelpers.h"

// ── BootMenu — boot button popup overlay (960×540) ────────────────────────────
//
// Drawn on top of whatever is currently on screen (no fillScreen).
// Dismiss by tapping outside the box.
//
// Row 1 (y=195): [SONG] [INSTRUMENTS] [SETTINGS]
// Row 2 (y=395): [          PAIR          ]

enum class BootMenuResult : uint8_t {
    NONE,
    SONG,
    INSTRUMENTS,
    SETTINGS,
    PAIR,
    BACKUP,
    PERFORM,
    PIXELPOST,
    DISMISSED,
};

static const int BM_BOX_X   = 130;
static const int BM_BOX_Y   = 140;
static const int BM_BOX_W   = 700;
static const int BM_BOX_H   = 390;   // extended for second row
static const int BM_BTN_Y   = BM_BOX_Y + 55;  // = 195
static const int BM_BTN_H   = 150;
static const int BM_BTN_W   = 210;
static const int BM_BTN_GAP = 15;
static const int BM_SONG_X  = BM_BOX_X + 20;                      // = 150
static const int BM_INST_X  = BM_SONG_X + BM_BTN_W + BM_BTN_GAP; // = 375
static const int BM_SETT_X  = BM_INST_X + BM_BTN_W + BM_BTN_GAP; // = 600

// Row 2: BACKUP + PERFORM + PIXELPOST + PAIR side by side
static const int BM_BTN2_Y   = BM_BTN_Y + BM_BTN_H + 25;  // = 370
static const int BM_BTN2_H   = 100;
static const int BM_BTN2_W   = 160;
static const int BM_BTN2_GAP = 15;
// Total: 4*160 + 3*15 = 685, margin = (700-685)/2 = 7
static const int BM_BACKUP_X    = BM_BOX_X + 8;                                // = 138
static const int BM_PERFORM_X   = BM_BACKUP_X  + BM_BTN2_W + BM_BTN2_GAP;     // = 313
static const int BM_PIXELPOST_X = BM_PERFORM_X + BM_BTN2_W + BM_BTN2_GAP;     // = 488
static const int BM_PAIR_X      = BM_PIXELPOST_X + BM_BTN2_W + BM_BTN2_GAP;   // = 663
static const int BM_PAIR_W      = BM_BTN2_W;

class BootMenu {
public:
    BootMenu(EPD_PainterAdafruit& display, GT911_Lite& touch);

    void open(bool noSong = false);
    void draw();

    bool isOpen()  const { return _open; }
    void dismiss()       { _open = false; }

    BootMenuResult poll();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    bool _open;
    bool _wasDown;
    bool _noSong;

    bool hitSong       (int sx, int sy) const;
    bool hitInstruments(int sx, int sy) const;
    bool hitSettings   (int sx, int sy) const;
    bool hitPair       (int sx, int sy) const;
    bool hitPerform    (int sx, int sy) const;
    bool hitBackup     (int sx, int sy) const;
    bool hitPixelpost  (int sx, int sy) const;
    bool hitInsideBox  (int sx, int sy) const;
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

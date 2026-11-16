#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "UIHelpers.h"

// ── BootMenu — main menu popup overlay (960×540) ──────────────────────────────
//
// Opened by the tracker's MENU button (the name is historical — the BOOT
// button now toggles the backlight).  open() draws it on top of whatever is
// currently on screen (no fillScreen).  The next tap closes it: on a button it
// returns that button's result, anywhere else — inside the box or out —
// DISMISSED.  SONG is greyed when no song is loaded; a tap on it then just
// dismisses.
//
// Row 1: [SONG] [INSTRUMENTS] [SETTINGS]
// Row 2: [BACKUP] [PERFORM] [POSTS]
// Row 3:          [ORGAN] [SAMPLES]
//
// PAIR lives on SettingsPage (WIFI section) — it's a once-per-device action,
// not worth a slot on the menu you hit mid-gig.

enum class BootMenuResult : uint8_t {
    NONE,
    SONG,
    INSTRUMENTS,
    SETTINGS,
    BACKUP,
    PERFORM,
    PIXELPOST,
    DRAWBAR_ORGAN,
    SAMPLES,
    DISMISSED,
};

static const int BM_BOX_X   = 130;
static const int BM_BOX_Y   = 80;
static const int BM_BOX_W   = 700;
static const int BM_BOX_H   = 450;   // three rows
static const int BM_BTN_Y   = BM_BOX_Y + 50;  // = 130
static const int BM_BTN_H   = 140;
static const int BM_BTN_W   = 210;
static const int BM_BTN_GAP = 15;
static const int BM_SONG_X  = BM_BOX_X + 20;                      // = 150
static const int BM_INST_X  = BM_SONG_X + BM_BTN_W + BM_BTN_GAP; // = 375
static const int BM_SETT_X  = BM_INST_X + BM_BTN_W + BM_BTN_GAP; // = 600

// Row 2: BACKUP + PERFORM + PIXELPOST, column-aligned under row 1
static const int BM_BTN2_Y   = BM_BTN_Y + BM_BTN_H + 22;  // = 292
static const int BM_BTN2_H   = 95;
static const int BM_BTN2_W   = BM_BTN_W;
static const int BM_BACKUP_X    = BM_SONG_X;   // = 150
static const int BM_PERFORM_X   = BM_INST_X;   // = 375
static const int BM_PIXELPOST_X = BM_SETT_X;   // = 600

// Row 3: ORGAN in the centre column (full additive drawbar organ on the
// server — it has always sat there), SAMPLES in the right column.
static const int BM_BTN3_Y  = BM_BTN2_Y + BM_BTN2_H + 18;   // = 405
static const int BM_BTN3_H  = 95;
static const int BM_ORGAN_X = BM_BOX_X + (BM_BOX_W - BM_BTN_W) / 2;   // = 375
static const int BM_ORGAN_W = BM_BTN_W;
static const int BM_SAMPLES_X = BM_SETT_X;                            // = 600
static const int BM_SAMPLES_W = BM_BTN_W;

class BootMenu {
public:
    BootMenu(EPD_PainterAdafruit& display, GT911_Lite& touch);

    void open(bool noSong = false);
    void draw();

    bool isOpen()  const { return _open; }

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
    bool hitPerform    (int sx, int sy) const;
    bool hitBackup     (int sx, int sy) const;
    bool hitPixelpost  (int sx, int sy) const;
    bool hitOrgan      (int sx, int sy) const;
    bool hitSamples    (int sx, int sy) const;
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

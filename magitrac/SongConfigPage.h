#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "UIHelpers.h"
#include "HoldRepeat.h"
#include "MidiNotePicker.h"

// ── SongConfigPage — BPM + MIDI IN + SLOT ENABLE settings ───────────────────
//
//  y=  0  ┌─ Header: "SONG SETTINGS"  [HOME x=830,w=130]  (50px)
//  y= 50  ├─ "BPM" label strip (20px)
//  y= 70  ├─ 3 BPM rows × 50px: INITIAL / MIN / MAX
//  y=220  ├─ "MIDI IN" label strip (20px)
//  y=240  ├─ 3 MIDI IN rows × 50px: CHANNEL / NOTE LOW / NOTE HIGH
//  y=390  ├─ "SLOT ENABLE" label strip (20px)
//  y=410  ├─ 4 toggle buttons: [A] [B] [C] [D]  (80px)
//  y=490  ├─ "TRANSPOSE CH" label strip (15px)
//  y=505  ├─ 16 toggle buttons: [1]..[16]  (35px)
//  y=540  └─

// ── Shared header ─────────────────────────────────────────────────────────────
static const int SC_HDR_H       = 50;
static const int SC_HOME_X      = 830;
static const int SC_HOME_W      = 130;

// ── BPM section ───────────────────────────────────────────────────────────────
static const int SC_BPM_LBL_Y   = SC_HDR_H;
static const int SC_BPM_LBL_H   = 20;
static const int SC_BPM_Y       = SC_BPM_LBL_Y + SC_BPM_LBL_H;   // 70
static const int SC_BPM_ROW_H   = 50;
static const int SC_NUM_BPM     = 3;

static const int SC_ROW_LABEL_X = 30;
static const int SC_ROW_VAL_X   = 590;
static const int SC_ROW_VAL_W   = 130;
static const int SC_ROW_MINUS_X = 740;
static const int SC_ROW_PLUS_X  = 825;
static const int SC_ROW_BTN_W   = 75;
static const int SC_ROW_BTN_H   = 38;

// ── MIDI IN section (same row height as BPM) ─────────────────────────────────
static const int SC_MI_LBL_Y    = SC_BPM_Y + SC_NUM_BPM * SC_BPM_ROW_H;  // 220
static const int SC_MI_LBL_H    = 20;
static const int SC_MI_Y        = SC_MI_LBL_Y + SC_MI_LBL_H;              // 240
static const int SC_MI_ROW_H    = 50;
static const int SC_NUM_MI      = 3;

// ── SLOT ENABLE section ───────────────────────────────────────────────────────
static const int SC_SL_LBL_Y    = SC_MI_Y + SC_NUM_MI * SC_MI_ROW_H;     // 390
static const int SC_SL_LBL_H    = 20;
static const int SC_SL_Y        = SC_SL_LBL_Y + SC_SL_LBL_H;             // 410
static const int SC_SL_H        = 80;
static const int SC_SL_BTN_W    = 160;
static const int SC_SL_BTN_H    = 60;
static const int SC_SL_GAP      = 20;
static const int SC_SL_COUNT    = 4;
// Centred horizontally: total = 4*160 + 3*20 = 700, margin = (960-700)/2 = 130
static const int SC_SL_X0       = 130;

// ── TRANSPOSE CHANNELS section ────────────────────────────────────────────────
// 16 toggle buttons (1..16) — one per MIDI channel.  On = follows performer
// transpose; off = fixed pitch.  Default disables ch 10 (drums).
static const int SC_TR_LBL_Y    = SC_SL_Y + SC_SL_H;                     // 490
static const int SC_TR_LBL_H    = 15;
static const int SC_TR_Y        = SC_TR_LBL_Y + SC_TR_LBL_H;             // 505
static const int SC_TR_H        = 35;
static const int SC_TR_BTN_W    = 50;
static const int SC_TR_BTN_H    = 30;
static const int SC_TR_GAP      = 4;
static const int SC_TR_COUNT    = 16;
// Centred: total = 16*50 + 15*4 = 860, margin = (960-860)/2 = 50
static const int SC_TR_X0       = 50;

// ── Class ─────────────────────────────────────────────────────────────────────

class SongConfigPage {
public:
    SongConfigPage(EPD_PainterAdafruit& display, GT911_Lite& touch,
                   Song& song);

    void open();
    void draw();
    bool poll();         // returns true when HOME tapped

    bool patchPending() const { return _patchPending; }
    void clearPatch()         { _patchPending = false; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;
    bool                 _wasDown;
    bool                 _patchPending; // set whenever any song config value changes

    HoldRepeat _hold;  // type: 1=BPM, 2=MIDIIN

    // Note picker overlay — opened by tapping NOTE LOW / NOTE HIGH row.
    MidiNotePicker _picker;
    int            _pickerField = -1;  // -1 = inactive, 1 = NOTE LOW, 2 = NOTE HIGH

    void drawPage0();
    void drawHeader();
    void drawBpmSection();
    void drawBpmRow(int field);
    void drawMidiInSection();
    void drawMidiInRow(int field);
    void drawSlotSection();
    void drawTransposeSection();

    void bpmLabel(int field, char* out) const;
    void bpmValue(int field, char* out) const;
    void adjustBpm(int field, int delta);

    void midiInLabel(int field, char* out) const;
    void midiInValue(int field, char* out) const;
    void adjustMidiIn(int field, int delta);

    void fireHeld();
    bool hitHome      (int sx, int sy) const;
    int  hitRowMinus  (int baseY, int rowH, int numRows, int sx, int sy) const;
    int  hitRowPlus   (int baseY, int rowH, int numRows, int sx, int sy) const;
    int  hitSlot      (int sx, int sy) const;
    int  hitTranspose (int sx, int sy) const;

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

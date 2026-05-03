#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "UIHelpers.h"
#include "Constants.h"
#include "NumpadPopup.h"
#include "HoldRepeat.h"
#include <I2C_BM8563.h>

// ── SettingsPage — global device settings ────────────────────────────────────
//
//  y=  0  ┌─ Header: "SETTINGS"  [HOME x=830,w=130]  (50px) ────────────────┐
//  y= 50  ├─ "DATE / TIME" label strip (20px) ──────────────────────────────┤
//  y= 70  ├─ Time:  HH:MM:SS   (120px, tappable) ──────────────────────────┤
//  y=190  ├─ Date:  DD/MM/YY   (120px, tappable) ──────────────────────────┤
//  y=310  ├─ "MIDI" label strip (20px) ──────────────────────────────────────┤
//  y=330  ├─ MAX BANK row (50px)  [−] value [+] ─────────────────────────────┤
//  y=380  ├─ MAX PROGRAM row (50px) ──────────────────────────────────────────┤
//  y=430  └─ ────────────────────────────────────────────────────────────────┘

static const int SP_W          = 960;
static const int SP_H          = 540;

static const int SP_HDR_H      = 50;
static const int SP_HOME_X     = 830;
static const int SP_HOME_W     = 130;

static const int SP_LBL_Y      = SP_HDR_H;              // 50
static const int SP_LBL_H      = 20;

static const int SP_TIME_Y     = SP_LBL_Y + SP_LBL_H;  // 70
static const int SP_ROW_H      = 120;
static const int SP_DATE_Y     = SP_TIME_Y + SP_ROW_H;  // 190

// Label sits left, value sits right
static const int SP_LABEL_X    = 40;
static const int SP_VAL_X      = 300;

// ── MIDI section ──────────────────────────────────────────────────────────────
static const int SP_MIDI_LBL_Y = SP_DATE_Y + SP_ROW_H;   // 310
static const int SP_MIDI_LBL_H = 20;
static const int SP_MIDI_Y     = SP_MIDI_LBL_Y + SP_MIDI_LBL_H;  // 330
static const int SP_MIDI_ROW_H = 50;
static const int SP_NUM_MIDI   = 2;    // MAX BANK, MAX PROGRAM

static const int SP_MIDI_VAL_X   = 590;
static const int SP_MIDI_VAL_W   = 130;
static const int SP_MIDI_MINUS_X = 740;
static const int SP_MIDI_PLUS_X  = 825;
static const int SP_MIDI_BTN_W   = 75;
static const int SP_MIDI_BTN_H   = 38;

// ── Global MIDI limits (persisted via NVS) ────────────────────────────────────
// Loaded on boot, saved when changed on SettingsPage.
// ColumnEditor uses these for program/bank wrap-around.
extern uint8_t gMaxBank;       // 1-based display (stored as 0-based internally)
extern uint8_t gMaxProgram;    // 1-based display (stored as 0-based internally)

void loadMidiLimits();         // call once in setup()
void saveMidiLimits();         // called by SettingsPage after change

class SettingsPage {
public:
    SettingsPage(EPD_PainterAdafruit& display, GT911_Lite& touch,
                 I2C_BM8563& rtc);

    void open();
    void draw();
    bool poll();      // returns true when HOME tapped

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    I2C_BM8563&          _rtc;
    NumpadPopup          _numpad;

    bool    _wasDown;
    uint8_t _editing;   // 0=none, 1=time, 2=date

    // Current RTC values
    int16_t _year;
    int8_t  _month, _day, _hour, _minute, _second;

    HoldRepeat _hold;  // MIDI rows hold-repeat

    void drawHeader();
    void drawSectionLabel();
    void drawTimeRow();
    void drawDateRow();
    void drawMidiSection();
    void drawMidiRow(int field);
    void adjustMidi(int field, int delta);
    void fireMidiHeld();
    void readRTC();
    void applyNumpadResult();

    bool hitHome(int sx, int sy) const;
    bool hitTime(int sx, int sy) const;
    bool hitDate(int sx, int sy) const;
    int  hitMidiMinus(int sx, int sy) const;
    int  hitMidiPlus(int sx, int sy) const;
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
};

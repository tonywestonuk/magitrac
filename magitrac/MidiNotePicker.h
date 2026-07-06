#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"

// ── MidiNotePicker — full-screen popup for selecting a MIDI note (0..127) ────
//
// Mirrors the NOTE/OCTAVE button strip from the main note editor, but
// operates on a raw MIDI byte rather than a Song cell.  Used by
// SongConfigPage for the NOTE LOW / NOTE HIGH filter values.
//
// Layout (960×540):
//   y=  0  ┌─ Header: title centred, CANCEL + OK in corners (60px)
//   y= 60  ├─ Big current-value display: "C-5 (60)"                (80px)
//   y=140  ├─ "NOTE" label strip (24px)
//   y=164  ├─ 12 semitone buttons × 80px × 100px                 (100px)
//   y=264  ├─ "OCTAVE" label strip (24px)
//   y=288  ├─ 11 octave buttons (0..10) × 80px × 100px           (100px)
//   y=388  ├─ Optional hint text                                  (32px)
//   y=440  ├─ CANCEL / OK action buttons                         (100px)
//   y=540  └─
//
// Caller flow:
//   _picker.open(initialNote, minVal, maxVal, "MIDI-IN NOTE LOW");
//   ...draw + poll until accepted()/cancelled() is true...
//   if (_picker.accepted()) value = _picker.value();

class MidiNotePicker {
public:
    MidiNotePicker(EPD_PainterAdafruit& display, GT911_Lite& touch);

    void open(uint8_t initialNote, uint8_t minVal, uint8_t maxVal,
              const char* title);
    void draw();
    // Returns true when the popup should close (OK or CANCEL tapped).
    bool poll();

    bool    accepted() const { return _accepted; }
    uint8_t value()    const { return _value; }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    bool     _open       = false;
    bool     _wasDown    = false;
    bool     _accepted   = false;

    uint8_t  _value      = 60;
    uint8_t  _semitone   = 0;
    uint8_t  _octave     = 5;
    uint8_t  _min        = 0;
    uint8_t  _max        = 127;
    char     _title[32]  = {};

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    void drawHeader();
    void drawValue();
    void drawNoteButtons();
    void drawOctaveButtons();
    void drawHint();
    void drawActions();
    void redrawValueAndButtons();

    // Returns -1 on miss.
    int hitSemitone(int sx, int sy) const;
    int hitOctave(int sx, int sy) const;
};

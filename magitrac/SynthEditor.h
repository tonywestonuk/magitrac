#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "UIHelpers.h"
#include "OrganSynth.h"

// ── SynthEditor — the organ's SYNTH sound, full screen (960×540) ─────────────
//
// Shown by OrganPage in place of its own body while SYNTH is being edited, so
// the server stays in organ mode and the keyboard keeps sounding the patch.
// Header: starting-point presets + BACK.  Left: the two oscillators' waveform
// buttons and their sliders; right: filter and envelope.  Every change is
// sent to the server at once (ORGAN_OP_SYNTH); OrganPage writes it into the
// song when touch() reports CHANGED.
//
// Horizontal sliders, one setting each, with the value in real units beside
// them (Hz, ms, cents), so the screen reads without the manual.

class SynthEditor {
public:
    explicit SynthEditor(EPD_PainterAdafruit& display) : _d(display) {}

    enum class Result : uint8_t { NONE, CHANGED, BACK };

    // Edit `params` (ORGAN_SYNTH_PARAMS bytes, owned by the caller) and draw
    // the whole screen.
    void   open(uint8_t* params);
    void   draw();
    // One touch sample in screen coordinates (down = finger on the glass).
    Result touch(bool down, int sx, int sy);

private:
    EPD_PainterAdafruit& _d;
    uint8_t* _p       = nullptr;
    bool     _wasDown = false;
    int      _drag    = -1;     // setting locked under the finger, or -1

    void drawHeader();
    void drawWaves(int osc);
    void drawSlider(int param);
    void drawTitles();
    void set(int param, int value);
    int  sliderAt(int sx, int sy) const;
    int  sliderValueAt(int param, int sx) const;
};

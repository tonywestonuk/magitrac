#pragma once
#include "EPD_Painter_Adafruit.h"
#include "Constants.h"

// ── Shared button drawing utility ─────────────────────────────────────────────
//
// Draws a filled rectangle with a border and a centred label.
// Colour palette: 0=white 1=ltgrey 2=dkgrey 3=black (EPD 4-shade).
// ts: Adafruit textSize — char cell is (ts*6) × (ts*8) pixels.

void uiButton(EPD_PainterAdafruit& d,
              int x, int y, int w, int h,
              const char* label,
              uint8_t bg, uint8_t fg,
              int ts = 3);

// Two-line variant — line1 on top, line2 below with a small gap.
void uiButton2Line(EPD_PainterAdafruit& d,
                   int x, int y, int w, int h,
                   const char* line1, const char* line2,
                   uint8_t bg, uint8_t fg,
                   int ts = 3);

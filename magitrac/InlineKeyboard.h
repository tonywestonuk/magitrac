#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"

// ── InlineKeyboard — a QWERTY keyboard drawn under a text box on the page ────
//
// Unlike KeyboardPopup (a full screen with its own one-line field) this only
// draws the keys, in the bottom part of the screen from y = top; the page
// owns the text, the box and the cursor.  The page feeds it touch-downs:
// key(sx, sy) returns what was hit, already in the right case.  SHIFT and 123
// are handled here (they redraw the keys and return KEY_NONE).
//
//   Q W E R T Y U I O P
//    A S D F G H J K L
//   [SHIFT] Z X C V B N M [ BKSP ]
//   [123][CLEAR][<][  SPACE  ][>][DONE]
//
// SHIFT is one-shot: it capitalises the next letter.  123 swaps the letter
// rows for digits and punctuation.

class InlineKeyboard {
public:
    enum : int { KEY_NONE = 0, KEY_BKSP, KEY_LEFT, KEY_RIGHT, KEY_DONE, KEY_CLEAR };
    static const int ROW_H = 76;
    static const int HEIGHT = 4 * ROW_H;

    explicit InlineKeyboard(EPD_PainterAdafruit& d) : _d(d) {}

    void reset(bool shift) { _sym = false; _shift = shift; }
    void draw(int top);
    // Printable char (>= ' '), one of the KEY_ codes, or KEY_NONE.
    int  key(int sx, int sy);
    int  top() const { return _top; }

private:
    EPD_PainterAdafruit& _d;
    int  _top   = 0;
    bool _sym   = false;
    bool _shift = false;

    void drawKey(int x, int y, int w, const char* label, bool inverted = false);
    void drawArrow(int x, int y, int w, bool left);
};

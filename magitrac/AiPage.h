#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "InlineKeyboard.h"
#include "MagiMsg.h"

// ── AiPage — ask Claude to write / change the song ──────────────────────────
//
// MENU → AI (shown only when the client is on an external WiFi network: the
// server needs the internet).  Tap the prompt box to type a request: a
// keyboard opens under the box, and tapping in the box moves the cursor.
// DONE puts the status and the buttons back; SEND it.
// The server sends its song and the request to the Claude API, streams the
// answer, and — if it parses — makes it the active song (unsaved: SAVE on the
// Songs page writes it).  The new song reaches this client the usual way
// (MSG_ACTIVE_SONG_CHANGED → pull), whatever page is open.  UNDO puts the song
// it replaced back.  The prompt is kept between visits.
//
//   [ AI                                              BACK ]
//   ┌ prompt (tap to type) ──────────────────────────────┐
//   └────────────────────────────────────────────────────┘
//     status title / message                    or  the keyboard
//   [ SEND ]  [ UNDO ]   or  [ CANCEL ] while working    while typing

static const int AIP_HDR_H  = 60;
static const int AIP_BACK_X = 820;
static const int AIP_BACK_W = 125;

class AiPage {
public:
    AiPage(EPD_PainterAdafruit& display, GT911_Lite& touch)
        : _d(display), _touch(touch), _keyboard(display) {}

    enum class Result : uint8_t { NONE, BACK };

    void   open();
    Result poll();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    InlineKeyboard       _keyboard;

    bool     _typing    = false;
    int      _cursor    = 0;           // index into sPrompt
    int      _heldKey   = 0;           // BKSP / arrows repeat while held
    uint32_t _heldMs    = 0;
    bool     _repeating = false;
    bool     _wasDown   = false;
    bool     _backArmed = false;
    bool     _waiting   = false;       // START/UNDO sent, no status yet
    uint32_t _waitMs    = 0;
    MsgAiStatus _st     = {};          // last status from the server
    char     _note[64]  = {};          // a client-side message (not connected...)

    static char sPrompt[AI_PROMPT_LEN];

    bool busy() const {
        return _st.state == AI_ST_SENDING || _st.state == AI_ST_THINKING ||
               _st.state == AI_ST_WRITING;
    }
    void draw();
    void drawHeader();
    void drawPrompt();
    void drawStatus();
    void drawButtons();
    void startTyping(int sx, int sy);
    void stopTyping();
    void typeKey(int k);
    int  cursorAt(int sx, int sy) const;
    int  labels(const char* out[2]) const;
    int  buttonAt(int sx, int sy) const;
    void send(uint8_t op);
    void rawToScreen(int rx, int ry, int& sx, int& sy) const { sx = ry; sy = 540 - rx; }
};

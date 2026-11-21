#include "AiPage.h"
#include "ServerPairing.h"
#include "UIHelpers.h"
#include "Constants.h"
#include "MagiLink.h"
#include <string.h>

char AiPage::sPrompt[AI_PROMPT_LEN] = "Create a happy song in G";

static const uint32_t AIP_REPLY_MS = 5000;   // a request with no status by then failed

// Prompt box: the same place whether or not the keyboard is up under it.
static const int PR_X = 16, PR_Y = AIP_HDR_H + 4, PR_W = 928, PR_H = 166;
static const int PR_TS = 3, PR_CHARS = (PR_W - 32) / (6 * PR_TS), PR_LINES = 5;
static const int PR_TX = PR_X + 16, PR_TY = PR_Y + 10, PR_LH = 8 * PR_TS + 6, PR_CW = 6 * PR_TS;
static const int KB_Y  = PR_Y + PR_H + 4;

static const uint32_t KEY_REPEAT_DELAY_MS = 450, KEY_REPEAT_MS = 90;

// Status lines.
static const int ST_Y      = PR_Y + PR_H + 16;
static const int ST_TITLE_Y = ST_Y + 18;
static const int ST_MSG_Y  = ST_TITLE_Y + 44;
static const int ST_H      = 400 - ST_Y;
static const int ST_CHARS  = 74;

// Buttons.
static const int BT_Y = 410, BT_H = 110, BT_W = 300, BT_GAP = 40;
static const int BT_X0 = (960 - (2 * BT_W + BT_GAP)) / 2;
static const int BT1_X = (960 - BT_W) / 2;

void AiPage::open() {
    _typing    = false;
    _heldKey   = 0;
    _wasDown   = _touch.isTouched;
    _backArmed = false;
    _note[0]   = '\0';
    _st        = MsgAiStatus();
    _st.state  = AI_ST_IDLE;
    MsgAiStatus stale;
    while (gServerPairing.takeAiStatus(stale)) {}
    _d.clear();
    draw();
    _d.paint();
    send(AI_OP_STATUS);   // a request may still be running from an earlier visit
}

// ── Drawing ──────────────────────────────────────────────────────────────────

static void centred(EPD_PainterAdafruit& d, const char* s, int y, int ts) {
    d.setTextSize(ts);
    d.setCursor((960 - (int)strlen(s) * 6 * ts) / 2, y);
    d.print(s);
}

// Word-wrap s into at most maxLines lines of maxChars, drawn from (x, y).
static void wrapped(EPD_PainterAdafruit& d, const char* s, int x, int y, int ts,
                    int maxChars, int maxLines, bool centre) {
    d.setTextSize(ts);
    char line[128];
    int  lineH = 8 * ts + 6;
    for (int ln = 0; ln < maxLines && *s; ln++) {
        int n = (int)strlen(s);
        int take = n;
        if (n > maxChars) {
            take = maxChars;
            while (take > 0 && s[take] != ' ') take--;
            if (take == 0) take = maxChars;
        }
        if (take > (int)sizeof(line) - 1) take = sizeof(line) - 1;
        memcpy(line, s, take);
        line[take] = '\0';
        int lx = centre ? (960 - take * 6 * ts) / 2 : x;
        d.setCursor(lx, y + ln * lineH);
        d.print(line);
        s += take;
        while (*s == ' ') s++;
    }
}

// The prompt's lines, broken as wrapped() breaks them, so the cursor and a
// tap can be placed exactly.  Returns how many lines the text needs (it may
// be more than maxOut; only the first maxOut are filled in).
static int layoutLines(const char* s, int maxChars, int* start, int* len, int maxOut) {
    const char* base = s;
    int lines = 0;
    while (*s) {
        int n = (int)strlen(s);
        int take = n;
        if (n > maxChars) {
            take = maxChars;
            while (take > 0 && s[take] != ' ') take--;
            if (take == 0) take = maxChars;
        }
        if (lines < maxOut) { start[lines] = (int)(s - base); len[lines] = take; }
        lines++;
        s += take;
        while (*s == ' ') s++;
    }
    if (lines == 0 && maxOut > 0) { start[0] = 0; len[0] = 0; }
    return lines;
}

void AiPage::drawHeader() {
    _d.fillRect(0, 0, 960, AIP_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(16, (AIP_HDR_H - 24) / 2);
    _d.print("AI - ask Claude");
    uiButton(_d, AIP_BACK_X, 8, AIP_BACK_W, 44, "BACK", COL_WHITE, COL_BLACK, 3);
}

void AiPage::drawPrompt() {
    _d.fillRect(PR_X, PR_Y, PR_W, PR_H, COL_WHITE);
    _d.drawRect(PR_X,     PR_Y,     PR_W,     PR_H,     COL_BLACK);
    _d.drawRect(PR_X + 1, PR_Y + 1, PR_W - 2, PR_H - 2, COL_BLACK);
    if (!sPrompt[0] && !_typing) {
        _d.setTextColor(COL_DKGREY);
        wrapped(_d, "Tap here and type what you want, e.g. \"Create a happy song in G\"",
                PR_TX, PR_TY, PR_TS, PR_CHARS, PR_LINES, false);
        return;
    }
    int start[PR_LINES], len[PR_LINES];
    int n = layoutLines(sPrompt, PR_CHARS, start, len, PR_LINES);
    if (n > PR_LINES) n = PR_LINES;
    char line[PR_CHARS + 1];
    _d.setTextSize(PR_TS);
    _d.setTextColor(busy() ? COL_DKGREY : COL_BLACK);
    for (int i = 0; i < n; i++) {
        memcpy(line, sPrompt + start[i], len[i]);
        line[len[i]] = '\0';
        _d.setCursor(PR_TX, PR_TY + i * PR_LH);
        _d.print(line);
    }
    if (!_typing) return;
    int ln = 0;
    for (int i = 1; i < n; i++) if (start[i] <= _cursor) ln = i;
    int col = _cursor - start[ln];
    if (col > PR_CHARS) col = PR_CHARS;
    _d.fillRect(PR_TX + col * PR_CW - 2, PR_TY + ln * PR_LH - 3, 3, 8 * PR_TS + 6, COL_BLACK);
}

// The text index nearest a tap in the box.
int AiPage::cursorAt(int sx, int sy) const {
    int start[PR_LINES], len[PR_LINES];
    int n = layoutLines(sPrompt, PR_CHARS, start, len, PR_LINES);
    if (n < 1) return 0;
    if (n > PR_LINES) n = PR_LINES;
    int ln = (sy - PR_TY) / PR_LH;
    if (sy < PR_TY) ln = 0;
    if (ln >= n) return (int)strlen(sPrompt);
    int col = (sx - PR_TX + PR_CW / 2) / PR_CW;
    if (col < 0) col = 0;
    if (col > len[ln]) col = len[ln];
    return start[ln] + col;
}

void AiPage::drawStatus() {
    _d.fillRect(0, ST_Y, 960, ST_H, COL_WHITE);
    char title[48] = "";
    char msg[200]  = "";
    switch (_st.state) {
        case AI_ST_IDLE:
            snprintf(title, sizeof(title), "Ready");
            snprintf(msg, sizeof(msg), "SEND asks Claude for a new version of this song. It replaces the song "
                        "in memory - SAVE it on the Songs page if you like it, or UNDO.");
            break;
        case AI_ST_SENDING:
            snprintf(title, sizeof(title), "Sending the song...");
            break;
        case AI_ST_THINKING:
            snprintf(title, sizeof(title), "Claude is thinking  %lus", (unsigned long)_st.seconds);
            snprintf(msg, sizeof(msg), "This can take a minute or two");
            break;
        case AI_ST_WRITING:
            snprintf(title, sizeof(title), "Writing the song  %lus", (unsigned long)_st.seconds);
            snprintf(msg, sizeof(msg), "%.1f KB received", _st.bytes / 1024.0);
            break;
        case AI_ST_DONE:
            snprintf(title, sizeof(title), "Done - the new song is loaded");
            snprintf(msg, sizeof(msg), "%s", _st.text);
            break;
        case AI_ST_FAILED:
            snprintf(title, sizeof(title), "That didn't work");
            snprintf(msg, sizeof(msg), "%s", _st.text);
            break;
        case AI_ST_UNDONE:
            snprintf(title, sizeof(title), "Undone");
            snprintf(msg, sizeof(msg), "%s", _st.text);
            break;
    }
    if (_waiting) { snprintf(title, sizeof(title), "Asking the server..."); msg[0] = '\0'; }
    _d.setTextColor(COL_BLACK);
    centred(_d, title, ST_TITLE_Y, 4);
    _d.setTextColor(_note[0] ? COL_BLACK : COL_DKGREY);
    wrapped(_d, _note[0] ? _note : msg, 0, ST_MSG_Y, 2, ST_CHARS, 4, true);
}

int AiPage::labels(const char* out[2]) const {
    if (_waiting) return 0;
    if (busy()) { out[0] = "CANCEL"; return 1; }
    out[0] = "SEND";
    if (_st.canUndo) { out[1] = "UNDO"; return 2; }
    return 1;
}

void AiPage::drawButtons() {
    _d.fillRect(0, BT_Y - 4, 960, BT_H + 8, COL_WHITE);
    const char* lab[2];
    int n = labels(lab);
    if (n == 1) {
        bool primary = !busy();
        uiButton(_d, BT1_X, BT_Y, BT_W, BT_H, lab[0],
                 primary ? COL_BLACK : COL_WHITE, primary ? COL_WHITE : COL_BLACK, 4);
    } else if (n == 2) {
        uiButton(_d, BT_X0, BT_Y, BT_W, BT_H, lab[0], COL_BLACK, COL_WHITE, 4);
        uiButton(_d, BT_X0 + BT_W + BT_GAP, BT_Y, BT_W, BT_H, lab[1], COL_WHITE, COL_BLACK, 4);
    }
}

void AiPage::draw() {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    drawPrompt();
    if (_typing) {
        _keyboard.draw(KB_Y);
    } else {
        drawStatus();
        drawButtons();
    }
}

int AiPage::buttonAt(int sx, int sy) const {
    if (sy < BT_Y || sy >= BT_Y + BT_H) return -1;
    const char* lab[2];
    int n = labels(lab);
    if (n == 1) return (sx >= BT1_X && sx < BT1_X + BT_W) ? 0 : -1;
    for (int i = 0; i < n; i++) {
        int x = BT_X0 + i * (BT_W + BT_GAP);
        if (sx >= x && sx < x + BT_W) return i;
    }
    return -1;
}

// ── Typing ───────────────────────────────────────────────────────────────────

void AiPage::startTyping(int sx, int sy) {
    _typing  = true;
    _heldKey = 0;
    _keyboard.reset(sPrompt[0] == '\0');
    _cursor  = cursorAt(sx, sy);
    _d.clear();
    draw();
    _d.paint();
}

void AiPage::stopTyping() {
    _typing  = false;
    _heldKey = 0;
    _d.clear();
    draw();
    _d.paint();
}

void AiPage::typeKey(int k) {
    int n = (int)strlen(sPrompt);
    if (_cursor > n) _cursor = n;
    switch (k) {
        case InlineKeyboard::KEY_NONE:  return;
        case InlineKeyboard::KEY_DONE:  stopTyping(); return;
        case InlineKeyboard::KEY_LEFT:  if (_cursor > 0) _cursor--; break;
        case InlineKeyboard::KEY_RIGHT: if (_cursor < n) _cursor++; break;
        case InlineKeyboard::KEY_CLEAR: sPrompt[0] = '\0'; _cursor = 0; break;
        case InlineKeyboard::KEY_BKSP:
            if (_cursor > 0) {
                memmove(sPrompt + _cursor - 1, sPrompt + _cursor, n - _cursor + 1);
                _cursor--;
            }
            break;
        default: {
            if (n >= (int)sizeof(sPrompt) - 1) return;
            memmove(sPrompt + _cursor + 1, sPrompt + _cursor, n - _cursor + 1);
            sPrompt[_cursor] = (char)k;
            int dummyS[1], dummyL[1];
            if (layoutLines(sPrompt, PR_CHARS, dummyS, dummyL, 1) > PR_LINES) {   // box full
                memmove(sPrompt + _cursor, sPrompt + _cursor + 1, n - _cursor + 1);
                return;
            }
            _cursor++;
        }
    }
    drawPrompt();
    _d.paintLater();
}

// ── Requests ─────────────────────────────────────────────────────────────────

void AiPage::send(uint8_t op) {
    _note[0] = '\0';
    if (!gMagiLink.isConnected() || !gServerPairing.sendAiRequest(op, sPrompt)) {
        strcpy(_note, "Not connected to the server");
        _waiting = false;
    } else if (op == AI_OP_START || op == AI_OP_UNDO) {
        _waiting = true;
        _waitMs  = millis();
    }
    draw();
    _d.paintLater();
}

// ── Poll ─────────────────────────────────────────────────────────────────────

AiPage::Result AiPage::poll() {
    MsgAiStatus s;
    if (gServerPairing.takeAiStatus(s)) {
        bool full = (s.state != _st.state) || _waiting || s.canUndo != _st.canUndo;
        _st      = s;
        _waiting = false;
        _note[0] = '\0';
        if (!_typing) {   // the status isn't on screen while typing; DONE draws it
            if (full) draw();
            else      drawStatus();
            _d.paintLater();
        }
    }
    if (_waiting && (uint32_t)(millis() - _waitMs) >= AIP_REPLY_MS) {
        _waiting = false;
        strcpy(_note, "No reply from the server");
        if (!_typing) { draw(); _d.paintLater(); }
    }

    // BKSP and the arrows repeat while held (read() reports nothing while the
    // finger stays put).
    if (_typing && _heldKey && _wasDown) {
        uint32_t now = millis();
        if ((uint32_t)(now - _heldMs) >= (_repeating ? KEY_REPEAT_MS : KEY_REPEAT_DELAY_MS)) {
            _heldMs    = now;
            _repeating = true;
            typeKey(_heldKey);
        }
    }

    if (!_touch.read()) return Result::NONE;
    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (down && !_wasDown) {
        _wasDown   = true;
        _backArmed = (sy < AIP_HDR_H && sx >= AIP_BACK_X);
        bool inBox = sx >= PR_X && sx < PR_X + PR_W && sy >= PR_Y && sy < PR_Y + PR_H;
        if (_typing) {
            if (inBox) {
                _cursor = cursorAt(sx, sy);
                drawPrompt();
                _d.paintLater();
                return Result::NONE;
            }
            int k = _keyboard.key(sx, sy);
            if (k == InlineKeyboard::KEY_BKSP || k == InlineKeyboard::KEY_LEFT ||
                k == InlineKeyboard::KEY_RIGHT) {
                _heldKey   = k;
                _heldMs    = millis();
                _repeating = false;
            }
            typeKey(k);
            return Result::NONE;
        }
        if (_waiting) return Result::NONE;
        if (!busy() && inBox) {
            startTyping(sx, sy);
            return Result::NONE;
        }
        int b = buttonAt(sx, sy);
        if (b < 0) return Result::NONE;
        if (busy())                send(AI_OP_CANCEL);
        else if (b == 0)           send(AI_OP_START);
        else if (_st.canUndo)      send(AI_OP_UNDO);
    } else if (!down && _wasDown) {
        _wasDown = false;
        _heldKey = 0;
        if (_backArmed && sy < AIP_HDR_H && sx >= AIP_BACK_X) {
            _backArmed = false;
            // A request keeps running on the server; its song still arrives.
            return Result::BACK;
        }
        _backArmed = false;
    }
    return Result::NONE;
}

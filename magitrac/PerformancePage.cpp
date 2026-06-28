#include "PerformancePage.h"
#include "ServerPairing.h"
#include <string.h>
#include <stdio.h>

static const uint32_t FLASH_INTERVAL_MS = 400;  // toggle rate for queued pad

PerformancePage::PerformancePage(EPD_PainterAdafruit& display,
                                 GT911_Lite& touch, Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _wasDown(false)
    , _playingPattern(0)
    , _queuedPattern(-1)
    , _flashState(false)
    , _lastFlashMs(0)
    , _patchPending(false)
    , _holdTarget(HoldTarget::NONE)
    , _holdStartMs(0)
    , _holdFired(false)
    , _holdPadIdx(-1)
    , _editing(false)
    , _keyboard(display, touch)
    , _kbdPadIdx(-1)
{}

void PerformancePage::open(uint8_t currentPattern, bool stopped) {
    _wasDown        = _touch.isTouched;
    _playingPattern = stopped ? (int8_t)-1 : (int8_t)currentPattern;
    _queuedPattern  = -1;
    _flashState     = false;
    _lastFlashMs    = millis();
    _holdTarget     = HoldTarget::NONE;
    _holdFired      = false;
    _holdPadIdx     = -1;
    _editing        = false;
    _patchPending   = false;
    _kbdPadIdx      = -1;
    _lightArmedBtn  = -1;
    _titleOverride[0] = '\0';
}

void PerformancePage::setTitleOverride(const char* name) {
    if (!name || !*name) {
        _titleOverride[0] = '\0';
        return;
    }
    strncpy(_titleOverride, name, sizeof(_titleOverride) - 1);
    _titleOverride[sizeof(_titleOverride) - 1] = '\0';
}

void PerformancePage::draw() {
    _d.fillScreen(COL_WHITE);
    if (_editing) {
        drawEditView();
    } else {
        drawPerfHeader();
        drawPads();
        drawLightStrip();
    }
}

void PerformancePage::setPlayingPattern(uint8_t pat) {
    if (_editing) return;  // don't update pads while editing
    if ((int8_t)pat == _playingPattern) return;
    int8_t oldPlaying = _playingPattern;
    _playingPattern = (int8_t)pat;

    // If the queued block just became the playing block, clear the queue
    if (_queuedPattern >= 0 && (uint8_t)_queuedPattern == pat) {
        int8_t oldQueued = _queuedPattern;
        _queuedPattern = -1;
        if (oldQueued < PP_PAD_COUNT) drawPad(oldQueued);
    }

    if (oldPlaying >= 0 && oldPlaying < PP_PAD_COUNT) drawPad(oldPlaying);
    if (pat < PP_PAD_COUNT) drawPad(pat);
    _d.paintLater();
}

PerformancePage::Result PerformancePage::poll() {
    if (_editing) {
        // Keyboard popup takes over input
        if (_keyboard.isOpen()) {
            if (_keyboard.poll()) {
                if (_keyboard.isDone() && _kbdPadIdx >= 0) {
                    _patchPending = true;
                    drawEditRow(_kbdPadIdx);
                }
                _kbdPadIdx = -1;
                _d.fillScreen(COL_WHITE);
                drawEditView();
                _d.paint();
            }
            return Result::NONE;
        }
        if (pollEdit()) {
            _editing = false;
            _wasDown = _touch.isTouched;
            _holdTarget = HoldTarget::NONE;
            _d.fillScreen(COL_WHITE);
            drawPerfHeader();
            drawPads();
            drawLightStrip();
            _d.paint();
        }
        return Result::NONE;
    }
    return pollPerf();
}

// ═════════════════════════════════════════════════════════════════════════════
// ── Performance pad view ─────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

PerformancePage::Result PerformancePage::pollPerf() {
    // Flash animation for queued pad
    if (_queuedPattern >= 0) {
        uint32_t now = millis();
        if (now - _lastFlashMs >= FLASH_INTERVAL_MS) {
            _lastFlashMs = now;
            _flashState = !_flashState;
            drawPad(_queuedPattern);
            _d.paintLater();
        }
    }

    // Anti-phantom validation: redraw the rejected-touch counter when it
    // changes, rate-limited so the redraw (an EPD refresh) can't drive its own
    // phantom→reject→redraw storm.
    if (_touch.rejectedTouches != _lastRejShown &&
        millis() - _lastRejDrawMs >= 2000) {
        _lastRejDrawMs = millis();
        drawRej();
        _d.paintLater();
    }

    // Refresh touch state once per poll.  read() returns true only on a
    // state change (rising/falling); when false, _touch.isTouched still
    // holds the last-seen value, which is what the hold timer needs.
    bool hadEvent = _touch.read();
    bool down     = _touch.isTouched;

    // ── Hold timer — gated on `down` so a slow main-loop tick that misses
    //   the finger-up between polls can't time out a quick pad tap into a
    //   STOP.  Previously this fired on elapsed-time alone; a >1s stall
    //   anywhere in the main loop would convert a 200ms tap into a hold. ──
    if (down && _holdTarget != HoldTarget::NONE && !_holdFired) {
        uint32_t needed = (_holdTarget == HoldTarget::PAD)
                              ? PP_PAD_HOLD_MS
                              : PP_HOLD_MS;
        if (millis() - _holdStartMs >= needed) {
            _holdFired = true;
            if (_holdTarget == HoldTarget::HOME) {
                _holdTarget = HoldTarget::NONE;
                return Result::HOME;
            }
            if (_holdTarget == HoldTarget::SETLIST) {
                _holdTarget = HoldTarget::NONE;
                return Result::SETLIST;
            }
            if (_holdTarget == HoldTarget::EDIT) {
                _editing = true;
                _wasDown = true;
                _holdTarget = HoldTarget::NONE;
                _d.fillScreen(COL_WHITE);
                drawEditView();
                _d.paint();
                return Result::NONE;
            }
            if (_holdTarget == HoldTarget::PAD) {
                // 1-second pad hold → STOP playback and dark all pads.
                // Keep _holdTarget = PAD so the eventual finger-up release
                // is swallowed by the falling-edge handler (no pad tap).
                if (gServerPairing.isPaired()) {
                    gServerPairing.sendControl(MSG_STOP);
                }
                int8_t oldPlaying = _playingPattern;
                int8_t oldQueued  = _queuedPattern;
                _playingPattern = -1;
                _queuedPattern  = -1;
                if (oldPlaying >= 0 && oldPlaying < PP_PAD_COUNT) drawPad(oldPlaying);
                if (oldQueued  >= 0 && oldQueued  < PP_PAD_COUNT) drawPad(oldQueued);
                _d.paintLater();
                return Result::NONE;
            }
        }
    }

    if (!hadEvent) return Result::NONE;

    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    bool rising  = (down  && !_wasDown);
    bool falling = (!down && _wasDown);
    _wasDown = down;

    // ── Start hold on finger-down over header buttons or a pad ───────────────
    if (rising) {
        if (hitHome(sx, sy)) {
            _holdTarget  = HoldTarget::HOME;
            _holdStartMs = millis();
            _holdFired   = false;
        } else if (hitEdit(sx, sy)) {
            _holdTarget  = HoldTarget::EDIT;
            _holdStartMs = millis();
            _holdFired   = false;
        } else if (hitSetlist(sx, sy)) {
            _holdTarget  = HoldTarget::SETLIST;
            _holdStartMs = millis();
            _holdFired   = false;
        } else if (hitLightBtn(sx, sy) >= 0) {
            // Arm on press; FX/RELEASE fire on release (tap), WHITE is momentary.
            _lightArmedBtn = (int8_t)hitLightBtn(sx, sy);
            _holdTarget    = HoldTarget::NONE;
            if (_lightArmedBtn == 2 && gServerPairing.isPaired()) {
                gServerPairing.sendPixelpostOverride(PPO_WHITE_ON);   // ramp to white
                _lightManual = true;
                drawLightStrip();        // light the button while held
                _d.paintLater();
            }
        } else {
            int padIdx = hitPad(sx, sy);
            if (padIdx >= 0 && padIdx < _song.numPatterns) {
                _holdTarget  = HoldTarget::PAD;
                _holdPadIdx  = (int8_t)padIdx;
                _holdStartMs = millis();
                _holdFired   = false;
            } else {
                _holdTarget = HoldTarget::NONE;
            }
        }
    }

    // ── Finger-up ────────────────────────────────────────────────────────────
    if (falling) {
        // Light strip release.  WHITE (momentary) fires OFF regardless of where
        // the finger lifts, so a drift-off can't leave the lights stuck on.  The
        // tap buttons (FX/RELEASE) only fire if released over the same button.
        if (_lightArmedBtn >= 0) {
            int lb = _lightArmedBtn;
            _lightArmedBtn = -1;
            _holdTarget = HoldTarget::NONE;
            if (lb == 2) {
                if (gServerPairing.isPaired()) gServerPairing.sendPixelpostOverride(PPO_WHITE_OFF);
                drawLightStrip();        // un-light the button
                _d.paintLater();
            } else if (hitLightBtn(sx, sy) == lb) {
                fireLightButton(lb);
            }
            return Result::NONE;
        }
        HoldTarget tgt   = _holdTarget;
        bool       fired = _holdFired;
        _holdTarget = HoldTarget::NONE;
        _holdFired  = false;

        // Header buttons: any release (held or not) is swallowed — the held
        // action already fired, and a brief tap is intentionally a no-op.
        if (tgt == HoldTarget::HOME || tgt == HoldTarget::EDIT
            || tgt == HoldTarget::SETLIST) {
            return Result::NONE;
        }

        // Pad hold already fired (STOP applied): swallow the release so the
        // pad-tap path below doesn't immediately seek+play again.
        if (tgt == HoldTarget::PAD && fired) {
            return Result::NONE;
        }

        // Quick pad release (< PP_PAD_HOLD_MS) → existing pad-tap behaviour.
        // Re-detect the pad at the release point to preserve drag-to-pad.
        int padIdx = hitPad(sx, sy);
        if (padIdx >= 0 && padIdx < _song.numPatterns) {
            uint8_t pat = (uint8_t)padIdx;

            // If the server is stopped (e.g. fresh song just loaded from the
            // setlist), any pad press seeks to that block AND starts playback.
            // Pad mode (IMM/QUE) is only meaningful while running.
            if (gServerPairing.isPaired() && !gServerPairing.serverPlaying()) {
                gServerPairing.sendSeek(pat, 0);
                gServerPairing.sendControl(MSG_PLAY);
                int8_t oldPlaying = _playingPattern;
                _playingPattern = (int8_t)pat;
                if (oldPlaying >= 0 && oldPlaying < PP_PAD_COUNT) drawPad(oldPlaying);
                drawPad(padIdx);
                _d.paintLater();
                return Result::NONE;
            }

            uint8_t padMode = _song.perfPads[padIdx].mode;

            if (padMode == 0) {
                // IMMEDIATE
                if (gServerPairing.isPaired()) {
                    gServerPairing.sendSeek(pat, 0);
                }
                int8_t oldPlaying = _playingPattern;
                _playingPattern = (int8_t)pat;
                if (oldPlaying >= 0 && oldPlaying < PP_PAD_COUNT) drawPad(oldPlaying);
                drawPad(padIdx);
                _d.paintLater();
            } else {
                // QUEUED
                if (_queuedPattern == (int8_t)pat) {
                    gServerPairing.sendCancelQueue();
                    _queuedPattern = -1;
                    drawPad(padIdx);
                    _d.paintLater();
                } else {
                    if (gServerPairing.isPaired()) {
                        gServerPairing.sendQueueBlock(pat);
                    }
                    int8_t oldQueued = _queuedPattern;
                    _queuedPattern = (int8_t)pat;
                    _flashState    = true;
                    _lastFlashMs   = millis();
                    if (oldQueued >= 0 && oldQueued < PP_PAD_COUNT) drawPad(oldQueued);
                    drawPad(padIdx);
                    _d.paintLater();
                }
            }
        }
    }

    return Result::NONE;
}

void PerformancePage::drawPerfHeader() {
    _d.fillRect(0, 0, 960, PP_HDR_H, COL_BLACK);

    // Title — squeezed to the left to make room for SETLIST/EDIT/HOME.
    // _titleOverride wins when set (e.g. setlist entry display name).
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    const char* title = _titleOverride[0] ? _titleOverride : "PERFORMANCE";
    int maxChars = (PP_SETLIST_X - 20) / 18;   // = 28 at textSize 3
    char buf[40];
    int n = (int)strlen(title);
    if (n > maxChars) n = maxChars;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf, title, n);
    buf[n] = '\0';
    int tw = n * 18;
    _d.setCursor((PP_SETLIST_X - tw) / 2, (PP_HDR_H - 24) / 2);
    _d.print(buf);

    uiButton(_d, PP_SETLIST_X, 0, PP_SETLIST_W, PP_HDR_H, "SETLIST",
             COL_BLACK, COL_WHITE, 3);
    uiButton(_d, PP_EDIT_X, 0, PP_EDIT_W, PP_HDR_H, "EDIT",
             COL_BLACK, COL_WHITE, 3);
    uiButton(_d, PP_HOME_X, 0, PP_HOME_W, PP_HDR_H, "HOME",
             COL_BLACK, COL_WHITE, 3);
    drawRej();
}

// Anti-phantom validation counter — small "R:N" at the far-left of the header
// showing how many single-frame phantom touches the driver has rejected.
// Temporary; remove with the rest of the diagnostics once proven in the field.
void PerformancePage::drawRej() {
    _lastRejShown = _touch.rejectedTouches;
    _d.fillRect(0, 0, 70, 16, COL_BLACK);
    _d.setTextSize(1);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(4, 4);
    char buf[16];
    snprintf(buf, sizeof(buf), "R:%lu", (unsigned long)_lastRejShown);
    _d.print(buf);
}

void PerformancePage::drawPads() {
    for (int i = 0; i < PP_PAD_COUNT; i++) {
        drawPad(i);
    }
}

void PerformancePage::drawPad(int idx) {
    int x = padX(idx);
    int y = padY(idx);

    bool isPlaying  = (idx == (int)_playingPattern);
    bool isQueued   = (idx == (int)_queuedPattern);
    bool isDisabled = (idx >= _song.numPatterns);

    uint8_t bg, fg;

    if (isDisabled) {
        bg = COL_WHITE;
        fg = COL_DKGREY;
    } else if (isPlaying) {
        bg = COL_BLACK;
        fg = COL_WHITE;
    } else if (isQueued) {
        if (_flashState) {
            bg = COL_BLACK;
            fg = COL_WHITE;
        } else {
            bg = COL_WHITE;
            fg = COL_BLACK;
        }
    } else {
        bg = COL_WHITE;
        fg = COL_BLACK;
    }

    _d.fillRect(x, y, PP_PAD_W, PP_PAD_H, bg);
    _d.drawRect(x, y, PP_PAD_W, PP_PAD_H, fg);
    _d.drawRect(x + 1, y + 1, PP_PAD_W - 2, PP_PAD_H - 2, fg);

    // Pad label — use custom name if set, otherwise block number
    const PerfPadConfig& cfg = _song.perfPads[idx];
    bool hasName = (cfg.name[0] != '\0');

    // Block number — large
    char numStr[4];
    snprintf(numStr, sizeof(numStr), "%d", idx + 1);
    _d.setTextSize(5);
    _d.setTextColor(fg);
    int charW = 5 * 6;
    int charH = 5 * 8;
    int labelW = (int)strlen(numStr) * charW;

    if (hasName) {
        // Number above, name below
        _d.setCursor(x + (PP_PAD_W - labelW) / 2, y + 25);
        _d.print(numStr);

        _d.setTextSize(3);
        _d.setTextColor(fg);
        int maxChars = (PP_PAD_W - 20) / 18;
        char truncName[PERF_PAD_NAME_LEN];
        strncpy(truncName, cfg.name, maxChars);
        truncName[maxChars] = '\0';
        int nw = (int)strlen(truncName) * 18;
        _d.setCursor(x + (PP_PAD_W - nw) / 2, y + PP_PAD_H - 55);
        _d.print(truncName);
    } else {
        // Just the number, centred
        _d.setCursor(x + (PP_PAD_W - labelW) / 2, y + (PP_PAD_H - charH) / 2);
        _d.print(numStr);
    }

    // Mode indicator — small text in bottom-right corner
    if (!isDisabled && idx < _song.numPatterns) {
        const char* modeStr = (cfg.mode == 0) ? "IMM" : "QUE";
        _d.setTextSize(1);
        _d.setTextColor(fg);
        _d.setCursor(x + PP_PAD_W - 22, y + PP_PAD_H - 12);
        _d.print(modeStr);
    }
}

int PerformancePage::padX(int idx) const {
    int col = idx % PP_PAD_COLS;
    return PP_PAD_MARGIN + col * (PP_PAD_W + PP_PAD_GAP);
}

int PerformancePage::padY(int idx) const {
    int row = idx / PP_PAD_COLS;
    return PP_PAD_Y0 + row * (PP_PAD_H + PP_PAD_ROW_GAP);
}

bool PerformancePage::hitHome(int sx, int sy) const {
    return sx >= PP_HOME_X && sx < PP_HOME_X + PP_HOME_W
        && sy >= PP_BTN_Y && sy < PP_BTN_Y + PP_BTN_H;
}

bool PerformancePage::hitEdit(int sx, int sy) const {
    return sx >= PP_EDIT_X && sx < PP_EDIT_X + PP_EDIT_W
        && sy >= PP_BTN_Y && sy < PP_BTN_Y + PP_BTN_H;
}

bool PerformancePage::hitSetlist(int sx, int sy) const {
    return sx >= PP_SETLIST_X && sx < PP_SETLIST_X + PP_SETLIST_W
        && sy >= PP_BTN_Y && sy < PP_BTN_Y + PP_BTN_H;
}

int PerformancePage::hitPad(int sx, int sy) const {
    for (int i = 0; i < PP_PAD_COUNT; i++) {
        int x = padX(i);
        int y = padY(i);
        if (sx >= x && sx < x + PP_PAD_W && sy >= y && sy < y + PP_PAD_H)
            return i;
    }
    return -1;
}

// ── Light-control strip (PixelPost) ──────────────────────────────────────────
// PREV / NEXT cycle the PixelPost effect, POW blacks the lights out — all three
// grab control away from the PXL POST track.  RELEASE hands it back.  The first
// three are always live (when paired); RELEASE is only enabled once we've
// grabbed control, so it reads as "nothing to release" until then.

void PerformancePage::drawLightStrip() {
    _d.fillRect(0, PP_LIGHT_Y, 960, PP_LIGHT_H, COL_WHITE);
    bool paired = gServerPairing.isPaired();
    static const char* const LABELS[PP_LIGHT_COUNT] = { "< FX", "FX >", "WHITE", "RELEASE" };
    for (int i = 0; i < PP_LIGHT_COUNT; i++) {
        int x = PP_LIGHT_MARGIN + i * (PP_LIGHT_W + PP_LIGHT_GAP);
        uint8_t bg, fg;
        if (!paired)        { bg = COL_WHITE;  fg = COL_DKGREY; }   // no server → inert
        else if (i == 2)    {                                       // WHITE — momentary
            if (_lightArmedBtn == 2) { bg = COL_BLACK;  fg = COL_WHITE; }  // held = lit
            else                     { bg = COL_LTGREY; fg = COL_BLACK; }
        }
        else if (i == 3)    {                                       // RELEASE
            if (_lightManual) { bg = COL_LTGREY; fg = COL_BLACK; }
            else              { bg = COL_WHITE;  fg = COL_DKGREY; } // nothing to release yet
        }
        else                { bg = COL_LTGREY; fg = COL_BLACK; }    // PREV / NEXT
        uiButton(_d, x, PP_LIGHT_Y + 2, PP_LIGHT_W, PP_LIGHT_H - 4, LABELS[i], bg, fg, 3);
    }
}

int PerformancePage::hitLightBtn(int sx, int sy) const {
    if (sy < PP_LIGHT_Y || sy >= PP_LIGHT_Y + PP_LIGHT_H) return -1;
    for (int i = 0; i < PP_LIGHT_COUNT; i++) {
        int x = PP_LIGHT_MARGIN + i * (PP_LIGHT_W + PP_LIGHT_GAP);
        if (sx >= x && sx < x + PP_LIGHT_W) return i;
    }
    return -1;
}

void PerformancePage::fireLightButton(int btn) {
    if (!gServerPairing.isPaired()) return;
    switch (btn) {
        case 0: gServerPairing.sendPixelpostOverride(PPO_PREV);    _lightManual = true;  break;
        case 1: gServerPairing.sendPixelpostOverride(PPO_NEXT);    _lightManual = true;  break;
        case 3: gServerPairing.sendPixelpostOverride(PPO_RELEASE); _lightManual = false; break;
        default: return;   // case 2 (WHITE) is momentary — handled inline in pollPerf
    }
    drawLightStrip();
    _d.paintLater();
}

// ═════════════════════════════════════════════════════════════════════════════
// ── Pad edit view ────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

bool PerformancePage::pollEdit() {
    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    bool falling = (!down && _wasDown);
    if (down && !_wasDown) _wasDown = true;
    if (!down) _wasDown = false;

    if (!falling) return false;

    // BACK button
    if (hitBack(sx, sy)) return true;

    // Name tap — open keyboard
    int nameIdx = hitEditName(sx, sy);
    if (nameIdx >= 0 && nameIdx < PERF_PAD_COUNT) {
        _kbdPadIdx = (int8_t)nameIdx;
        _keyboard.open(_song.perfPads[nameIdx].name, PERF_PAD_NAME_LEN - 1);
        _d.fillScreen(COL_WHITE);
        _keyboard.draw();
        _d.paint();
        return false;
    }

    // Mode toggle
    int modeIdx = hitEditMode(sx, sy);
    if (modeIdx >= 0 && modeIdx < PERF_PAD_COUNT) {
        _song.perfPads[modeIdx].mode = (_song.perfPads[modeIdx].mode == 0) ? 1 : 0;
        _patchPending = true;
        drawEditRow(modeIdx);
        _d.paintLater();
        return false;
    }

    return false;
}

void PerformancePage::drawEditView() {
    drawEditHeader();
    for (int i = 0; i < PERF_PAD_COUNT; i++) {
        drawEditRow(i);
    }
}

void PerformancePage::drawEditHeader() {
    _d.fillRect(0, 0, 960, PP_HDR_H, COL_BLACK);

    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    int tw = 9 * 18;  // "PAD SETUP"
    _d.setCursor((PP_HOME_X - tw) / 2, (PP_HDR_H - 24) / 2);
    _d.print("PAD SETUP");

    uiButton(_d, PP_HOME_X, 0, PP_HOME_W, PP_HDR_H, "HOME",
             COL_BLACK, COL_WHITE, 3);
}

void PerformancePage::drawEditRow(int idx) {
    int y = PE_ROW_Y0 + idx * PE_ROW_H;
    bool disabled = (idx >= _song.numPatterns);

    // Clear row area
    _d.fillRect(0, y, 960, PE_ROW_H, COL_WHITE);

    uint8_t fg = disabled ? COL_DKGREY : COL_BLACK;

    // Pad number
    char numStr[4];
    snprintf(numStr, sizeof(numStr), "%d", idx + 1);
    _d.setTextSize(3);
    _d.setTextColor(fg);
    _d.setCursor(PE_NUM_X + (PE_NUM_W - (int)strlen(numStr) * 18) / 2,
                 y + (PE_ROW_H - 24) / 2);
    _d.print(numStr);

    // Name field — bordered box with text
    uint8_t nameBg = disabled ? COL_WHITE : COL_WHITE;
    uint8_t nameFg = disabled ? COL_DKGREY : COL_BLACK;
    _d.fillRect(PE_NAME_X, y + 5, PE_NAME_W, PE_ROW_H - 10, nameBg);
    _d.drawRect(PE_NAME_X, y + 5, PE_NAME_W, PE_ROW_H - 10, nameFg);

    const char* displayName;
    char defaultName[4];
    if (_song.perfPads[idx].name[0] != '\0') {
        displayName = _song.perfPads[idx].name;
    } else {
        snprintf(defaultName, sizeof(defaultName), "%d", idx + 1);
        displayName = defaultName;
    }
    _d.setTextSize(3);
    _d.setTextColor(nameFg);
    _d.setCursor(PE_NAME_X + 10, y + (PE_ROW_H - 24) / 2);
    _d.print(displayName);

    // Mode toggle button
    const char* modeLabel = (_song.perfPads[idx].mode == 0) ? "IMMEDIATE" : "QUEUED";
    uint8_t modeBg = disabled ? COL_WHITE : COL_LTGREY;
    uint8_t modeFg = disabled ? COL_DKGREY : COL_BLACK;
    uiButton(_d, PE_MODE_X, y + 5, PE_MODE_W, PE_ROW_H - 10,
             modeLabel, modeBg, modeFg, 2);

    // Separator line
    _d.drawFastHLine(20, y + PE_ROW_H - 1, 920, COL_LTGREY);
}

bool PerformancePage::hitBack(int sx, int sy) const {
    return sx >= PP_HOME_X && sx < PP_HOME_X + PP_HOME_W
        && sy >= PP_BTN_Y && sy < PP_BTN_Y + PP_BTN_H;
}

int PerformancePage::hitEditName(int sx, int sy) const {
    if (sx < PE_NAME_X || sx >= PE_NAME_X + PE_NAME_W) return -1;
    int row = (sy - PE_ROW_Y0) / PE_ROW_H;
    if (row < 0 || row >= PERF_PAD_COUNT) return -1;
    int rowY = PE_ROW_Y0 + row * PE_ROW_H;
    if (sy < rowY + 5 || sy >= rowY + PE_ROW_H - 5) return -1;
    return row;
}

int PerformancePage::hitEditMode(int sx, int sy) const {
    if (sx < PE_MODE_X || sx >= PE_MODE_X + PE_MODE_W) return -1;
    int row = (sy - PE_ROW_Y0) / PE_ROW_H;
    if (row < 0 || row >= PERF_PAD_COUNT) return -1;
    int rowY = PE_ROW_Y0 + row * PE_ROW_H;
    if (sy < rowY + 5 || sy >= rowY + PE_ROW_H - 5) return -1;
    return row;
}

// ═════════════════════════════════════════════════════════════════════════════

void PerformancePage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

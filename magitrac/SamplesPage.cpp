#include "SamplesPage.h"
#include "ServerPairing.h"
#include "UIHelpers.h"
#include "Constants.h"
#include "SampleDebug.h"   // [SMPDBG] timing lines (off unless SAMPLE_DEBUG is 1)
#include "MagiLink.h"      // gMagiLink.isConnected() — gates the lease renewals
#include <string.h>

// Name text inside a square: size 3 (18×24 px per character) — the same size
// as the STOP button's label.
static const int SMP_TEXT_SIZE  = 3;
static const int SMP_CHAR_W     = 6 * SMP_TEXT_SIZE;   // 18
static const int SMP_LINE_CHARS = 6;    // 6 × 18 = 108 px of the 144
static const int SMP_MAX_LINES  = 5;    // 5 × 28 − 4 = 136 px of the 144
static const int SMP_LINE_H     = 8 * SMP_TEXT_SIZE + 4;   // 24 px glyph + 4 px leading

void SamplesPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

// Filename → display name: drop the ".wav".
static void displayName(const char* fname, char* out, int outLen) {
    strncpy(out, fname ? fname : "", outLen - 1);
    out[outLen - 1] = '\0';
    int n = (int)strlen(out);
    if (n > 4 && strcasecmp(out + n - 4, ".wav") == 0) out[n - 4] = '\0';
}

// Break `name` into up to SMP_MAX_LINES lines of at most SMP_LINE_CHARS,
// preferring to break at a space, or just after a '_' or '-'.  Whatever does
// not fit in SMP_MAX_LINES is dropped.  Returns the line count.
static int wrapName(const char* name, char lines[][SMP_LINE_CHARS + 1]) {
    int n = 0;
    const char* p = name;
    while (n < SMP_MAX_LINES) {
        while (*p == ' ') p++;
        int len = (int)strlen(p);
        if (len == 0) break;
        int take = len;
        if (take > SMP_LINE_CHARS) {
            take = SMP_LINE_CHARS;
            for (int i = SMP_LINE_CHARS; i > 2; i--) {      // latest break wins
                if (p[i] == ' ' || p[i - 1] == '_' || p[i - 1] == '-') {
                    take = i;
                    break;
                }
            }
        }
        int keep = take;                         // trailing spaces would
        while (keep > 0 && p[keep - 1] == ' ') keep--;   // skew the centring
        memcpy(lines[n], p, keep);
        lines[n][keep] = '\0';
        n++;
        p += take;
    }
    return n;
}

void SamplesPage::resetTouch() {
    _wasDown   = _touch.isTouched;
    _homeArmed = false;
    _pressIdx  = -1;
}

// The page's leases with the server (see SamplesPage.h).  Skipped while the
// link is down: a send into a dead link blocks for seconds.
void SamplesPage::sendLeases() {
    _leaseMs = millis();
    if (!gMagiLink.isConnected()) return;
    gServerPairing.sendSampleEdit(SAMPLE_EDIT_PADS, 1);
    if (_keysId) gServerPairing.sendSampleEdit(SAMPLE_EDIT_KEYS, _keysId);
}

void SamplesPage::redrawCellOf(uint8_t sampleId) {
    if (!sampleId) return;
    for (int cell = 0; cell < SMP_PER_PAGE; cell++) {
        const SampleCacheEntry* e = gServerPairing.sampleListEntry(_page * SMP_PER_PAGE + cell);
        if (e && e->id == sampleId) { drawCell(cell, false); return; }
    }
}

void SamplesPage::open(bool backLabel) {
    _page      = 0;
    _backLabel = backLabel;
    _keysId    = 0;
    resetTouch();
    // The server rescans /samples on every request, so files copied on since
    // the last visit show up.
    gServerPairing.requestSampleList();
    sendLeases();
    _d.clear();
    draw();
    _d.paint();
}

void SamplesPage::reopen() {
    _keysId = 0;           // the editor had the keys, and gave them up on BACK
    resetTouch();
    if (_refetch) {        // ADD may have put a new sample in /samples
        _refetch = false;
        gServerPairing.requestSampleList();
    }
    sendLeases();
    _d.clear();
    draw();
    _d.paint();
}

bool SamplesPage::listReady() const {
    return gServerPairing.sampleListState() == SampleListState::READY;
}

int SamplesPage::pageCount() const {
    int n = listReady() ? gServerPairing.sampleListCount() : 0;
    int pages = (n + SMP_PER_PAGE - 1) / SMP_PER_PAGE;
    return pages > 0 ? pages : 1;
}

void SamplesPage::drawHeader() {
    _d.fillRect(0, 0, 960, SMP_HDR_H, COL_BLACK);
    uiButton(_d, SMP_STOP_X, SMP_BTN_Y, SMP_STOP_W, SMP_BTN_H, "STOP",
             COL_WHITE, COL_BLACK, 3);
    uiButton(_d, SMP_ADD_X, SMP_BTN_Y, SMP_ADD_W, SMP_BTN_H, "ADD",
             COL_WHITE, COL_BLACK, 3);

    int pages = pageCount();
    if (pages > 1) {
        uiButton(_d, SMP_PREV_X, SMP_BTN_Y, SMP_PREV_W, SMP_BTN_H, "<",
                 COL_WHITE, (_page > 0) ? COL_BLACK : COL_DKGREY, 3);
        uiButton(_d, SMP_NEXT_X, SMP_BTN_Y, SMP_NEXT_W, SMP_BTN_H, ">",
                 COL_WHITE, (_page < pages - 1) ? COL_BLACK : COL_DKGREY, 3);
        char t[12];
        snprintf(t, sizeof(t), "%d/%d", _page + 1, pages);
        _d.setTextSize(3);
        _d.setTextColor(COL_WHITE);
        _d.setCursor(SMP_PAGE_X + (SMP_PAGE_W - (int)strlen(t) * 18) / 2,
                     SMP_BTN_Y + (SMP_BTN_H - 24) / 2);
        _d.print(t);
    }

    uiButton(_d, SMP_HOME_X, SMP_BTN_Y, SMP_HOME_W, SMP_BTN_H,
             _backLabel ? "BACK" : "HOME", COL_WHITE, COL_BLACK, 3);
}

void SamplesPage::drawCell(int cell, bool pressed) {
    int idx = _page * SMP_PER_PAGE + cell;
    const SampleCacheEntry* e = gServerPairing.sampleListEntry(idx);
    if (!e) return;

    int x = SMP_X0 + (cell % SMP_COLS) * (SMP_CELL + SMP_GAP);
    int y = SMP_Y0 + (cell / SMP_COLS) * (SMP_CELL + SMP_GAP);
    // Black with white text = the sample on the keys.  A finger on a square
    // shows as the opposite of however it normally looks.
    bool black = pressed != (_keysId != 0 && e->id == _keysId);
    uint8_t bg = black ? COL_BLACK : COL_WHITE;
    uint8_t fg = black ? COL_WHITE : COL_BLACK;

    _d.fillRect(x, y, SMP_CELL, SMP_CELL, bg);
    _d.drawRect(x,     y,     SMP_CELL,     SMP_CELL,     COL_BLACK);
    _d.drawRect(x + 1, y + 1, SMP_CELL - 2, SMP_CELL - 2, COL_BLACK);

    char name[SAMPLE_NAME_LEN];
    displayName(e->name, name, sizeof(name));
    char lines[SMP_MAX_LINES][SMP_LINE_CHARS + 1];
    int  n = wrapName(name, lines);

    _d.setTextSize(SMP_TEXT_SIZE);
    _d.setTextColor(fg);
    int ty = y + (SMP_CELL - (n * SMP_LINE_H - 4)) / 2;
    for (int i = 0; i < n; i++) {
        _d.setCursor(x + (SMP_CELL - (int)strlen(lines[i]) * SMP_CHAR_W) / 2,
                     ty + i * SMP_LINE_H);
        _d.print(lines[i]);
    }
}

void SamplesPage::draw() {
    _seenState = (uint8_t)gServerPairing.sampleListState();
    _d.fillScreen(COL_WHITE);

    int count = listReady() ? gServerPairing.sampleListCount() : 0;
    int pages = pageCount();
    if (_page >= pages) _page = pages - 1;
    if (_page < 0)      _page = 0;
    drawHeader();

    if (count == 0) {
        SampleListState st = gServerPairing.sampleListState();
        const char* msg =
            (st == SampleListState::READY)   ? "No samples found in /samples" :
            (st == SampleListState::WAITING ||
             st == SampleListState::PARTIAL) ? "Loading samples..."
                                             : "Not connected";
        _d.setTextSize(3);
        _d.setTextColor(COL_BLACK);
        _d.setCursor((960 - (int)strlen(msg) * 18) / 2, 270);
        _d.print(msg);
        return;
    }

    for (int cell = 0; cell < SMP_PER_PAGE; cell++) {
        if (_page * SMP_PER_PAGE + cell >= count) break;
        drawCell(cell, false);
    }
}

// The gap between squares belongs to the nearer one — no dead strips.
int SamplesPage::hitCell(int sx, int sy) const {
    const int pitch = SMP_CELL + SMP_GAP;
    int rx = sx - SMP_X0 + SMP_GAP / 2;
    int ry = sy - SMP_Y0 + SMP_GAP / 2;
    if (rx < 0 || ry < 0) return -1;
    int cx = rx / pitch, cy = ry / pitch;
    if (cx >= SMP_COLS || cy >= SMP_ROWS) return -1;
    return cy * SMP_COLS + cx;
}

SamplesPage::Result SamplesPage::poll() {
    // The list arrives a page at a time after open(); repaint when its state
    // moves (WAITING → … → READY).
    if ((uint8_t)gServerPairing.sampleListState() != _seenState) {
        _pressIdx = -1;
        draw();
        _d.paintLater();
    }

    // Keep the leases alive.
    if ((uint32_t)(millis() - _leaseMs) >= SAMPLE_KEYS_RENEW_MS) sendLeases();

    // read() is true only on a state change; isTouched holds the last-seen
    // state either way, which is what the hold timer needs.
    bool ev   = _touch.read();
    bool down = _touch.isTouched;

    // ── Hold a square ────────────────────────────────────────────────────────
    // Gated on the finger still being down (checked after read(), so a
    // release is seen first): a stalled loop must not turn a tap into a hold.
    if (down && _pressIdx >= 0 &&
        (uint32_t)(millis() - _pressMs) >= SMP_HOLD_MS) {
        int cell = _pressIdx - _page * SMP_PER_PAGE;
        const SampleCacheEntry* e = gServerPairing.sampleListEntry(_pressIdx);
        _pressIdx = -1;
        if (e) {
            if (gServerPairing.serverPlaying()) {
                // Song playing: the held square plays its sample, once.  (No
                // editor here — it makes the server scan the WAV off the SD
                // card, which would starve a sample the song is streaming.)
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_PLAY, e->id);
                drawCell(cell, false);       // finger feedback off; the release does nothing more
                _d.paintLater();
                return Result::NONE;
            }
            // Song stopped: edit the sample.  The editor opens quiet and
            // takes the keys.
            gServerPairing.sendSampleEdit(SAMPLE_EDIT_STOP, 0);
            _keysId = 0;
            _editId = e->id;
            displayName(e->name, _editName, sizeof(_editName));
            SMPDBG("pad held %lu ms -> edit id=%u '%s'",
                   (unsigned long)(millis() - _pressMs), (unsigned)_editId, _editName);
            return Result::EDIT;
        }
    }

    if (!ev) return Result::NONE;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (down && !_wasDown) {
        // ── Press ────────────────────────────────────────────────────────────
        _wasDown   = true;
        _homeArmed = false;
        _pressIdx  = -1;

        if (sy < SMP_HDR_H) {
            int pages = pageCount();
            if (sx >= SMP_HOME_X) {
                _homeArmed = true;       // fires on release (see below)
            } else if (sx >= SMP_STOP_X && sx < SMP_STOP_X + SMP_STOP_W) {
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_STOP, 0);
            } else if (sx >= SMP_ADD_X && sx < SMP_ADD_X + SMP_ADD_W) {
                // Off to record a new sample.  Silence the page and give up
                // the leases: the server frees the squares' samples, which
                // leaves room for the take (30 s of it is 1.9 MB).
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_STOP, 0);
                if (_keysId) gServerPairing.sendSampleEdit(SAMPLE_EDIT_KEYS, 0);
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_PADS, 0);
                _keysId  = 0;
                _refetch = true;
                return Result::ADD;
            } else if (pages > 1 && sx >= SMP_PREV_X && sx < SMP_PREV_X + SMP_PREV_W) {
                if (_page > 0) { _page--; draw(); _d.paintLater(); }
            } else if (pages > 1 && sx >= SMP_NEXT_X && sx < SMP_NEXT_X + SMP_NEXT_W) {
                if (_page < pages - 1) { _page++; draw(); _d.paintLater(); }
            }
            return Result::NONE;
        }

        if (!listReady()) return Result::NONE;
        int cell = hitCell(sx, sy);
        if (cell < 0) return Result::NONE;
        int idx = _page * SMP_PER_PAGE + cell;
        const SampleCacheEntry* e = gServerPairing.sampleListEntry(idx);
        if (!e) return Result::NONE;

        // Nothing sounds on a press: a tap (decided on release) puts the
        // sample on the keys, a hold edits or plays it.
        _pressIdx = idx;
        _pressMs  = millis();
        drawCell(cell, true);
        _d.paintLater();

    } else if (down && _wasDown) {
        // ── Drag: sliding off the square cancels the hold ────────────────────
        if (_pressIdx >= 0 &&
            hitCell(sx, sy) != _pressIdx - _page * SMP_PER_PAGE) {
            drawCell(_pressIdx - _page * SMP_PER_PAGE, false);
            _pressIdx = -1;
            _d.paintLater();
        }

    } else if (!down && _wasDown) {
        // ── Release ──────────────────────────────────────────────────────────
        _wasDown = false;
        if (_pressIdx >= 0) {
            // A tap: this sample goes on the keys — or comes off, if it was
            // already there.
            int cell = _pressIdx - _page * SMP_PER_PAGE;
            const SampleCacheEntry* e = gServerPairing.sampleListEntry(_pressIdx);
            _pressIdx = -1;
            if (e) {
                uint8_t was = _keysId;
                _keysId = (was == e->id) ? 0 : e->id;
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_KEYS, _keysId);
                _leaseMs = millis();
                redrawCellOf(was);           // the square that had the keys goes white
            }
            drawCell(cell, false);           // this one shows where it now stands
            _d.paintLater();
        } else if (_homeArmed) {
            // On release, so the tracker that is drawn next never sees the
            // tail of this tap.
            _homeArmed = false;
            if (sy < SMP_HDR_H && sx >= SMP_HOME_X) {
                // Leaving: silence the page's sounds, give the keys back, and
                // let the server stop holding the squares' samples for us.
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_STOP, 0);
                if (_keysId) gServerPairing.sendSampleEdit(SAMPLE_EDIT_KEYS, 0);
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_PADS, 0);
                _keysId = 0;
                return Result::HOME;
            }
        }
    }
    return Result::NONE;
}

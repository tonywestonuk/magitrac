#include "ColumnNoteEditorPage.h"
#include "UIHelpers.h"
#include "Constants.h"
#include <string.h>
#include <stdio.h>

ColumnNoteEditorPage::ColumnNoteEditorPage(EPD_PainterAdafruit& display,
                                           GT911_Lite& touch,
                                           Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _hexpad(display, touch)
    , _patIdx(0)
    , _col(1)
    , _segment(0)
    , _selRow(0)
    , _topPitch(49)         // C-4 at the top of the window
    , _wasDown(false)
    , _selStartCol(0)
    , _selEndCol(0)
    , _clipLen(0)
    , _drag(CneDrag::NONE)
    , _dragStartX(0)
    , _dragStartY(0)
    , _dragStartTopPitch(49)
    , _dragStartCellCol(0)
    , _dragStartCellPitchIdx(0)
    , _popup(CnePopup::NONE)
    , _dirtyRow(0xFF)
    , _dirtyAll(false)
{}

NoteGrid ColumnNoteEditorPage::grid() {
    return NoteGrid(_song.notePool, &_song.noteFreeHead,
                    &pat().noteHead);
}

uint8_t ColumnNoteEditorPage::numSegments() const {
    uint8_t len = _song.patterns[_patIdx].length;
    if (len == 0) return 1;
    return (len + 15) / 16;
}

uint8_t ColumnNoteEditorPage::segmentRow(uint8_t i) const {
    uint16_t r = (uint16_t)_segment * 16 + i;
    if (r >= _song.patterns[_patIdx].length) return 0xFF;
    return (uint8_t)r;
}

void ColumnNoteEditorPage::open(uint8_t patIdx, uint8_t col, uint8_t row) {
    _patIdx      = patIdx;
    _col         = col;
    if (row >= _song.patterns[patIdx].length) row = 0;
    _selRow      = row;
    _segment     = row / 16;
    _selStartCol = row % 16;
    _selEndCol   = row % 16;
    _wasDown     = false;
    _clipLen     = 0;
    _drag        = CneDrag::NONE;
    _popup       = CnePopup::NONE;
    _dirtyRow    = 0xFF;
    _dirtyAll    = false;
    // Leave _topPitch at its existing value across opens — feels less jarring.
}

void ColumnNoteEditorPage::draw() {
    _d.fillRect(0, 0, CNE_W, CNE_H, COL_WHITE);
    drawHeader();
    drawSelectorStrip();
    drawGrid();
    drawRightPanel();
}

// ── Header ───────────────────────────────────────────────────────────────────

void ColumnNoteEditorPage::drawHeader() {
    _d.fillRect(0, CNE_HDR_Y, CNE_W, CNE_HDR_H, COL_BLACK);

    // "Col Note Editor   {col} - {name}" — left-aligned.
    char extra[20];
    if (isInputCol()) {
        snprintf(extra, sizeof(extra), "%u - INPUT", (unsigned)_col + 1);
    } else {
        char name[12];
        const char* src = _song.columns[_col].name;
        size_t n = 0;
        while (n < sizeof(name) - 1 && src[n]) { name[n] = src[n]; n++; }
        name[n] = '\0';
        snprintf(extra, sizeof(extra), "%u - %s",
                 (unsigned)_col + 1, name[0] ? name : "(unnamed)");
    }
    char title[48];
    snprintf(title, sizeof(title), "Col Note Editor   %s", extra);

    _d.setTextColor(COL_WHITE);
    _d.setTextSize(3);
    _d.setCursor(12, CNE_HDR_Y + (CNE_HDR_H - 24) / 2);
    _d.print(title);

    uiButton(_d, CNE_HOME_X, CNE_HDR_Y + 4, CNE_HOME_W, CNE_HDR_H - 8,
             "HOME", COL_WHITE, COL_BLACK, 3);
}

// ── Selector strip — 16 row-cell selectors above the grid ────────────────────

void ColumnNoteEditorPage::drawSelectorStrip() {
    int y = CNE_SEL_Y;
    int h = CNE_SEL_H;
    _d.fillRect(CNE_GRID_X, y, CNE_GRID_W, h, COL_WHITE);
    uint8_t selIdx = _selRow % 16;
    for (int i = 0; i < CNE_COLS; i++) {
        int x = CNE_GRID_X + i * CNE_CELL;
        bool isSelected = (i >= _selStartCol && i <= _selEndCol);
        bool isCursor   = (i == selIdx);
        if (isSelected) _d.fillRect(x + 2, y + 2, CNE_CELL - 4, h - 4, COL_LTGREY);
        _d.drawRect(x + 2, y + 2, CNE_CELL - 4, h - 4,
                    isCursor ? COL_BLACK : COL_DKGREY);
    }
}

// ── Grid — pitch labels + 13 × 16 cells ──────────────────────────────────────

void ColumnNoteEditorPage::drawGrid() {
    // Clear the entire grid region first — partial redraws (pitch-axis drag,
    // cell taps) would otherwise leave stale labels and stale cell contents
    // because Adafruit GFX text rendering is transparent.
    _d.fillRect(0, CNE_GRID_Y, CNE_GRID_X + CNE_GRID_W, CNE_GRID_H, COL_WHITE);

    NoteGrid g = grid();
    uint8_t selIdx = _selRow % 16;
    bool isInput = isInputCol();

    // Pitch labels & rows
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    for (int i = 0; i < CNE_VIS_PITCHES; i++) {
        uint8_t pitch = (_topPitch >= i) ? (uint8_t)(_topPitch - i) : 0;
        int rowY = CNE_GRID_Y + i * CNE_CELL;

        // Pitch label
        char buf[5];
        if (pitch >= 1 && pitch <= NOTE_MAX) {
            noteToString(pitch, buf);
        } else {
            buf[0] = '-'; buf[1] = '-'; buf[2] = '-'; buf[3] = '\0';
        }
        _d.setCursor(8, rowY + (CNE_CELL - 16) / 2);
        _d.print(buf);

        // 16 cells
        for (int c = 0; c < CNE_COLS; c++) {
            int cx = CNE_GRID_X + c * CNE_CELL;
            int cxC = cx + CNE_CELL / 2;
            int cyC = rowY + CNE_CELL / 2;

            // Light grey background for the currently-selected row-cell column
            if (c == selIdx) _d.fillRect(cx + 1, rowY + 1, CNE_CELL - 2, CNE_CELL - 2, COL_LTGREY);
            // Cell border
            _d.drawRect(cx + 2, rowY + 2, CNE_CELL - 4, CNE_CELL - 4, COL_DKGREY);

            uint8_t patRow = segmentRow(c);
            if (patRow == 0xFF) continue;  // beyond pattern length
            Note n = g.get(patRow, _col);
            if (n.note == NOTE_EMPTY) continue;

            // NOTE_OFF: thick black X across every visible pitch cell.
            // Adafruit GFX has no native thick line — draw two parallel
            // offset 1px lines for a 2px-wide stroke.
            if (n.note == NOTE_OFF) {
                for (int o = 0; o <= 1; o++) {
                    _d.drawLine(cx + 5 + o,            rowY + 5,
                                cx + CNE_CELL - 6 + o, rowY + CNE_CELL - 6, COL_BLACK);
                    _d.drawLine(cx + CNE_CELL - 6 + o, rowY + 5,
                                cx + 5 + o,            rowY + CNE_CELL - 6, COL_BLACK);
                }
                continue;
            }

            // For all other notes, render only on cells that "match":
            //   NOTE_ANY → every visible pitch matches
            //   specific pitch → only the cell at that pitch matches
            bool match = (n.note == NOTE_ANY) || (n.note == pitch);
            if (!match) continue;

            bool hasIcon = isInput &&
                           (n.effect == EFFECT_WAIT || n.effect == EFFECT_SYNC);

            // Highlight body — dark-grey filled square (skipped when an icon
            // will overlay, so the black icon keeps contrast on white).
            if (!hasIcon) {
                _d.fillRect(cx + 5, rowY + 5, CNE_CELL - 10, CNE_CELL - 10, COL_DKGREY);
            }

            // WAIT / SYNC icons — input col only.
            if (isInput && n.effect == EFFECT_WAIT) {
                _d.fillCircle(cxC, cyC, 12, COL_BLACK);
                _d.fillRect(cxC - 11, cyC - 2, 22, 4, COL_WHITE);
            } else if (isInput && n.effect == EFFECT_SYNC) {
                _d.drawCircle(cxC, cyC, 10, COL_BLACK);
                _d.drawCircle(cxC, cyC, 11, COL_BLACK);
                _d.fillTriangle(cxC + 10, cyC - 3, cxC + 10, cyC + 3, cxC + 14, cyC, COL_BLACK);
                _d.fillTriangle(cxC - 10, cyC - 3, cxC - 10, cyC + 3, cxC - 14, cyC, COL_BLACK);
            }
        }
    }

    // Beat-group dividers (every 4 cells)
    for (int c = 4; c < CNE_COLS; c += 4) {
        int x = CNE_GRID_X + c * CNE_CELL;
        _d.drawLine(x, CNE_SEL_Y, x, CNE_GRID_Y + CNE_GRID_H, COL_DKGREY);
    }
}

// ── Right info panel ─────────────────────────────────────────────────────────

void ColumnNoteEditorPage::drawRightPanel() {
    drawNavRow();
    drawRowLine();
    drawVelAttrRow();
    drawActionButton();
    drawCopyPasteRow();
}

void ColumnNoteEditorPage::drawNavRow() {
    _d.fillRect(CNE_RP_X, CNE_RP_NAV_Y, CNE_RP_W, CNE_RP_NAV_H, COL_WHITE);
    uiButton(_d, CNE_RP_X, CNE_RP_NAV_Y, CNE_RP_PREV_W, CNE_RP_NAV_H,
             "<", COL_WHITE, COL_BLACK, 4);
    uiButton(_d, CNE_RP_X + CNE_RP_W - CNE_RP_NEXT_W, CNE_RP_NAV_Y,
             CNE_RP_NEXT_W, CNE_RP_NAV_H,
             ">", COL_WHITE, COL_BLACK, 4);

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d.%u / %02d",
             _patIdx + 1, _segment, _song.numPatterns);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(buf) * 6 * 3;
    int cx = CNE_RP_X + CNE_RP_PREV_W + (CNE_RP_W - CNE_RP_PREV_W - CNE_RP_NEXT_W - tw) / 2;
    _d.setCursor(cx, CNE_RP_NAV_Y + (CNE_RP_NAV_H - 24) / 2);
    _d.print(buf);
}

void ColumnNoteEditorPage::drawRowLine() {
    _d.fillRect(CNE_RP_X, CNE_RP_ROW_Y, CNE_RP_W, 28, COL_WHITE);
    char buf[16];
    snprintf(buf, sizeof(buf), "ROW: %02u", (unsigned)_selRow);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(CNE_RP_X, CNE_RP_ROW_Y);
    _d.print(buf);
}

void ColumnNoteEditorPage::drawVelAttrRow() {
    // Clear VEL row + ATTR row (covers either col 0 or output-col layout).
    _d.fillRect(CNE_RP_X, CNE_RP_VEL_Y, CNE_RP_W,
                (CNE_RP_ATTR_Y + CNE_RP_FIELD_H) - CNE_RP_VEL_Y, COL_WHITE);

    NoteGrid g = grid();
    Note n = g.get(_selRow, _col);

    // VEL label + button
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(CNE_RP_X, CNE_RP_VEL_Y + 8);
    _d.print("VEL:");

    char vbuf[4];
    if (n.note == NOTE_EMPTY || (n.velocity & 0x80)) {
        vbuf[0] = '-'; vbuf[1] = '-'; vbuf[2] = '\0';
    } else {
        snprintf(vbuf, sizeof(vbuf), "%02X", n.velocity);
    }
    int btnX = CNE_RP_X + CNE_RP_LBL_W;
    int btnW = CNE_RP_W - CNE_RP_LBL_W;
    uiButton(_d, btnX, CNE_RP_VEL_Y, btnW, CNE_RP_FIELD_H, vbuf,
             COL_WHITE, COL_BLACK, 3);

    if (!isInputCol()) {
        _d.setCursor(CNE_RP_X, CNE_RP_ATTR_Y + 8);
        _d.print("ATTR:");
        char abuf[6];
        if (n.note == NOTE_EMPTY && n.effect == 0 && n.param == 0) {
            abuf[0] = '-'; abuf[1] = '-'; abuf[2] = '-'; abuf[3] = '-'; abuf[4] = '\0';
        } else {
            snprintf(abuf, sizeof(abuf), "%02X%02X", n.effect, n.param);
        }
        uiButton(_d, btnX, CNE_RP_ATTR_Y, btnW, CNE_RP_FIELD_H, abuf,
                 COL_WHITE, COL_BLACK, 3);
    }
}

void ColumnNoteEditorPage::drawActionButton() {
    _d.fillRect(CNE_RP_X, CNE_RP_ACT_Y, CNE_RP_W, CNE_RP_ACT_H, COL_WHITE);
    if (isInputCol()) {
        // Three buttons: ANY  WAIT  SYNC
        int w = CNE_RP_W / 3 - 4;
        uiButton(_d, CNE_RP_X,                 CNE_RP_ACT_Y, w, CNE_RP_ACT_H,
                 "ANY",  COL_WHITE, COL_BLACK, 3);
        uiButton(_d, CNE_RP_X + (w + 6),       CNE_RP_ACT_Y, w, CNE_RP_ACT_H,
                 "WAIT", COL_WHITE, COL_BLACK, 3);
        uiButton(_d, CNE_RP_X + 2 * (w + 6),   CNE_RP_ACT_Y, w, CNE_RP_ACT_H,
                 "SYNC", COL_WHITE, COL_BLACK, 3);
    } else {
        uiButton(_d, CNE_RP_X, CNE_RP_ACT_Y, CNE_RP_W, CNE_RP_ACT_H,
                 "OFF", COL_WHITE, COL_BLACK, 3);
    }
}

void ColumnNoteEditorPage::drawCopyPasteRow(int pressed) {
    int w = (CNE_RP_W - 10) / 2;
    uint8_t cBg = (pressed == 0) ? COL_BLACK : COL_WHITE;
    uint8_t cFg = (pressed == 0) ? COL_WHITE : COL_BLACK;
    uint8_t pBg = (pressed == 1) ? COL_BLACK : COL_WHITE;
    uint8_t pFg = (pressed == 1) ? COL_WHITE : COL_BLACK;
    uiButton(_d, CNE_RP_X,           CNE_RP_CP_Y, w, CNE_RP_CP_H,
             "COPY",  cBg, cFg, 3);
    uiButton(_d, CNE_RP_X + w + 10,  CNE_RP_CP_Y, w, CNE_RP_CP_H,
             "PASTE", pBg, pFg, 3);
}

// ── Coord & hit tests ────────────────────────────────────────────────────────

void ColumnNoteEditorPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = CNE_H - rx;
}

static inline bool inRect(int sx, int sy, int x, int y, int w, int h) {
    return sx >= x && sx < x + w && sy >= y && sy < y + h;
}

bool ColumnNoteEditorPage::hitHome(int sx, int sy) const {
    return inRect(sx, sy, CNE_HOME_X, CNE_HDR_Y, CNE_HOME_W, CNE_HDR_H);
}
bool ColumnNoteEditorPage::hitNavPrev(int sx, int sy) const {
    return inRect(sx, sy, CNE_RP_X, CNE_RP_NAV_Y, CNE_RP_PREV_W, CNE_RP_NAV_H);
}
bool ColumnNoteEditorPage::hitNavNext(int sx, int sy) const {
    return inRect(sx, sy, CNE_RP_X + CNE_RP_W - CNE_RP_NEXT_W,
                  CNE_RP_NAV_Y, CNE_RP_NEXT_W, CNE_RP_NAV_H);
}
bool ColumnNoteEditorPage::hitVelField(int sx, int sy) const {
    return inRect(sx, sy, CNE_RP_X + CNE_RP_LBL_W, CNE_RP_VEL_Y,
                  CNE_RP_W - CNE_RP_LBL_W, CNE_RP_FIELD_H);
}
bool ColumnNoteEditorPage::hitAttrField(int sx, int sy) const {
    if (isInputCol()) return false;
    return inRect(sx, sy, CNE_RP_X + CNE_RP_LBL_W, CNE_RP_ATTR_Y,
                  CNE_RP_W - CNE_RP_LBL_W, CNE_RP_FIELD_H);
}
bool ColumnNoteEditorPage::hitAction(int sx, int sy, uint8_t& which) const {
    if (sy < CNE_RP_ACT_Y || sy >= CNE_RP_ACT_Y + CNE_RP_ACT_H) return false;
    if (isInputCol()) {
        int w = CNE_RP_W / 3 - 4;
        for (int i = 0; i < 3; i++) {
            int x = CNE_RP_X + i * (w + 6);
            if (sx >= x && sx < x + w) { which = (uint8_t)i; return true; }
        }
        return false;
    }
    if (sx < CNE_RP_X || sx >= CNE_RP_X + CNE_RP_W) return false;
    which = 0;
    return true;
}
bool ColumnNoteEditorPage::hitCopy(int sx, int sy) const {
    int w = (CNE_RP_W - 10) / 2;
    return inRect(sx, sy, CNE_RP_X, CNE_RP_CP_Y, w, CNE_RP_CP_H);
}
bool ColumnNoteEditorPage::hitPaste(int sx, int sy) const {
    int w = (CNE_RP_W - 10) / 2;
    return inRect(sx, sy, CNE_RP_X + w + 10, CNE_RP_CP_Y, w, CNE_RP_CP_H);
}
bool ColumnNoteEditorPage::hitSelector(int sx, int sy, int8_t& col) const {
    // Generous touch zone: title-bar bottom down to the bottom of the visual
    // tab strip. The strip itself is only 18 px tall, hard to hit precisely.
    if (sy < CNE_HDR_H || sy >= CNE_SEL_Y + CNE_SEL_H) return false;
    if (sx < CNE_GRID_X || sx >= CNE_GRID_X + CNE_GRID_W) return false;
    col = (int8_t)((sx - CNE_GRID_X) / CNE_CELL);
    return true;
}
bool ColumnNoteEditorPage::hitGridCell(int sx, int sy, int8_t& col, int8_t& pitchIdx) const {
    if (sx < CNE_GRID_X || sx >= CNE_GRID_X + CNE_GRID_W) return false;
    if (sy < CNE_GRID_Y || sy >= CNE_GRID_Y + CNE_GRID_H) return false;
    col      = (int8_t)((sx - CNE_GRID_X) / CNE_CELL);
    pitchIdx = (int8_t)((sy - CNE_GRID_Y) / CNE_CELL);
    return true;
}

// ── Edit operations ──────────────────────────────────────────────────────────

void ColumnNoteEditorPage::clampTopPitch() {
    int lo = CNE_VIS_PITCHES;        // bottom row needs pitch >= 1
    int hi = NOTE_MAX;
    if ((int)_topPitch < lo) _topPitch = (uint8_t)lo;
    if ((int)_topPitch > hi) _topPitch = (uint8_t)hi;
}

void ColumnNoteEditorPage::markDirty(uint8_t row) {
    _dirtyRow = row;
}

void ColumnNoteEditorPage::applyCellTap(uint8_t segCol, uint8_t pitch) {
    uint8_t row = segmentRow(segCol);
    if (row == 0xFF) return;
    if (pitch < 1 || pitch > NOTE_MAX) return;

    NoteGrid g = grid();
    Note n = g.get(row, _col);

    _selRow      = row;
    _selStartCol = segCol;
    _selEndCol   = segCol;

    if (n.note == NOTE_EMPTY) {
        // Empty cell → set pitched note with default velocity.
        Note nn = { pitch, VEL_DEFAULT, 0, 0 };
        g.set(row, _col, nn);
    } else if (n.note == pitch) {
        // Tap-on-match clears the note (velocity/effect/param go too).
        g.clear(row, _col);
    } else {
        // Different pitch / OFF / ANY → replace with this pitch, keeping
        // velocity (if specific) so feel is preserved when moving notes.
        Note nn = n;
        nn.note = pitch;
        if (nn.velocity == 0 && !(nn.velocity & 0x80)) nn.velocity = VEL_DEFAULT;
        g.set(row, _col, nn);
    }
    markDirty(row);
}

void ColumnNoteEditorPage::applyAction(uint8_t which) {
    NoteGrid g = grid();
    uint8_t row = _selRow;
    Note n = g.get(row, _col);

    if (isInputCol()) {
        // 0 = ANY, 1 = WAIT, 2 = SYNC
        if (which == 0) {
            // ANY: set note to NOTE_ANY, keep effect/param.
            n.note = NOTE_ANY;
            if (n.velocity == 0 && !(n.velocity & 0x80)) n.velocity = VEL_DEFAULT;
        } else if (which == 1) {
            // WAIT: set effect, param=0; if no note yet, default to NOTE_ANY.
            if (n.note == NOTE_EMPTY) n.note = NOTE_ANY;
            n.effect = EFFECT_WAIT;
            n.param  = 0;
        } else {
            // SYNC: same shape as WAIT.
            if (n.note == NOTE_EMPTY) n.note = NOTE_ANY;
            n.effect = EFFECT_SYNC;
            n.param  = 0;
        }
        g.set(row, _col, n);
    } else {
        // Output col: OFF
        Note nn = { NOTE_OFF, VEL_DEFAULT, 0, 0 };
        g.set(row, _col, nn);
    }
    markDirty(row);
}

void ColumnNoteEditorPage::applyCopy() {
    NoteGrid g = grid();
    uint8_t a = _selStartCol;
    uint8_t b = _selEndCol;
    if (a > b) { uint8_t t = a; a = b; b = t; }
    _clipLen = 0;
    for (uint8_t i = a; i <= b && _clipLen < CNE_COLS; i++) {
        uint8_t row = segmentRow(i);
        _clip[_clipLen++] = (row == 0xFF) ? Note{0,0,0,0} : g.get(row, _col);
    }
}

void ColumnNoteEditorPage::applyPaste() {
    if (_clipLen == 0) return;
    NoteGrid g = grid();
    uint8_t segStart = _selStartCol;     // paste starts at current cursor
    for (uint8_t i = 0; i < _clipLen; i++) {
        uint8_t segCol = segStart + i;
        if (segCol >= CNE_COLS) break;
        uint8_t row = segmentRow(segCol);
        if (row == 0xFF) break;
        const Note& n = _clip[i];
        if (n.note == NOTE_EMPTY && n.effect == 0 && n.param == 0) {
            g.clear(row, _col);
        } else {
            g.set(row, _col, n);
        }
    }
    _dirtyAll = true;   // bulk edit — caller resyncs the song
}

void ColumnNoteEditorPage::stepSegment(int delta) {
    int s = (int)_segment + delta;
    int p = (int)_patIdx;
    int nSeg = numSegments();

    if (s >= nSeg) {
        if (p + 1 < (int)_song.numPatterns) { p++; s = 0; }
        else                                 { s = nSeg - 1; }
    } else if (s < 0) {
        if (p > 0) {
            p--;
            int prevN = (_song.patterns[p].length + 15) / 16;
            if (prevN < 1) prevN = 1;
            s = prevN - 1;
        } else {
            s = 0;
        }
    }
    _patIdx  = (uint8_t)p;
    _segment = (uint8_t)s;
    // Keep selection cursor at the same column index within the new segment;
    // recompute _selRow to match.
    uint8_t r = segmentRow(_selStartCol);
    if (r == 0xFF) {
        _selStartCol = 0; _selEndCol = 0;
        r = segmentRow(0);
    }
    _selRow = (r == 0xFF) ? 0 : r;
}

// ── Poll — full Slice 2 state machine ────────────────────────────────────────

bool ColumnNoteEditorPage::poll() {
    // ── Popup mode: defer to the popup until it closes ───────────────────────
    if (_popup != CnePopup::NONE) {
        if (!_hexpad.poll()) return false;
        if (_hexpad.isDone()) {
            NoteGrid g = grid();
            Note n = g.get(_selRow, _col);
            if (_popup == CnePopup::VEL) {
                if (_hexpad.isCleared()) n.velocity = VEL_DEFAULT;
                else                     n.velocity = _hexpad.effect();   // 1st byte
            } else { // ATTR
                if (_hexpad.isCleared()) { n.effect = 0; n.param = 0; }
                else { n.effect = _hexpad.effect(); n.param = _hexpad.param(); }
            }
            // Only commit if the cell already has a note — otherwise edits
            // would create an invalid note=EMPTY but vel/effect set.
            if (n.note != NOTE_EMPTY) {
                g.set(_selRow, _col, n);
                markDirty(_selRow);
            }
        }
        _popup = CnePopup::NONE;
        _d.clear();
        draw();
        _d.paintLater();
        return false;
    }

    // ── Normal touch state machine ───────────────────────────────────────────
    if (!_touch.read()) return false;
    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // Drag-in-progress: track movement on pitch axis or selector strip.
    if (down && _wasDown) {
        if (_drag == CneDrag::GRID_MAYBE || _drag == CneDrag::GRID_PITCH) {
            int dy = sy - _dragStartY;
            if (_drag == CneDrag::GRID_MAYBE && abs(dy) >= 12) {
                _drag = CneDrag::GRID_PITCH;
            }
            if (_drag == CneDrag::GRID_PITCH) {
                int delta = dy / CNE_CELL;            // finger down = positive dy
                int newTop = (int)_dragStartTopPitch + delta;
                if (newTop < CNE_VIS_PITCHES) newTop = CNE_VIS_PITCHES;
                if (newTop > NOTE_MAX)        newTop = NOTE_MAX;
                if ((uint8_t)newTop != _topPitch) {
                    _topPitch = (uint8_t)newTop;
                    drawGrid();
                    _d.paintLater();
                }
            }
            return false;
        }
        if (_drag == CneDrag::SELECTOR) {
            int8_t c;
            if (hitSelector(sx, sy, c)) {
                if (c != _dragStartCellCol &&
                    (c != _selEndCol || _dragStartCellCol < c)) {
                    if (c >= _dragStartCellCol) {
                        _selStartCol = (uint8_t)_dragStartCellCol;
                        _selEndCol   = (uint8_t)c;
                    } else {
                        _selStartCol = (uint8_t)c;
                        _selEndCol   = (uint8_t)_dragStartCellCol;
                    }
                    drawSelectorStrip();
                    drawGrid();
                    _d.paintLater();
                }
            }
            return false;
        }
        return false;
    }

    // Rising edge ─────────────────────────────────────────────────────────────
    if (down && !_wasDown) {
        _wasDown = true;
        // Selector strip — start of multi-select drag.
        int8_t c;
        if (hitSelector(sx, sy, c)) {
            _drag             = CneDrag::SELECTOR;
            _dragStartCellCol = c;
            _selStartCol      = (uint8_t)c;
            _selEndCol        = (uint8_t)c;
            uint8_t r         = segmentRow((uint8_t)c);
            if (r != 0xFF) _selRow = r;
            drawSelectorStrip();
            drawGrid();
            drawRowLine();
            drawVelAttrRow();
            _d.paintLater();
            return false;
        }
        // Grid cell — could be a tap (set/clear pitch) or a pitch-axis drag.
        int8_t pidx;
        if (hitGridCell(sx, sy, c, pidx)) {
            _drag                  = CneDrag::GRID_MAYBE;
            _dragStartX            = sx;
            _dragStartY            = sy;
            _dragStartTopPitch     = _topPitch;
            _dragStartCellCol      = c;
            _dragStartCellPitchIdx = pidx;
            return false;
        }
        // All other targets fire on release.
        return false;
    }

    // Falling edge ────────────────────────────────────────────────────────────
    if (!down && _wasDown) {
        _wasDown = false;
        CneDrag was = _drag;
        _drag = CneDrag::NONE;

        if (was == CneDrag::GRID_PITCH) {
            // Drag committed — no tap.
            return false;
        }
        if (was == CneDrag::GRID_MAYBE) {
            // Treat as tap on the cell.
            uint8_t pitch = (_topPitch >= _dragStartCellPitchIdx)
                          ? (uint8_t)(_topPitch - _dragStartCellPitchIdx) : 0;
            applyCellTap((uint8_t)_dragStartCellCol, pitch);
            draw();
            _d.paintLater();
            return false;
        }
        if (was == CneDrag::SELECTOR) {
            // Drag finished — selection already committed during drag.
            return false;
        }

        // Other tap targets
        if (hitHome(sx, sy)) return true;

        if (hitNavPrev(sx, sy)) {
            stepSegment(-1);
            draw();
            _d.paintLater();
            return false;
        }
        if (hitNavNext(sx, sy)) {
            stepSegment(+1);
            draw();
            _d.paintLater();
            return false;
        }
        if (hitVelField(sx, sy)) {
            NoteGrid g = grid();
            Note n = g.get(_selRow, _col);
            uint8_t v = (n.velocity & 0x80) ? 0 : n.velocity;
            _d.clear();
            _hexpad.open(v, 0, 2, "Set Velocity", /*fingerDown*/ false);
            _hexpad.draw();
            _d.paint();
            _popup = CnePopup::VEL;
            return false;
        }
        if (hitAttrField(sx, sy)) {
            NoteGrid g = grid();
            Note n = g.get(_selRow, _col);
            _d.clear();
            _hexpad.open(n.effect, n.param, 4, "Set Effect", /*fingerDown*/ false);
            _hexpad.draw();
            _d.paint();
            _popup = CnePopup::ATTR;
            return false;
        }
        uint8_t which;
        if (hitAction(sx, sy, which)) {
            applyAction(which);
            draw();
            _d.paintLater();
            return false;
        }
        if (hitCopy(sx, sy)) {
            drawCopyPasteRow(0);
            _d.paint();
            applyCopy();
            delay(120);
            drawCopyPasteRow();
            _d.paintLater();
            return false;
        }
        if (hitPaste(sx, sy)) {
            drawCopyPasteRow(1);
            _d.paint();
            delay(120);
            applyPaste();
            draw();           // full redraw — restores button to normal too
            _d.paintLater();
            return false;
        }
    }
    return false;
}

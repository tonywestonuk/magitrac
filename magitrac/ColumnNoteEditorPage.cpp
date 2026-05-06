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
    , _patIdx(0)
    , _col(1)
    , _segment(0)
    , _selRow(0)
    , _topPitch(49)         // C-4 at the top of the window
    , _wasDown(false)
    , _selStartCol(0)
    , _selEndCol(0)
    , _clipLen(0)
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

    _d.setTextColor(COL_WHITE);
    _d.setTextSize(3);
    const char* title = "Column Note Editor";
    int tw = (int)strlen(title) * 6 * 3;
    _d.setCursor((CNE_W - tw) / 2, CNE_HDR_Y + (CNE_HDR_H - 24) / 2);
    _d.print(title);

    uiButton(_d, CNE_HOME_X, CNE_HDR_Y + 4, CNE_HOME_W, CNE_HDR_H - 8,
             "HOME", COL_WHITE, COL_BLACK, 3);
}

// ── Selector strip — 16 row-cell selectors above the grid ────────────────────

void ColumnNoteEditorPage::drawSelectorStrip() {
    int y = CNE_SEL_Y;
    int h = CNE_SEL_H;
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

            // NOTE_OFF: X spans every visible pitch in the column (output cols).
            if (n.note == NOTE_OFF) {
                _d.drawLine(cx + 5,            rowY + 5,
                            cx + CNE_CELL - 6, rowY + CNE_CELL - 6, COL_BLACK);
                _d.drawLine(cx + CNE_CELL - 6, rowY + 5,
                            cx + 5,            rowY + CNE_CELL - 6, COL_BLACK);
                continue;
            }

            // For all other notes, render only on cells that "match":
            //   NOTE_ANY → every visible pitch matches
            //   specific pitch → only the cell at that pitch matches
            bool match = (n.note == NOTE_ANY) || (n.note == pitch);
            if (!match) continue;

            // Highlight body — grey shade for input col, filled dark square for output.
            if (isInput) {
                _d.fillRect(cx + 4, rowY + 4, CNE_CELL - 8, CNE_CELL - 8, COL_LTGREY);
            } else {
                _d.fillRect(cx + 5, rowY + 5, CNE_CELL - 10, CNE_CELL - 10, COL_DKGREY);
            }

            // WAIT / SYNC icons overlay the highlighted cell(s) — input col only.
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
    drawColLine();
    drawRowLine();
    drawVelAttrRow();
    drawActionButton();
    drawCopyPasteRow();
}

void ColumnNoteEditorPage::drawNavRow() {
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

void ColumnNoteEditorPage::drawColLine() {
    char buf[24];
    if (isInputCol()) {
        snprintf(buf, sizeof(buf), "COL: INPUT");
    } else {
        char name[12];
        const char* src = _song.columns[_col].name;
        size_t n = 0;
        while (n < sizeof(name) - 1 && src[n]) { name[n] = src[n]; n++; }
        name[n] = '\0';
        snprintf(buf, sizeof(buf), "COL: %s", name[0] ? name : "(unnamed)");
    }
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(CNE_RP_X, CNE_RP_COL_Y);
    _d.print(buf);
}

void ColumnNoteEditorPage::drawRowLine() {
    char buf[16];
    snprintf(buf, sizeof(buf), "ROW: %02u", (unsigned)_selRow);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(CNE_RP_X, CNE_RP_ROW_Y);
    _d.print(buf);
}

void ColumnNoteEditorPage::drawVelAttrRow() {
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

void ColumnNoteEditorPage::drawCopyPasteRow() {
    int w = (CNE_RP_W - 10) / 2;
    uiButton(_d, CNE_RP_X,           CNE_RP_CP_Y, w, CNE_RP_CP_H,
             "COPY",  COL_WHITE, COL_BLACK, 3);
    uiButton(_d, CNE_RP_X + w + 10,  CNE_RP_CP_Y, w, CNE_RP_CP_H,
             "PASTE", COL_WHITE, COL_BLACK, 3);
}

// ── Touch handling — HOME only for Slice 1 ───────────────────────────────────

void ColumnNoteEditorPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = CNE_H - rx;
}

bool ColumnNoteEditorPage::hitHome(int sx, int sy) const {
    return sx >= CNE_HOME_X && sx < CNE_HOME_X + CNE_HOME_W &&
           sy >= CNE_HDR_Y  && sy < CNE_HDR_Y  + CNE_HDR_H;
}

bool ColumnNoteEditorPage::poll() {
    if (!_touch.read()) return false;
    bool down = _touch.isTouched;

    if (down && !_wasDown) {
        _wasDown = true;
        return false;
    }
    if (!down && _wasDown) {
        _wasDown = false;
        int sx, sy;
        rawToScreen(_touch.x, _touch.y, sx, sy);
        if (hitHome(sx, sy)) return true;
        // Slice 2 will dispatch to grid / selector / nav / popups here.
    }
    return false;
}

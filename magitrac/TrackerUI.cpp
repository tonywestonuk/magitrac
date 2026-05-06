#include "TrackerUI.h"
#include "NoteGrid.h"
#include "UIHelpers.h"
#include "SongSource.h"
#include <stdio.h>
#include <string.h>

// ── Constructor ───────────────────────────────────────────────────────────────

TrackerUI::TrackerUI(EPD_PainterAdafruit& display, Song& song, TrackerEngine& engine)
    : _d(display)
    , _song(song)
    , _engine(engine)
    , _serverConnected(false)
    , _serverPlaying(false)
    , _selRow(0)
    , _selCol(0)
    , _colOffset(0)
    , _midiInputCol(1)
    , _stepAdvance(1)
    , _editMode(false)
    , _velCapture(false)
    , _noSong(false)
    , _emptyCellCached(false)
    , _battPct(-1)
    , _battCharging(false)
    , _memPct(-1)
{
    _statusMsg[0] = '\0';
    _clockStr[0]  = '\0';
}

void TrackerUI::setServerConnected(bool connected) {
    _serverConnected = connected;
}

void TrackerUI::setBattery(int pct, bool charging) {
    _battPct      = pct;
    _battCharging = charging;
}

void TrackerUI::setMemPct(int pct) {
    _memPct = pct;
}

void TrackerUI::setServerPlaying(bool playing) {
    _serverPlaying = playing;
}

void TrackerUI::setStatus(const char* msg) {
    strncpy(_statusMsg, msg ? msg : "", 31);
    _statusMsg[31] = '\0';
}

void TrackerUI::setClock(int hour, int minute, int day, int month, int year) {
    snprintf(_clockStr, sizeof(_clockStr), "%02d:%02d  %02d/%02d/%02d",
             hour, minute, day, month, year % 100);
}

void TrackerUI::setNoSong(bool v) {
    _noSong = v;
}

// ── Public API ────────────────────────────────────────────────────────────────

void TrackerUI::drawAll() {
    _d.fillScreen(COL_WHITE);
    drawHeader();

    // Column header strip — black background, white text
    _d.fillRect(0, CHAN_HDR_Y, DISPLAY_W, COL_HDR_H, COL_BLACK);
    _d.setTextSize(HDR_TEXT_SIZE);
    _d.setTextColor(COL_WHITE);
    for (int vi = 0; vi < VISIBLE_COLUMNS; vi++) {
        int  col   = _colOffset + vi;
        int  x     = ROW_NUM_W + vi * COL_W;
        char label[12];
        bool isMuted = false;
        if (col == INPUT_COLUMN) {
            snprintf(label, sizeof(label), "MIDI IN");
        } else {
            const ColumnSettings& cs = _song.columns[col];
            isMuted = (cs.mute != 0);
            if (cs.name[0] != '\0')
                snprintf(label, sizeof(label), "%s", cs.name);
            else
                snprintf(label, sizeof(label), "COLUMN %d", col + 1);
        }
        int labelW = strlen(label) * HDR_CHAR_W;
        int labelX = x + (COL_W - MUTE_BTN_W - labelW) / 2;
        int labelY = CHAN_HDR_Y + (COL_HDR_H - HDR_CHAR_H) / 2;
        _d.setCursor(labelX, labelY);
        _d.print(label);

        // MIDI step-input indicator: small filled down-triangle left of label
        if (col == _midiInputCol && col != INPUT_COLUMN) {
            int tx = labelX - 14;   // 14px left of label text
            int ty = CHAN_HDR_Y + (COL_HDR_H - 8) / 2;  // vertically centred
            _d.fillTriangle(tx, ty, tx + 8, ty, tx + 4, ty + 7, COL_WHITE);
        }

        // Mute icon — right edge of output columns
        if (col != INPUT_COLUMN) {
            int iconX = x + COL_W - MUTE_BTN_W + 6;
            int iconY = CHAN_HDR_Y + (COL_HDR_H - 18) / 2;
            drawSpeakerIcon(iconX, iconY, isMuted, COL_WHITE);
        }

        if (vi > 0) vline(x, CHAN_HDR_Y, GRID_Y + GRID_H, COL_BLACK);
    }

    vline(ROW_NUM_W - 1, CHAN_HDR_Y, GRID_Y + GRID_H, COL_BLACK);

    drawGrid();
    drawStatusBar();

    // Outer border and section separators
    _d.drawRect(0, 0, DISPLAY_W, DISPLAY_H, COL_BLACK);
    hline(HEADER_H - 1, COL_BLACK);
    hline(STATUS_Y, COL_BLACK);
}

void TrackerUI::drawHeader() {
    _d.fillRect(0, HEADER_Y, DISPLAY_W, HEADER_H, COL_WHITE);

    _d.setTextSize(HDR_TEXT_SIZE);
    _d.setTextColor(COL_BLACK);

    // ── Left: MENU button ─────────────────────────────────────────────────────
    drawButton(0, 0, MENU_BTN_W, HEADER_H, "MENU", COL_BLACK, COL_WHITE);

    // ── Left-centre: clock then status (textSize 3) ─────────────────────────
    int cursorX = MENU_BTN_W + 10;
    int hdrTextY = HEADER_Y + (HEADER_H - HDR_CHAR_H) / 2;
    if (_clockStr[0]) {
        _d.setTextSize(HDR_TEXT_SIZE);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(cursorX, hdrTextY);
        _d.print(_clockStr);
        cursorX += strlen(_clockStr) * HDR_CHAR_W + HDR_CHAR_W;  // one char gap
    }
    if (_statusMsg[0]) {
        _d.setTextSize(HDR_TEXT_SIZE);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(cursorX, hdrTextY);
        _d.print(_statusMsg);
    }

    // ── Right: battery icon + percentage ──────────────────────────────────────
    if (_battPct >= 0) {
        // Battery icon — body 32×14px, terminal nub 3×6px
        const int bodyW = 32;
        const int bodyH = 14;
        const int nubW  = 3;
        const int bx    = DISPLAY_W - bodyW - nubW - 6;        // = 919
        const int by    = HEADER_Y + (HEADER_H - bodyH) / 2;   // centred

        _d.fillRect(bx, by, bodyW, bodyH, COL_WHITE);
        _d.drawRect(bx, by, bodyW, bodyH, COL_BLACK);
        _d.fillRect(bx + bodyW, by + 4, nubW, 6, COL_BLACK);

        if (_battCharging) {
            _d.fillRect(bx + 2, by + 2, bodyW - 4, bodyH - 4, COL_BLACK);
            drawLightningBolt(bx + 2 + (bodyW - 4 - 8) / 2, by + 2,
                              8, bodyH - 4, COL_WHITE);
        } else {
            int fillW = (_battPct * (bodyW - 4)) / 100;
            if (fillW > 0)
                _d.fillRect(bx + 2, by + 2, fillW, bodyH - 4, COL_BLACK);
        }

        // Percentage text — left of battery icon (textSize 3)
        char batBuf[8];
        snprintf(batBuf, sizeof(batBuf), "%d%%", _battPct);
        int tw = strlen(batBuf) * HDR_CHAR_W;
        _d.setTextSize(HDR_TEXT_SIZE);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(bx - tw - 4, HEADER_Y + (HEADER_H - HDR_CHAR_H) / 2);
        _d.print(batBuf);
    }

    hline(HEADER_H - 1, COL_BLACK);
}

void TrackerUI::drawGrid() {
    if (_noSong) {
        // Blank grid with centred two-line message
        _d.fillRect(0, GRID_Y, DISPLAY_W, GRID_H, COL_WHITE);
        _d.setTextSize(4);
        _d.setTextColor(COL_BLACK);
        const char* line1 = "NO SONG LOADED";
        const char* line2 = "ON SERVER";
        int x1 = (DISPLAY_W - (int)strlen(line1) * CHAR_W) / 2;
        int x2 = (DISPLAY_W - (int)strlen(line2) * CHAR_W) / 2;
        int y1 = GRID_Y + (GRID_H / 2) - CHAR_H - 4;
        int y2 = GRID_Y + (GRID_H / 2) + 4;
        _d.setCursor(x1, y1);
        _d.print(line1);
        _d.setCursor(x2, y2);
        _d.print(line2);
        return;
    }

    if (!_emptyCellCached) buildEmptyCellCache();

    // Each drawRow fills its own background — no need to clear the whole grid first.
    int top = topRow();
    for (int vi = 0; vi < VISIBLE_ROWS; vi++) {
        drawRow(vi, top + vi);
    }

    // Dividers drawn after rows so row fills don't erase them.
    vline(ROW_NUM_W - 1, GRID_Y, GRID_Y + GRID_H, COL_BLACK);
    for (int vi = 1; vi < VISIBLE_COLUMNS; vi++) {
        vline(ROW_NUM_W + vi * COL_W, GRID_Y, GRID_Y + GRID_H, COL_BLACK);
    }
}

void TrackerUI::drawStatusBar() {
    _d.fillRect(0, STATUS_Y + 1, DISPLAY_W, STATUS_H - 1, COL_WHITE);

    int btnY = STATUS_Y + (STATUS_H - 42) / 2;

    // PLAY / STOP only available in client-server mode
    // Active button is black-filled; inactive is white with black outline.
    if (_serverConnected) {
        uint8_t playBg = _serverPlaying ? COL_BLACK : COL_WHITE;
        uint8_t playFg = _serverPlaying ? COL_WHITE : COL_BLACK;
        uint8_t stopBg = _serverPlaying ? COL_WHITE : COL_BLACK;
        uint8_t stopFg = _serverPlaying ? COL_BLACK : COL_WHITE;
        drawButton(8,   btnY, 90, 42, "PLAY", playBg, playFg);
        drawButton(104, btnY, 90, 42, "STOP", stopBg, stopFg);
    }

    // Song name
    _d.setTextSize(HDR_TEXT_SIZE);
    _d.setTextColor(COL_BLACK);
    char buf[48];
    snprintf(buf, sizeof(buf), "%-14s", _song.name);
    _d.setCursor(210, STATUS_Y + (STATUS_H - HDR_CHAR_H) / 2);
    _d.print(buf);

    // Edit / step-advance / velocity-capture — only shown when stopped
    if (_serverConnected && !_serverPlaying) {
        drawButton(594, btnY, 36, 42, "e",
                   _editMode ? COL_BLACK : COL_WHITE,
                   _editMode ? COL_WHITE : COL_BLACK);
        char stepLabel[4];
        snprintf(stepLabel, sizeof(stepLabel), "+%d", _stepAdvance);
        drawButton(640, btnY, 52, 42, stepLabel, COL_WHITE, COL_BLACK);
        drawButton(702, btnY, 36, 42, "v",
                   _velCapture ? COL_BLACK : COL_WHITE,
                   _velCapture ? COL_WHITE : COL_BLACK);
    }

    // Block navigation  [<]  01/08  [>]  — flush right
    drawButton(748, btnY, 60, 42, "<", COL_WHITE, COL_BLACK);
    snprintf(buf, sizeof(buf), "%02d/%02d",
             _engine.currentPattern() + 1, _song.numPatterns);
    _d.setCursor(814, STATUS_Y + (STATUS_H - HDR_CHAR_H) / 2);
    _d.print(buf);
    drawButton(900, btnY, 60, 42, ">", COL_WHITE, COL_BLACK);
}

void TrackerUI::setSelected(int8_t row, int8_t col) {
    _selRow = row;
    _selCol = col;
}

void TrackerUI::setMidiInputCol(uint8_t col) {
    if (col >= 1 && col < MAX_COLUMNS) _midiInputCol = col;
}

void TrackerUI::cycleStepAdvance() {
    _stepAdvance = (_stepAdvance % 4) + 1;  // 1→2→3→4→1
}

void TrackerUI::setColOffset(uint8_t offset) {
    int maxOffset = MAX_COLUMNS - VISIBLE_COLUMNS;
    if ((int)offset > maxOffset) offset = (uint8_t)maxOffset;
    _colOffset = offset;
}

// ── Hit-test helpers ──────────────────────────────────────────────────────────

bool TrackerUI::hitGrid(int tx, int ty, int8_t& outRow, int8_t& outCol) const {
    if (tx < ROW_NUM_W || tx >= DISPLAY_W)      return false;
    if (ty < GRID_Y    || ty >= GRID_Y + GRID_H) return false;

    int vi  = (ty - GRID_Y) / ROW_H;
    int vcol = (tx - ROW_NUM_W) / COL_W;
    int pat = topRow() + vi;
    int col = _colOffset + vcol;

    if (pat < 0 || pat >= _song.patterns[_engine.currentPattern()].length) return false;
    if (vcol < 0 || vcol >= VISIBLE_COLUMNS) return false;
    if (col  < 0 || col  >= MAX_COLUMNS)     return false;

    outRow = (int8_t)pat;
    outCol = (int8_t)col;
    return true;
}

bool TrackerUI::hitPlayButton(int tx, int ty) const {
    int btnY = STATUS_Y + (STATUS_H - 42) / 2;
    return (tx >= 8 && tx < 98 && ty >= btnY - 3 && ty < btnY + 45);
}

bool TrackerUI::hitStopButton(int tx, int ty) const {
    int btnY = STATUS_Y + (STATUS_H - 42) / 2;
    return (tx >= 104 && tx < 194 && ty >= btnY - 3 && ty < btnY + 45);
}

bool TrackerUI::hitStepAdvance(int tx, int ty) const {
    int btnY = STATUS_Y + (STATUS_H - 42) / 2;
    return (tx >= 640 && tx < 692 && ty >= btnY - 3 && ty < btnY + 45);
}

bool TrackerUI::hitVelCapture(int tx, int ty) const {
    int btnY = STATUS_Y + (STATUS_H - 42) / 2;
    return (tx >= 702 && tx < 738 && ty >= btnY - 3 && ty < btnY + 45);
}

bool TrackerUI::hitEditMode(int tx, int ty) const {
    int btnY = STATUS_Y + (STATUS_H - 42) / 2;
    return (tx >= 594 && tx < 630 && ty >= btnY - 3 && ty < btnY + 45);
}

bool TrackerUI::hitBlockPrev(int tx, int ty) const {
    int btnY = STATUS_Y + (STATUS_H - 42) / 2;
    return (tx >= 748 && tx < 808 && ty >= btnY - 3 && ty < btnY + 45);
}

bool TrackerUI::hitBlockNext(int tx, int ty) const {
    int btnY = STATUS_Y + (STATUS_H - 42) / 2;
    return (tx >= 900 && tx < 960 && ty >= btnY - 3 && ty < btnY + 45);
}

bool TrackerUI::hitSongName(int tx, int ty) const {
    // Song name drawn at x=210; edit button starts at x=594.
    return (tx >= 200 && tx < 580 && ty >= STATUS_Y && ty < STATUS_Y + STATUS_H);
}

bool TrackerUI::hitBlockLabel(int tx, int ty) const {
    // xx/xx label sits between < (ends 808) and > (starts 900).
    return (tx >= 808 && tx < 900 && ty >= STATUS_Y && ty < STATUS_Y + STATUS_H);
}

bool TrackerUI::hitMenuButton(int tx, int ty) const {
    return (tx >= 0 && tx < MENU_BTN_W && ty >= HEADER_Y && ty < HEADER_Y + HEADER_H);
}

bool TrackerUI::hitMuteButton(int tx, int ty, int8_t& col) const {
    if (ty < CHAN_HDR_Y || ty >= CHAN_HDR_Y + COL_HDR_H) return false;
    if (tx < ROW_NUM_W) return false;
    int vi = (tx - ROW_NUM_W) / COL_W;
    if (vi < 0 || vi >= VISIBLE_COLUMNS) return false;
    int c = _colOffset + vi;
    if (c == INPUT_COLUMN) return false;
    if (c >= MAX_COLUMNS) return false;
    // Mute icon occupies the rightmost MUTE_BTN_W pixels of the column
    int colRight = ROW_NUM_W + (vi + 1) * COL_W;
    if (tx < colRight - MUTE_BTN_W) return false;
    col = (int8_t)c;
    return true;
}

bool TrackerUI::hitColumnHeader(int tx, int ty, int8_t& col, bool inclInput) const {
    if (ty < CHAN_HDR_Y || ty >= CHAN_HDR_Y + COL_HDR_H) return false;
    if (tx < ROW_NUM_W) return false;
    int vi = (tx - ROW_NUM_W) / COL_W;
    if (vi < 0 || vi >= VISIBLE_COLUMNS) return false;
    int c = _colOffset + vi;
    if (c == INPUT_COLUMN && !inclInput) return false;  // col 0 only editable via long-press
    if (c >= MAX_COLUMNS) return false;
    // Exclude the mute button zone (right edge)
    int colRight = ROW_NUM_W + (vi + 1) * COL_W;
    if (tx >= colRight - MUTE_BTN_W) return false;
    col = (int8_t)c;
    return true;
}

// ── Private ───────────────────────────────────────────────────────────────────

int TrackerUI::topRow() const {
    int centre = (_serverConnected && _serverPlaying) ? (int)_engine.currentRow() : (int)_selRow;
    return centre - READER_IDX;
}

void TrackerUI::buildEmptyCellCache() {
    // Render one empty cell (white bg + "--- -- -----") into the framebuffer at a
    // temporary position, then copy the pixels out into _emptyCellCache.
    // The framebuffer is 8bpp (one byte per pixel), stride = DISPLAY_W.
    const int tmpX = ROW_NUM_W;
    const int tmpY = GRID_Y;

    _d.fillRect(tmpX, tmpY, COL_W, ROW_H, COL_WHITE);
    Note empty = {0, VEL_DEFAULT, 0, 0};
    drawCell(tmpX + 10, tmpY + TEXT_PAD_Y, empty, COL_BLACK);

    uint8_t* fb = _d.getBuffer();
    for (int row = 0; row < ROW_H; row++) {
        memcpy(_emptyCellCache + row * COL_W,
               fb + (tmpY + row) * DISPLAY_W + tmpX,
               COL_W);
    }
    _emptyCellCached = true;
}

void TrackerUI::stampEmptyCell(int x, int y) {
    // Copy the pre-rendered empty cell into the framebuffer at (x, y).
    uint8_t* fb = _d.getBuffer();
    for (int row = 0; row < ROW_H; row++) {
        memcpy(fb + (y + row) * DISPLAY_W + x,
               _emptyCellCache + row * COL_W,
               COL_W);
    }
}


void TrackerUI::drawRow(int vi, int patRow) {
    int  y      = GRID_Y + vi * ROW_H;
    bool reader = (vi == READER_IDX);

    Pattern& pat   = _song.patterns[_engine.currentPattern()];
    bool     valid = (patRow >= 0 && patRow < pat.length);

    if (reader) {
        _d.fillRect(0, y, ROW_NUM_W, ROW_H, COL_DKGREY);
        for (int vi2 = 0; vi2 < VISIBLE_COLUMNS; vi2++) {
            int     col   = _colOffset + vi2;
            int     cx    = ROW_NUM_W + vi2 * COL_W;
            uint8_t colBg = (col == INPUT_COLUMN) ? COL_LTGREY : COL_DKGREY;
            _d.fillRect(cx, y, COL_W, ROW_H, colBg);
        }
    } else {
        _d.fillRect(0, y, DISPLAY_W, ROW_H, COL_WHITE);
    }

    // Row number
    _d.setTextSize(TEXT_SIZE);
    _d.setTextColor(reader ? COL_WHITE : COL_BLACK);
    if (valid) {
        char rowStr[4];
        snprintf(rowStr, sizeof(rowStr), "%02d", patRow);
        _d.setCursor(6, y + TEXT_PAD_Y);
        _d.print(rowStr);
    }

    // Column cells
    if (valid) {
        NoteGrid grid(_song.notePool, &_song.patterns[_engine.currentPattern()].noteHead);
        for (int vi2 = 0; vi2 < VISIBLE_COLUMNS; vi2++) {
            int     col     = _colOffset + vi2;
            int     cx      = ROW_NUM_W + vi2 * COL_W;
            Note    n       = grid.get((uint8_t)patRow, (uint8_t)col);
            bool    empty   = (n.note == 0 && n.effect == 0 && n.param == 0);
            bool    isInput = (col == INPUT_COLUMN);
            if (empty && !reader && !isInput) {
                stampEmptyCell(cx, y);  // memcpy pre-rendered "--- -- -----"
            } else {
                uint8_t fg = reader ? (isInput ? COL_BLACK : COL_WHITE) : COL_BLACK;
                drawCell(cx + 10, y + TEXT_PAD_Y, n, fg, !isInput);
            }
        }
    }
}

void TrackerUI::drawCell(int x, int y, const Note& note, uint8_t fg, bool showInstrument) {
    char noteBuf[4];
    noteToString(note.note, noteBuf);

    _d.setTextSize(TEXT_SIZE);
    _d.setTextColor(fg);

    _d.setCursor(x, y); _d.print(noteBuf);

    if (showInstrument) {
        // Output columns: velocity + 4-char effect/param field
        char velBuf[3];
        velToString(note.note == NOTE_EMPTY ? VEL_DEFAULT : note.velocity, velBuf);
        _d.setCursor(x + 80, y); _d.print(velBuf);

        char fxBuf[5];
        effectToString5(note.effect, note.param, fxBuf);
        _d.setCursor(x + 150, y); _d.print(fxBuf);
    } else {
        char metaBuf[5];
        col0MetaToString(note, metaBuf);
        _d.setCursor(x + 150, y); _d.print(metaBuf);
    }
}

void TrackerUI::drawButton(int x, int y, int w, int h,
                            const char* label, uint8_t bg, uint8_t fg, int ts) {
    uiButton(_d, x, y, w, h, label, bg, fg, ts);
}

void TrackerUI::hline(int y, uint8_t colour) {
    _d.drawFastHLine(0, y, DISPLAY_W, colour);
}

void TrackerUI::vline(int x, int y1, int y2, uint8_t colour) {
    _d.drawFastVLine(x, y1, y2 - y1, colour);
}

void TrackerUI::drawLightningBolt(int x, int y, int w, int h, uint8_t col) {
    // Z-shaped bolt: upper arm top-right→mid-left, crossbar, lower arm mid-right→bottom-left
    // Drawn 2px wide for visibility on e-paper.
    int mid = h / 2;
    for (int t = 0; t < 2; t++) {
        _d.drawLine(x + w - 1 + t, y,         x + t,         y + mid,   col);
        _d.drawLine(x + t,         y + mid,    x + w/2 + t,   y + mid,   col);
        _d.drawLine(x + w/2 + t,   y + mid,    x + t,         y + h - 1, col);
    }
}

void TrackerUI::drawSpeakerIcon(int x, int y, bool muted, uint8_t fg) {
    // Speaker icon ~20px wide × 18px tall, drawn at (x, y).
    // Body: 6×8 rectangle.  Cone: triangle extending 6px right.
    // Sound waves: two small arcs when unmuted.
    // Mute: diagonal strike-through line.

    int bodyW = 5, bodyH = 8;
    int bodyX = x, bodyY = y + 5;

    // Speaker body (rectangle)
    _d.fillRect(bodyX, bodyY, bodyW, bodyH, fg);

    // Cone (triangle extending right from body)
    int coneX = bodyX + bodyW;
    _d.fillTriangle(coneX, bodyY,
                    coneX, bodyY + bodyH - 1,
                    coneX + 6, bodyY - 3, fg);
    _d.fillTriangle(coneX, bodyY + bodyH - 1,
                    coneX + 6, bodyY - 3,
                    coneX + 6, bodyY + bodyH + 2, fg);

    if (!muted) {
        // Sound waves — two small arcs to the right of the cone
        int wx = coneX + 8;
        int cy = bodyY + bodyH / 2;
        // Inner wave
        for (int dy = -3; dy <= 3; dy++)
            _d.drawPixel(wx, cy + dy, fg);
        _d.drawPixel(wx + 1, cy - 4, fg);
        _d.drawPixel(wx + 1, cy + 4, fg);
        // Outer wave
        for (int dy = -5; dy <= 5; dy++)
            _d.drawPixel(wx + 3, cy + dy, fg);
        _d.drawPixel(wx + 4, cy - 6, fg);
        _d.drawPixel(wx + 4, cy + 6, fg);
    } else {
        // Diagonal strike-through line (top-right to bottom-left, 2px wide)
        int lx0 = x + 16, ly0 = y;
        int lx1 = x - 1,  ly1 = y + 17;
        _d.drawLine(lx0, ly0, lx1, ly1, fg);
        _d.drawLine(lx0 + 1, ly0, lx1 + 1, ly1, fg);
    }
}

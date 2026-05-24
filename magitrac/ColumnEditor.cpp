#include "ColumnEditor.h"
#include "SettingsPage.h"   // for gMaxBank, gMaxProgram
#include "NoteGrid.h"
#include "ServerPairing.h"  // for gServerPairing — sample list fetch
#include <string.h>

static inline bool isSfx(uint8_t ch) { return ch == SFX_CHANNEL; }
static inline bool isPxl(uint8_t ch) { return ch == PIXELPOST_CHANNEL; }

// Strip ".wav" suffix and truncate to fit the column NAME field.
static void copySampleNameToCol(const char* fname, char* nameField) {
    char tmp[SAMPLE_NAME_LEN];
    strncpy(tmp, fname, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    int n = (int)strlen(tmp);
    if (n > 4 && strcasecmp(tmp + n - 4, ".wav") == 0) tmp[n - 4] = '\0';
    strncpy(nameField, tmp, INSTRUMENT_NAME_LEN - 1);
    nameField[INSTRUMENT_NAME_LEN - 1] = '\0';
}

// Returns index in the cache of the entry with id == cs.program, or -1.
static int sampleIndexForId(uint8_t id) {
    int n = gServerPairing.sampleListCount();
    for (int i = 0; i < n; i++) {
        const SampleCacheEntry* e = gServerPairing.sampleListEntry(i);
        if (e && e->id == id) return i;
    }
    return -1;
}

ColumnEditor::ColumnEditor(EPD_PainterAdafruit& display, GT911_Lite& touch,
                           Song& song, Instrument* instruments)
    : _d(display), _touch(touch), _song(song), _instruments(instruments)
    , _patIdx(0), _col(1), _wasDown(false), _patchPending(false)
    , _picking(false), _pickingSample(false), _naming(false), _pickPage(0)
    , _keyboard(display, touch)
    , _action(Action::NONE), _actionDst(0), _resyncMask(0)
    , _pressedOnName(false)
    , _sampleListSeenState(0)
{}

ColumnSettings& ColumnEditor::cs() {
    return _song.columns[_col];
}

void ColumnEditor::open(uint8_t patternIdx, uint8_t col) {
    _patIdx        = patternIdx;
    _col           = col;
    _wasDown       = false;
    _patchPending  = false;
    _picking       = false;
    _pickingSample = false;
    _naming        = false;
    _pickPage      = 0;
    _action        = Action::NONE;
    _actionDst     = 0;
    _resyncMask    = 0;
    _pressedOnName = false;
    // If the column is on SFX, trigger a sample-list refresh now so the
    // picker / PROG +/- has fresh data by the time the user reaches them.
    // Spec: refresh every time the editor sees an SFX column.
    if (isSfx(_song.columns[_col].midiChannel) && gServerPairing.isPaired()) {
        gServerPairing.requestSampleList();
    }
}

// ── Drawing ──────────────────────────────────────────────────────────────────

void ColumnEditor::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = CE_H - rx;
}

void ColumnEditor::drawHeader() {
    _d.fillRect(0, 0, CE_W, CE_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);

    char title[32];
    const ColumnSettings& c = _song.columns[_col];
    if (c.name[0])
        snprintf(title, sizeof(title), "COL %d: %s", _col + 1, c.name);
    else
        snprintf(title, sizeof(title), "COLUMN %d SETTINGS", _col + 1);
    _d.setCursor(10, (CE_HDR_H - 24) / 2);
    _d.print(title);

    uiButton(_d, CE_HOME_X, 0, CE_HOME_W, CE_HDR_H, "HOME", COL_BLACK, COL_WHITE, 3);
}

void ColumnEditor::fieldLabel(int field, char* out) const {
    static const char* labels[] = { "MIDI CH", "BANK", "PROGRAM", "VOLUME", "TRANSPOSE", "NAME" };
    strcpy(out, labels[field]);
}

void ColumnEditor::fieldValue(int field, char* out) const {
    const ColumnSettings& c = _song.columns[_col];
    switch (field) {
        case 0:  // MIDI CH
            if      (c.midiChannel == 0)                strcpy(out, "OFF");
            else if (c.midiChannel == SFX_CHANNEL)      strcpy(out, "SFX");
            else if (c.midiChannel == PIXELPOST_CHANNEL) strcpy(out, "PXL");
            else                                         snprintf(out, 16, "%d", c.midiChannel);
            break;
        case 1:  // BANK — meaningless on SFX and PXL
            if (isSfx(c.midiChannel) || isPxl(c.midiChannel)) strcpy(out, "--");
            else                                              snprintf(out, 16, "%d", c.bankMSB + 1);
            break;
        case 2:  // PROGRAM — on SFX shows sample id (0 = none); PXL has none
            if (isSfx(c.midiChannel)) {
                if (c.program == 0) strcpy(out, "--");
                else                snprintf(out, 16, "%d", c.program);
            } else if (isPxl(c.midiChannel)) {
                strcpy(out, "--");
            } else {
                snprintf(out, 16, "%d", c.program + 1);
            }
            break;
        case 3: snprintf(out, 16, "%d", c.volume);     break;
        case 4: snprintf(out, 16, "%+d", c.transpose); break;
        case 5: snprintf(out, 16, "%s", c.name[0] ? c.name : "--"); break;
    }
}

void ColumnEditor::drawFieldRow(int field) {
    int y = CE_ROW_Y + field * CE_ROW_H;
    _d.fillRect(0, y, CE_W, CE_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, y + CE_ROW_H - 1, CE_W, COL_LTGREY);

    int btnY = y + (CE_ROW_H - CE_BTN_H) / 2;

    char label[16], value[16];
    fieldLabel(field, label);
    fieldValue(field, value);

    bool sfx = isSfx(_song.columns[_col].midiChannel);
    bool pxl = isPxl(_song.columns[_col].midiChannel);
    // BANK is meaningless on SFX and PXL; PROGRAM is meaningless on PXL.
    // Render greyed and disable +/-.
    bool disabled = (sfx && field == 1) || (pxl && (field == 1 || field == 2));
    uint16_t fg = disabled ? COL_DKGREY : COL_BLACK;

    _d.setTextSize(3);
    _d.setTextColor(fg);
    _d.setCursor(CE_LABEL_X, y + (CE_ROW_H - 24) / 2);
    _d.print(label);

    // Value box
    _d.fillRect(CE_VAL_X, btnY, CE_VAL_W, CE_BTN_H, COL_WHITE);
    _d.drawRect(CE_VAL_X, btnY, CE_VAL_W, CE_BTN_H, disabled ? COL_LTGREY : COL_BLACK);
    int vw = strlen(value) * 18;
    _d.setCursor(CE_VAL_X + (CE_VAL_W - vw) / 2, btnY + (CE_BTN_H - 24) / 2);
    _d.setTextColor(fg);
    _d.print(value);

    // −/+ buttons (not for NAME field — NAME row hosts the PICK INSTR/SAMPLE
    // button instead).  BANK +/- is hidden when on SFX.
    if (field < 5) {
        if (disabled) {
            // No buttons drawn; row is purely informational.
        } else {
            uiButton(_d, CE_MINUS_X, btnY, CE_BTN_W, CE_BTN_H, "-", COL_WHITE, COL_BLACK, 3);
            uiButton(_d, CE_PLUS_X,  btnY, CE_BTN_W, CE_BTN_H, "+", COL_WHITE, COL_BLACK, 3);
        }
    } else if (!pxl) {
        // PXL has no instrument/sample picker — name is fixed to "PIXEL POST".
        const char* pickLabel = sfx ? "PICK SAMPLE" : "PICK INSTR";
        uiButton(_d, CE_PICK_X, btnY, CE_PICK_W, CE_BTN_H,
                 pickLabel, COL_WHITE, COL_BLACK, 2);
    }
}

void ColumnEditor::drawAllFields() {
    for (int i = 0; i < CE_NUM_FIELDS; i++) drawFieldRow(i);
}

void ColumnEditor::drawPickButton() {
    // PICK INSTR button is rendered as part of the NAME row (field 5).
    drawFieldRow(5);
}

void ColumnEditor::draw() {
    _d.fillScreen(COL_WHITE);
    drawHeader();

    // Label strip
    _d.fillRect(0, CE_LBL_Y, CE_W, CE_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, CE_LBL_Y + (CE_LBL_H - 16) / 2);
    _d.print("MIDI SETTINGS");

    drawAllFields();   // also draws PICK INSTR (in NAME row right gap)
    drawActionBar();
}

// ── Instrument list overlay ──────────────────────────────────────────────────

void ColumnEditor::drawPickList() {
    // Draw over the field rows area
    int y0 = CE_LBL_Y;
    _d.fillRect(0, y0, CE_W, CE_H - y0, COL_WHITE);

    // Label
    _d.fillRect(0, y0, CE_W, CE_LBL_H, COL_DKGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(10, y0 + 2);
    _d.print("SELECT INSTRUMENT");

    int listY = y0 + CE_LBL_H;
    int start = _pickPage * CE_LIST_ROWS;
    for (int i = 0; i < CE_LIST_ROWS; i++) {
        int idx = start + i;
        if (idx >= MAX_INSTRUMENTS) break;
        int ry = listY + i * CE_LIST_ROW_H;

        _d.drawFastHLine(0, ry + CE_LIST_ROW_H - 1, CE_W, COL_LTGREY);

        char label[32];
        if (_instruments) {
            const Instrument& inst = _instruments[idx];
            snprintf(label, sizeof(label), "%02X: %s  B%d P%d V%d",
                     idx, inst.name, inst.bankMSB + 1, inst.program + 1, inst.volume);
        } else {
            snprintf(label, sizeof(label), "%02X", idx);
        }
        _d.setTextSize(2);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(20, ry + (CE_LIST_ROW_H - 16) / 2);
        _d.print(label);
    }

    // Nav buttons at bottom
    int navY = listY + CE_LIST_ROWS * CE_LIST_ROW_H;
    int totalPages = (MAX_INSTRUMENTS + CE_LIST_ROWS - 1) / CE_LIST_ROWS;
    char pg[16];
    snprintf(pg, sizeof(pg), "%d/%d", _pickPage + 1, totalPages);

    uiButton(_d, CE_LIST_PREV_X, navY, CE_LIST_NAV_W, CE_BTN_H, "PREV", COL_WHITE, COL_BLACK, 3);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int pw = strlen(pg) * 18;
    _d.setCursor((CE_W - pw) / 2, navY + (CE_BTN_H - 24) / 2);
    _d.print(pg);
    uiButton(_d, CE_LIST_NEXT_X, navY, CE_LIST_NAV_W, CE_BTN_H, "NEXT", COL_WHITE, COL_BLACK, 3);

    // CANCEL button
    uiButton(_d, (CE_W - 200) / 2, navY + CE_BTN_H + 8, 200, CE_BTN_H, "CANCEL", COL_DKGREY, COL_WHITE, 3);
}

// ── Sample picker overlay (SFX columns) ──────────────────────────────────────

void ColumnEditor::drawPickSampleList() {
    int y0 = CE_LBL_Y;
    _d.fillRect(0, y0, CE_W, CE_H - y0, COL_WHITE);

    _d.fillRect(0, y0, CE_W, CE_LBL_H, COL_DKGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(10, y0 + 2);
    _d.print("SELECT SAMPLE");

    int listY = y0 + CE_LBL_H;
    int total = gServerPairing.sampleListCount();
    SampleListState st = gServerPairing.sampleListState();

    // Loading / empty messaging — centred over the list area.
    const char* msg = nullptr;
    if (st == SampleListState::WAITING || st == SampleListState::PARTIAL)
        msg = "Loading samples...";
    else if (total == 0)
        msg = "No samples found in /samples";

    if (msg) {
        _d.setTextSize(3);
        _d.setTextColor(COL_DKGREY);
        int mw = (int)strlen(msg) * 18;
        _d.setCursor((CE_W - mw) / 2, listY + (CE_LIST_ROWS * CE_LIST_ROW_H - 24) / 2);
        _d.print(msg);
    } else {
        int start = _pickPage * CE_LIST_ROWS;
        for (int i = 0; i < CE_LIST_ROWS; i++) {
            int idx = start + i;
            if (idx >= total) break;
            const SampleCacheEntry* e = gServerPairing.sampleListEntry(idx);
            if (!e) break;
            int ry = listY + i * CE_LIST_ROW_H;

            _d.drawFastHLine(0, ry + CE_LIST_ROW_H - 1, CE_W, COL_LTGREY);

            char label[48];
            snprintf(label, sizeof(label), "%3u: %s", e->id, e->name);
            _d.setTextSize(2);
            _d.setTextColor(COL_BLACK);
            _d.setCursor(20, ry + (CE_LIST_ROW_H - 16) / 2);
            _d.print(label);
        }
    }

    int navY = listY + CE_LIST_ROWS * CE_LIST_ROW_H;
    int totalPages = total > 0 ? (total + CE_LIST_ROWS - 1) / CE_LIST_ROWS : 1;
    char pg[16];
    snprintf(pg, sizeof(pg), "%d/%d", _pickPage + 1, totalPages);

    uiButton(_d, CE_LIST_PREV_X, navY, CE_LIST_NAV_W, CE_BTN_H, "PREV", COL_WHITE, COL_BLACK, 3);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int pw = strlen(pg) * 18;
    _d.setCursor((CE_W - pw) / 2, navY + (CE_BTN_H - 24) / 2);
    _d.print(pg);
    uiButton(_d, CE_LIST_NEXT_X, navY, CE_LIST_NAV_W, CE_BTN_H, "NEXT", COL_WHITE, COL_BLACK, 3);

    uiButton(_d, (CE_W - 200) / 2, navY + CE_BTN_H + 8, 200, CE_BTN_H, "CANCEL", COL_DKGREY, COL_WHITE, 3);
}

// ── Action bar (COPY / SWAP) ─────────────────────────────────────────────────

void ColumnEditor::drawActionBar() {
    int btnY = CE_ACT_Y + (CE_ACT_H - CE_ACT_BTN_H) / 2;
    _d.fillRect(0, CE_ACT_Y, CE_W, CE_ACT_H, COL_WHITE);
    _d.drawFastHLine(0, CE_ACT_Y, CE_W, COL_LTGREY);

    char clearLabel[20], copyLabel[20], swapLabel[20];
    snprintf(clearLabel, sizeof(clearLabel), "CLEAR COL %d",   _col + 1);
    snprintf(copyLabel,  sizeof(copyLabel),  "COPY COL %d TO...", _col + 1);
    snprintf(swapLabel,  sizeof(swapLabel),  "SWAP COL %d WITH...", _col + 1);

    uiButton(_d, CE_CLEAR_X, btnY, CE_CLEAR_W, CE_ACT_BTN_H,
             clearLabel, COL_WHITE, COL_BLACK, 2);
    uiButton(_d, CE_COPY_X,  btnY, CE_COPY_W,  CE_ACT_BTN_H,
             copyLabel,  COL_WHITE, COL_BLACK, 2);
    uiButton(_d, CE_SWAP_X,  btnY, CE_SWAP_W,  CE_ACT_BTN_H,
             swapLabel,  COL_WHITE, COL_BLACK, 2);
}

// ── Column-picker overlay (COPY/SWAP destination) ────────────────────────────

void ColumnEditor::drawColumnPicker() {
    int y0 = CE_LBL_Y;
    _d.fillRect(0, y0, CE_W, CE_H - y0, COL_WHITE);

    // Title strip
    const char* title = (_action == Action::PICK_COPY_DST)
                        ? "COPY TO COLUMN..." : "SWAP WITH COLUMN...";
    _d.fillRect(0, CE_PICKCOL_TITLE_Y, CE_W, CE_PICKCOL_TITLE_H, COL_LTGREY);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(title) * 18;
    _d.setCursor((CE_W - tw) / 2, CE_PICKCOL_TITLE_Y + (CE_PICKCOL_TITLE_H - 24) / 2);
    _d.print(title);

    // Column buttons (cols 1..MAX_COLUMNS-1; col 0 is INPUT, excluded)
    for (int c = 1; c < MAX_COLUMNS; c++) {
        int slot = c - 1;
        int row = slot / CE_PICKCOL_COLS;
        int colInRow = slot % CE_PICKCOL_COLS;
        int x = CE_PICKCOL_X0 + colInRow * (CE_PICKCOL_BTN_W + CE_PICKCOL_GAP);
        int y = CE_PICKCOL_GRID_Y + row * (CE_PICKCOL_BTN_H + CE_PICKCOL_GAP);

        bool isSelf = (c == _col);
        char label[24];
        const ColumnSettings& cs = _song.columns[c];
        if (cs.name[0]) snprintf(label, sizeof(label), "COL %d: %s", c + 1, cs.name);
        else            snprintf(label, sizeof(label), "COL %d", c + 1);

        if (isSelf) {
            // Disabled — current column
            _d.fillRect(x, y, CE_PICKCOL_BTN_W, CE_PICKCOL_BTN_H, COL_WHITE);
            _d.drawRect(x, y, CE_PICKCOL_BTN_W, CE_PICKCOL_BTN_H, COL_LTGREY);
            _d.setTextSize(2);
            _d.setTextColor(COL_DKGREY);
            int lw = (int)strlen(label) * 12;
            _d.setCursor(x + (CE_PICKCOL_BTN_W - lw) / 2,
                         y + (CE_PICKCOL_BTN_H - 16) / 2 - 10);
            _d.print(label);
            _d.setCursor(x + (CE_PICKCOL_BTN_W - 9*12) / 2,
                         y + (CE_PICKCOL_BTN_H - 16) / 2 + 14);
            _d.print("(current)");
        } else {
            uiButton(_d, x, y, CE_PICKCOL_BTN_W, CE_PICKCOL_BTN_H,
                     label, COL_WHITE, COL_BLACK, 2);
        }
    }

    // CANCEL
    uiButton(_d, (CE_W - CE_PICKCOL_CANCEL_W) / 2, CE_PICKCOL_CANCEL_Y,
             CE_PICKCOL_CANCEL_W, CE_PICKCOL_CANCEL_H,
             "CANCEL", COL_DKGREY, COL_WHITE, 3);
}

int ColumnEditor::hitColumnPicker(int sx, int sy) const {
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (c == _col) continue;
        int slot = c - 1;
        int row = slot / CE_PICKCOL_COLS;
        int colInRow = slot % CE_PICKCOL_COLS;
        int x = CE_PICKCOL_X0 + colInRow * (CE_PICKCOL_BTN_W + CE_PICKCOL_GAP);
        int y = CE_PICKCOL_GRID_Y + row * (CE_PICKCOL_BTN_H + CE_PICKCOL_GAP);
        if (sx >= x && sx < x + CE_PICKCOL_BTN_W &&
            sy >= y && sy < y + CE_PICKCOL_BTN_H) {
            return c;
        }
    }
    return -1;
}

// ── Copy-confirm dialog ──────────────────────────────────────────────────────

void ColumnEditor::drawCopyConfirm() {
    int y0 = CE_LBL_Y;
    _d.fillRect(0, y0, CE_W, CE_H - y0, COL_WHITE);

    char title[40];
    snprintf(title, sizeof(title), "OVERWRITE COL %d?", _actionDst + 1);
    _d.setTextSize(4);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(title) * 24;
    _d.setCursor((CE_W - tw) / 2, CE_CONF_TITLE_Y);
    _d.print(title);

    char body1[64], body2[64];
    snprintf(body1, sizeof(body1), "Replaces all settings AND notes");
    snprintf(body2, sizeof(body2), "in column %d (across every block).", _actionDst + 1);
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    int b1w = (int)strlen(body1) * 12;
    int b2w = (int)strlen(body2) * 12;
    _d.setCursor((CE_W - b1w) / 2, CE_CONF_BODY_Y);
    _d.print(body1);
    _d.setCursor((CE_W - b2w) / 2, CE_CONF_BODY_Y + 30);
    _d.print(body2);

    uiButton(_d, CE_CONF_YES_X, CE_CONF_BTN_Y, CE_CONF_BTN_W, CE_CONF_BTN_H,
             "YES", COL_BLACK, COL_WHITE, 4);
    uiButton(_d, CE_CONF_NO_X,  CE_CONF_BTN_Y, CE_CONF_BTN_W, CE_CONF_BTN_H,
             "NO",  COL_WHITE, COL_BLACK, 4);
}

void ColumnEditor::drawClearConfirm() {
    int y0 = CE_LBL_Y;
    _d.fillRect(0, y0, CE_W, CE_H - y0, COL_WHITE);

    char title[40];
    snprintf(title, sizeof(title), "CLEAR COL %d?", _col + 1);
    _d.setTextSize(4);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(title) * 24;
    _d.setCursor((CE_W - tw) / 2, CE_CONF_TITLE_Y);
    _d.print(title);

    char body1[64], body2[64];
    snprintf(body1, sizeof(body1), "Resets settings AND deletes all");
    snprintf(body2, sizeof(body2), "notes in column %d (across every block).", _col + 1);
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    int b1w = (int)strlen(body1) * 12;
    int b2w = (int)strlen(body2) * 12;
    _d.setCursor((CE_W - b1w) / 2, CE_CONF_BODY_Y);
    _d.print(body1);
    _d.setCursor((CE_W - b2w) / 2, CE_CONF_BODY_Y + 30);
    _d.print(body2);

    uiButton(_d, CE_CONF_YES_X, CE_CONF_BTN_Y, CE_CONF_BTN_W, CE_CONF_BTN_H,
             "YES", COL_BLACK, COL_WHITE, 4);
    uiButton(_d, CE_CONF_NO_X,  CE_CONF_BTN_Y, CE_CONF_BTN_W, CE_CONF_BTN_H,
             "NO",  COL_WHITE, COL_BLACK, 4);
}

// ── Apply COPY / SWAP / CLEAR ────────────────────────────────────────────────

void ColumnEditor::doCopyColumn(uint8_t srcCol, uint8_t dstCol) {
    if (srcCol == dstCol) return;
    if (srcCol >= MAX_COLUMNS || dstCol >= MAX_COLUMNS) return;

    // 1) ColumnSettings
    _song.columns[dstCol] = _song.columns[srcCol];

    // 2) Notes — for every pattern, every row: dst <- src
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        Pattern& pat = _song.patterns[p];
        NoteGrid g(_song.notePool, &_song.noteFreeHead, &pat.noteHead);
        for (uint8_t r = 0; r < MAX_ROWS; r++) {
            Note srcN = g.get(r, srcCol);
            if (srcN.note == NOTE_EMPTY) {
                g.clear(r, dstCol);
            } else {
                g.set(r, dstCol, srcN);
            }
        }
    }

    _resyncMask |= (uint16_t)(1u << dstCol);
}

void ColumnEditor::doSwapColumn(uint8_t a, uint8_t b) {
    if (a == b) return;
    if (a >= MAX_COLUMNS || b >= MAX_COLUMNS) return;

    // 1) ColumnSettings swap
    ColumnSettings tmp = _song.columns[a];
    _song.columns[a]   = _song.columns[b];
    _song.columns[b]   = tmp;

    // 2) Notes — for every pattern, every row: swap a/b
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        Pattern& pat = _song.patterns[p];
        NoteGrid g(_song.notePool, &_song.noteFreeHead, &pat.noteHead);
        for (uint8_t r = 0; r < MAX_ROWS; r++) {
            Note nA = g.get(r, a);
            Note nB = g.get(r, b);
            if (nB.note == NOTE_EMPTY) g.clear(r, a); else g.set(r, a, nB);
            if (nA.note == NOTE_EMPTY) g.clear(r, b); else g.set(r, b, nA);
        }
    }

    _resyncMask |= (uint8_t)((1u << a) | (1u << b));
}

void ColumnEditor::doClearColumn(uint8_t col) {
    if (col >= MAX_COLUMNS) return;

    // 1) ColumnSettings — reset to init defaults (zeroed; volume = 100)
    memset(&_song.columns[col], 0, sizeof(ColumnSettings));
    _song.columns[col].volume = 100;

    // 2) Notes — clear every cell in this column across every pattern
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        Pattern& pat = _song.patterns[p];
        NoteGrid g(_song.notePool, &_song.noteFreeHead, &pat.noteHead);
        for (uint8_t r = 0; r < MAX_ROWS; r++) {
            g.clear(r, col);
        }
    }

    _resyncMask |= (uint8_t)(1u << col);
}

// ── Field adjustment ─────────────────────────────────────────────────────────

void ColumnEditor::adjustField(int field, int delta) {
    ColumnSettings& c = cs();
    switch (field) {
        case 0: {  // MIDI CH: 0(off)..16, 17=SFX, 18=PXL (pixel_post)
            int v = (int)c.midiChannel + delta;
            if (v < 0) v = 0;
            if (v > PIXELPOST_CHANNEL) v = PIXELPOST_CHANNEL;
            bool enteredSfx = (v == SFX_CHANNEL       && c.midiChannel != SFX_CHANNEL);
            bool enteredPxl = (v == PIXELPOST_CHANNEL && c.midiChannel != PIXELPOST_CHANNEL);
            c.midiChannel = (uint8_t)v;
            // First entry into SFX — start fetching the sample list so the
            // picker has something to show.
            if (enteredSfx && gServerPairing.isPaired()) {
                gServerPairing.requestSampleList();
            }
            // First entry into PXL — fix the column name to "PIXEL POST".
            // PXL columns have no per-column picker; the name is the label.
            if (enteredPxl) {
                strncpy(c.name, "PIXEL POST", INSTRUMENT_NAME_LEN - 1);
                c.name[INSTRUMENT_NAME_LEN - 1] = '\0';
            }
            break;
        }
        case 1: {  // BANK — locked on SFX and PXL
            if (isSfx(c.midiChannel) || isPxl(c.midiChannel)) return;
            int v = (int)c.bankMSB + delta;
            c.bankMSB = (uint8_t)constrain(v, 0, (int)gMaxBank);
            break;
        }
        case 2: {  // PROGRAM — locked on PXL
            if (isPxl(c.midiChannel)) return;
            if (isSfx(c.midiChannel)) {
                // Walk through the cached sample list by index.  PROG holds
                // the manifest id; PROG=0 = "no sample".  Map current id to
                // an index, bump it, write back the new id and name.
                int n = gServerPairing.sampleListCount();
                if (n <= 0) return;
                int curIdx = sampleIndexForId(c.program);
                int newIdx;
                if (curIdx < 0) {
                    newIdx = (delta > 0) ? 0 : n - 1;
                } else {
                    newIdx = curIdx + delta;
                    if (newIdx < 0)  newIdx = 0;
                    if (newIdx >= n) newIdx = n - 1;
                }
                const SampleCacheEntry* e = gServerPairing.sampleListEntry(newIdx);
                if (e) {
                    c.program = e->id;
                    copySampleNameToCol(e->name, c.name);
                }
                break;
            }
            int v = (int)c.program + delta;
            if (v > (int)gMaxProgram) {
                v = 0;
                if (c.bankMSB < gMaxBank) c.bankMSB++;
            } else if (v < 0) {
                v = (int)gMaxProgram;
                if (c.bankMSB > 0) c.bankMSB--;
            }
            c.program = (uint8_t)v;
            break;
        }
        case 3: {  // VOLUME
            int v = (int)c.volume + delta;
            c.volume = (uint8_t)constrain(v, 0, 127);
            break;
        }
        case 4: {  // TRANSPOSE
            int v = (int)c.transpose + delta;
            c.transpose = (int8_t)constrain(v, -24, 24);
            break;
        }
    }
    _patchPending = true;
}

void ColumnEditor::fireHeld() {
    int f = _hold.field();
    if (f >= 0 && f < 5) {
        adjustField(f, _hold.delta());
        // CH can flip the column in/out of SFX, which changes how BANK / PROG
        // / NAME render — repaint them.  PROG on SFX rewrites NAME.  PROG in
        // MIDI mode may wrap into BANK.
        if (f == 0) {
            drawAllFields();
            drawHeader();
        } else if (f == 2) {
            drawFieldRow(f);
            if (isSfx(_song.columns[_col].midiChannel)) drawFieldRow(5);
            else                                        drawFieldRow(1);
            drawHeader();
        } else {
            drawFieldRow(f);
        }
        _d.paint();
    }
}

// ── Poll ─────────────────────────────────────────────────────────────────────

bool ColumnEditor::poll() {
    // ── Naming — keyboard active ─────────────────────────────────────────────
    if (_naming) {
        if (_keyboard.poll()) {
            if (_keyboard.isDone()) _patchPending = true;
            _naming = false;
            _d.fillScreen(COL_WHITE);
            draw();
            _d.paintLater();
        }
        return false;
    }

    // Repaint the sample picker when the async list arrives.
    if (_pickingSample) {
        uint8_t cur = (uint8_t)gServerPairing.sampleListState();
        if (cur != _sampleListSeenState) {
            _sampleListSeenState = cur;
            drawPickSampleList();
            _d.paint();
        }
    }

    // Hold-repeat
    if (_hold.active() && _hold.tickFast()) fireHeld();

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int tx, ty;
    rawToScreen(_touch.x, _touch.y, tx, ty);

    bool rising  = (down && !_wasDown);
    bool falling = (!down && _wasDown);
    _wasDown = down;

    if (falling) _hold.release();

    if (!rising && !falling) return false;

    // ── COPY/SWAP destination picker ─────────────────────────────────────────
    if (_action == Action::PICK_COPY_DST || _action == Action::PICK_SWAP_DST) {
        _pressedOnName = false;
        if (!rising) return false;

        // CANCEL
        int cancelX = (CE_W - CE_PICKCOL_CANCEL_W) / 2;
        if (ty >= CE_PICKCOL_CANCEL_Y && ty < CE_PICKCOL_CANCEL_Y + CE_PICKCOL_CANCEL_H &&
            tx >= cancelX && tx < cancelX + CE_PICKCOL_CANCEL_W) {
            _action = Action::NONE;
            draw();
            _d.paint();
            return false;
        }

        int dst = hitColumnPicker(tx, ty);
        if (dst >= 1) {
            if (_action == Action::PICK_SWAP_DST) {
                doSwapColumn(_col, (uint8_t)dst);
                _action = Action::NONE;
                draw();
                _d.paint();
            } else {
                // COPY needs confirm
                _actionDst = (uint8_t)dst;
                _action    = Action::CONFIRM_COPY;
                drawCopyConfirm();
                _d.paint();
            }
        }
        return false;
    }

    // ── Copy / Clear confirm dialog ──────────────────────────────────────────
    if (_action == Action::CONFIRM_COPY || _action == Action::CONFIRM_CLEAR) {
        _pressedOnName = false;
        if (!rising) return false;

        if (ty >= CE_CONF_BTN_Y && ty < CE_CONF_BTN_Y + CE_CONF_BTN_H) {
            if (tx >= CE_CONF_YES_X && tx < CE_CONF_YES_X + CE_CONF_BTN_W) {
                if (_action == Action::CONFIRM_COPY) {
                    doCopyColumn(_col, _actionDst);
                } else {
                    doClearColumn(_col);
                }
                _action = Action::NONE;
                draw();
                _d.paint();
                return false;
            }
            if (tx >= CE_CONF_NO_X && tx < CE_CONF_NO_X + CE_CONF_BTN_W) {
                _action = Action::NONE;
                draw();
                _d.paint();
                return false;
            }
        }
        return false;
    }

    // ── Sample picker (SFX) ──────────────────────────────────────────────────
    if (_pickingSample) {
        if (!rising) return false;

        int listY = CE_LBL_Y + CE_LBL_H;
        int navY  = listY + CE_LIST_ROWS * CE_LIST_ROW_H;
        int total = gServerPairing.sampleListCount();
        int totalPages = total > 0 ? (total + CE_LIST_ROWS - 1) / CE_LIST_ROWS : 1;

        // List rows — pick this sample
        if (ty >= listY && ty < navY && total > 0) {
            int row = (ty - listY) / CE_LIST_ROW_H;
            int idx = _pickPage * CE_LIST_ROWS + row;
            const SampleCacheEntry* e = gServerPairing.sampleListEntry(idx);
            if (e) {
                ColumnSettings& c = cs();
                c.program = e->id;
                copySampleNameToCol(e->name, c.name);
                _patchPending = true;
            }
            _pickingSample = false;
            draw();
            _d.paint();
            return false;
        }

        // PREV / NEXT
        if (ty >= navY && ty < navY + CE_BTN_H) {
            if (tx < CE_LIST_NAV_W) {
                if (_pickPage > 0) _pickPage--;
            } else if (tx >= CE_LIST_NEXT_X) {
                if (_pickPage + 1 < totalPages) _pickPage++;
            }
            drawPickSampleList();
            _d.paint();
            return false;
        }

        // CANCEL
        if (ty >= navY + CE_BTN_H + 8 && ty < navY + CE_BTN_H * 2 + 8) {
            _pickingSample = false;
            draw();
            _d.paint();
        }
        return false;
    }

    // ── Picking mode ─────────────────────────────────────────────────────────
    if (_picking) {
        if (!rising) return false;

        int listY = CE_LBL_Y + CE_LBL_H;
        int navY  = listY + CE_LIST_ROWS * CE_LIST_ROW_H;

        // Instrument list rows
        if (ty >= listY && ty < navY) {
            int row = (ty - listY) / CE_LIST_ROW_H;
            int idx = _pickPage * CE_LIST_ROWS + row;
            if (idx < MAX_INSTRUMENTS && _instruments) {
                // Copy instrument settings to column
                ColumnSettings& c = cs();
                const Instrument& inst = _instruments[idx];
                c.bankMSB   = inst.bankMSB;
                c.program   = inst.program;
                c.volume    = inst.volume;
                c.transpose = inst.transpose;
                strncpy(c.name, inst.name, INSTRUMENT_NAME_LEN);
                _patchPending = true;
            }
            _picking = false;
            draw();
            _d.paint();
            return false;
        }

        // PREV / NEXT
        if (ty >= navY && ty < navY + CE_BTN_H) {
            int totalPages = (MAX_INSTRUMENTS + CE_LIST_ROWS - 1) / CE_LIST_ROWS;
            if (tx < CE_LIST_NAV_W) {
                if (_pickPage > 0) _pickPage--;
            } else if (tx >= CE_LIST_NEXT_X) {
                if (_pickPage + 1 < totalPages) _pickPage++;
            }
            drawPickList();
            _d.paint();
            return false;
        }

        // CANCEL button
        if (ty >= navY + CE_BTN_H + 8 && ty < navY + CE_BTN_H * 2 + 8) {
            _picking = false;
            draw();
            _d.paint();
            return false;
        }

        return false;
    }

    // ── Normal mode ──────────────────────────────────────────────────────────

    // HOME button — falling edge
    if (falling && ty < CE_HDR_H && tx >= CE_HOME_X) {
        return true;
    }
    // NAME row — open keyboard on falling edge so the lift is consumed here.
    // Skip the right-side gap which is occupied by the PICK INSTR button.
    // Require that the press also began on the NAME row, so a confirm-dialog
    // YES/NO tap (which spatially overlaps the NAME row) doesn't fall through.
    {
        int nameY = CE_ROW_Y + 5 * CE_ROW_H;
        bool inPick = (ty >= CE_PICK_Y && ty < CE_PICK_Y + CE_PICK_H &&
                       tx >= CE_PICK_X && tx < CE_PICK_X + CE_PICK_W);
        bool inName = (ty >= nameY && ty < nameY + CE_ROW_H && !inPick);
        if (rising) _pressedOnName = inName;
        if (falling) {
            bool wasOnName = _pressedOnName;
            _pressedOnName = false;
            // PXL columns have a fixed name ("PIXEL POST") — no keyboard.
            if (wasOnName && inName && !isPxl(cs().midiChannel)) {
                _keyboard.open(cs().name, INSTRUMENT_NAME_LEN);
                _naming = true;
                _d.fillScreen(COL_WHITE);
                draw();
                _keyboard.draw();
                _d.paintLater();
                return false;
            }
        }
    }
    if (!rising) return false;

    // Field rows
    for (int f = 0; f < CE_NUM_FIELDS; f++) {
        int ry = CE_ROW_Y + f * CE_ROW_H;
        if (ty < ry || ty >= ry + CE_ROW_H) continue;
        int btnY = ry + (CE_ROW_H - CE_BTN_H) / 2;

        if (f == 5) break;  // NAME handled above on falling edge
        // Suppress +/- on disabled rows: BANK is locked on SFX and PXL,
        // PROGRAM is locked on PXL.
        if (f == 1 && (isSfx(cs().midiChannel) || isPxl(cs().midiChannel))) break;
        if (f == 2 && isPxl(cs().midiChannel)) break;
        // −/+ buttons (fields 0-4)
        int sign = 0;
        if (tx >= CE_MINUS_X && tx < CE_MINUS_X + CE_BTN_W)     sign = -1;
        else if (tx >= CE_PLUS_X && tx < CE_PLUS_X + CE_BTN_W)  sign = +1;
        if (sign == 0) break;

        adjustField(f, sign);
        if (f == 0) {
            // CH may have flipped to/from SFX — repaint whole panel.
            drawAllFields();
        } else if (f == 2) {
            drawFieldRow(f);
            if (isSfx(cs().midiChannel)) drawFieldRow(5);  // NAME overwritten by sample
            else                         drawFieldRow(1);  // program wrap may change bank
        } else {
            drawFieldRow(f);
        }
        drawHeader();
        _d.paint();
        _hold.start(f, sign);
        return false;
    }

    // PICK INSTRUMENT / SAMPLE button — no picker on PXL.
    if (!isPxl(cs().midiChannel) &&
        ty >= CE_PICK_Y && ty < CE_PICK_Y + CE_PICK_H) {
        if (tx >= CE_PICK_X && tx < CE_PICK_X + CE_PICK_W) {
            _pickPage = 0;
            if (isSfx(cs().midiChannel)) {
                _pickingSample = true;
                // Always refresh on open — server scans dir and rewrites
                // samples.txt if anything changed.
                if (gServerPairing.isPaired()) gServerPairing.requestSampleList();
                _sampleListSeenState = (uint8_t)gServerPairing.sampleListState();
                drawPickSampleList();
            } else {
                _picking = true;
                drawPickList();
            }
            _d.paint();
        }
        return false;
    }

    // Action bar (CLEAR / COPY / SWAP)
    if (ty >= CE_ACT_Y && ty < CE_ACT_Y + CE_ACT_H) {
        if (tx >= CE_CLEAR_X && tx < CE_CLEAR_X + CE_CLEAR_W) {
            _action = Action::CONFIRM_CLEAR;
            drawClearConfirm();
            _d.paint();
            return false;
        }
        if (tx >= CE_COPY_X && tx < CE_COPY_X + CE_COPY_W) {
            _action = Action::PICK_COPY_DST;
            drawColumnPicker();
            _d.paint();
            return false;
        }
        if (tx >= CE_SWAP_X && tx < CE_SWAP_X + CE_SWAP_W) {
            _action = Action::PICK_SWAP_DST;
            drawColumnPicker();
            _d.paint();
            return false;
        }
    }

    return false;
}

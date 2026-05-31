#include "BlockSettingsPage.h"
#include "NoteGrid.h"
#include <string.h>
#include <stdio.h>

static const char* NOTE_NAMES[12] = {
    "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
};

// ── Constructor ───────────────────────────────────────────────────────────────

BlockSettingsPage::BlockSettingsPage(EPD_PainterAdafruit& display,
                                     GT911_Lite& touch, Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _patIdx(0)
    , _selNote(0)
    , _wasDown(false)
    , _confirmDelete(false)
    , _confirmSplit(false)
    , _didDuplicate(false)
    , _didSplit(false)
    , _keyChangePending(false)
    , _keyChangePat(0)
    , _navChangePending(false)
    , _navChangePat(0)
{}

// ── open() ────────────────────────────────────────────────────────────────────

void BlockSettingsPage::open(uint8_t patIdx) {
    _patIdx        = (patIdx < _song.numPatterns) ? patIdx : 0;
    _selNote       = 0;
    _confirmDelete    = false;
    _confirmSplit     = false;
    _didDuplicate     = false;
    _didSplit         = false;
    _keyChangePending = false;
    _navChangePending = false;
    _holdTrpBtn       = 0;
    _confirmSetAll    = false;
    _setAllAction     = 0;
    _wasDown          = _touch.isTouched;
    Serial.printf("[BSP] open patIdx=%d\n", _patIdx);
}

// ── draw() ────────────────────────────────────────────────────────────────────

void BlockSettingsPage::draw() {
    _d.fillScreen(0);
    drawHeader();
    drawNavRow();
    drawLengthRow();
    drawSectionLabel(BSP_LBL_Y, BSP_LBL_H, "INPUT NOTES");
    drawNoteButtons();
    drawBlockRow();
    drawTransposeRow();
    drawKeyChangeRow();
    drawEndNavRow();
}

// ── Section drawing ───────────────────────────────────────────────────────────

void BlockSettingsPage::drawHeader() {
    _d.fillRect(0, BSP_HDR_Y, BSP_W, BSP_HDR_H, 3);
    _d.setTextSize(3);
    _d.setTextColor(0);
    const char* title = "BLOCK SETTINGS";
    int tw = strlen(title) * 18;
    _d.setCursor((BSP_W - tw) / 2, BSP_HDR_Y + (BSP_HDR_H - 24) / 2);
    _d.print(title);

    // HOME button — top-right, matching SongConfigPage style
    btn(BSP_HOME_X, BSP_HDR_Y, BSP_HOME_W, BSP_HDR_H, "HOME", true);

    // Subheading strip
    _d.fillRect(0, BSP_SUB_Y, BSP_W, BSP_SUB_H, 1);
    _d.setTextSize(2);
    _d.setTextColor(3);
    _d.setCursor(10, BSP_SUB_Y + (BSP_SUB_H - 16) / 2);
    _d.print("BLOCKS");
}

void BlockSettingsPage::drawNavRow() {
    _d.fillRect(0, BSP_NAV_Y, BSP_W, BSP_NAV_H, 0);
    _d.drawFastHLine(0, BSP_NAV_Y + BSP_NAV_H - 1, BSP_W, 3);

    // ── Nav arrows (always shown) ─────────────────────────────────────────────
    btn(0, BSP_NAV_Y, BSP_PREV_W, BSP_NAV_H, "<",
        false, _patIdx == 0);

    // Block index label between arrows
    char label[8];
    snprintf(label, sizeof(label), "%02d/%02d", _patIdx + 1, _song.numPatterns);
    _d.setTextSize(3);
    _d.setTextColor(3);
    int lw = strlen(label) * 18;
    int labelCentreX = BSP_PREV_W + (BSP_NEXT_X - BSP_PREV_W) / 2;
    _d.setCursor(labelCentreX - lw / 2, BSP_NAV_Y + (BSP_NAV_H - 24) / 2);
    _d.print(label);

    btn(BSP_NEXT_X, BSP_NAV_Y, BSP_NEXT_W, BSP_NAV_H, ">",
        false, _patIdx >= _song.numPatterns - 1);

    // ── Action buttons (right side) ───────────────────────────────────────────
    if (_confirmDelete || _confirmSplit) {
        // Replace action buttons with confirmation prompt
        // Label area spans NEW + DUPL slots
        int labelW = BSP_DUP_X + BSP_DUP_W - BSP_NEW_X;
        _d.fillRect(BSP_NEW_X, BSP_NAV_Y, labelW, BSP_NAV_H, 0);
        _d.drawRect(BSP_NEW_X, BSP_NAV_Y, labelW, BSP_NAV_H, 3);
        _d.setTextSize(2);
        _d.setTextColor(3);
        const char* line1 = _confirmDelete ? "Delete This" : "Split This";
        const char* line2 = "Block?";
        int totalH = 16 + 4 + 16;
        int startY = BSP_NAV_Y + (BSP_NAV_H - totalH) / 2;
        _d.setCursor(BSP_NEW_X + (labelW - (int)strlen(line1) * 12) / 2, startY);
        _d.print(line1);
        _d.setCursor(BSP_NEW_X + (labelW - (int)strlen(line2) * 12) / 2, startY + 20);
        _d.print(line2);

        btn(BSP_SPL_X, BSP_NAV_Y, BSP_SPL_W, BSP_NAV_H, "YES", false);
        btn(BSP_DEL_X, BSP_NAV_Y, BSP_DEL_W, BSP_NAV_H, "NO", false);
    } else {
        btn(BSP_NEW_X, BSP_NAV_Y, BSP_NEW_W, BSP_NAV_H, "NEW",
            false, _song.numPatterns >= MAX_PATTERNS);
        btn(BSP_DUP_X, BSP_NAV_Y, BSP_DUP_W, BSP_NAV_H, "DUPL",
            false, _song.numPatterns >= MAX_PATTERNS);
        btn(BSP_SPL_X, BSP_NAV_Y, BSP_SPL_W, BSP_NAV_H, "SPLT",
            false, !canSplit());
        btn(BSP_DEL_X, BSP_NAV_Y, BSP_DEL_W, BSP_NAV_H, "DEL",
            false, _song.numPatterns <= 1);
    }
}

void BlockSettingsPage::drawLengthRow() {
    _d.fillRect(0, BSP_LEN_Y, BSP_W, BSP_LEN_H, 0);
    _d.drawFastHLine(0, BSP_LEN_Y + BSP_LEN_H - 1, BSP_W, 3);

    int btnY = BSP_LEN_Y + (BSP_LEN_H - BSP_BTN_H) / 2;

    _d.setTextSize(3);
    _d.setTextColor(3);
    _d.setCursor(10, BSP_LEN_Y + (BSP_LEN_H - 24) / 2);
    _d.print("LEN:");

    for (int i = 0; i < BSP_LEN_COUNT; i++) {
        int x = BSP_LEN_LBL_W + i * BSP_LEN_BTN_W;
        char lbl[4];
        snprintf(lbl, sizeof(lbl), "%d", BSP_LENGTHS[i]);
        btn(x, btnY, BSP_LEN_BTN_W, BSP_BTN_H, lbl,
            pat().length == BSP_LENGTHS[i]);
    }
}

void BlockSettingsPage::drawSectionLabel(int y, int h, const char* text) {
    _d.fillRect(0, y, BSP_W, h, 2);
    _d.setTextSize(2);
    _d.setTextColor(0);
    _d.setCursor(10, y + (h - 16) / 2);
    _d.print(text);
    _d.drawFastHLine(0, y + h - 1, BSP_W, 3);
}

void BlockSettingsPage::drawNoteButtons() {
    _d.fillRect(0, BSP_NOT_Y, BSP_W, BSP_NOT_H, 0);

    for (int i = 0; i < 12; i++) {
        int x = i * BSP_NOT_W;
        bool sel = (i == (int)_selNote);
        InputNoteEntry& e = pat().inputNotes[i];

        bool hasSettings = (e.switchMode != BlockSwitch::STAY
                         || e.transposeAction != TransposeAction::KEEP);
        uint8_t bg = sel ? 3 : (hasSettings ? 1 : 0);
        uint8_t fg = sel ? 0 : 3;

        _d.fillRect(x, BSP_NOT_Y, BSP_NOT_W, BSP_NOT_H, bg);
        _d.drawRect(x, BSP_NOT_Y, BSP_NOT_W, BSP_NOT_H, 3);

        _d.setTextSize(3);
        _d.setTextColor(fg);
        int nw = strlen(NOTE_NAMES[i]) * 18;
        _d.setCursor(x + (BSP_NOT_W - nw) / 2, BSP_NOT_Y + 6);
        _d.print(NOTE_NAMES[i]);

        char ind[8] = "";
        if (e.switchMode == BlockSwitch::SAME_POS)
            snprintf(ind, sizeof(ind), ">%02d", e.switchTarget + 1);
        else if (e.switchMode == BlockSwitch::TOP)
            snprintf(ind, sizeof(ind), "^%02d", e.switchTarget + 1);
        if (e.transposeAction == TransposeAction::NOTE) {
            int len = strlen(ind);
            snprintf(ind + len, sizeof(ind) - len, "T");
        } else if (e.transposeAction == TransposeAction::CUSTOM) {
            char tv[5];
            snprintf(tv, sizeof(tv), e.transposeValue >= 0 ? "+%d" : "%d",
                     (int)e.transposeValue);
            int len = strlen(ind);
            snprintf(ind + len, sizeof(ind) - len, "%s", tv);
        }
        if (ind[0] != '\0') {
            _d.setTextSize(2);
            _d.setTextColor(fg);
            int iw = strlen(ind) * 12;
            _d.setCursor(x + (BSP_NOT_W - iw) / 2, BSP_NOT_Y + 46);
            _d.print(ind);
        }
    }
}

void BlockSettingsPage::drawBlockRow() {
    _d.fillRect(0, BSP_BLK_Y, BSP_W, BSP_BLK_H, 0);
    _d.drawFastHLine(0, BSP_BLK_Y + BSP_BLK_H - 1, BSP_W, 3);

    int btnY = BSP_BLK_Y + (BSP_BLK_H - BSP_BTN_H) / 2;
    InputNoteEntry& e = entry();

    _d.setTextSize(3);
    _d.setTextColor(3);
    _d.setCursor(10, BSP_BLK_Y + (BSP_BLK_H - 24) / 2);
    _d.print("BLOCK:");

    btn(BSP_BLK_STAY_X, btnY, BSP_BLK_STAY_W, BSP_BTN_H, "STAY",
        e.switchMode == BlockSwitch::STAY);
    btn(BSP_BLK_SPOS_X, btnY, BSP_BLK_SPOS_W, BSP_BTN_H, "SAME POS",
        e.switchMode == BlockSwitch::SAME_POS);
    btn(BSP_BLK_TOP_X, btnY, BSP_BLK_TOP_W, BSP_BTN_H, "TOP",
        e.switchMode == BlockSwitch::TOP);

    bool active = (e.switchMode != BlockSwitch::STAY);

    _d.setTextSize(3);
    _d.setTextColor(active ? 3 : 1);
    _d.setCursor(BSP_BLK_MINUS_X - 36, BSP_BLK_Y + (BSP_BLK_H - 24) / 2);
    _d.print(">");

    btn(BSP_BLK_MINUS_X, btnY, BSP_BLK_ARROW_W, BSP_BTN_H, "-", false, !active);

    char val[4];
    snprintf(val, sizeof(val), "%02d", e.switchTarget + 1);
    _d.fillRect(BSP_BLK_VAL_X, btnY, BSP_BLK_VAL_W, BSP_BTN_H, active ? 0 : 1);
    _d.drawRect(BSP_BLK_VAL_X, btnY, BSP_BLK_VAL_W, BSP_BTN_H, active ? 3 : 1);
    _d.setTextSize(3);
    _d.setTextColor(active ? 3 : 2);
    int vw = strlen(val) * 18;
    _d.setCursor(BSP_BLK_VAL_X + (BSP_BLK_VAL_W - vw) / 2,
                 btnY + (BSP_BTN_H - 24) / 2);
    _d.print(val);

    btn(BSP_BLK_PLUS_X, btnY, BSP_BLK_ARROW_W, BSP_BTN_H, "+", false, !active);
}

void BlockSettingsPage::drawTransposeRow() {
    _d.fillRect(0, BSP_TRP_Y, BSP_W, BSP_TRP_H, 0);
    _d.drawFastHLine(0, BSP_TRP_Y + BSP_TRP_H - 1, BSP_W, 3);

    int btnY = BSP_TRP_Y + (BSP_TRP_H - BSP_BTN_H) / 2;
    InputNoteEntry& e = entry();

    _d.setTextSize(3);
    _d.setTextColor(3);
    _d.setCursor(10, BSP_TRP_Y + (BSP_TRP_H - 24) / 2);
    _d.print("TRANSP:");

    btn(BSP_TRP_KEEP_X, btnY, BSP_TRP_KEEP_W, BSP_BTN_H, "KEEP",
        e.transposeAction == TransposeAction::KEEP);
    btn(BSP_TRP_NOTE_X, btnY, BSP_TRP_NOTE_W, BSP_BTN_H, "NOTE",
        e.transposeAction == TransposeAction::NOTE);
    btn(BSP_TRP_CUST_X, btnY, BSP_TRP_CUST_W, BSP_BTN_H, "CUSTOM",
        e.transposeAction == TransposeAction::CUSTOM);

    bool active = (e.transposeAction == TransposeAction::CUSTOM);

    btn(BSP_TRP_MINUS_X, btnY, BSP_BLK_ARROW_W, BSP_BTN_H, "-", false, !active);

    char val[6];
    snprintf(val, sizeof(val), e.transposeValue >= 0 ? "+%d" : "%d",
             (int)e.transposeValue);
    _d.fillRect(BSP_TRP_VAL_X, btnY, BSP_TRP_VAL_W, BSP_BTN_H, active ? 0 : 1);
    _d.drawRect(BSP_TRP_VAL_X, btnY, BSP_TRP_VAL_W, BSP_BTN_H, active ? 3 : 1);
    _d.setTextSize(3);
    _d.setTextColor(active ? 3 : 2);
    int vw = strlen(val) * 18;
    _d.setCursor(BSP_TRP_VAL_X + (BSP_TRP_VAL_W - vw) / 2,
                 btnY + (BSP_BTN_H - 24) / 2);
    _d.print(val);

    btn(BSP_TRP_PLUS_X, btnY, BSP_BLK_ARROW_W, BSP_BTN_H, "+", false, !active);
}


void BlockSettingsPage::drawSetAllOverlay() {
    // Draw over the transpose row area — white background
    _d.fillRect(0, BSP_TRP_Y, BSP_W, BSP_TRP_H, 0);
    _d.drawFastHLine(0, BSP_TRP_Y + BSP_TRP_H - 1, BSP_W, 3);

    const char* action = (_setAllAction == (uint8_t)TransposeAction::NOTE) ? "NOTE" : "KEEP";
    char msg[40];
    snprintf(msg, sizeof(msg), "Set all keys to %s?", action);
    _d.setTextSize(3);
    _d.setTextColor(3);
    int tw = strlen(msg) * 18;
    _d.setCursor((BSP_W - tw) / 2, BSP_TRP_Y + (BSP_TRP_H - 24) / 2);
    _d.print(msg);

    // YES and NO buttons at fixed positions
    btn(BSP_W - 280, BSP_TRP_Y + 14, 120, BSP_BTN_H, "YES", true);
    btn(BSP_W - 140, BSP_TRP_Y + 14, 120, BSP_BTN_H, "NO", false);
}

void BlockSettingsPage::drawKeyChangeRow() {
    _d.fillRect(0, BSP_KCH_Y, BSP_W, BSP_KCH_H, 0);
    _d.drawFastHLine(0, BSP_KCH_Y + BSP_KCH_H - 1, BSP_W, 3);

    int btnY = BSP_KCH_Y + (BSP_KCH_H - BSP_BTN_H) / 2;

    _d.setTextSize(3);
    _d.setTextColor(3);
    _d.setCursor(10, BSP_KCH_Y + (BSP_KCH_H - 24) / 2);
    _d.print("KEY-CHG:");

    btn(BSP_KCH_SPOS_X, btnY, BSP_KCH_SPOS_W, BSP_BTN_H, "SAME POS",
        pat().keyChangeMode == (uint8_t)KeyChangeMode::SAME_POS);
    btn(BSP_KCH_TOP_X, btnY, BSP_KCH_TOP_W, BSP_BTN_H, "TOP",
        pat().keyChangeMode == (uint8_t)KeyChangeMode::TOP);
}

void BlockSettingsPage::drawEndNavRow() {
    _d.fillRect(0, BSP_END_Y, BSP_W, BSP_END_H, 0);
    _d.drawFastHLine(0, BSP_END_Y + BSP_END_H - 1, BSP_W, 3);

    int btnY = BSP_END_Y + (BSP_END_H - BSP_BTN_H) / 2;

    _d.setTextSize(3);
    _d.setTextColor(3);
    _d.setCursor(10, BSP_END_Y + (BSP_END_H - 24) / 2);
    _d.print("NEXT:");

    uint8_t nav  = pat().blockEndNav;
    bool    isRnt = (nav == NAV_RNT);
    uint8_t mode = nav & NAV_MODE_MASK;

    btn(BSP_END_LOOP_X, btnY, BSP_END_LOOP_W, BSP_BTN_H, "LOOP",
        !isRnt && mode == NAV_LOOP);
    btn(BSP_END_FWD_X, btnY, BSP_END_FWD_W, BSP_BTN_H, "FWD",
        !isRnt && mode == NAV_FWD);
    btn(BSP_END_BACK_X, btnY, BSP_END_BACK_W, BSP_BTN_H, "BACK",
        !isRnt && mode == NAV_BACK);
    btn(BSP_END_ABS_X, btnY, BSP_END_ABS_W, BSP_BTN_H, "ABS",
        !isRnt && mode == NAV_ABS);
    btn(BSP_END_RNT_X, btnY, BSP_END_RNT_W, BSP_BTN_H, "RNT", isRnt);

    // Value cell only active for FWD / BACK / ABS — LOOP and RNT carry no target.
    bool active = !isRnt && (mode != NAV_LOOP);
    uint8_t target = nav & NAV_TARGET_MASK;

    btn(BSP_END_MINUS_X, btnY, BSP_END_ARROW_W, BSP_BTN_H, "-", false, !active);

    char val[4];
    if (!active)            val[0] = '\0';
    else if (mode == NAV_ABS) snprintf(val, sizeof(val), "%02d", target + 1);
    else                      snprintf(val, sizeof(val), "%d", target);

    _d.fillRect(BSP_END_VAL_X, btnY, BSP_END_VAL_W, BSP_BTN_H, active ? 0 : 1);
    _d.drawRect(BSP_END_VAL_X, btnY, BSP_END_VAL_W, BSP_BTN_H, active ? 3 : 1);
    _d.setTextSize(3);
    _d.setTextColor(active ? 3 : 2);
    int vw = strlen(val) * 18;
    _d.setCursor(BSP_END_VAL_X + (BSP_END_VAL_W - vw) / 2,
                 btnY + (BSP_BTN_H - 24) / 2);
    _d.print(val);

    btn(BSP_END_PLUS_X, btnY, BSP_END_ARROW_W, BSP_BTN_H, "+", false, !active);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void BlockSettingsPage::btn(int x, int y, int w, int h, const char* label,
                             bool highlighted, bool greyed) {
    uint8_t bg = greyed ? 0 : (highlighted ? 3 : 0);
    uint8_t fg = highlighted ? 0 : (greyed ? 2 : 3);
    _d.fillRect(x, y, w, h, bg);
    _d.drawRect(x, y, w, h, greyed ? 1 : 3);
    _d.setTextSize(3);
    _d.setTextColor(fg);
    int lw = strlen(label) * 18;
    _d.setCursor(x + (w - lw) / 2, y + (h - 24) / 2);
    _d.print(label);
}

void BlockSettingsPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = BSP_H - rx;
}

void BlockSettingsPage::doDelete() {
    for (uint8_t i = _patIdx; i < _song.numPatterns - 1; i++)
        _song.patterns[i] = _song.patterns[i + 1];
    _song.numPatterns--;
    if (_song.startPattern == _patIdx && _patIdx > 0)
        _song.startPattern--;
    else if (_song.startPattern > _patIdx)
        _song.startPattern--;
    if (_patIdx >= _song.numPatterns) _patIdx = _song.numPatterns - 1;
    _selNote = 0;
    Serial.printf("[BSP] DELETE, now %d patterns\n", _song.numPatterns);
}

bool BlockSettingsPage::canSplit() const {
    if (_song.numPatterns >= MAX_PATTERNS) return false;
    uint8_t len = _song.patterns[_patIdx].length;
    // Only lengths that halve to a valid length: 32→16, 48→24, 64→32
    return (len == 32 || len == 48 || len == 64);
}

void BlockSettingsPage::doSplit() {
    Pattern& src = _song.patterns[_patIdx];
    uint8_t halfLen = src.length / 2;

    // Create new pattern — copy metadata from source
    uint8_t newIdx = _song.numPatterns++;
    _song.patterns[newIdx] = src;
    _song.patterns[newIdx].noteHead = NOTE_NULL;
    _song.patterns[newIdx].length   = halfLen;

    // Move notes from second half of source into new pattern,
    // adjusting row numbers so they start at 0.
    // We need two passes: first copy second-half notes to new pattern,
    // then remove them from source.
    struct SplitCtx { Song* song; uint8_t dstPat; uint8_t halfLen; };
    SplitCtx ctx = { &_song, newIdx, halfLen };

    // Pass 1: copy second-half notes to new pattern
    NoteGrid srcGrid(_song.notePool, &src.noteHead);
    srcGrid.forAll([](uint8_t row, uint8_t col, const Note& n, void* cv) {
        SplitCtx* c = (SplitCtx*)cv;
        if (row >= c->halfLen) {
            NoteGrid g(c->song->notePool, &c->song->noteFreeHead,
                       &c->song->patterns[c->dstPat].noteHead);
            g.set(row - c->halfLen, col, n);
        }
    }, &ctx);

    // Pass 2: remove second-half notes from source
    NoteGrid srcMut(_song.notePool, &_song.noteFreeHead, &src.noteHead);
    for (uint8_t r = halfLen; r < src.length; r++) {
        for (uint8_t c = 0; c < MAX_COLUMNS; c++) {
            srcMut.clear(r, c);
        }
    }

    // Truncate source
    src.length = halfLen;

    _didSplit = true;
    _selNote  = 0;
    Serial.printf("[BSP] SPLIT pat %d (len %d) -> new pat %d (len %d)\n",
                  _patIdx, halfLen, newIdx, halfLen);
}

// ── poll() ────────────────────────────────────────────────────────────────────

bool BlockSettingsPage::poll() {
    // Hold-to-set-all: check timer every frame, even without new touch data
    if (_holdTrpBtn > 0 && !_confirmSetAll) {
        if (millis() - _holdTrpStart >= 1000) {
            _confirmSetAll = true;
            _setAllAction  = (_holdTrpBtn == 1) ? (uint8_t)TransposeAction::KEEP
                                                 : (uint8_t)TransposeAction::NOTE;
            _holdTrpBtn    = 0;
            drawSetAllOverlay();
            _d.paint();
            return false;
        }
    }

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int  tx, ty;
    rawToScreen(_touch.x, _touch.y, tx, ty);

    bool rising  = (down  && !_wasDown);
    bool falling = (!down && _wasDown);
    _wasDown = down;

    if (falling) _holdTrpBtn = 0;

    // Confirmation overlay — handle before normal input
    if (_confirmSetAll) {
        if (!rising) return false;
        int btnY = BSP_TRP_Y + 14;
        if (ty >= btnY && ty < btnY + BSP_BTN_H) {
            if (tx >= BSP_W - 280 && tx < BSP_W - 160) {
                // YES — apply to all 12 keys
                TransposeAction act = (TransposeAction)_setAllAction;
                for (int i = 0; i < 12; i++)
                    pat().inputNotes[i].transposeAction = act;
                _confirmSetAll = false;
                drawTransposeRow(); drawNoteButtons(); _d.paint();
                return false;
            }
            if (tx >= BSP_W - 140 && tx < BSP_W - 20) {
                // NO — dismiss
                _confirmSetAll = false;
                drawTransposeRow(); _d.paint();
                return false;
            }
        }
        return false;
    }

    if (!rising && !falling) return false;

    // HOME — falling edge on header button
    if (falling && ty < BSP_HDR_H && tx >= BSP_HOME_X) {
        Serial.println("[BSP] HOME");
        return true;
    }

    if (!rising) return false;

    // ── Nav + action row ──────────────────────────────────────────────────────
    if (ty >= BSP_NAV_Y && ty < BSP_NAV_Y + BSP_NAV_H) {

        if (_confirmDelete || _confirmSplit) {
            // Confirm overlay: YES at SPL position, NO at DEL position
            if (tx >= BSP_SPL_X && tx < BSP_SPL_X + BSP_SPL_W) {
                if (_confirmDelete) {
                    doDelete();
                    _confirmDelete = false;
                } else {
                    doSplit();
                    _confirmSplit = false;
                }
                draw(); _d.paint();
            } else if (tx >= BSP_DEL_X) {
                _confirmDelete = false;
                _confirmSplit  = false;
                drawNavRow(); _d.paint();
            }
            // Tapping the label or nav arrows while confirming does nothing
            return false;
        }

        // Nav arrows
        if (tx < BSP_PREV_W && _patIdx > 0) {
            _patIdx--;
            _selNote = 0;
            draw(); _d.paint();

        } else if (tx >= BSP_NEXT_X && tx < BSP_NEXT_X + BSP_NEXT_W
                   && _patIdx < _song.numPatterns - 1) {
            _patIdx++;
            _selNote = 0;
            draw(); _d.paint();

        // Action buttons
        } else if (tx >= BSP_NEW_X && tx < BSP_NEW_X + BSP_NEW_W
                   && _song.numPatterns < MAX_PATTERNS) {
            uint8_t idx = _song.numPatterns++;
            Pattern& p  = _song.patterns[idx];
            memset(&p, 0, sizeof(p));
            p.noteHead = NOTE_NULL;
            p.length = 16;
            p.referenceNote = 0;
            _patIdx  = idx;
            _selNote = 0;
            Serial.printf("[BSP] NEW pattern %d\n", idx);
            draw(); _d.paint();

        } else if (tx >= BSP_DUP_X && tx < BSP_DUP_X + BSP_DUP_W
                   && _song.numPatterns < MAX_PATTERNS) {
            uint8_t srcIdx = _patIdx;
            uint8_t idx    = _song.numPatterns++;
            // Copy metadata only; noteHead must start empty
            _song.patterns[idx]          = _song.patterns[srcIdx];
            _song.patterns[idx].noteHead = NOTE_NULL;
            // Deep-copy notes into new pool slots
            struct Ctx { Song* song; uint8_t dstPat; };
            Ctx ctx = { &_song, idx };
            NoteGrid src(_song.notePool, &_song.patterns[srcIdx].noteHead);
            src.forAll([](uint8_t row, uint8_t col, const Note& n, void* cv) {
                Ctx* c = (Ctx*)cv;
                NoteGrid g(c->song->notePool, &c->song->noteFreeHead,
                           &c->song->patterns[c->dstPat].noteHead);
                g.set(row, col, n);
            }, &ctx);
            _patIdx       = idx;
            _selNote      = 0;
            _didDuplicate = true;
            Serial.printf("[BSP] DUPLICATE -> pattern %d\n", idx);
            draw(); _d.paint();

        } else if (tx >= BSP_SPL_X && tx < BSP_SPL_X + BSP_SPL_W
                   && canSplit()) {
            _confirmSplit = true;
            drawNavRow(); _d.paint();

        } else if (tx >= BSP_DEL_X && _song.numPatterns > 1) {
            _confirmDelete = true;
            drawNavRow(); _d.paint();
        }
        return false;
    }

    // ── Length row ────────────────────────────────────────────────────────────
    if (ty >= BSP_LEN_Y && ty < BSP_LEN_Y + BSP_LEN_H) {
        int idx = (tx - BSP_LEN_LBL_W) / BSP_LEN_BTN_W;
        if (idx >= 0 && idx < BSP_LEN_COUNT) {
            pat().length = BSP_LENGTHS[idx];
            Serial.printf("[BSP] length=%d\n", pat().length);
            drawLengthRow(); _d.paint();
        }
        return false;
    }

    // ── Note buttons ──────────────────────────────────────────────────────────
    if (ty >= BSP_NOT_Y && ty < BSP_NOT_Y + BSP_NOT_H) {
        int note = tx / BSP_NOT_W;
        if (note >= 0 && note < 12) {
            _selNote = (uint8_t)note;
            Serial.printf("[BSP] sel note %d (%s)\n", note, NOTE_NAMES[note]);
            drawNoteButtons();
            drawBlockRow();
            drawTransposeRow();
            _d.paint();
        }
        return false;
    }

    // ── Block switch row ──────────────────────────────────────────────────────
    if (ty >= BSP_BLK_Y && ty < BSP_BLK_Y + BSP_BLK_H) {
        InputNoteEntry& e = entry();

        if (tx >= BSP_BLK_STAY_X && tx < BSP_BLK_STAY_X + BSP_BLK_STAY_W) {
            e.switchMode = BlockSwitch::STAY;
            drawBlockRow(); drawNoteButtons(); _d.paint();

        } else if (tx >= BSP_BLK_SPOS_X && tx < BSP_BLK_SPOS_X + BSP_BLK_SPOS_W) {
            e.switchMode = BlockSwitch::SAME_POS;
            drawBlockRow(); drawNoteButtons(); _d.paint();

        } else if (tx >= BSP_BLK_TOP_X && tx < BSP_BLK_TOP_X + BSP_BLK_TOP_W) {
            e.switchMode = BlockSwitch::TOP;
            drawBlockRow(); drawNoteButtons(); _d.paint();

        } else if (e.switchMode != BlockSwitch::STAY) {
            if (tx >= BSP_BLK_MINUS_X && tx < BSP_BLK_MINUS_X + BSP_BLK_ARROW_W) {
                if (e.switchTarget > 0) {
                    e.switchTarget--;
                    drawBlockRow(); drawNoteButtons(); _d.paint();
                }
            } else if (tx >= BSP_BLK_PLUS_X && tx < BSP_BLK_PLUS_X + BSP_BLK_ARROW_W) {
                if (e.switchTarget < _song.numPatterns - 1) {
                    e.switchTarget++;
                    drawBlockRow(); drawNoteButtons(); _d.paint();
                }
            }
        }
        return false;
    }

    // ── Key-change row ────────────────────────────────────────────────────────
    if (ty >= BSP_KCH_Y && ty < BSP_KCH_Y + BSP_KCH_H) {
        if (tx >= BSP_KCH_SPOS_X && tx < BSP_KCH_SPOS_X + BSP_KCH_SPOS_W) {
            pat().keyChangeMode = (uint8_t)KeyChangeMode::SAME_POS;
            _keyChangePending = true; _keyChangePat = _patIdx;
            drawKeyChangeRow(); _d.paint();
        } else if (tx >= BSP_KCH_TOP_X && tx < BSP_KCH_TOP_X + BSP_KCH_TOP_W) {
            pat().keyChangeMode = (uint8_t)KeyChangeMode::TOP;
            _keyChangePending = true; _keyChangePat = _patIdx;
            drawKeyChangeRow(); _d.paint();
        }
        return false;
    }

    // ── End-nav row (NEXT BLK) ───────────────────────────────────────────────
    if (ty >= BSP_END_Y && ty < BSP_END_Y + BSP_END_H) {
        uint8_t nav    = pat().blockEndNav;
        bool    isRnt  = (nav == NAV_RNT);
        uint8_t mode   = nav & NAV_MODE_MASK;
        // RNT has no target — treat it as 0 when re-encoding into another mode.
        uint8_t target = isRnt ? 0 : (nav & NAV_TARGET_MASK);

        if (tx >= BSP_END_LOOP_X && tx < BSP_END_LOOP_X + BSP_END_LOOP_W) {
            pat().blockEndNav = NAV_LOOP;
            _navChangePending = true; _navChangePat = _patIdx;
            drawEndNavRow(); _d.paint();

        } else if (tx >= BSP_END_FWD_X && tx < BSP_END_FWD_X + BSP_END_FWD_W) {
            pat().blockEndNav = NAV_FWD | (target > 0 ? target : 1);
            _navChangePending = true; _navChangePat = _patIdx;
            drawEndNavRow(); _d.paint();

        } else if (tx >= BSP_END_BACK_X && tx < BSP_END_BACK_X + BSP_END_BACK_W) {
            pat().blockEndNav = NAV_BACK | (target > 0 ? target : 1);
            _navChangePending = true; _navChangePat = _patIdx;
            drawEndNavRow(); _d.paint();

        } else if (tx >= BSP_END_ABS_X && tx < BSP_END_ABS_X + BSP_END_ABS_W) {
            pat().blockEndNav = NAV_ABS | target;
            _navChangePending = true; _navChangePat = _patIdx;
            drawEndNavRow(); _d.paint();

        } else if (tx >= BSP_END_RNT_X && tx < BSP_END_RNT_X + BSP_END_RNT_W) {
            pat().blockEndNav = NAV_RNT;
            _navChangePending = true; _navChangePat = _patIdx;
            drawEndNavRow(); _d.paint();

        } else if (!isRnt && mode != NAV_LOOP) {
            if (tx >= BSP_END_MINUS_X && tx < BSP_END_MINUS_X + BSP_END_ARROW_W) {
                if (target > 0) {
                    pat().blockEndNav = mode | (target - 1);
                    _navChangePending = true; _navChangePat = _patIdx;
                    drawEndNavRow(); _d.paint();
                }
            } else if (tx >= BSP_END_PLUS_X && tx < BSP_END_PLUS_X + BSP_END_ARROW_W) {
                uint8_t maxT = (mode == NAV_ABS) ? (_song.numPatterns - 1) : 63;
                if (target < maxT) {
                    pat().blockEndNav = mode | (target + 1);
                    _navChangePending = true; _navChangePat = _patIdx;
                    drawEndNavRow(); _d.paint();
                }
            }
        }
        return false;
    }

    // ── Transpose row ─────────────────────────────────────────────────────────
    if (ty >= BSP_TRP_Y && ty < BSP_TRP_Y + BSP_TRP_H) {
        InputNoteEntry& e = entry();

        if (tx >= BSP_TRP_KEEP_X && tx < BSP_TRP_KEEP_X + BSP_TRP_KEEP_W) {
            e.transposeAction = TransposeAction::KEEP;
            _holdTrpBtn = 1; _holdTrpStart = millis();
            drawTransposeRow(); drawNoteButtons(); _d.paint();

        } else if (tx >= BSP_TRP_NOTE_X && tx < BSP_TRP_NOTE_X + BSP_TRP_NOTE_W) {
            e.transposeAction = TransposeAction::NOTE;
            _holdTrpBtn = 2; _holdTrpStart = millis();
            drawTransposeRow(); drawNoteButtons(); _d.paint();

        } else if (tx >= BSP_TRP_CUST_X && tx < BSP_TRP_CUST_X + BSP_TRP_CUST_W) {
            e.transposeAction = TransposeAction::CUSTOM;
            drawTransposeRow(); drawNoteButtons(); _d.paint();

        } else if (e.transposeAction == TransposeAction::CUSTOM) {
            if (tx >= BSP_TRP_MINUS_X && tx < BSP_TRP_MINUS_X + BSP_BLK_ARROW_W) {
                if (e.transposeValue > -12) {
                    e.transposeValue--;
                    drawTransposeRow(); drawNoteButtons(); _d.paint();
                }
            } else if (tx >= BSP_TRP_PLUS_X && tx < BSP_TRP_PLUS_X + BSP_BLK_ARROW_W) {
                if (e.transposeValue < 12) {
                    e.transposeValue++;
                    drawTransposeRow(); drawNoteButtons(); _d.paint();
                }
            }
        }
        return false;
    }

    return false;
}

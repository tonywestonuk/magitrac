#include "NoteEditor.h"
#include "NoteGrid.h"
#include <string.h>
#include <stdio.h>

static const char* NOTE_NAMES[12] = {
    "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
};

// ── Static clipboard ──────────────────────────────────────────────────────────
bool    NoteEditor::_clipValid    = false;
bool    NoteEditor::_clipHasNote  = false;
uint8_t NoteEditor::_clipSemitone = 0;
uint8_t NoteEditor::_clipOctave   = 4;
uint8_t NoteEditor::_clipVelocity = VEL_DEFAULT;
uint8_t NoteEditor::_clipEffect   = 0;
uint8_t NoteEditor::_clipParam    = 0;

// ── Constructor ───────────────────────────────────────────────────────────────

NoteEditor::NoteEditor(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _song(nullptr)
    , _undoBuf(nullptr)
    , _patternIdx(0)
    , _row(0)
    , _col(0)
    , _open(false)
    , _wasDown(false)
    , _hasNote(false)
    , _offNote(false)
    , _semitone(0)
    , _octave(4)
    , _velocity(VEL_DEFAULT)
    , _effect(0)
    , _param(0)
    , _waitSet(false)
    , _syncSet(false)
    , _hexpad(display, touch)
    , _pendingSync(false)
    , _syncPattern(0)
    , _syncRow(0)
    , _syncCol(0)
{}

// ── open() ────────────────────────────────────────────────────────────────────

void NoteEditor::open(Song* song, uint8_t patternIdx, uint8_t row, uint8_t col,
                      UndoEntry* undoBuf, Instrument* instruments) {
    _song        = song;
    _undoBuf     = undoBuf;
    _instruments = instruments;
    _patternIdx = patternIdx;
    _row        = row;
    _col        = col;
    _open       = true;
    _wasDown    = _touch.isTouched;  // swallow any lingering touch state
    loadRow();
    Serial.printf("[NoteEditor] open  col=%d row=%d\n", col, row);
}

void NoteEditor::loadRow() {
    NoteGrid grid(_song->notePool, &_song->patterns[_patternIdx].noteHead);
    Note n = grid.get(_row, _col);

    // Snapshot pre-edit state so undo always knows what was here before
    if (_undoBuf) {
        _undoBuf->valid      = true;
        _undoBuf->patternIdx = _patternIdx;
        _undoBuf->row        = _row;
        _undoBuf->column     = _col;
        _undoBuf->before     = n;
    }

    _velocity = n.velocity;
    _effect   = n.effect;
    _param    = n.param;

    if (n.note == NOTE_OFF) {
        _offNote  = true;
        _hasNote  = false;
        _semitone = 0;
        _octave   = 4;
    } else if (n.note == NOTE_EMPTY || n.note == NOTE_ANY) {
        _offNote  = false;
        _hasNote  = false;
        _semitone = 0;
        _octave   = 4;
        if (n.note == NOTE_EMPTY) _velocity = VEL_DEFAULT;
    } else {
        _offNote  = false;
        _hasNote  = true;
        _semitone = (n.note - 1) % 12;
        _octave   = (n.note - 1) / 12;
    }

    if (_col == INPUT_COLUMN) {
        _waitSet = (n.effect == EFFECT_WAIT);
        _syncSet = (n.effect == EFFECT_SYNC);
    }
}

// ── Full draw ─────────────────────────────────────────────────────────────────

void NoteEditor::draw() {
    _d.fillScreen(0);  // white
    drawHeader();
    sectionLabel(PE_NLBL_Y, PE_NLBL_H, "NOTE");
    drawNoteButtons();
    sectionLabel(PE_OLBL_Y, PE_OLBL_H, "OCTAVE");
    drawOctaveButtons();
    if (_col == INPUT_COLUMN) {
        sectionLabel(PE_ILBL_Y, PE_ILBL_H, "INPUT OPTIONS");
        drawInputOptions();
    } else {
        sectionLabel(PE_ILBL_Y, PE_ILBL_H, "NOTE OFF / VELOCITY / ATTRIBUTES");
        drawInstControl();
    }
    drawActionButtons();
}

// ── Section drawing ───────────────────────────────────────────────────────────

void NoteEditor::drawHeader() {
    _d.fillRect(0, 0, PE_W, PE_HDR_H, 3);  // black bar

    int patLen  = _song->patterns[_patternIdx].length;
    int btnY    = (PE_HDR_H - 40) / 2;      // vertically centre 40px buttons
    int textY   = (PE_HDR_H - 24) / 2;

    // [<] prev-row button — greyed out at row 0
    bool canPrev = (_row > 0);
    _d.fillRect(0, btnY, PE_HDR_NAVBTN_W, 40, canPrev ? 0 : 2);  // white or grey
    _d.drawRect(0, btnY, PE_HDR_NAVBTN_W, 40, canPrev ? 0 : 2);
    _d.setTextSize(3);
    _d.setTextColor(3);  // black text on light button
    int lw = 1 * 18;
    _d.setCursor((PE_HDR_NAVBTN_W - lw) / 2, btnY + (40 - 24) / 2);
    _d.print("<");

    // [>] next-row button — greyed out at last row
    bool canNext = ((int)_row < patLen - 1);
    int  nxX = PE_W - PE_HDR_NAVBTN_W;
    _d.fillRect(nxX, btnY, PE_HDR_NAVBTN_W, 40, canNext ? 0 : 2);
    _d.drawRect(nxX, btnY, PE_HDR_NAVBTN_W, 40, canNext ? 0 : 2);
    _d.setTextColor(3);
    _d.setCursor(nxX + (PE_HDR_NAVBTN_W - lw) / 2, btnY + (40 - 24) / 2);
    _d.print(">");

    // Cell address
    _d.setTextColor(0);  // white text on black
    char addr[24];
    if (_col == INPUT_COLUMN)
        snprintf(addr, sizeof(addr), "MIDI IN  ROW %02d / %02d", _row, patLen - 1);
    else
        snprintf(addr, sizeof(addr), "COL%d  ROW %02d / %02d", _col + 1, _row, patLen - 1);
    int addrW = strlen(addr) * 18;
    _d.setCursor((PE_W - addrW) / 2, textY);
    _d.print(addr);
}

void NoteEditor::drawNoteButtons() {
    _d.fillRect(0, PE_NBTN_Y, PE_W, PE_NBTN_H, 0);
    for (int i = 0; i < 12; i++) {
        popupBtn(i * PE_NBTN_W, PE_NBTN_Y, PE_NBTN_W, PE_NBTN_H,
                 NOTE_NAMES[i], _hasNote && _semitone == (uint8_t)i);
    }
}

void NoteEditor::drawOctaveButtons() {
    _d.fillRect(0, PE_OBTN_Y, PE_W, PE_OBTN_H, 0);
    for (int i = 0; i < 8; i++) {
        char label[3];
        snprintf(label, sizeof(label), "%d", i);
        popupBtn(i * PE_OBTN_W, PE_OBTN_Y, PE_OBTN_W, PE_OBTN_H,
                 label, _hasNote && _octave == (uint8_t)i);
    }
}

void NoteEditor::drawInstControl() {
    _d.fillRect(0, PE_IBTN_Y, PE_W, PE_IBTN_H, 0);

    // [CLR](160) [OFF](160) [-](80) VV(160) [+](80) [EEPP-](320)
    popupBtn(0,   PE_IBTN_Y, 160, PE_IBTN_H, "CLR", false);
    popupBtn(160, PE_IBTN_Y, 160, PE_IBTN_H, "OFF", _offNote);
    drawVelocity();
    drawAttrButton();
}

void NoteEditor::drawVelocity() {
    // Velocity: x=320, 320px wide: [-](80) value(160) [+](80)
    const int vx = 320;
    const int arrW = 80;
    const int valW = 160;
    int y = PE_IBTN_Y;
    int h = PE_IBTN_H;

    // [-] button
    popupBtn(vx, y, arrW, h, "-", false);

    // Value display
    bool isDef = (_velocity & 0x80);
    char velStr[4];
    if (isDef) {
        snprintf(velStr, sizeof(velStr), "--");
    } else {
        snprintf(velStr, sizeof(velStr), "%02X", _velocity);
    }
    _d.fillRect(vx + arrW, y, valW, h, isDef ? 0 : 1);
    _d.drawRect(vx + arrW, y, valW, h, 3);
    _d.setTextSize(3);
    _d.setTextColor(3);
    int lw = strlen(velStr) * 18;
    _d.setCursor(vx + arrW + (valW - lw) / 2, y + (h - 24) / 2);
    _d.print(velStr);

    // [+] button
    popupBtn(vx + arrW + valW, y, arrW, h, "+", false);
}

void NoteEditor::drawAttrButton() {
    // Attribute button at x=640, 320px wide — shows "EEPP" or "----"
    const int ax = 640;
    const int aw = 320;
    int y = PE_IBTN_Y;
    int h = PE_IBTN_H;

    char attrStr[5];
    effectToString5(_effect, _param, attrStr);

    popupBtn(ax, y, aw, h, attrStr, false);
}

void NoteEditor::drawInputOptions() {
    _d.fillRect(0, PE_IBTN_Y, PE_W, PE_IBTN_H, 0);
    int btnW = PE_W / 3;  // 320px each: CLR | WAIT | SYNC
    popupBtn(0,          PE_IBTN_Y, btnW, PE_IBTN_H, "CLR",  false);
    popupBtn(btnW,       PE_IBTN_Y, btnW, PE_IBTN_H, "WAIT", _waitSet);
    popupBtn(btnW * 2,   PE_IBTN_Y, btnW, PE_IBTN_H, "SYNC", _syncSet);
}

void NoteEditor::drawActionButtons() {
    int actY  = PE_ACT_Y;
    int actH  = PE_H - actY;
    int textY = actY + (actH - 24) / 2;
    int w     = PE_ACT_BTN_W;

    // COPY — light grey, always available
    _d.fillRect(PE_ACT_COPY_X,  actY, w, actH, 1);
    _d.drawRect(PE_ACT_COPY_X,  actY, w, actH, 3);
    _d.setTextSize(3);
    _d.setTextColor(3);
    int lw = 4 * 18;  // "COPY"
    _d.setCursor(PE_ACT_COPY_X + (w - lw) / 2, textY);
    _d.print("COPY");

    // PASTE — light grey if clipboard available, dark grey + lighter text if empty
    uint8_t pasteBg = _clipValid ? 1 : 2;
    uint8_t pasteFg = _clipValid ? 3 : 1;
    _d.fillRect(PE_ACT_PASTE_X, actY, w, actH, pasteBg);
    _d.drawRect(PE_ACT_PASTE_X, actY, w, actH, 3);
    _d.setTextColor(pasteFg);
    lw = 5 * 18;  // "PASTE"
    _d.setCursor(PE_ACT_PASTE_X + (w - lw) / 2, textY);
    _d.print("PASTE");

    // OK — black background
    _d.fillRect(PE_ACT_OK_X,    actY, w, actH, 3);
    _d.drawRect(PE_ACT_OK_X,    actY, w, actH, 3);
    _d.setTextColor(0);
    lw = 2 * 18;  // "OK"
    _d.setCursor(PE_ACT_OK_X + (w - lw) / 2, textY);
    _d.print("OK");
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void NoteEditor::sectionLabel(int y, int h, const char* text) {
    _d.fillRect(0, y, PE_W, h, 2);   // dark grey strip
    _d.setTextSize(2);
    _d.setTextColor(0);               // white text on dark grey
    _d.setCursor(10, y + (h - 16) / 2);
    _d.print(text);
    _d.drawFastHLine(0, y + h - 1, PE_W, 3);
}

void NoteEditor::popupBtn(int x, int y, int w, int h,
                           const char* label, bool highlighted) {
    uint8_t bg = highlighted ? 3 : 0;
    uint8_t fg = highlighted ? 0 : 3;
    _d.fillRect(x, y, w, h, bg);
    _d.drawRect(x, y, w, h, 3);
    _d.setTextSize(3);
    _d.setTextColor(fg);
    int lw = strlen(label) * 18;
    _d.setCursor(x + (w - lw) / 2, y + (h - 24) / 2);
    _d.print(label);
}

void NoteEditor::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = PE_H - rx;
}

void NoteEditor::commit() {
    Note n;
    n.velocity = _velocity;
    if (_offNote) {
        n.note   = NOTE_OFF;
        n.effect = _effect;
        n.param  = _param;
    } else {
        if (_col == INPUT_COLUMN && (_waitSet || _syncSet) && !_hasNote) {
            n.note = NOTE_ANY;  // WAIT/SYNC with no specific note = trigger on any
        } else {
            n.note = _hasNote ? makeNote(_semitone, _octave) : NOTE_EMPTY;
        }
        if (_col == INPUT_COLUMN) {
            n.effect = _waitSet ? EFFECT_WAIT : (_syncSet ? EFFECT_SYNC : 0);
            n.param  = 0;
        } else {
            n.effect = _effect;
            n.param  = _param;
        }
    }
    NoteGrid grid(_song->notePool, &_song->noteFreeHead, &_song->patterns[_patternIdx].noteHead);
    if (n.note == NOTE_EMPTY)
        grid.clear(_row, _col);
    else
        grid.set(_row, _col, n);
    _pendingSync = true;
    _syncPattern = _patternIdx;
    _syncRow     = _row;
    _syncCol     = _col;
    Serial.printf("[NoteEditor] commit: note=%d fx=%d\n", n.note, n.effect);
}

// ── Touch polling ─────────────────────────────────────────────────────────────

bool NoteEditor::pollTouch() {
    // If hexpad is open, delegate to it
    if (_hexpad.isOpen()) {
        if (_hexpad.poll()) {
            // Hexpad closed — apply result
            if (_hexpad.isDone()) {
                if (_hexpad.isCleared()) {
                    _effect = 0;
                    _param  = 0;
                } else {
                    _effect = _hexpad.effect();
                    _param  = _hexpad.param();
                }
            }
            // Redraw note editor
            _d.clear();
            draw();
            _d.paint();
        }
        return false;
    }

    // Hold-repeat for velocity +/-
    if (_hold.active() && _hold.tickFast()) {
        int d = _hold.delta();
        if (_velocity & 0x80) {
            _velocity = 100;
        } else {
            int v = (int)_velocity + d;
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            _velocity = (uint8_t)v;
        }
        drawVelocity();
        _d.paint();
    }

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int  tx, ty;
    rawToScreen(_touch.x, _touch.y, tx, ty);

    bool rising  = (down  && !_wasDown);
    bool falling = (!down && _wasDown);
    _wasDown = down;

    if (falling) _hold.release();

    if (!rising && !falling) return false;

    // ── Header row-navigation buttons ────────────────────────────────────────
    if (rising && ty < PE_HDR_H) {
        int patLen = _song->patterns[_patternIdx].length;

        if (tx < PE_HDR_NAVBTN_W && _row > 0) {
            // [<] previous row — commit current, load previous
            commit();
            _row--;
            loadRow();
            draw();
            _d.paint();
            Serial.printf("[NoteEditor] nav to row %d\n", _row);
        } else if (tx >= PE_W - PE_HDR_NAVBTN_W && (int)_row < patLen - 1) {
            // [>] next row — commit current, load next
            commit();
            _row++;
            loadRow();
            draw();
            _d.paint();
            Serial.printf("[NoteEditor] nav to row %d\n", _row);
        }
        return false;
    }

    // ── Note buttons — tap to select; tap again to clear ─────────────────────
    if (rising && ty >= PE_NBTN_Y && ty < PE_NBTN_Y + PE_NBTN_H) {
        int semi = tx / PE_NBTN_W;
        if (semi >= 0 && semi < 12) {
            _offNote = false;
            if (_hasNote && _semitone == (uint8_t)semi) {
                // Tapping the already-selected note clears it
                _hasNote = false;
                Serial.printf("[NoteEditor] note cleared\n");
            } else {
                _semitone = (uint8_t)semi;
                _hasNote  = true;
                Serial.printf("[NoteEditor] note selected: %s\n", NOTE_NAMES[semi]);
            }
            drawNoteButtons();
            drawOctaveButtons();   // octave highlights also clear when note clears
            if (_col == INPUT_COLUMN) drawInputOptions();
            else                      drawInstControl();
            drawHeader();
            _d.paint();
        }
        return false;
    }

    // ── Octave buttons — activate on press ────────────────────────────────────
    if (rising && ty >= PE_OBTN_Y && ty < PE_OBTN_Y + PE_OBTN_H) {
        int oct = tx / PE_OBTN_W;
        if (oct >= 0 && oct < 8) {
            _offNote = false;
            _octave  = (uint8_t)oct;
            _hasNote = true;
            drawOctaveButtons();
            if (_col == INPUT_COLUMN) drawInputOptions();
            else                      drawInstControl();
            drawHeader();
            _d.paint();
            Serial.printf("[NoteEditor] octave selected: %d\n", oct);
        }
        return false;
    }

    // ── Output column: [CLR](160) [OFF](160) [-](80) VV(160) [+](80) [attr](320)
    if (_col != INPUT_COLUMN &&
        rising && ty >= PE_IBTN_Y && ty < PE_IBTN_Y + PE_IBTN_H) {
        if (tx < 160) {
            // [CLR] — delete note from linked list
            _hasNote  = false;
            _offNote  = false;
            _velocity = VEL_DEFAULT;
            _effect   = 0;
            _param    = 0;
            commit();
            drawNoteButtons();
            drawOctaveButtons();
            drawInstControl();
            drawHeader();
        } else if (tx < 320) {
            // [OFF] — toggle; clears note selection when turned on
            _offNote = !_offNote;
            if (_offNote) _hasNote = false;
            drawNoteButtons();
            drawOctaveButtons();
            drawInstControl();
            drawHeader();
        } else if (tx < 640) {
            // Velocity: [-](320-400) value(400-560) [+](560-640)
            const int vx = 320;
            const int arrW = 80;
            const int valW = 160;
            int relX = tx - vx;
            if (relX < arrW) {
                // [-] decrease — immediate + hold-repeat
                if (_velocity & 0x80) {
                    _velocity = 100;
                } else if (_velocity > 0) {
                    _velocity--;
                }
                _hold.start(0, -1);
            } else if (relX >= arrW + valW) {
                // [+] increase — immediate + hold-repeat
                if (_velocity & 0x80) {
                    _velocity = 100;
                } else if (_velocity < 127) {
                    _velocity++;
                }
                _hold.start(0, +1);
            } else {
                // Tap value — toggle between default and explicit
                _velocity = (_velocity & 0x80) ? 100 : VEL_DEFAULT;
            }
            drawVelocity();
        } else {
            // [attr] — open hex editor for effect/param
            _d.clear();
            _hexpad.open(_effect, _param, 4, "Set Effect");
            _hexpad.draw();
            _d.paint();
            return false;
        }
        _d.paint();
        return false;
    }

    // ── Input options (col 0): [CLR](320) [WAIT](320) [SYNC](320) ─────────────
    if (_col == INPUT_COLUMN &&
        rising && ty >= PE_IBTN_Y && ty < PE_IBTN_Y + PE_IBTN_H) {
        int btnW = PE_W / 3;
        if (tx < btnW) {
            // [CLR] — delete note from linked list
            _hasNote  = false;
            _offNote  = false;
            _waitSet  = false;
            _syncSet  = false;
            _velocity = VEL_DEFAULT;
            _effect   = 0;
            _param    = 0;
            commit();
            drawNoteButtons();
            drawOctaveButtons();
            drawInputOptions();
            drawHeader();
        } else if (tx < btnW * 2) {
            // WAIT: toggle on (clears SYNC), or off
            bool on = !_waitSet;
            _waitSet = on; _syncSet = false;
            drawInputOptions();
        } else {
            // SYNC: toggle on (clears WAIT), or off
            bool on = !_syncSet;
            _syncSet = on; _waitSet = false;
            drawInputOptions();
        }
        _d.paint();
        return false;
    }

    // ── COPY / PASTE — activate on press for quick repeat workflow ───────────
    int actY = PE_ACT_Y;
    if (rising && ty >= actY) {
        if (tx >= PE_ACT_COPY_X && tx < PE_ACT_PASTE_X) {
            // COPY — snapshot working copy into clipboard
            _clipHasNote   = _hasNote;
            _clipSemitone  = _semitone;
            _clipOctave    = _octave;
            _clipVelocity  = _velocity;
            if (_col == INPUT_COLUMN) {
                _clipEffect = _waitSet ? EFFECT_WAIT : (_syncSet ? EFFECT_SYNC : 0);
                _clipParam  = 0;
            } else {
                _clipEffect = _effect;
                _clipParam  = _param;
            }
            _clipValid  = true;
            drawActionButtons();   // PASTE button lights up
            _d.paint();
            Serial.println("[NoteEditor] COPY");
            return false;
        }

        if (tx >= PE_ACT_PASTE_X && tx < PE_ACT_OK_X && _clipValid) {
            // PASTE — load clipboard into working copy
            _hasNote   = _clipHasNote;
            _semitone  = _clipSemitone;
            _octave    = _clipOctave;
            _velocity  = _clipVelocity;
            _effect    = _clipEffect;
            _param     = _clipParam;
            if (_col == INPUT_COLUMN) {
                _waitSet = (_clipEffect == EFFECT_WAIT);
                _syncSet = (_clipEffect == EFFECT_SYNC);
            }
            drawNoteButtons();
            drawOctaveButtons();
            if (_col == INPUT_COLUMN) drawInputOptions();
            else                      drawInstControl();
            drawHeader();
            _d.paint();
            Serial.println("[NoteEditor] PASTE");
            return false;
        }
        return false;  // hit action row but no match (e.g. PASTE with empty clipboard)
    }

    // ── OK — activate on release ──────────────────────────────────────────────
    if (falling && ty >= actY && tx >= PE_ACT_OK_X) {
        Serial.println("[NoteEditor] OK");
        commit();
        _open = false;
        return true;
    }

    return false;
}

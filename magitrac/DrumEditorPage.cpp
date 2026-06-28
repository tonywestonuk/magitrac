#include "DrumEditorPage.h"
#include "UIHelpers.h"
#include "NoteGrid.h"
#include "ServerPairing.h"
#include "DrumTrackParser.h"
#include "DrumKits.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

extern ServerPairing gServerPairing;

static const int TRACKER_TO_MIDI_OFFSET = 11;
// DRUM_MIDI_CHANNEL comes from DrumKits.h (#define, 10 = GM percussion).

// ── GM drum-kit table (MIDI 35..81) ──────────────────────────────────────────
// Each entry: { MIDI note, 2-char abbreviation, full name }.

struct GmDrum { uint8_t midi; const char* abbr; const char* name; };

static const GmDrum GM_DRUMS[] = {
    { 35, "BD", "Ac Bass Drum"    },
    { 36, "BD", "Bass Drum 1"     },
    { 37, "RS", "Side Stick"      },
    { 38, "SD", "Acoustic Snare"  },
    { 39, "CP", "Hand Clap"       },
    { 40, "SD", "Electric Snare"  },
    { 41, "LT", "Low Floor Tom"   },
    { 42, "CH", "Closed Hi-Hat"   },
    { 43, "LT", "High Floor Tom"  },
    { 44, "PH", "Pedal Hi-Hat"    },
    { 45, "MT", "Low Tom"         },
    { 46, "OH", "Open Hi-Hat"     },
    { 47, "MT", "Low-Mid Tom"     },
    { 48, "HT", "Hi-Mid Tom"      },
    { 49, "CY", "Crash Cymbal 1"  },
    { 50, "HT", "High Tom"        },
    { 51, "RD", "Ride Cymbal 1"   },
    { 52, "CC", "Chinese Cymbal"  },
    { 53, "RB", "Ride Bell"       },
    { 54, "TB", "Tambourine"      },
    { 55, "SC", "Splash Cymbal"   },
    { 56, "CB", "Cowbell"         },
    { 57, "CY", "Crash Cymbal 2"  },
    { 58, "VS", "Vibraslap"       },
    { 59, "RD", "Ride Cymbal 2"   },
    { 60, "HB", "High Bongo"      },
    { 61, "LB", "Low Bongo"       },
    { 62, "MC", "Mute Hi Conga"   },
    { 63, "OC", "Open Hi Conga"   },
    { 64, "LC", "Low Conga"       },
    { 65, "HI", "High Timbale"    },
    { 66, "LI", "Low Timbale"     },
    { 67, "HA", "High Agogo"      },
    { 68, "LA", "Low Agogo"       },
    { 69, "CA", "Cabasa"          },
    { 70, "MR", "Maraca"          },
    { 71, "SW", "Short Whistle"   },
    { 72, "LW", "Long Whistle"    },
    { 73, "SG", "Short Guiro"     },
    { 74, "LG", "Long Guiro"      },
    { 75, "CL", "Claves"          },
    { 76, "HW", "Hi Wood Block"   },
    { 77, "LW", "Low Wood Block"  },
    { 78, "MU", "Mute Cuica"      },
    { 79, "OU", "Open Cuica"      },
    { 80, "MT", "Mute Triangle"   },
    { 81, "OT", "Open Triangle"   },
};
static const int GM_DRUMS_N = sizeof(GM_DRUMS) / sizeof(GM_DRUMS[0]);

static const GmDrum* gmFind(uint8_t midiNote) {
    for (int i = 0; i < GM_DRUMS_N; i++) {
        if (GM_DRUMS[i].midi == midiNote) return &GM_DRUMS[i];
    }
    return nullptr;
}

void gmDrumLabel(uint8_t trackerNote, char* buf) {
    uint8_t midi = (uint8_t)(trackerNote + TRACKER_TO_MIDI_OFFSET);
    const GmDrum* d = gmFind(midi);
    if (d) {
        buf[0] = d->abbr[0];
        buf[1] = d->abbr[1];
        buf[2] = '\0';
        return;
    }
    // Fallback: tracker-style note name e.g. "C-2"
    static const char* NAMES[12] = {
        "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
    };
    if (trackerNote >= 1 && trackerNote <= 96) {
        int semi = (trackerNote - 1) % 12;
        int oct  = (trackerNote - 1) / 12;
        buf[0] = NAMES[semi][0];
        buf[1] = NAMES[semi][1];
        buf[2] = (char)('0' + oct);
        buf[3] = '\0';
    } else {
        buf[0] = '?'; buf[1] = '?'; buf[2] = '\0';
    }
}

const char* gmDrumName(uint8_t trackerNote) {
    uint8_t midi = (uint8_t)(trackerNote + TRACKER_TO_MIDI_OFFSET);
    const GmDrum* d = gmFind(midi);
    return d ? d->name : nullptr;
}

// ── Constructor ──────────────────────────────────────────────────────────────

DrumEditorPage::DrumEditorPage(EPD_PainterAdafruit& display, GT911_Lite& touch,
                               Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _patIdx(0)
    , _wasDown(false)
    , _importRequested(false)
    , _resyncMask(0)
    , _laneCount(0)
    , _page(0)
    , _pageCount(1)
    , _pageCols(16)
    , _pageStartRow(0)
    , _laneH(DEP_LANE_H_MAX)
    , _cellW(DEP_GRID_W / 16)
    , _audPlaying(false)
    , _audStep(0)
    , _audNextMs(0)
    , _audStepMs(125)
    , _tempo(120)
    , _pickerActive(false)
    , _pickerScroll(0)
    , _pickerWasDown(false)
    , _pickerDragStartY(0)
    , _pickerDragStartScroll(0)
    , _pickerDragMoved(false)
    , _pickerDragActive(false)
    , _pickerAudActive(false)
    , _pickerAudMidi(0)
    , _pickerAudOffMs(0)
    , _pickerScrollPx(0)
    , _pickerDragStartPx(0)
    , _pickerVelTrackY(0)
    , _pickerVelTrackMs(0)
    , _pickerVelPxPerMs(0)
    , _pickerInertiaActive(false)
    , _pickerInertiaLastTickMs(0)
{
    memset(_laneNotes, 0, sizeof(_laneNotes));
    memset(_cellOn,    0, sizeof(_cellOn));
}

// ── open / draw ──────────────────────────────────────────────────────────────

void DrumEditorPage::open(uint8_t patIdx) {
    _patIdx          = (patIdx < _song.numPatterns) ? patIdx : 0;
    _wasDown         = _touch.isTouched;
    _importRequested = false;
    _resyncMask      = 0;
    _pickerActive    = false;
    _audPlaying      = false;
    _audStep         = 0;
    _page            = 0;
    recomputeStepMs();
    rebuildLanes();
    recomputeGeometry();
    setPage(0);
}

void DrumEditorPage::draw() {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    drawTransport();
    drawLaneLabels();
    drawGrid();
    overlayPlayhead();
}

// ── Lane derivation ──────────────────────────────────────────────────────────

void DrumEditorPage::rebuildLanes() {
    _laneCount = 0;
    NoteGrid g(_song.notePool, &pat().noteHead);
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel != DRUM_MIDI_CHANNEL) continue;
        for (uint8_t r = 0; r < pat().length; r++) {
            Note n = g.get(r, c);
            if (n.note == 0 || n.note == NOTE_OFF) continue;
            // Insert sorted, skip duplicates
            int ins = _laneCount;
            for (int i = 0; i < _laneCount; i++) {
                if (_laneNotes[i] == n.note) { ins = -1; break; }
                if (_laneNotes[i] > n.note)  { ins = i;  break; }
            }
            if (ins < 0) continue;
            if (_laneCount >= DEP_MAX_LANES) continue;
            for (int i = _laneCount; i > ins; i--) _laneNotes[i] = _laneNotes[i - 1];
            _laneNotes[ins] = n.note;
            _laneCount++;
        }
    }
}

bool DrumEditorPage::laneHasNote(uint8_t trackerNote) const {
    for (int i = 0; i < _laneCount; i++)
        if (_laneNotes[i] == trackerNote) return true;
    return false;
}

void DrumEditorPage::addLane(uint8_t trackerNote) {
    if (laneHasNote(trackerNote)) return;
    if (_laneCount >= DEP_MAX_LANES) return;
    int ins = _laneCount;
    for (int i = 0; i < _laneCount; i++) {
        if (_laneNotes[i] > trackerNote) { ins = i; break; }
    }
    for (int i = _laneCount; i > ins; i--) _laneNotes[i] = _laneNotes[i - 1];
    _laneNotes[ins] = trackerNote;
    _laneCount++;
    recomputeGeometry();
    rebuildCellOn();
}

// ── Geometry & paging ────────────────────────────────────────────────────────

void DrumEditorPage::recomputeGeometry() {
    uint8_t L = pat().length;
    if (L <= 32) { _pageCount = 1; _pageCols = L; }
    else         { _pageCount = 2; _pageCols = L / 2; }

    if (_page >= _pageCount) _page = 0;
    _pageStartRow = _page * _pageCols;

    int lanes = (_laneCount > 0) ? _laneCount : 1;
    int h = DEP_GRID_H / lanes;
    if (h < DEP_LANE_H_MIN) h = DEP_LANE_H_MIN;
    if (h > DEP_LANE_H_MAX) h = DEP_LANE_H_MAX;
    _laneH = h;

    _cellW = DEP_GRID_W / _pageCols;
}

void DrumEditorPage::setPage(int p) {
    if (p < 0 || p >= _pageCount) return;
    _page = p;
    _pageStartRow = _page * _pageCols;
    rebuildCellOn();
}

void DrumEditorPage::rebuildCellOn() {
    memset(_cellOn, 0, sizeof(_cellOn));
    NoteGrid g(_song.notePool, &pat().noteHead);
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel != DRUM_MIDI_CHANNEL) continue;
        for (int s = 0; s < _pageCols; s++) {
            int row = _pageStartRow + s;
            if (row >= pat().length) break;
            Note n = g.get((uint8_t)row, (uint8_t)c);
            if (n.note == 0 || n.note == NOTE_OFF) continue;
            for (int L = 0; L < _laneCount; L++) {
                if (_laneNotes[L] == n.note) {
                    _cellOn[L][s] = true;
                    break;
                }
            }
        }
    }
}

// ── Header ───────────────────────────────────────────────────────────────────

void DrumEditorPage::drawHeader() {
    _d.fillRect(0, 0, DEP_W, DEP_HDR_H, COL_BLACK);

    if (_pageCount > 1) {
        uiButton(_d, DEP_PREV_X, 4, DEP_PREV_W, DEP_HDR_H - 8, "<",
                 COL_BLACK, COL_WHITE, 3);
        char pl[8]; snprintf(pl, sizeof(pl), "%d/%d", _page + 1, _pageCount);
        _d.setTextSize(2);
        _d.setTextColor(COL_WHITE);
        int plW = (int)strlen(pl) * 12;
        _d.setCursor(DEP_PAGEL_X + (DEP_PAGEL_W - plW) / 2,
                     (DEP_HDR_H - 16) / 2);
        _d.print(pl);
        uiButton(_d, DEP_NEXT_X, 4, DEP_NEXT_W, DEP_HDR_H - 8, ">",
                 COL_BLACK, COL_WHITE, 3);
    }

    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    const char* title = "DRUM EDITOR";
    int tw = (int)strlen(title) * 18;
    _d.setCursor((DEP_W - tw) / 2, (DEP_HDR_H - 24) / 2);
    _d.print(title);

    uiButton(_d, DEP_ADD_X, 0, DEP_ADD_W, DEP_HDR_H, "+ DRUM",
             COL_BLACK, COL_WHITE, 3);
    uiButton(_d, DEP_HOME_X, 0, DEP_HOME_W, DEP_HDR_H, "BACK",
             COL_BLACK, COL_WHITE, 3);
}

// ── Transport strip (PLAY + BPM) ─────────────────────────────────────────────

void DrumEditorPage::drawTransport() {
    _d.fillRect(0, DEP_TRANS_Y, DEP_W, DEP_TRANS_H, COL_WHITE);
    _d.drawFastHLine(0, DEP_TRANS_Y + DEP_TRANS_H - 1, DEP_W, COL_BLACK);

    drawPlayButton();

    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(DEP_BPM_LBL_X,
                 DEP_TRANS_Y + (DEP_TRANS_H - 24) / 2);
    _d.print("BPM");

    int btnY = DEP_TRANS_Y + (DEP_TRANS_H - DEP_BPM_BTN_H) / 2;
    uiButton(_d, DEP_BPM_MINUS_X, btnY, DEP_BPM_BTN_W, DEP_BPM_BTN_H, "-",
             COL_WHITE, COL_BLACK, 3);
    uiButton(_d, DEP_BPM_PLUS_X,  btnY, DEP_BPM_BTN_W, DEP_BPM_BTN_H, "+",
             COL_WHITE, COL_BLACK, 3);
    drawBpmValue();

    uiButton(_d, DEP_IMPORT_X, btnY, DEP_IMPORT_W, DEP_BPM_BTN_H, "IMPORT",
             COL_WHITE, COL_BLACK, 3);
}

void DrumEditorPage::drawBpmValue() {
    int btnY = DEP_TRANS_Y + (DEP_TRANS_H - DEP_BPM_BTN_H) / 2;
    _d.drawRect(DEP_BPM_VAL_X, btnY, DEP_BPM_VAL_W, DEP_BPM_BTN_H, COL_BLACK);
    _d.fillRect(DEP_BPM_VAL_X + 1, btnY + 1, DEP_BPM_VAL_W - 2,
                DEP_BPM_BTN_H - 2, COL_WHITE);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", _tempo);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(buf) * 18;
    _d.setCursor(DEP_BPM_VAL_X + (DEP_BPM_VAL_W - tw) / 2,
                 btnY + (DEP_BPM_BTN_H - 24) / 2);
    _d.print(buf);
}

void DrumEditorPage::drawPlayButton() {
    const char* lbl = _audPlaying ? "STOP" : "PLAY";
    uint8_t bg = _audPlaying ? COL_BLACK : COL_WHITE;
    uint8_t fg = _audPlaying ? COL_WHITE : COL_BLACK;
    int btnY = DEP_TRANS_Y + (DEP_TRANS_H - DEP_BPM_BTN_H) / 2;
    uiButton(_d, DEP_PLAY_X, btnY, DEP_PLAY_W, DEP_BPM_BTN_H, lbl, bg, fg, 3);
}

// ── Lane labels ──────────────────────────────────────────────────────────────

void DrumEditorPage::drawLaneLabels() {
    for (int r = 0; r < _laneCount; r++) {
        int cy = DEP_GRID_Y + r * _laneH + _laneH / 2;
        int lblW = 44;
        int lblH = (_laneH < 24) ? _laneH - 4 : 22;
        int lblX = (DEP_LBL_W - lblW) / 2;
        int lblY = cy - lblH / 2;

        _d.fillRect(lblX, lblY, lblW, lblH, COL_LTGREY);
        _d.drawRect(lblX, lblY, lblW, lblH, COL_BLACK);

        char buf[5];
        gmDrumLabel(_laneNotes[r], buf);
        _d.setTextSize(2);
        _d.setTextColor(COL_BLACK);
        int tw = (int)strlen(buf) * 12;
        _d.setCursor(lblX + (lblW - tw) / 2, lblY + (lblH - 16) / 2);
        _d.print(buf);
    }
}

// ── Grid ─────────────────────────────────────────────────────────────────────

void DrumEditorPage::drawGrid() {
    int gridH = _laneCount * _laneH;
    if (gridH > DEP_GRID_H) gridH = DEP_GRID_H;
    int gridW = _pageCols * _cellW;

    _d.fillRect(DEP_GRID_X, DEP_GRID_Y, gridW, gridH, COL_WHITE);

    // Bar-group separators every 4 steps
    for (int s = 4; s < _pageCols; s += 4) {
        int x = DEP_GRID_X + s * _cellW;
        _d.drawFastVLine(x, DEP_GRID_Y, gridH, COL_LTGREY);
    }

    // Idle dots
    for (int r = 0; r < _laneCount; r++) {
        int cy = DEP_GRID_Y + r * _laneH + _laneH / 2;
        for (int s = 0; s < _pageCols; s++) {
            int cx = DEP_GRID_X + s * _cellW + _cellW / 2;
            _d.fillCircle(cx, cy, DEP_IDLE_R, COL_LTGREY);
        }
    }

    // Active notes on top
    for (int r = 0; r < _laneCount; r++) {
        for (int s = 0; s < _pageCols; s++) {
            if (_cellOn[r][s]) drawCell(r, s);
        }
    }
}

void DrumEditorPage::drawCell(int lane, int step) {
    int cx = DEP_GRID_X + step * _cellW + _cellW / 2;
    int cy = DEP_GRID_Y + lane * _laneH + _laneH / 2;
    int r  = DEP_DOT_R;
    if (_cellW / 2 - 2 < r) r = _cellW / 2 - 2;
    if (_laneH / 2 - 2 < r) r = _laneH / 2 - 2;
    if (r < 6) r = 6;
    _d.fillCircle(cx, cy, r, COL_BLACK);
}

void DrumEditorPage::drawStepColumn(int step, bool highlighted) {
    if (step < 0 || step >= _pageCols) return;
    int x = DEP_GRID_X + step * _cellW;
    int gridH = _laneCount * _laneH;
    if (gridH <= 0) return;

    uint8_t bg = highlighted ? COL_LTGREY : COL_WHITE;
    _d.fillRect(x, DEP_GRID_Y, _cellW, gridH, bg);

    // Bar-group separator runs on the left edge of every 4th step.
    if (step > 0 && (step % 4) == 0) {
        _d.drawFastVLine(x, DEP_GRID_Y, gridH, COL_LTGREY);
    }
    // ...and on the right edge if the next step is a 4-boundary.
    int nextStep = step + 1;
    if (nextStep < _pageCols && (nextStep % 4) == 0) {
        _d.drawFastVLine(x + _cellW, DEP_GRID_Y, gridH, COL_LTGREY);
    }

    for (int r = 0; r < _laneCount; r++) {
        int cx = x + _cellW / 2;
        int cy = DEP_GRID_Y + r * _laneH + _laneH / 2;
        if (!highlighted) _d.fillCircle(cx, cy, DEP_IDLE_R, COL_LTGREY);
        if (_cellOn[r][step]) drawCell(r, step);
    }
}

void DrumEditorPage::overlayPlayhead() {
    if (!_audPlaying) return;
    int pageStep = (int)_audStep - _pageStartRow;
    if (pageStep < 0 || pageStep >= _pageCols) return;
    drawStepColumn(pageStep, true);
}

// ── Hit tests ────────────────────────────────────────────────────────────────

bool DrumEditorPage::hitHome(int sx, int sy) const {
    return (sx >= DEP_HOME_X && sx < DEP_HOME_X + DEP_HOME_W
            && sy >= 0 && sy < DEP_HDR_H);
}

bool DrumEditorPage::hitAddDrum(int sx, int sy) const {
    return (sx >= DEP_ADD_X && sx < DEP_ADD_X + DEP_ADD_W
            && sy >= 0 && sy < DEP_HDR_H);
}

bool DrumEditorPage::hitPrevPage(int sx, int sy) const {
    if (_pageCount <= 1 || _page == 0) return false;
    return (sx >= DEP_PREV_X && sx < DEP_PREV_X + DEP_PREV_W
            && sy >= 0 && sy < DEP_HDR_H);
}

bool DrumEditorPage::hitNextPage(int sx, int sy) const {
    if (_pageCount <= 1 || _page >= _pageCount - 1) return false;
    return (sx >= DEP_NEXT_X && sx < DEP_NEXT_X + DEP_NEXT_W
            && sy >= 0 && sy < DEP_HDR_H);
}

bool DrumEditorPage::hitGridCell(int sx, int sy, int& lane, int& step) const {
    if (sx < DEP_GRID_X || sx >= DEP_GRID_X + _pageCols * _cellW) return false;
    if (sy < DEP_GRID_Y || sy >= DEP_GRID_Y + _laneCount * _laneH) return false;
    step = (sx - DEP_GRID_X) / _cellW;
    lane = (sy - DEP_GRID_Y) / _laneH;
    return (lane >= 0 && lane < _laneCount && step >= 0 && step < _pageCols);
}

bool DrumEditorPage::hitPlay(int sx, int sy) const {
    int btnY = DEP_TRANS_Y + (DEP_TRANS_H - DEP_BPM_BTN_H) / 2;
    return (sx >= DEP_PLAY_X && sx < DEP_PLAY_X + DEP_PLAY_W
            && sy >= btnY && sy < btnY + DEP_BPM_BTN_H);
}

bool DrumEditorPage::hitBpmMinus(int sx, int sy) const {
    int btnY = DEP_TRANS_Y + (DEP_TRANS_H - DEP_BPM_BTN_H) / 2;
    return (sx >= DEP_BPM_MINUS_X && sx < DEP_BPM_MINUS_X + DEP_BPM_BTN_W
            && sy >= btnY && sy < btnY + DEP_BPM_BTN_H);
}

bool DrumEditorPage::hitBpmPlus(int sx, int sy) const {
    int btnY = DEP_TRANS_Y + (DEP_TRANS_H - DEP_BPM_BTN_H) / 2;
    return (sx >= DEP_BPM_PLUS_X && sx < DEP_BPM_PLUS_X + DEP_BPM_BTN_W
            && sy >= btnY && sy < btnY + DEP_BPM_BTN_H);
}

bool DrumEditorPage::hitImport(int sx, int sy) const {
    int btnY = DEP_TRANS_Y + (DEP_TRANS_H - DEP_BPM_BTN_H) / 2;
    return (sx >= DEP_IMPORT_X && sx < DEP_IMPORT_X + DEP_IMPORT_W
            && sy >= btnY && sy < btnY + DEP_BPM_BTN_H);
}

void DrumEditorPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = DEP_H - rx;
}

// ── Allocation rules ─────────────────────────────────────────────────────────

int DrumEditorPage::findColumnHoldingNote(uint8_t row, uint8_t trackerNote) const {
    NoteGrid g(_song.notePool,
               &_song.patterns[_patIdx].noteHead);
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel != DRUM_MIDI_CHANNEL) continue;
        Note n = g.get(row, (uint8_t)c);
        if (n.note == trackerNote) return c;
    }
    return -1;
}

int DrumEditorPage::findColumnForPlace(uint8_t row, uint8_t trackerNote) {
    NoteGrid g(_song.notePool, &pat().noteHead);

    // Pass 1: ch10 col that already carries this drum elsewhere, with row free
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel != DRUM_MIDI_CHANNEL) continue;
        bool hasDrumElsewhere = false;
        bool rowFree = (g.get(row, (uint8_t)c).note == 0);
        for (uint8_t r = 0; r < pat().length; r++) {
            if (r == row) continue;
            if (g.get(r, (uint8_t)c).note == trackerNote) {
                hasDrumElsewhere = true;
                break;
            }
        }
        if (hasDrumElsewhere && rowFree) return c;
    }
    // Pass 2: any ch10 col with this row free
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel != DRUM_MIDI_CHANNEL) continue;
        if (g.get(row, (uint8_t)c).note == 0) return c;
    }
    // Pass 3: allocate a fresh column
    return allocateNewDrumColumn();
}

int DrumEditorPage::allocateNewDrumColumn() {
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel == 0) {
            ColumnSettings& cs = _song.columns[c];
            memset(&cs, 0, sizeof(cs));
            cs.midiChannel = DRUM_MIDI_CHANNEL;
            cs.program     = 0;
            cs.volume      = 100;
            strncpy(cs.name, "DRUMS", INSTRUMENT_NAME_LEN - 1);
            cs.name[INSTRUMENT_NAME_LEN - 1] = '\0';
            if (gServerPairing.isPaired()) {
                gServerPairing.sendSongPatch(_song, &cs, sizeof(cs));
            }
            return c;
        }
    }
    return -1;
}

void DrumEditorPage::placeStep(int lane, int step) {
    if (lane < 0 || lane >= _laneCount) return;
    if (step < 0 || step >= _pageCols)  return;
    uint8_t trackerNote = _laneNotes[lane];
    uint8_t row = (uint8_t)(_pageStartRow + step);

    int col = findColumnForPlace(row, trackerNote);
    if (col < 0) return;   // all columns occupied — silent no-op

    NoteGrid g(_song.notePool, &_song.noteFreeHead, &pat().noteHead);
    Note n = {};
    n.note     = trackerNote;
    n.velocity = 100;
    n.effect   = 0;
    n.param    = 0;
    if (!g.set(row, (uint8_t)col, n)) return;

    _cellOn[lane][step] = true;

    if (gServerPairing.isPaired()) {
        gServerPairing.sendNoteSet(_song, _patIdx, row, (uint8_t)col);
    }
}

void DrumEditorPage::clearStep(int lane, int step) {
    if (lane < 0 || lane >= _laneCount) return;
    if (step < 0 || step >= _pageCols)  return;
    uint8_t trackerNote = _laneNotes[lane];
    uint8_t row = (uint8_t)(_pageStartRow + step);

    int col = findColumnHoldingNote(row, trackerNote);
    if (col < 0) return;

    NoteGrid g(_song.notePool, &_song.noteFreeHead, &pat().noteHead);
    g.clear(row, (uint8_t)col);
    _cellOn[lane][step] = false;

    if (gServerPairing.isPaired()) {
        gServerPairing.sendNoteSet(_song, _patIdx, row, (uint8_t)col);
    }
}

// ── Picker (overlay) ─────────────────────────────────────────────────────────

void DrumEditorPage::openPicker() {
    _pickerActive          = true;
    _pickerScroll          = 0;
    _pickerWasDown         = _touch.isTouched;
    _pickerDragMoved       = false;
    _pickerDragStartY      = 0;
    _pickerDragStartScroll = 0;
    _pickerDragActive      = false;
    _pickerAudActive       = false;
    _pickerScrollPx        = 0;
    _pickerVelPxPerMs      = 0;
    _pickerInertiaActive   = false;
}

void DrumEditorPage::closePicker(bool redraw) {
    if (_pickerAudActive && gServerPairing.isPaired()) {
        gServerPairing.sendAuditionRawNote(DRUM_MIDI_CHANNEL, _pickerAudMidi, 0);
    }
    _pickerAudActive = false;
    _pickerActive    = false;
    _wasDown         = _touch.isTouched;
    if (redraw) {
        draw();
        _d.paint();
    }
}

void DrumEditorPage::drawPicker() {
    _d.fillScreen(COL_WHITE);

    _d.fillRect(0, 0, DEP_W, DEP_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    const char* title = "ADD DRUM";
    int tw = (int)strlen(title) * 18;
    _d.setCursor((DEP_W - tw) / 2, (DEP_HDR_H - 24) / 2);
    _d.print(title);
    uiButton(_d, DEP_HOME_X, 0, DEP_HOME_W, DEP_HDR_H, "CANCEL",
             COL_BLACK, COL_WHITE, 3);

    for (int i = 0; i < DEP_PCK_ROWS; i++) {
        int gi = _pickerScroll + i;
        drawPickerRow(i, gi);
    }
}

void DrumEditorPage::drawPickerRow(int visIdx, int globalIdx) {
    int y = DEP_PCK_Y + visIdx * DEP_PCK_ROW_H;
    _d.fillRect(DEP_PCK_X, y, DEP_PCK_W, DEP_PCK_ROW_H, COL_WHITE);
    if (globalIdx < 0 || globalIdx >= GM_DRUMS_N) return;

    const GmDrum& d = GM_DRUMS[globalIdx];
    uint8_t trackerNote = (uint8_t)(d.midi - TRACKER_TO_MIDI_OFFSET);
    bool already = laneHasNote(trackerNote);

    uint8_t fg = already ? COL_DKGREY : COL_BLACK;

    _d.drawRect(DEP_PCK_X, y, DEP_PCK_W, DEP_PCK_ROW_H,
                already ? COL_LTGREY : COL_BLACK);

    char buf[40];
    snprintf(buf, sizeof(buf), "[%s] %s", d.abbr, d.name);
    _d.setTextSize(3);
    _d.setTextColor(fg);
    _d.setCursor(DEP_PCK_X + 20, y + (DEP_PCK_ROW_H - 24) / 2);
    _d.print(buf);

    int playBtnX = DEP_PCK_X + DEP_PCK_W - DEP_PCK_PLAY_W - DEP_PCK_PLAY_MARGIN;
    int playBtnY = y + (DEP_PCK_ROW_H - DEP_PCK_PLAY_H) / 2;

    char midiTxt[16];
    snprintf(midiTxt, sizeof(midiTxt), "MIDI %d", d.midi);
    int mtW = (int)strlen(midiTxt) * 18;
    _d.setCursor(playBtnX - mtW - 16,
                 y + (DEP_PCK_ROW_H - 24) / 2);
    _d.print(midiTxt);

    _d.drawRect(playBtnX, playBtnY, DEP_PCK_PLAY_W, DEP_PCK_PLAY_H, COL_BLACK);
    int triLeft  = playBtnX + 22;
    int triRight = playBtnX + DEP_PCK_PLAY_W - 18;
    int triTop   = playBtnY + 8;
    int triBot   = playBtnY + DEP_PCK_PLAY_H - 8;
    int triMidY  = playBtnY + DEP_PCK_PLAY_H / 2;
    _d.fillTriangle(triLeft, triTop, triLeft, triBot, triRight, triMidY, COL_BLACK);
}

bool DrumEditorPage::hitPickerPlay(int sx, int sy, int& visIdx) const {
    if (sy < DEP_PCK_Y) return false;
    int rel = sy - DEP_PCK_Y;
    int rowIdx = rel / DEP_PCK_ROW_H;
    if (rowIdx < 0 || rowIdx >= DEP_PCK_ROWS) return false;
    int btnX = DEP_PCK_X + DEP_PCK_W - DEP_PCK_PLAY_W - DEP_PCK_PLAY_MARGIN;
    // Hit area: full row height; left edge starts at the MIDI label's left
    // edge (covers MIDI label + gap + play button + right margin).  This
    // makes "tap to add the drum" require touching the drum-name area.
    int hitLeft  = btnX - DEP_PCK_PLAY_HIT_EXTRA_LEFT;
    int hitRight = DEP_PCK_X + DEP_PCK_W;
    if (sx < hitLeft || sx >= hitRight) return false;
    visIdx = rowIdx;
    return true;
}

void DrumEditorPage::pickerAuditionStart(uint8_t midiNote) {
    if (_pickerAudActive && gServerPairing.isPaired()) {
        gServerPairing.sendAuditionRawNote(DRUM_MIDI_CHANNEL, _pickerAudMidi, 0);
    }
    _pickerAudMidi   = midiNote;
    _pickerAudOffMs  = millis() + DEP_PCK_PLAY_OFF_MS;
    _pickerAudActive = true;
    if (gServerPairing.isPaired()) {
        gServerPairing.sendAuditionRawNote(DRUM_MIDI_CHANNEL, midiNote, 100);
    }
}

void DrumEditorPage::pickerAuditionTick() {
    if (!_pickerAudActive) return;
    if ((int32_t)(millis() - _pickerAudOffMs) < 0) return;
    if (gServerPairing.isPaired()) {
        gServerPairing.sendAuditionRawNote(DRUM_MIDI_CHANNEL, _pickerAudMidi, 0);
    }
    _pickerAudActive = false;
}

void DrumEditorPage::pickerInertiaTick() {
    if (!_pickerInertiaActive) return;
    uint32_t now = millis();
    int dt = (int)(now - _pickerInertiaLastTickMs);
    if (dt < 1) return;
    if (dt > 50) dt = 50;
    _pickerInertiaLastTickMs = now;

    _pickerScrollPx += (int)(_pickerVelPxPerMs * dt);

    int maxRow = GM_DRUMS_N - DEP_PCK_ROWS;
    if (maxRow < 0) maxRow = 0;
    int maxPx = maxRow * DEP_PCK_ROW_H;
    if (_pickerScrollPx <= 0) {
        _pickerScrollPx       = 0;
        _pickerVelPxPerMs     = 0;
        _pickerInertiaActive  = false;
    } else if (_pickerScrollPx >= maxPx) {
        _pickerScrollPx       = maxPx;
        _pickerVelPxPerMs     = 0;
        _pickerInertiaActive  = false;
    }

    // Time-based exponential decay (~520 ms time constant).
    float decay = expf(-(float)dt / 520.0f);
    _pickerVelPxPerMs *= decay;
    if (fabsf(_pickerVelPxPerMs) < 0.02f) {
        _pickerVelPxPerMs    = 0;
        _pickerInertiaActive = false;
    }

    int newScroll = _pickerScrollPx / DEP_PCK_ROW_H;
    if (newScroll != _pickerScroll) {
        _pickerScroll = newScroll;
        for (int i = 0; i < DEP_PCK_ROWS; i++) drawPickerRow(i, _pickerScroll + i);
        _d.paintLater();
    }
}

bool DrumEditorPage::pollPicker() {
    pickerAuditionTick();
    pickerInertiaTick();
    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    bool rising  = (down  && !_pickerWasDown);
    bool falling = (!down && _pickerWasDown);
    _pickerWasDown = down;

    if (rising) {
        if (hitHome(sx, sy)) { closePicker(true); return false; }
        // Play button takes priority over drag: tap audition; no row select.
        int playVis;
        if (hitPickerPlay(sx, sy, playVis)) {
            int gi = _pickerScroll + playVis;
            if (gi >= 0 && gi < GM_DRUMS_N) {
                pickerAuditionStart(GM_DRUMS[gi].midi);
            }
            _pickerDragActive = false;
            return false;
        }
        if (sy >= DEP_PCK_Y && sy < DEP_PCK_Y + DEP_PCK_ROWS * DEP_PCK_ROW_H
                && sx >= DEP_PCK_X && sx < DEP_PCK_X + DEP_PCK_W) {
            _pickerInertiaActive   = false;
            _pickerVelPxPerMs      = 0;
            _pickerScrollPx        = _pickerScroll * DEP_PCK_ROW_H;
            _pickerDragStartPx     = _pickerScrollPx;
            _pickerDragStartY      = sy;
            _pickerDragStartScroll = _pickerScroll;
            _pickerVelTrackY       = sy;
            _pickerVelTrackMs      = millis();
            _pickerDragMoved       = false;
            _pickerDragActive      = true;
        }
        return false;
    }

    if (down) {
        if (!_pickerDragActive) return false;
        int dy = sy - _pickerDragStartY;
        if (!_pickerDragMoved && (dy > DEP_PCK_DRAG_THRESH || dy < -DEP_PCK_DRAG_THRESH)) {
            _pickerDragMoved = true;
        }
        if (_pickerDragMoved) {
            _pickerScrollPx = _pickerDragStartPx - dy;
            int maxRow = GM_DRUMS_N - DEP_PCK_ROWS;
            if (maxRow < 0) maxRow = 0;
            int maxPx  = maxRow * DEP_PCK_ROW_H;
            if (_pickerScrollPx < 0)     _pickerScrollPx = 0;
            if (_pickerScrollPx > maxPx) _pickerScrollPx = maxPx;

            uint32_t now = millis();
            int sampleDt = (int)(now - _pickerVelTrackMs);
            if (sampleDt >= 16) {
                int dyS = sy - _pickerVelTrackY;
                float inst = -(float)dyS / (float)sampleDt;
                _pickerVelPxPerMs = _pickerVelPxPerMs * 0.4f + inst * 0.6f;
                _pickerVelTrackY  = sy;
                _pickerVelTrackMs = now;
            }

            int newScroll = _pickerScrollPx / DEP_PCK_ROW_H;
            if (newScroll != _pickerScroll) {
                _pickerScroll = newScroll;
                for (int i = 0; i < DEP_PCK_ROWS; i++) {
                    drawPickerRow(i, _pickerScroll + i);
                }
                _d.paintLater();
            }
        }
        return false;
    }

    if (falling && _pickerDragActive && _pickerDragMoved) {
        if (fabsf(_pickerVelPxPerMs) > 0.08f) {
            _pickerInertiaActive     = true;
            _pickerInertiaLastTickMs = millis();
        } else {
            _pickerVelPxPerMs = 0;
        }
    }

    if (falling && _pickerDragActive && !_pickerDragMoved) {
        if (sy >= DEP_PCK_Y && sy < DEP_PCK_Y + DEP_PCK_ROWS * DEP_PCK_ROW_H
                && sx >= DEP_PCK_X && sx < DEP_PCK_X + DEP_PCK_W) {
            int visIdx = (sy - DEP_PCK_Y) / DEP_PCK_ROW_H;
            int gi     = _pickerScroll + visIdx;
            if (gi >= 0 && gi < GM_DRUMS_N) {
                const GmDrum& d = GM_DRUMS[gi];
                uint8_t trackerNote = (uint8_t)(d.midi - TRACKER_TO_MIDI_OFFSET);
                if (!laneHasNote(trackerNote)) {
                    addLane(trackerNote);
                    closePicker(true);
                    _pickerDragActive = false;
                    return false;
                }
            }
        }
    }
    if (falling) _pickerDragActive = false;

    return false;
}

// ── Audition ─────────────────────────────────────────────────────────────────

void DrumEditorPage::recomputeStepMs() {
    if (_tempo < DEP_BPM_MIN) _tempo = DEP_BPM_MIN;
    if (_tempo > DEP_BPM_MAX) _tempo = DEP_BPM_MAX;
    // 16th-note grid: step_ms = 60000 / (BPM * 4) = 15000 / BPM
    _audStepMs = (uint16_t)(15000 / _tempo);
}

void DrumEditorPage::auditionStart() {
    if (pat().length == 0) return;
    recomputeStepMs();
    // Send program change once so the synth uses this column's drum kit.
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel == DRUM_MIDI_CHANNEL) {
            if (gServerPairing.isPaired()) {
                gServerPairing.sendAuditionProgram(DRUM_MIDI_CHANNEL,
                                                    _song.columns[c].program);
            }
            break;
        }
    }
    _audStep    = 0;
    _audNextMs  = millis();
    _audPlaying = true;
    // Jump back to page 0 so the user sees the playhead from row 0.
    if (_page != 0) setPage(0);
    draw();
    _d.paint();
}

void DrumEditorPage::auditionStop() {
    if (!_audPlaying) return;
    _audPlaying = false;
    // Repaint to clear the marker.
    drawPlayButton();
    drawGrid();
    _d.paint();
}

void DrumEditorPage::auditionTick() {
    if (!_audPlaying) return;
    uint32_t now = millis();
    if ((int32_t)(now - _audNextMs) < 0) return;

    // Fire all drums at the current row.
    NoteGrid g(_song.notePool, &pat().noteHead);
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel != DRUM_MIDI_CHANNEL) continue;
        Note n = g.get(_audStep, (uint8_t)c);
        if (n.note == 0 || n.note == NOTE_OFF) continue;
        uint8_t midi = (uint8_t)(n.note + TRACKER_TO_MIDI_OFFSET);
        uint8_t vel  = (n.velocity & 0x80) ? 100 : n.velocity;
        if (gServerPairing.isPaired()) {
            gServerPairing.sendAuditionRawNote(DRUM_MIDI_CHANNEL, midi, vel);
        }
    }

    int prevStep = _audStep;
    _audStep = (uint8_t)((_audStep + 1) % pat().length);
    _audNextMs += _audStepMs;
    if ((int32_t)(now - _audNextMs) > (int32_t)_audStepMs) _audNextMs = now;

    // Marker / page handling.
    int prevPageStep = prevStep - _pageStartRow;
    int newPage      = _audStep / _pageCols;
    if (newPage != _page) {
        setPage(newPage);
        draw();
        _d.paint();
    } else {
        int newPageStep = (int)_audStep - _pageStartRow;
        if (prevPageStep >= 0 && prevPageStep < _pageCols) {
            drawStepColumn(prevPageStep, false);
        }
        if (newPageStep >= 0 && newPageStep < _pageCols) {
            drawStepColumn(newPageStep, true);
        }
        _d.paintLater();
    }
}

// ── Main poll ────────────────────────────────────────────────────────────────

bool DrumEditorPage::poll() {
    if (_pickerActive) {
        pollPicker();
        return false;
    }

    auditionTick();

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    bool rising  = (down  && !_wasDown);
    bool falling = (!down && _wasDown);
    _wasDown = down;

    if (!rising && !falling) return false;

    if (falling && hitHome(sx, sy)) {
        auditionStop();
        return true;
    }

    if (rising) {
        if (hitPlay(sx, sy)) {
            if (_audPlaying) auditionStop();
            else             auditionStart();
            return false;
        }
        if (hitBpmMinus(sx, sy)) {
            int t = (int)_tempo - DEP_BPM_STEP;
            if (t < DEP_BPM_MIN) t = DEP_BPM_MIN;
            _tempo = (uint16_t)t;
            recomputeStepMs();
            drawBpmValue();
            _d.paintLater();
            return false;
        }
        if (hitBpmPlus(sx, sy)) {
            int t = (int)_tempo + DEP_BPM_STEP;
            if (t > DEP_BPM_MAX) t = DEP_BPM_MAX;
            _tempo = (uint16_t)t;
            recomputeStepMs();
            drawBpmValue();
            _d.paintLater();
            return false;
        }
        if (hitImport(sx, sy)) {
            auditionStop();
            _importRequested = true;
            return true;   // main loop opens drum-track import overlay
        }
        if (hitAddDrum(sx, sy)) {
            auditionStop();
            openPicker();
            drawPicker();
            _d.paint();
            return false;
        }
        if (hitPrevPage(sx, sy)) {
            setPage(_page - 1);
            draw();
            _d.paint();
            return false;
        }
        if (hitNextPage(sx, sy)) {
            setPage(_page + 1);
            draw();
            _d.paint();
            return false;
        }
        int lane, step;
        if (hitGridCell(sx, sy, lane, step)) {
            if (_cellOn[lane][step]) clearStep(lane, step);
            else                     placeStep(lane, step);
            drawGrid();
            overlayPlayhead();
            _d.paintLater();
            return false;
        }
    }

    return false;
}

// ── Drum-track import (drum-editor mode) ─────────────────────────────────────

void DrumEditorPage::importBlock(const DrumPatternFile& file, int blockIdx,
                                 int kitIdx) {
    const DrumPatternBlock* blk = file.block(blockIdx);
    if (!blk) return;

    // The gm_map.txt fetched by DrumTrackImportPage isn't visible here, so
    // fall back to the hardcoded defaults — same table both pages share at
    // session start.  If the user customised gm_map.txt the import-page
    // already loaded those overrides for its own preview, but importing
    // through the drum editor uses the defaults.  Good enough until we
    // share the parsed map.
    static const DrumGmMap kGmDefaults;

    Pattern& pp = pat();
    NoteGrid g(_song.notePool, &_song.noteFreeHead, &pp.noteHead);

    // 1) Clear all ch10 notes in this pattern.
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel != DRUM_MIDI_CHANNEL) continue;
        for (uint8_t r = 0; r < pp.length; r++) g.clear(r, (uint8_t)c);
        _resyncMask |= (1u << c);
    }

    // 2) Apply the chosen kit's program to every ch10 column.  Columns
    //    that get freshly allocated in step 3 inherit the kit at allocation
    //    time (allocateNewDrumColumn writes program 0; patch below).
    uint8_t kitProgram = (kitIdx >= 0 && kitIdx < DRUM_KIT_COUNT)
                        ? DRUM_KITS[kitIdx].program : 0;
    for (int c = 1; c < MAX_COLUMNS; c++) {
        if (_song.columns[c].midiChannel != DRUM_MIDI_CHANNEL) continue;
        if (_song.columns[c].program == kitProgram) continue;
        _song.columns[c].program = kitProgram;
        if (gServerPairing.isPaired()) {
            gServerPairing.sendSongPatch(_song, &_song.columns[c],
                                          sizeof(ColumnSettings));
        }
    }

    // 3) Walk active instruments in the block and place each hit using the
    //    standard allocation rules.  Block has 16 steps; repeat-then-truncate
    //    to fill the pattern length (mirrors DrumTrackImportPage's behaviour).
    int activeIdx[DRUM_PATTERN_MAX_INSTRS];
    int activeN = blk->activeIndices(activeIdx, DRUM_PATTERN_MAX_INSTRS);

    for (int i = 0; i < activeN; i++) {
        const DrumPatternInstr& instr = blk->instrs[activeIdx[i]];
        const DrumGmEntry* m = kGmDefaults.find(instr.code);
        if (!m) continue;
        int trackerInt = (int)m->midiNote - 11;   // TRACKER_TO_MIDI_OFFSET
        if (trackerInt < 1 || trackerInt > NOTE_MAX) continue;
        uint8_t trackerNote = (uint8_t)trackerInt;

        for (uint8_t r = 0; r < pp.length; r++) {
            uint8_t step = (uint8_t)(r % DRUM_PATTERN_STEPS);
            if (!instr.hits[step]) continue;

            int col = findColumnForPlace(r, trackerNote);
            if (col < 0) break;   // out of columns

            // If allocateNewDrumColumn just ran, force the kit's program in too.
            if (_song.columns[col].program != kitProgram) {
                _song.columns[col].program = kitProgram;
                if (gServerPairing.isPaired()) {
                    gServerPairing.sendSongPatch(_song, &_song.columns[col],
                                                  sizeof(ColumnSettings));
                }
            }

            Note n = {};
            n.note     = trackerNote;
            n.velocity = m->velocity;
            n.effect   = 0;
            n.param    = 0;
            if (!g.set(r, (uint8_t)col, n)) break;
            _resyncMask |= (1u << col);
        }
    }

    // 4) Full per-row resync for every touched ch10 column.  This covers both
    //    new note writes AND the cleared cells from step 1 (which the server
    //    otherwise wouldn't learn about).
    if (gServerPairing.isPaired()) {
        for (int c = 1; c < MAX_COLUMNS; c++) {
            if (!(_resyncMask & (1u << c))) continue;
            for (uint8_t r = 0; r < pp.length; r++) {
                gServerPairing.sendNoteSet(_song, _patIdx, r, (uint8_t)c);
            }
        }
    }

    // 5) Re-derive lane list + cell-on matrix from the new column data.
    rebuildLanes();
    recomputeGeometry();
    setPage(0);
}

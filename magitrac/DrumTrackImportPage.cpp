#include "DrumTrackImportPage.h"
#include "ServerPairing.h"
#include "UIHelpers.h"
#include "NoteGrid.h"
#include "DrumKits.h"
#include <string.h>
#include <stdio.h>

extern ServerPairing gServerPairing;

// ── Tracker-note conversion ──────────────────────────────────────────────────
// midi_player.cpp on the server adds TRACKER_TO_MIDI_OFFSET (= 11) before
// emitting MIDI.  So tracker note value = MIDI note - 11.  Mirrors that
// constant locally so we don't depend on the server-only header.
static const int TRACKER_TO_MIDI_OFFSET = 11;

// ── gm_map.txt cache (session-wide) ─────────────────────────────────────────
static DrumGmMap sGmMap;            // initialised with hardcoded defaults
static bool      sGmMapFetched = false;   // true once server returned the file

// Drum kit table (SAM2695 GS slots) is shared with ColumnEditor — see DrumKits.h.

// ── Layout ───────────────────────────────────────────────────────────────────
static const int DT_HDR_H    = 50;
static const int DT_HOME_X   = 830;
static const int DT_HOME_W   = 130;
static const int DT_HBACK_X  = 0;
static const int DT_HBACK_W  = 130;

// File picker — single-column drag-scroll, mirrors BackupRestorePage.
static const int DT_FL_Y           = 70;
static const int DT_FL_ROW_H       = 70;
static const int DT_FL_ROWS        = 6;
static const int DT_FL_X           = 20;
static const int DT_FL_W           = 920;
static const int DT_FL_DRAG_THRESH = 12;

// Block-choose layout.
static const int DT_BC_FILE_Y      = 60;
static const int DT_BC_BLK_Y       = 130;
static const int DT_BC_BLK_H       = 100;
static const int DT_BC_MINUS_X     = 60;
static const int DT_BC_PLUS_X      = 760;
static const int DT_BC_PM_W        = 140;
static const int DT_BC_PM_H        = 100;
static const int DT_BC_INFO_Y      = 248;
static const int DT_BC_ALLOC_Y     = 294;
static const int DT_BC_KIT_Y       = 336;   // kit selector row
static const int DT_BC_KIT_BTN_W   = 80;
static const int DT_BC_KIT_BTN_H   = 50;
static const int DT_BC_KIT_MINUS_X = 60;
static const int DT_BC_KIT_PLUS_X  = 960 - 60 - DT_BC_KIT_BTN_W;
static const int DT_BC_WARN_Y      = 398;
static const int DT_BC_BTN_Y       = 450;
static const int DT_BC_BTN_W       = 240;
static const int DT_BC_BTN_H       = 80;
static const int DT_BC_CANCEL_X    = 40;
static const int DT_BC_IMPORT_X    = 960 - DT_BC_BTN_W - 40;
static const int DT_BC_AUD_W       = 200;
static const int DT_BC_AUD_X       = (960 - DT_BC_AUD_W) / 2;

// Error / OK button.
static const int DT_OK_X = (960 - 200) / 2;
static const int DT_OK_Y = 440;
static const int DT_OK_W = 200;
static const int DT_OK_H = 70;

// Small helper — wipe the framebuffer before drawing a fresh page.
// EPD_PainterAdafruit::clear() does a *physical* anti-ghost refresh, not
// a framebuffer wipe; for inside-the-page redraws we want fillScreen.
static inline void wipeFB(EPD_PainterAdafruit& d) {
    d.fillScreen(COL_WHITE);
}

// ── Construction / open ──────────────────────────────────────────────────────
DrumTrackImportPage::DrumTrackImportPage(EPD_PainterAdafruit& display,
                                         GT911_Lite& touch, Song& song)
    : _d(display), _touch(touch), _song(song) {}

void DrumTrackImportPage::open(uint8_t patternIdx, uint8_t startCol) {
    _patIdx        = patternIdx;
    _startCol      = startCol;
    _mode          = Mode::COLUMN_EDITOR;
    _resyncMask    = 0;
    _imported      = false;
    _wasDown       = false;
    _scroll        = 0;
    _dragMoved     = false;
    _selectedBlock = 0;
    enterLoadingList();
}

void DrumTrackImportPage::openForDrumEditor(uint8_t patternIdx) {
    _patIdx        = patternIdx;
    _startCol      = 1;   // unused in this mode
    _mode          = Mode::DRUM_EDITOR;
    _resyncMask    = 0;
    _imported      = false;
    _wasDown       = false;
    _scroll        = 0;
    _dragMoved     = false;
    _selectedBlock = 0;
    enterLoadingList();
}

void DrumTrackImportPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

bool DrumTrackImportPage::readTouch(int& sx, int& sy, bool& down) {
    if (!_touch.read()) return false;
    down = _touch.isTouched;
    rawToScreen(_touch.x, _touch.y, sx, sy);
    return true;
}

// ── State transitions ───────────────────────────────────────────────────────
void DrumTrackImportPage::enterLoadingList() {
    _state = State::LOADING_LIST;
    gServerPairing.requestFileList(FK_DRUMTRACKS, 0);
    wipeFB(_d);
    drawLoading("Loading drum-track list...");
    _d.paintLater();
}

void DrumTrackImportPage::enterFilePicker() {
    _state = State::FILE_PICKER;
    _scroll = 0;
    _dragMoved = false;
    wipeFB(_d);
    drawFilePicker();
    _d.paintLater();
}

void DrumTrackImportPage::enterLoadingFile(const char* name) {
    _state = State::LOADING_FILE;
    gServerPairing.requestFileLoad(FK_DRUMTRACKS, name);
    wipeFB(_d);
    char what[80];
    snprintf(what, sizeof(what), "Loading %s ...", name);
    drawLoading(what);
    _d.paintLater();
}

void DrumTrackImportPage::enterLoadingMap() {
    _state = State::LOADING_MAP;
    gServerPairing.requestFileLoad(FK_DRUMTRACKS, "gm_map.txt");
    wipeFB(_d);
    drawLoading("Loading drum mapping...");
    _d.paintLater();
}

void DrumTrackImportPage::enterBlockChoose() {
    _state = State::BLOCK_CHOOSE;
    if (_selectedBlock >= _file.blockCount()) _selectedBlock = 0;
    wipeFB(_d);
    drawBlockChoose();
    _d.paintLater();
}

void DrumTrackImportPage::enterError(const char* msg) {
    _state = State::ERROR_VIEW;
    strncpy(_errMsg, msg, sizeof(_errMsg) - 1);
    _errMsg[sizeof(_errMsg) - 1] = '\0';
    wipeFB(_d);
    drawError();
    _d.paintLater();
}

// ── Drawing ──────────────────────────────────────────────────────────────────
void DrumTrackImportPage::drawHeader(const char* title, bool withBack) {
    _d.fillRect(0, 0, 960, DT_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    int tw = (int)strlen(title) * 18;
    _d.setCursor((960 - tw) / 2, (DT_HDR_H - 24) / 2);
    _d.print(title);
    if (withBack) {
        uiButton(_d, DT_HBACK_X, 0, DT_HBACK_W, DT_HDR_H, "BACK",
                 COL_BLACK, COL_WHITE, 3);
    }
    uiButton(_d, DT_HOME_X, 0, DT_HOME_W, DT_HDR_H, "HOME", COL_BLACK, COL_WHITE, 3);
}

void DrumTrackImportPage::drawLoading(const char* what) {
    drawHeader("IMPORT DRUM TRACK");
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(what) * 18;
    _d.setCursor((960 - tw) / 2, 250);
    _d.print(what);
}

void DrumTrackImportPage::drawFilePicker() {
    drawHeader("SELECT DRUM TRACK");

    int n = gServerPairing.fileListCount();
    if (n == 0) {
        _d.setTextSize(3);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(220, 250);
        _d.print("No drum tracks in /drumtracks/");
        return;
    }

    int start = _scroll;
    int count = n - start;
    if (count > DT_FL_ROWS) count = DT_FL_ROWS;

    _d.setTextColor(COL_BLACK);
    for (int i = 0; i < count; i++) {
        int idx = start + i;
        int y   = DT_FL_Y + i * DT_FL_ROW_H;
        _d.drawRect(DT_FL_X, y, DT_FL_W, DT_FL_ROW_H - 4, COL_BLACK);

        const char* nm = gServerPairing.fileListName(idx);
        char buf[FILE_NAME_LEN];
        strncpy(buf, nm, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        int len = (int)strlen(buf);
        if (len >= 4 && buf[len-4] == '.') buf[len-4] = '\0';  // strip .txt

        _d.setTextSize(3);
        _d.setCursor(DT_FL_X + 16, y + (DT_FL_ROW_H - 4 - 24) / 2);
        _d.print(buf);
    }

    if (n > DT_FL_ROWS) {
        char hint[40];
        int firstVis = _scroll + 1;
        int lastVis  = _scroll + DT_FL_ROWS;
        if (lastVis > n) lastVis = n;
        snprintf(hint, sizeof(hint), "%d-%d of %d  (drag to scroll)",
                 firstVis, lastVis, n);
        _d.setTextSize(2);
        int tw = (int)strlen(hint) * 12;
        _d.setCursor((960 - tw) / 2, DT_FL_Y + DT_FL_ROWS * DT_FL_ROW_H + 4);
        _d.print(hint);
    }
}

void DrumTrackImportPage::drawBlockChoose() {
    drawHeader("IMPORT BLOCK", /*withBack=*/true);

    const DrumPatternBlock* blk = _file.block(_selectedBlock);

    // "Block N / M" centred at the top.
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Block %d / %d",
                 _selectedBlock + 1, _file.blockCount());
        int tw = (int)strlen(buf) * 18;
        _d.setCursor((960 - tw) / 2, DT_BC_FILE_Y);
        _d.print(buf);
    }

    // ± buttons + the block's name (from inside the file) in the middle.
    uiButton(_d, DT_BC_MINUS_X, DT_BC_BLK_Y, DT_BC_PM_W, DT_BC_PM_H, "-",
             COL_WHITE, COL_BLACK, 5);
    uiButton(_d, DT_BC_PLUS_X,  DT_BC_BLK_Y, DT_BC_PM_W, DT_BC_PM_H, "+",
             COL_WHITE, COL_BLACK, 5);
    if (blk) {
        char nameBuf[DRUM_BLOCK_NAME_LEN];
        strncpy(nameBuf, blk->name, sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        if (strlen(nameBuf) > 33) nameBuf[33] = '\0';   // ~size 3 ≈ 33 chars
        _d.setTextSize(3);
        int tw = (int)strlen(nameBuf) * 18;
        _d.setCursor((960 - tw) / 2, DT_BC_BLK_Y + (DT_BC_BLK_H - 24) / 2);
        _d.print(nameBuf);
    }

    int activeIdx[DRUM_PATTERN_MAX_INSTRS];
    int activeN = blk ? blk->activeIndices(activeIdx, DRUM_PATTERN_MAX_INSTRS) : 0;

    char drumsLine[128];
    snprintf(drumsLine, sizeof(drumsLine), "Drums: ");
    for (int i = 0; i < activeN; i++) {
        if (i > 0) strncat(drumsLine, ", ", sizeof(drumsLine) - strlen(drumsLine) - 1);
        strncat(drumsLine, blk->instrs[activeIdx[i]].code,
                sizeof(drumsLine) - strlen(drumsLine) - 1);
    }
    _d.setTextSize(3);
    _d.setCursor(60, DT_BC_INFO_Y);
    _d.print(drumsLine);

    char allocLine[80];
    if (activeN == 0) {
        snprintf(allocLine, sizeof(allocLine), "(empty block — nothing to import)");
    } else {
        int endCol = (int)_startCol + activeN - 1;
        if (endCol >= MAX_COLUMNS) {
            snprintf(allocLine, sizeof(allocLine),
                     "Needs %d cols from col %d — exceeds max (%d)",
                     activeN, _startCol, MAX_COLUMNS - 1);
        } else {
            snprintf(allocLine, sizeof(allocLine),
                     "Will fill cols %d..%d  (%d cols)",
                     _startCol, endCol, activeN);
        }
    }
    _d.setCursor(60, DT_BC_ALLOC_Y);
    _d.print(allocLine);

    // Kit selector row — drives audition program-change + import program.
    uiButton(_d, DT_BC_KIT_MINUS_X, DT_BC_KIT_Y, DT_BC_KIT_BTN_W, DT_BC_KIT_BTN_H,
             "-", COL_WHITE, COL_BLACK, 4);
    uiButton(_d, DT_BC_KIT_PLUS_X,  DT_BC_KIT_Y, DT_BC_KIT_BTN_W, DT_BC_KIT_BTN_H,
             "+", COL_WHITE, COL_BLACK, 4);
    {
        char kitBuf[48];
        snprintf(kitBuf, sizeof(kitBuf), "Kit: %s", DRUM_KITS[_kitIdx].name);
        _d.setTextSize(3);
        _d.setTextColor(COL_BLACK);
        int tw = (int)strlen(kitBuf) * 18;
        _d.setCursor((960 - tw) / 2, DT_BC_KIT_Y + (DT_BC_KIT_BTN_H - 24) / 2);
        _d.print(kitBuf);
    }

    // Warning if any destination col already has notes in this pattern.
    bool overflow = (activeN > 0 && (int)_startCol + activeN - 1 >= MAX_COLUMNS);
    bool hasExisting = false;
    if (!overflow && activeN > 0) {
        const NoteGrid g(_song.notePool, &_song.patterns[_patIdx].noteHead);
        for (int i = 0; i < activeN && !hasExisting; i++) {
            uint8_t col = _startCol + i;
            for (uint8_t r = 0; r < _song.patterns[_patIdx].length; r++) {
                if (g.has(r, col)) { hasExisting = true; break; }
            }
        }
    }
    if (hasExisting) {
        _d.setTextSize(2);
        _d.setCursor(60, DT_BC_WARN_Y);
        _d.print("Destination columns already have notes - will be cleared.");
    }

    uiButton(_d, DT_BC_CANCEL_X, DT_BC_BTN_Y, DT_BC_BTN_W, DT_BC_BTN_H,
             "CANCEL", COL_WHITE, COL_BLACK, 4);
    bool canImport = (activeN > 0) && !overflow;
    uiButton(_d, DT_BC_IMPORT_X, DT_BC_BTN_Y, DT_BC_BTN_W, DT_BC_BTN_H,
             canImport ? "IMPORT" : "(N/A)",
             canImport ? COL_BLACK : COL_LTGREY,
             COL_WHITE, 4);
    if (activeN > 0) {
        const char* lbl = _audPlaying ? "STOP" : "PLAY";
        uint16_t bg = _audPlaying ? COL_BLACK : COL_WHITE;
        uint16_t fg = _audPlaying ? COL_WHITE : COL_BLACK;
        uiButton(_d, DT_BC_AUD_X, DT_BC_BTN_Y, DT_BC_AUD_W, DT_BC_BTN_H,
                 lbl, bg, fg, 4);
    }
}

void DrumTrackImportPage::drawError() {
    drawHeader("IMPORT ERROR");
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(_errMsg) * 18;
    _d.setCursor((960 - tw) / 2, 250);
    _d.print(_errMsg);
    uiButton(_d, DT_OK_X, DT_OK_Y, DT_OK_W, DT_OK_H, "OK",
             COL_WHITE, COL_BLACK, 3);
}

void DrumTrackImportPage::draw() {
    switch (_state) {
        case State::LOADING_LIST:  drawLoading("Loading drum-track list...");  break;
        case State::FILE_PICKER:   drawFilePicker();                            break;
        case State::LOADING_FILE:  drawLoading("Loading drum-track file..."); break;
        case State::LOADING_MAP:   drawLoading("Loading drum mapping...");     break;
        case State::BLOCK_CHOOSE:  drawBlockChoose();                           break;
        case State::APPLYING:      drawLoading("Applying...");                  break;
        case State::ERROR_VIEW:    drawError();                                 break;
        case State::DONE:          break;
    }
}

bool DrumTrackImportPage::hitHomeOrBack(int sx, int sy) const {
    if (sy >= 0 && sy < DT_HDR_H) {
        if (sx >= DT_HOME_X && sx < DT_HOME_X + DT_HOME_W) return true;
        if (_state == State::BLOCK_CHOOSE
            && sx >= DT_HBACK_X && sx < DT_HBACK_X + DT_HBACK_W) return true;
    }
    return false;
}

// ── State pollers ────────────────────────────────────────────────────────────
void DrumTrackImportPage::pollLoadingList() {
    FileListState s = gServerPairing.fileListState();
    if (s == FileListState::READY) {
        enterFilePicker();
    } else if (s == FileListState::ERROR) {
        enterError("Failed to load drum-track list");
    }
}

void DrumTrackImportPage::pollFilePicker() {
    int sx, sy; bool down;
    if (!readTouch(sx, sy, down)) return;

    if (down && !_wasDown) {
        _wasDown         = true;
        _dragStartY      = sy;
        _dragStartScroll = _scroll;
        _dragMoved       = false;
    } else if (down && _wasDown) {
        int dy = sy - _dragStartY;
        if (!_dragMoved && (dy >= DT_FL_DRAG_THRESH || dy <= -DT_FL_DRAG_THRESH)) {
            _dragMoved = true;
        }
        if (_dragMoved) {
            int rowDelta = -dy / DT_FL_ROW_H;
            int newScroll = _dragStartScroll + rowDelta;
            int n = gServerPairing.fileListCount();
            int maxScroll = n > DT_FL_ROWS ? n - DT_FL_ROWS : 0;
            if (newScroll < 0) newScroll = 0;
            if (newScroll > maxScroll) newScroll = maxScroll;
            if (newScroll != _scroll) {
                _scroll = newScroll;
                wipeFB(_d);
                drawFilePicker();
                _d.paintLater();
            }
        }
    } else if (!down && _wasDown) {
        _wasDown = false;
        if (_dragMoved) { _dragMoved = false; return; }

        if (hitHomeOrBack(sx, sy)) { _state = State::DONE; return; }

        if (sx >= DT_FL_X && sx < DT_FL_X + DT_FL_W) {
            int row = (sy - DT_FL_Y) / DT_FL_ROW_H;
            if (row >= 0 && row < DT_FL_ROWS) {
                int idx = _scroll + row;
                if (idx >= 0 && idx < gServerPairing.fileListCount()) {
                    const char* name = gServerPairing.fileListName(idx);
                    enterLoadingFile(name);
                }
            }
        }
    }
}

void DrumTrackImportPage::pollLoadingFile() {
    FileLoadState s = gServerPairing.fileLoadState();
    if (s == FileLoadState::READY) {
        if (!_file.parse((const char*)gServerPairing.fileLoadData(),
                         gServerPairing.fileLoadLen())) {
            enterError("Could not parse drum-track file");
            return;
        }
        if (!sGmMapFetched) enterLoadingMap();
        else                enterBlockChoose();
    } else if (s == FileLoadState::NOT_FOUND) {
        enterError("File not found on server");
    } else if (s == FileLoadState::ERROR) {
        enterError("File too large or transfer failed");
    }
}

void DrumTrackImportPage::pollLoadingMap() {
    FileLoadState s = gServerPairing.fileLoadState();
    if (s == FileLoadState::READY) {
        sGmMap.parseFromText((const char*)gServerPairing.fileLoadData(),
                             gServerPairing.fileLoadLen());
        sGmMapFetched = true;
        enterBlockChoose();
    } else if (s == FileLoadState::NOT_FOUND
               || s == FileLoadState::ERROR) {
        // Map missing or unreadable — proceed with built-in defaults.
        sGmMapFetched = true;
        enterBlockChoose();
    }
}

void DrumTrackImportPage::pollBlockChoose() {
    int sx, sy; bool down;
    if (!readTouch(sx, sy, down)) return;
    if (down && !_wasDown) { _wasDown = true; }
    else if (!down && _wasDown) {
        _wasDown = false;

        if (sy >= 0 && sy < DT_HDR_H) {
            if (sx >= DT_HOME_X && sx < DT_HOME_X + DT_HOME_W) {
                auditionStop();
                _state = State::DONE; return;
            }
            if (sx >= DT_HBACK_X && sx < DT_HBACK_X + DT_HBACK_W) {
                auditionStop();
                enterFilePicker(); return;
            }
        }

        if (sy >= DT_BC_BLK_Y && sy < DT_BC_BLK_Y + DT_BC_BLK_H) {
            if (sx >= DT_BC_MINUS_X && sx < DT_BC_MINUS_X + DT_BC_PM_W) {
                if (_selectedBlock > 0) {
                    _selectedBlock--;
                    wipeFB(_d); drawBlockChoose(); _d.paintLater();
                }
                return;
            }
            if (sx >= DT_BC_PLUS_X && sx < DT_BC_PLUS_X + DT_BC_PM_W) {
                if (_selectedBlock < _file.blockCount() - 1) {
                    _selectedBlock++;
                    wipeFB(_d); drawBlockChoose(); _d.paintLater();
                }
                return;
            }
        }

        if (sy >= DT_BC_KIT_Y && sy < DT_BC_KIT_Y + DT_BC_KIT_BTN_H) {
            bool changed = false;
            if (sx >= DT_BC_KIT_MINUS_X && sx < DT_BC_KIT_MINUS_X + DT_BC_KIT_BTN_W) {
                if (_kitIdx > 0) { _kitIdx--; changed = true; }
            } else if (sx >= DT_BC_KIT_PLUS_X && sx < DT_BC_KIT_PLUS_X + DT_BC_KIT_BTN_W) {
                if (_kitIdx < DRUM_KIT_COUNT - 1) { _kitIdx++; changed = true; }
            }
            if (changed) {
                sendDrumProgram();   // update synth so audition uses new kit
                wipeFB(_d); drawBlockChoose(); _d.paintLater();
                return;
            }
        }

        if (sy >= DT_BC_BTN_Y && sy < DT_BC_BTN_Y + DT_BC_BTN_H) {
            if (sx >= DT_BC_CANCEL_X && sx < DT_BC_CANCEL_X + DT_BC_BTN_W) {
                auditionStop();
                enterFilePicker(); return;
            }
            if (sx >= DT_BC_AUD_X && sx < DT_BC_AUD_X + DT_BC_AUD_W) {
                if (_audPlaying) auditionStop();
                else             auditionStart();
                wipeFB(_d); drawBlockChoose(); _d.paintLater();
                return;
            }
            if (sx >= DT_BC_IMPORT_X && sx < DT_BC_IMPORT_X + DT_BC_BTN_W) {
                auditionStop();
                if (_mode == Mode::DRUM_EDITOR) {
                    // Caller handles the actual placement via pickedFile()
                    // + pickedBlock() + pickedKit().
                    _imported = true;
                    _state    = State::DONE;
                    return;
                }
                _state = State::APPLYING;
                wipeFB(_d); drawLoading("Applying..."); _d.paintLater();
                if (applyImport()) {
                    _imported = true;
                    _state = State::DONE;
                } else {
                    enterError("Import failed (column overflow or pool full)");
                }
                return;
            }
        }
    }
}

void DrumTrackImportPage::pollError() {
    int sx, sy; bool down;
    if (!readTouch(sx, sy, down)) return;
    if (down && !_wasDown) { _wasDown = true; }
    else if (!down && _wasDown) {
        _wasDown = false;
        if (sx >= DT_OK_X && sx < DT_OK_X + DT_OK_W
            && sy >= DT_OK_Y && sy < DT_OK_Y + DT_OK_H) {
            _state = State::DONE;
        }
    }
}

bool DrumTrackImportPage::poll() {
    if (_state == State::BLOCK_CHOOSE) auditionTick();
    else                                auditionStop();

    switch (_state) {
        case State::LOADING_LIST: pollLoadingList(); break;
        case State::FILE_PICKER:  pollFilePicker();  break;
        case State::LOADING_FILE: pollLoadingFile(); break;
        case State::LOADING_MAP:  pollLoadingMap();  break;
        case State::BLOCK_CHOOSE: pollBlockChoose(); break;
        case State::APPLYING:     break;
        case State::ERROR_VIEW:   pollError();       break;
        case State::DONE:         break;
    }
    return _state == State::DONE;
}

// ── Audition ─────────────────────────────────────────────────────────────────
void DrumTrackImportPage::sendDrumProgram() {
    // Routed through the server's MIDI task (not a raw MidiData write) so the
    // program change stays coherent with the sequencer's running-status cache.
    gServerPairing.sendAuditionProgram(DRUM_MIDI_CHANNEL, DRUM_KITS[_kitIdx].program);
}

void DrumTrackImportPage::auditionStart() {
    int tempo = _file.tempo();
    if (tempo < 30 || tempo > 300) tempo = 120;
    _audStepMs = (uint16_t)(60000 / (tempo * 4));   // sixteenth-note grid
    _audStep   = 0;
    _audNextMs = millis();
    _audPlaying = true;
    sendDrumProgram();   // select the chosen kit before the first hit
}

void DrumTrackImportPage::auditionStop() {
    _audPlaying = false;
}

void DrumTrackImportPage::auditionTick() {
    if (!_audPlaying) return;
    uint32_t now = millis();
    if ((int32_t)(now - _audNextMs) < 0) return;

    const DrumPatternBlock* blk = _file.block(_selectedBlock);
    if (!blk) { auditionStop(); return; }

    int activeIdx[DRUM_PATTERN_MAX_INSTRS];
    int activeN = blk->activeIndices(activeIdx, DRUM_PATTERN_MAX_INSTRS);
    for (int i = 0; i < activeN; i++) {
        const DrumPatternInstr& instr = blk->instrs[activeIdx[i]];
        if (!instr.hits[_audStep]) continue;
        const DrumGmEntry* m = sGmMap.find(instr.code);
        if (!m) continue;
        gServerPairing.sendAuditionRawNote(10, m->midiNote, m->velocity);
    }

    _audStep = (_audStep + 1) % DRUM_PATTERN_STEPS;
    _audNextMs += _audStepMs;
    // Catch up if we drifted >1 step (UI redraw etc.) instead of bursting.
    if ((int32_t)(now - _audNextMs) > (int32_t)_audStepMs) _audNextMs = now;
}

// ── Apply ────────────────────────────────────────────────────────────────────
bool DrumTrackImportPage::applyImport() {
    const DrumPatternBlock* blk = _file.block(_selectedBlock);
    if (!blk) return false;

    int activeIdx[DRUM_PATTERN_MAX_INSTRS];
    int activeN = blk->activeIndices(activeIdx, DRUM_PATTERN_MAX_INSTRS);
    if (activeN == 0) return false;
    if ((int)_startCol + activeN - 1 >= MAX_COLUMNS) return false;

    Pattern& pat = _song.patterns[_patIdx];
    NoteGrid g(_song.notePool, &_song.noteFreeHead, &pat.noteHead);

    // 1) Clear destination cols for this pattern.
    for (int i = 0; i < activeN; i++) {
        uint8_t col = _startCol + i;
        for (uint8_t r = 0; r < MAX_ROWS; r++) g.clear(r, col);
    }

    // 2) Walk each active instrument, place notes with repeat-then-truncate,
    //    and route the destination column to MIDI channel 10 (GM percussion)
    //    with a friendly name derived from the drum code.
    bool poolOk = true;
    for (int i = 0; i < activeN; i++) {
        const DrumPatternInstr& instr = blk->instrs[activeIdx[i]];
        const DrumGmEntry* m = sGmMap.find(instr.code);
        if (!m) continue;
        int trackerNote = (int)m->midiNote - TRACKER_TO_MIDI_OFFSET;
        if (trackerNote < 1 || trackerNote > NOTE_MAX) continue;

        uint8_t col = _startCol + i;
        for (uint8_t r = 0; r < pat.length; r++) {
            uint8_t step = r % DRUM_PATTERN_STEPS;
            if (!instr.hits[step]) continue;
            Note n = {};
            n.note     = (uint8_t)trackerNote;
            n.velocity = m->velocity;
            n.effect   = 0;
            n.param    = 0;
            if (!g.set(r, col, n)) { poolOk = false; break; }
        }
        if (!poolOk) break;

        ColumnSettings& cs = _song.columns[col];
        memset(&cs, 0, sizeof(cs));
        cs.midiChannel = DRUM_MIDI_CHANNEL;
        cs.program     = DRUM_KITS[_kitIdx].program;    // playback matches audition
        cs.volume      = 100;
        strncpy(cs.name, m->name, INSTRUMENT_NAME_LEN - 1);
        cs.name[INSTRUMENT_NAME_LEN - 1] = '\0';

        _resyncMask |= (1u << col);
    }

    return poolOk;
}

#pragma once
#include <Arduino.h>
#include "Constants.h"
#include "EPD_Painter_Adafruit.h"
#include "TrackerData.h"
#include "TrackerEngine.h"

// ── Layout constants (960×540 display) ───────────────────────────────────────
//
//  All dimensions at textSize 4 (24×32px/char) for grid rows,
//  textSize 3 (18×24px/char) for header / column-header / status.
//
//   y=0    ┌─ Header (40px) ─────────────────────────────────────────┐
//   y=40   ├─ Column headers (34px) ─────────────────────────────────┤
//   y=74   ├─ Grid  10 rows × 40px = 400px ───────────────────────────┤
//   y=474  ├─ Status bar (66px) ─────────────────────────────────────┘
//   y=540

static const int DISPLAY_W  = 960;
static const int DISPLAY_H  = 540;

// Header
static const int HEADER_Y   = 0;
static const int HEADER_H   = 40;   // fits textSize-3 chars (24px) with 8px padding

// Column header strip
static const int CHAN_HDR_Y = HEADER_H;            // = 40
static const int COL_HDR_H  = 34;                   // fits textSize-3 chars (24px)

// Pattern grid
static const int GRID_Y       = CHAN_HDR_Y + COL_HDR_H;   // = 74
static const int ROW_H        = 40;                        // textSize-4 chars (32px) + 8px
static const int VISIBLE_ROWS = 10;                        // 10 × 40 = 400px
static const int GRID_H       = VISIBLE_ROWS * ROW_H;     // = 400

// Status bar
static const int STATUS_Y   = GRID_Y + GRID_H;            // = 474
static const int STATUS_H   = DISPLAY_H - STATUS_Y;       // = 66

// Reader line: fixed visible-row index the playback cursor sits on
static const int READER_IDX = 4;

// Column widths
static const int ROW_NUM_W        = 60;   // "63" at textSize-4 = 48px + 12px margin
static const int VISIBLE_COLUMNS  = 3;
static const int COL_W            = (DISPLAY_W - ROW_NUM_W) / VISIBLE_COLUMNS;   // = 300

// ── Text sizes ────────────────────────────────────────────────────────────────
// Grid rows: textSize 4 → 24×32px per char
static const int TEXT_SIZE  = 4;
static const int CHAR_W     = 24;
static const int CHAR_H     = 32;
static const int TEXT_PAD_Y = (ROW_H - CHAR_H) / 2;   // = 4

// Header / column-header / status: textSize 3 → 18×24px per char
static const int HDR_TEXT_SIZE = 3;
static const int HDR_CHAR_W    = 18;
static const int HDR_CHAR_H    = 24;

// Menu button — top-left corner of header
static const int MENU_BTN_W = 140;

// Mute button in column header (right edge of each output column)
static const int MUTE_BTN_W = 34;   // touch target width (right edge of column)

// ── Drag thresholds (used by TouchHandler) ────────────────────────────────────
static const int AXIS_LOCK_PX = 20;  // deadzone before axis is committed
static const int COL_DRAG_PX  = 80;  // horizontal pixels to shift one column


// ── TrackerUI ─────────────────────────────────────────────────────────────────

class TrackerUI {
public:
    TrackerUI(EPD_PainterAdafruit& display, Song& song, TrackerEngine& engine);

    // Full redraw — call once at startup and after clear()
    void drawAll();

    // Partial redraws — call when the relevant state changes
    void drawHeader();
    void drawGrid();
    void drawStatusBar();

    // Server-connected mode — enables PLAY/STOP buttons
    void setServerConnected(bool connected);
    bool serverConnected() const { return _serverConnected; }

    // Server play state — when playing, row position follows server; when stopped, follows cursor
    void setServerPlaying(bool playing);
    bool serverPlaying() const { return _serverPlaying; }

    // Battery indicator — pct 0-100 (-1 = unknown), charging shows lightning bolt
    void setBattery(int pct, bool charging);
    int  battPct()      const { return _battPct; }
    bool battCharging() const { return _battCharging; }

    // Memory used meter — pct 0-100 (-1 = unknown)
    void setMemPct(int pct);
    int  memPct() const { return _memPct; }

    // Connection status text shown in header (e.g. "Connecting...", "Connected")
    void setStatus(const char* msg);

    // Clock display in header — call periodically from main loop
    void setClock(int hour, int minute, int day, int month, int year);

    // No-song overlay — shown when server is set to OFF
    void setNoSong(bool v);
    bool noSong() const { return _noSong; }

    // Selection cursor
    void setSelected(int8_t row, int8_t col);
    int8_t selectedRow()    const { return _selRow; }
    int8_t selectedCol()    const { return _selCol; }

    // Column scroll offset — which column is shown in display column 0
    void    setColOffset(uint8_t offset);
    uint8_t colOffset()     const { return _colOffset; }

    // MIDI step-input target column — last output column (1+) edited via note editor
    void    setMidiInputCol(uint8_t col);
    uint8_t midiInputCol() const { return _midiInputCol; }

    // MIDI step-input row advance (1–4)
    uint8_t stepAdvance() const { return _stepAdvance; }
    void    cycleStepAdvance();   // 1→2→3→4→1

    // Hit-test helpers used by TouchHandler
    bool hitGrid(int tx, int ty, int8_t& row, int8_t& col) const;
    bool hitPlayButton(int tx, int ty)  const;
    bool hitStopButton(int tx, int ty)  const;
    bool hitBlockPrev(int tx, int ty)   const;
    bool hitBlockNext(int tx, int ty)   const;
    bool hitSongName(int tx, int ty)    const;  // tap on song name → save/load page
    bool hitBlockLabel(int tx, int ty)  const;  // tap on BLK:xx/xx → block settings
    bool hitMenuButton(int tx, int ty)  const;  // tap on MENU button → open menu
    bool hitMuteButton(int tx, int ty, int8_t& col) const;     // tap on mute icon (right edge of col header)
    bool hitColumnHeader(int tx, int ty, int8_t& col) const;  // tap on column header (not col 0)
    bool hitEditMode(int tx, int ty)   const;   // tap on [e] edit mode toggle
    bool hitStepAdvance(int tx, int ty) const;   // tap on [+N] step advance button
    bool hitVelCapture(int tx, int ty)  const;   // tap on [v] velocity capture toggle

    // Edit mode toggle — when active, MIDI step-input writes notes to current column
    bool editMode() const { return _editMode; }
    void toggleEditMode()  { _editMode = !_editMode; }

    // Velocity capture toggle — when active, MIDI step-input records velocity
    bool velCapture() const { return _velCapture; }
    void toggleVelCapture()  { _velCapture = !_velCapture; }

private:
    EPD_PainterAdafruit& _d;
    Song&                _song;
    TrackerEngine&       _engine;
    bool                 _serverConnected;
    bool                 _serverPlaying;
    int8_t               _selRow;
    int8_t               _selCol;
    uint8_t              _colOffset;   // first visible column index (0 for MVP)
    uint8_t              _midiInputCol; // MIDI step-input target (1+ = output col)
    uint8_t              _stepAdvance;  // rows to advance after MIDI step-input (1–4)
    bool                 _editMode;     // true = MIDI step-input writes to channel
    bool                 _velCapture;   // true = record MIDI velocity into notes
    bool                 _noSong;
    int                  _battPct;      // -1 = unknown
    bool                 _battCharging;
    int                  _memPct;       // -1 = unknown
    char                 _statusMsg[32];
    char                 _clockStr[20];  // "HH:MM  DD/MM/YY"

    int topRow() const;

    void drawRow(int visIdx, int patRow);
    void drawCell(int x, int y, const Note& note, uint8_t fg, bool showInstrument = true);
    void buildEmptyCellCache();
    void stampEmptyCell(int x, int y);  // memcpy pre-rendered cell into framebuffer

    // Pre-rendered empty cell (white bg + "--- -- -----" at TEXT_SIZE 4), built once.
    uint8_t _emptyCellCache[COL_W * ROW_H];
    bool    _emptyCellCached;

    // ts = Adafruit textSize for the label (defaults to HDR_TEXT_SIZE)
    void drawButton(int x, int y, int w, int h,
                    const char* label, uint8_t bg, uint8_t fg, int ts = HDR_TEXT_SIZE);

    void hline(int y, uint8_t colour);
    void vline(int x, int y1, int y2, uint8_t colour);
    void drawLightningBolt(int x, int y, int w, int h, uint8_t col);
    void drawSpeakerIcon(int x, int y, bool muted, uint8_t fg);
};

#pragma once
#include <Arduino.h>
#include "gt911_lite.h"
#include "TrackerUI.h"
#include "TrackerEngine.h"
#include "TrackerData.h"

enum class TouchAction {
    NONE,
    PLAY,
    STOP,
    CELL_SELECT,  // row/col in result fields
    COL_SCROLL,   // column offset changed — redraw needed
    CELL_TAP,     // tap without drag — open note editor
    BLOCK_PREV,      // navigate to previous block
    BLOCK_NEXT,      // navigate to next block
    SONG_NAME_TAP,   // tap on song name → open save/load page
    BLOCK_LABEL_TAP, // tap on BLK:xx/xx label → open block settings
    MENU_TAP,        // tap on MENU button → open/close menu
    MUTE_TAP,          // tap on mute icon in column header → toggle mute
    COLUMN_HEADER_TAP, // tap on column header (col 1+) → open column editor
    COLUMN_HEADER_HOLD,// 500ms hold on column header (any col incl. 0) → open note editor page
    EDIT_MODE_TAP,     // tap on [e] edit mode toggle
    STEP_ADVANCE_TAP,  // tap on [+N] step advance button → cycle 1→2→3→4→1
    VEL_CAPTURE_TAP    // tap on [v] velocity capture toggle
};

struct TouchResult {
    TouchAction action;
    int8_t      row;
    int8_t      col;
};

enum class TouchState {
    IDLE,
    DRAG_UNDECIDED,  // finger down on grid, axis not yet committed
    DRAG_VERTICAL,   // locked to row scroll
    DRAG_HORIZONTAL, // locked to column scroll
    BUTTON_PENDING   // finger down on a button — fires on release
};

enum class DragAxis { UNDECIDED, VERTICAL, HORIZONTAL };

class TouchHandler {
public:
    TouchHandler(GT911_Lite& touch, TrackerUI& ui, TrackerEngine& engine, Song& song);

    // Call every loop iteration. Returns action if something happened.
    TouchResult poll();

    // True while a finger is actively touching the grid (any drag state).
    // Check after poll() to detect grid press and release.
    bool isGridDown() const {
        return _state == TouchState::DRAG_UNDECIDED ||
               _state == TouchState::DRAG_VERTICAL  ||
               _state == TouchState::DRAG_HORIZONTAL;
    }

private:
    GT911_Lite&    _touch;
    TrackerUI&     _ui;
    TrackerEngine& _engine;
    Song&          _song;

    TouchState  _state;
    TouchAction _pendingButton;

    int     _dragStartX;
    int     _dragStartY;
    int8_t  _dragStartRow;
    int8_t  _dragStartCol;
    uint8_t _dragStartColOffset;
    bool    _wasDown;
    bool    _suppressTap;    // true if finger-down cancelled inertia — tap should not open editor

    // Column-header long-press: when finger goes down on a header, capture the
    // moment; if it stays down for 500ms, fire COLUMN_HEADER_HOLD and consume
    // the press so the falling edge does not also fire COLUMN_HEADER_TAP.
    uint32_t _holdStart;
    bool     _holdFired;

    // Velocity sampling for inertia launch
    int      _lastDragY;
    uint32_t _lastDragTime;
    float    _dragVel;       // rows/sec, updated continuously during drag

    // Inertia state
    bool     _inertiaActive;
    float    _inertiaRow;    // fractional row position
    float    _inertiaVel;    // rows per second
    uint32_t _lastInertiaTime;

    void rawToScreen(int rawX, int rawY, int& screenX, int& screenY) const;
};

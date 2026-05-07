#include "TouchHandler.h"
#include <math.h>


TouchHandler::TouchHandler(GT911_Lite& touch, TrackerUI& ui,
                           TrackerEngine& engine, Song& song)
    : _touch(touch)
    , _ui(ui)
    , _engine(engine)
    , _song(song)
    , _state(TouchState::IDLE)
    , _pendingButton(TouchAction::NONE)
    , _dragStartX(0)
    , _dragStartY(0)
    , _dragStartRow(0)
    , _dragStartCol(0)
    , _dragStartColOffset(0)
    , _wasDown(false)
    , _suppressTap(false)
    , _holdStart(0)
    , _holdFired(false)
    , _lastDragY(0)
    , _lastDragTime(0)
    , _dragVel(0.0f)
    , _inertiaActive(false)
    , _inertiaRow(0.0f)
    , _inertiaVel(0.0f)
    , _lastInertiaTime(0)
{}

void TouchHandler::rawToScreen(int rawX, int rawY, int& screenX, int& screenY) const {
    screenX = rawY;
    screenY = DISPLAY_H - rawX;
}

TouchResult TouchHandler::poll() {
    TouchResult result = { TouchAction::NONE, -1, -1 };

    // ── Inertia — advances every frame without needing a touch event ──────────
    if (_inertiaActive && !_wasDown) {
        uint32_t now = millis();
        float dt = (float)(now - _lastInertiaTime) / 1000.0f;
        _lastInertiaTime = now;

        if (dt > 0.0f && dt < 1.0f) {
            _inertiaRow += _inertiaVel * dt;
            _inertiaVel *= expf(-INERTIA_DECAY * dt);

            int patLen = _song.patterns[_engine.currentPattern()].length;
            // Always clamp at the pattern edges — coast to a stop, never wrap.
            if (_inertiaRow < 0.0f)           { _inertiaRow = 0.0f;              _inertiaVel = 0.0f; }
            if (_inertiaRow >= (float)patLen) { _inertiaRow = (float)(patLen-1); _inertiaVel = 0.0f; }

            int newRow = (int)(_inertiaRow + 0.5f);
            if (newRow < 0)       newRow = 0;
            if (newRow >= patLen) newRow = patLen - 1;

            if (newRow != (int)_ui.selectedRow()) {
                result.action = TouchAction::CELL_SELECT;
                result.row    = (int8_t)newRow;
                result.col    = _dragStartCol;
            }

            if (fabsf(_inertiaVel) < INERTIA_STOP_VEL) _inertiaActive = false;
        }
    }

    // ── Hold detection — runs every frame, independent of new touch events.
    //   _touch.read() below only returns true on state changes, so polls where
    //   the finger stays still produce no event; the elapsed-time check has to
    //   live before that early-return. _state stays in BUTTON_PENDING until
    //   the falling edge is processed, so it implies finger-still-down. ──────
    if (_state == TouchState::BUTTON_PENDING &&
        _pendingButton == TouchAction::COLUMN_HEADER_TAP &&
        !_holdFired && millis() - _holdStart >= 500) {
        result.action = TouchAction::COLUMN_HEADER_HOLD;
        result.col    = _dragStartCol;
        // The page that's about to open does its own touch polling, so we never
        // see the falling edge for this gesture. Hand the finger off cleanly:
        // reset to IDLE / !_wasDown so the next press after the page closes is
        // recognised as a fresh rising edge.
        _state     = TouchState::IDLE;
        _wasDown   = false;
        _holdFired = false;
        DBG("[TOUCH] -> COLUMN_HEADER_HOLD col=%d\n", _dragStartCol);
        return result;
    }

    if (!_touch.read()) return result;

    bool down = _touch.isTouched;
    int tx, ty;
    rawToScreen(_touch.x, _touch.y, tx, ty);

    // ── Falling edge — finger lifted ──────────────────────────────────────────
    if (!down) {
        DBG("[TOUCH] up   screen(%d,%d)\n", tx, ty);

        if (_state == TouchState::BUTTON_PENDING) {
            // Hold already fired during the press — release is silent.
            // Col 0 has no tap action; only output cols emit COLUMN_HEADER_TAP.
            if (_holdFired) {
                DBGLN("[TOUCH] -> release after hold (silent)");
            } else if (_pendingButton == TouchAction::COLUMN_HEADER_TAP &&
                       _dragStartCol == 0) {
                DBGLN("[TOUCH] -> col 0 tap suppressed");
            } else {
                result.action = _pendingButton;
                if (_pendingButton == TouchAction::COLUMN_HEADER_TAP ||
                    _pendingButton == TouchAction::MUTE_TAP)
                    result.col = _dragStartCol;
                DBGLN("[TOUCH] -> button fired");
            }
        } else if (_state == TouchState::DRAG_UNDECIDED) {
            if (!_suppressTap) {
                result.action = TouchAction::CELL_TAP;
                result.row    = _dragStartRow;
                result.col    = _dragStartCol;
                DBG("[TOUCH] -> CELL_TAP  row=%d  col=%d\n",
                    _dragStartRow, _dragStartCol);
            } else {
                DBGLN("[TOUCH] -> tap suppressed (was stopping inertia)");
            }
        } else if (_state == TouchState::DRAG_VERTICAL) {
            // Use the rolling velocity tracked during drag — GT911 up-event coords
            // are unreliable (often same as last position, giving zero delta).
            uint32_t elapsed = millis() - _lastDragTime;
            if (elapsed < 600 && fabsf(_dragVel) >= INERTIA_MIN_VEL) {
                _inertiaActive   = true;
                _inertiaRow      = (float)_ui.selectedRow();
                _inertiaVel      = _dragVel;
                _lastInertiaTime = millis();
                DBG("[TOUCH] inertia launched vel=%.1f\n", _dragVel);
            }
        }

        _state   = TouchState::IDLE;
        _wasDown = false;
        return result;
    }

    // ── Rising edge — finger pressed ──────────────────────────────────────────
    if (!_wasDown) {
        _suppressTap   = _inertiaActive;  // finger-down during inertia stops scroll, not tap
        _wasDown       = true;
        _inertiaActive = false;  // any new touch cancels inertia
        DBG("[TOUCH] down screen(%d,%d)\n", tx, ty);

        // Mute button tap — check before column header (it's within the header zone)
        int8_t colHdr;
        if (_ui.hitMuteButton(tx, ty, colHdr)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::MUTE_TAP;
            _dragStartCol = colHdr;
            return result;
        }

        // Column header tap — before grid hit test (header is above grid).
        // Pass inclInput=true so col 0 also enters the long-press path; on
        // release we filter col 0 out of the tap result.
        if (_ui.hitColumnHeader(tx, ty, colHdr, /*inclInput*/ true)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::COLUMN_HEADER_TAP;
            _dragStartCol = colHdr;
            _holdStart    = millis();
            _holdFired    = false;
            return result;
        }

        int8_t row, col;
        if (_ui.hitGrid(tx, ty, row, col)) {
            _state              = TouchState::DRAG_UNDECIDED;
            _dragStartX         = tx;
            _dragStartY         = ty;
            _dragStartRow       = row;
            _dragStartCol       = col;
            _dragStartColOffset = _ui.colOffset();
            _dragVel            = 0.0f;
            _lastDragY          = ty;
            _lastDragTime       = millis();
            DBG("[TOUCH] -> grid touch  row=%d  col=%d  colOffset=%d\n",
                row, col, _ui.colOffset());
            return result;
        }

        if (_ui.hitPlayButton(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::PLAY;
        } else if (_ui.hitStopButton(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::STOP;
        } else if (_ui.hitBlockPrev(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::BLOCK_PREV;
        } else if (_ui.hitBlockNext(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::BLOCK_NEXT;
        } else if (_ui.hitSongName(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::SONG_NAME_TAP;
        } else if (_ui.hitEditMode(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::EDIT_MODE_TAP;
        } else if (_ui.hitStepAdvance(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::STEP_ADVANCE_TAP;
        } else if (_ui.hitVelCapture(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::VEL_CAPTURE_TAP;
        } else if (_ui.hitBlockLabel(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::BLOCK_LABEL_TAP;
        } else if (_ui.hitMenuButton(tx, ty)) {
            _state = TouchState::BUTTON_PENDING;
            _pendingButton = TouchAction::MENU_TAP;
        } else {
            DBGLN("[TOUCH] -> no hit");
        }
        return result;
    }

    // ── Finger still down — handle drag ───────────────────────────────────────

    int deltaX = tx - _dragStartX;
    int deltaY = ty - _dragStartY;
    int absX   = deltaX < 0 ? -deltaX : deltaX;
    int absY   = deltaY < 0 ? -deltaY : deltaY;

    if (_state == TouchState::DRAG_UNDECIDED) {
        if (absX >= AXIS_LOCK_PX || absY >= AXIS_LOCK_PX) {
            if (absX >= absY) {
                _state = TouchState::DRAG_HORIZONTAL;
            } else {
                _state      = TouchState::DRAG_VERTICAL;
                _lastDragY    = ty;
                _lastDragTime = millis();
            }
            DBG("[TOUCH] axis locked: %s\n",
                _state == TouchState::DRAG_HORIZONTAL ? "HORIZONTAL" : "VERTICAL");
        }
        return result;
    }

    if (_state == TouchState::DRAG_VERTICAL) {
        int deltaRows = deltaY / ROW_H;
        int patLen    = _song.patterns[_engine.currentPattern()].length;
        int newRow    = _dragStartRow - deltaRows;
        if (newRow < 0)       newRow = 0;
        if (newRow >= patLen) newRow = patLen - 1;

        // Update rolling velocity — used on release, avoids relying on up-event coords
        uint32_t nowDrag = millis();
        uint32_t dtDragMs = nowDrag - _lastDragTime;
        if (dtDragMs > 0) {
            float dtDrag = (float)dtDragMs / 1000.0f;
            _dragVel = -(float)(ty - _lastDragY) / (float)ROW_H / dtDrag;
            if (_dragVel >  INERTIA_MAX_VEL) _dragVel =  INERTIA_MAX_VEL;
            if (_dragVel < -INERTIA_MAX_VEL) _dragVel = -INERTIA_MAX_VEL;
        }
        _lastDragY    = ty;
        _lastDragTime = nowDrag;

        if ((int8_t)newRow != _ui.selectedRow()) {
            DBG("[TOUCH] vert drag  row=%d\n", newRow);
            result.action = TouchAction::CELL_SELECT;
            result.row    = (int8_t)newRow;
            result.col    = _dragStartCol;
        }
    }

    if (_state == TouchState::DRAG_HORIZONTAL) {
        int  colShift  = -deltaX / COL_DRAG_PX;
        int  maxOffset = MAX_COLUMNS - VISIBLE_COLUMNS;
        int  newOffset = (int)_dragStartColOffset + colShift;
        if (newOffset < 0)          newOffset = 0;
        if (newOffset > maxOffset)  newOffset = maxOffset;

        if ((uint8_t)newOffset != _ui.colOffset()) {
            DBG("[TOUCH] horiz drag  colOffset=%d\n", newOffset);
            _ui.setColOffset((uint8_t)newOffset);
            result.action = TouchAction::COL_SCROLL;
        }
    }

    return result;
}

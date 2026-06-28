#include "ScrollViewport.h"
#include "Constants.h"
#include <math.h>

ScrollViewport::ScrollViewport(EPD_PainterAdafruit& d) : _d(d) {}

void ScrollViewport::configure(int x, int y, int w, int h, PaintFn paint, TapFn tap) {
    _x = x; _y = y; _w = w; _h = h;
    _paint = paint;
    _tap   = tap;
    reset();
}

void ScrollViewport::setStep(int px) { _step = (px < 1) ? 1 : px; }

void ScrollViewport::reset() {
    _offset    = 0.0f;
    _haveDrawn = false;
    _force     = true;
    _down      = false;
    _dragMoved = false;
    _vel       = 0.0f;
    _inertia   = false;
}

// Round to the nearest step.  Handles negatives symmetrically (the offset is
// never clamped, so it can be below zero).
int ScrollViewport::quant(float o) const {
    if (_step <= 1) return (int)lroundf(o);
    return (int)lroundf(o / (float)_step) * _step;
}

bool ScrollViewport::inRect(int sx, int sy) const {
    return sx >= _x && sx < _x + _w && sy >= _y && sy < _y + _h;
}

void ScrollViewport::redraw() { _force = true; flush(true); }

// Repaint the visible slice if the requested offset changed (or forced).  No
// clamping: the painter decides what to show at any offset and whether momentum
// should stop (return true) — the component just respects that.
void ScrollViewport::flush(bool immediate) {
    bool forced = _force;
    _force = false;

    int q = quant(_offset);
    if (_haveDrawn && q == _drawnOffset && !forced) return;
    if (!_paint) { _drawnOffset = q; _haveDrawn = true; return; }

    bool stop = _paint(_x, _y, _w, _h, q);
    if (stop) { _vel = 0.0f; _inertia = false; }

    _drawnOffset = q;
    _haveDrawn   = true;
    if (immediate) _d.paint(); else _d.paintLater();
}

void ScrollViewport::tick() {
    if (!_inertia) return;
    uint32_t now = millis();
    float dt = (float)(now - _inertiaMs) / 1000.0f;
    if (dt <= 0.0f) return;
    _inertiaMs = now;

    _offset += _vel * dt;
    _vel    *= expf(-SCROLL_DECAY * dt);

    if (_vel < SCROLL_STOP_VEL && _vel > -SCROLL_STOP_VEL) {
        _vel     = 0.0f;
        _inertia = false;
    }
    flush(false);   // painter may also stop us early via its return value
}

void ScrollViewport::poll(bool down, int sx, int sy) {
    // Rising edge — only engage if the press starts inside the viewport, so a
    // press on a host button above/below the band doesn't grab the scroll.
    if (down && !_down) {
        if (!inRect(sx, sy)) return;
        _down            = true;
        _dragMoved       = false;
        _inertia         = false;
        _vel             = 0.0f;
        _dragStartY      = sy;
        _dragStartOffset = _offset;
        _velLastY        = sy;
        _velLastMs       = millis();
        return;
    }

    // Held — pan once movement passes the threshold, sampling fling velocity.
    if (down && _down) {
        int dy = sy - _dragStartY;
        if (!_dragMoved && (dy >= SCROLL_DRAG_THRESH_PX || dy <= -SCROLL_DRAG_THRESH_PX))
            _dragMoved = true;
        if (_dragMoved) {
            // Drag DOWN (sy increases) reveals earlier content → offset shrinks.
            _offset = _dragStartOffset + (float)(_dragStartY - sy);
            uint32_t now = millis();
            uint32_t dms = now - _velLastMs;
            if (dms >= 16) {
                float dpx = (float)(_velLastY - sy);
                _vel = dpx / ((float)dms / 1000.0f);
                if (_vel >  SCROLL_MAX_VEL) _vel =  SCROLL_MAX_VEL;
                if (_vel < -SCROLL_MAX_VEL) _vel = -SCROLL_MAX_VEL;
                _velLastY  = sy;
                _velLastMs = now;
            }
            flush(false);
        }
        return;
    }

    // Falling edge — fling, or (if it never moved past the threshold) a tap.
    if (!down && _down) {
        _down = false;
        if (_dragMoved) {
            _dragMoved = false;
            if (_vel > SCROLL_MIN_VEL || _vel < -SCROLL_MIN_VEL) {
                _inertia   = true;
                _inertiaMs = millis();
            }
            return;   // a drag is never a tap
        }
        if (inRect(sx, sy) && _tap) {
            _tap(sx - _x, _drawnOffset + (sy - _y));
        }
    }
}

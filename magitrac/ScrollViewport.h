#pragma once
#include <Arduino.h>
#include <functional>
#include "EPD_Painter_Adafruit.h"

// ── ScrollViewport — reusable inertia drag-scroll controller ──────────────────
//
// Owns scroll physics (a pixel offset, momentum, tap-vs-drag) and repaint
// coalescing for a rectangular viewport.  It is drawing-agnostic: the host
// supplies a PaintFn that renders the visible slice and a TapFn for taps.
//
// There is no start and no end.  The offset is sovereign — it is moved only by
// dragging and by momentum, it is never clamped, and it may go NEGATIVE (drag
// above the first item) or run past the last item.  The component holds no
// notion of content size or item count.
//
// The painter is the sole authority on when momentum should die: PaintFn
// returns `true` to STOP inertia (typically when it has scrolled its content to
// — or off — an edge and wants the coast to end), or `false` to let it carry
// on.  Nothing is remembered between paints; the painter decides afresh every
// frame from whatever it is currently showing.  What a painter draws past its
// own edges (blank, or a pinned last page) is entirely up to the painter.
//
// PaintFn MUST fill the entire rect on every call.
//
// Tuning lives in Constants.h (SCROLL_*).  Quantise scrolling with setStep():
// step = rowHeight snaps line-by-line (and repaints once per line — the
// e-paper-friendly default for text lists); step = 1 is pixel-smooth.

class ScrollViewport {
public:
    // (x,y,w,h) = viewport rect in screen coords.  offsetY = the px offset being
    // requested (may be negative); render so content-pixel offsetY sits at rect
    // top.  Return `true` to stop any momentum, `false` to let it continue.
    using PaintFn = std::function<bool(int x, int y, int w, int h, int offsetY)>;
    // tx = relative to the rect's left edge.  ty = CONTENT space
    // (offsetY + (touchY - rectTop)), so the host does ty / itemHeight.
    using TapFn   = std::function<void(int tx, int ty)>;

    explicit ScrollViewport(EPD_PainterAdafruit& d);

    void configure(int x, int y, int w, int h, PaintFn paint, TapFn tap);
    void setStep(int px);          // scroll quantum; 1 = pixel-accurate (default)
    void reset();                  // offsetY = 0, kill momentum

    // Feed when a fresh touch sample arrives (touch.read() == true).
    void poll(bool fingerDown, int sx, int sy);
    // Call every loop — advances coasting and repaints on change.
    void tick();
    // Force a full repaint — call after configure/reset, or when the content
    // changes (e.g. a streamed page arrived, an item was added/removed).
    void redraw();

    int  offsetY()    const { return _drawnOffset; }
    bool isDragging() const { return _down && _dragMoved; }

private:
    EPD_PainterAdafruit& _d;
    PaintFn _paint;
    TapFn   _tap;

    int   _x = 0, _y = 0, _w = 0, _h = 0;
    int   _step = 1;

    float _offset      = 0.0f;   // continuous scroll position (px), unclamped
    int   _drawnOffset = 0;      // offset currently on screen
    bool  _haveDrawn   = false;  // false until the first paint
    bool  _force       = false;  // force the next flush to repaint

    // drag
    bool  _down       = false;
    bool  _dragMoved  = false;
    int   _dragStartY = 0;
    float _dragStartOffset = 0.0f;

    // velocity sampling
    int      _velLastY  = 0;
    uint32_t _velLastMs = 0;
    float    _vel       = 0.0f;

    // inertia
    bool     _inertia   = false;
    uint32_t _inertiaMs = 0;

    int  quant(float o) const;
    bool inRect(int sx, int sy) const;
    void flush(bool immediate);
};

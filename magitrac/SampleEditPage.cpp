#include "SampleEditPage.h"
#include "ServerPairing.h"
#include "UIHelpers.h"
#include "Constants.h"
#include "TrackerData.h"   // SFX_CHANNEL

void SampleEditPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

void SampleEditPage::open(uint8_t sampleId, const char* name, uint8_t col) {
    _sampleId = sampleId;
    _col      = col;
    strncpy(_name, name ? name : "", sizeof(_name) - 1);
    _name[sizeof(_name) - 1] = '\0';
    _loading    = true;
    _playing    = false;
    _wasDown    = _touch.isTouched;
    _dragMarker = -1;
    _dragScroll = false;
    _viewStart  = 0;
    _viewEnd    = 0;   // whole file

    gServerPairing.clearSampleInfo();
    gServerPairing.sendSampleEdit(SAMPLE_EDIT_GET, _sampleId);

    _d.clear();                    // anti-ghost cycle only — doesn't touch the FB
    _d.fillScreen(COL_WHITE);      // ...so wipe the bitmap too
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(360, 260);
    _d.print("LOADING...");
    drawHeader();
    _d.paint();
}

void SampleEditPage::adopt() {
    _info  = gServerPairing.sampleInfo();
    _start = _info.startFrame;
    _end   = _info.endFrame;          // 0 = end of file
    _loop  = _info.loop != 0;
    if (_start >= _info.totalFrames) _start = 0;
    if (_end > _info.totalFrames)    _end   = 0;
    _loading = false;
}

void SampleEditPage::sendMeta() {
    gServerPairing.sendSampleEdit(SAMPLE_EDIT_SET, _sampleId,
                                  _start, _end, _loop ? 1 : 0);
}

void SampleEditPage::fetchView() {
    gServerPairing.clearSampleInfo();
    gServerPairing.sendSampleEdit(SAMPLE_EDIT_GET, _sampleId,
                                  _viewStart, _viewEnd);
}

void SampleEditPage::formatTime(uint32_t frame, char* out, int outLen) const {
    uint32_t rate = _info.rate ? _info.rate : 1;
    uint32_t ms   = (uint32_t)(((uint64_t)frame * 1000) / rate);
    snprintf(out, outLen, "%lu.%03lus",
             (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
}

void SampleEditPage::drawHeader() {
    _d.fillRect(0, 0, 960, SE_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(12, (SE_HDR_H - 24) / 2);
    _d.print(_name);

    if (!_loading) {
        uiButton(_d, SE_LOOP_X, SE_BTN_Y, SE_LOOP_W, SE_BTN_H,
                 _loop ? "LOOP ON" : "LOOP OFF",
                 _loop ? COL_BLACK : COL_WHITE,
                 _loop ? COL_WHITE : COL_BLACK, 2);
        uiButton(_d, SE_PLAY_X, SE_BTN_Y, SE_PLAY_W, SE_BTN_H,
                 _playing ? "STOP" : "PLAY",
                 _playing ? COL_BLACK : COL_WHITE,
                 _playing ? COL_WHITE : COL_BLACK, 2);
    }
    uiButton(_d, SE_BACK_X, SE_BTN_Y, SE_BACK_W, SE_BTN_H, "BACK",
             COL_WHITE, COL_BLACK, 3);
}

int SampleEditPage::markerX(int which) const {
    uint32_t f = which ? (_end ? _end : _info.totalFrames) : _start;
    int64_t rel = (int64_t)f - (int64_t)_viewStart;
    if (rel < 0) rel = 0;
    uint32_t len = viewLen() ? viewLen() : 1;
    if ((uint64_t)rel > len) rel = len;
    int x = SE_WAVE_X + (int)(((uint64_t)rel * SE_WAVE_W) / len);
    if (x > SE_WAVE_X + SE_WAVE_W - 2) x = SE_WAVE_X + SE_WAVE_W - 2;
    return x;
}

uint32_t SampleEditPage::frameForX(int sx) const {
    int px = sx - SE_WAVE_X;
    if (px < 0) px = 0;
    if (px > SE_WAVE_W) px = SE_WAVE_W;
    return _viewStart + (uint32_t)(((uint64_t)px * viewLen()) / SE_WAVE_W);
}

void SampleEditPage::drawWave() {
    const int midY = SE_WAVE_Y + SE_WAVE_H / 2;
    _d.fillRect(SE_WAVE_X - 4, SE_WAVE_Y - 4, SE_WAVE_W + 8, SE_WAVE_H + 8, COL_WHITE);
    _d.drawRect(SE_WAVE_X - 4, SE_WAVE_Y - 4, SE_WAVE_W + 8, SE_WAVE_H + 8, COL_LTGREY);

    uint32_t len  = viewLen() ? viewLen() : 1;
    uint32_t endF = _end ? _end : _info.totalFrames;

    // Trim boundaries in view columns (clamped; may sit off-screen).
    int64_t stRel = ((int64_t)_start - (int64_t)_viewStart) * SAMPLE_OVERVIEW_N / (int64_t)len;
    int64_t enRel = ((int64_t)endF   - (int64_t)_viewStart) * SAMPLE_OVERVIEW_N / (int64_t)len;

    for (int c = 0; c < SAMPLE_OVERVIEW_N; c++) {
        int mn = _info.peaks[c * 2];
        int mx = _info.peaks[c * 2 + 1];
        int y0 = midY - (mx * (SE_WAVE_H / 2)) / 128;
        int y1 = midY - (mn * (SE_WAVE_H / 2)) / 128;
        if (y1 <= y0) y1 = y0 + 1;
        // Inside the trim = black, outside = light grey (still visible).
        uint8_t col = (c >= stRel && c < enRel) ? COL_BLACK : COL_LTGREY;
        _d.fillRect(SE_WAVE_X + c * 2, y0, 2, y1 - y0, col);
    }

    // Markers (only when their frame is inside the view).
    if (_start >= _viewStart && _start <= _viewEnd)
        _d.fillRect(markerX(0), SE_WAVE_Y - 4, 2, SE_WAVE_H + 8, COL_BLACK);
    if (endF >= _viewStart && endF <= _viewEnd)
        _d.fillRect(markerX(1), SE_WAVE_Y - 4, 2, SE_WAVE_H + 8, COL_BLACK);

    // View-window readout: window times, trim times, total.
    char a[16], b[16], t[96];
    _d.fillRect(SE_WAVE_X, SE_TIME_Y, SE_WAVE_W, 22, COL_WHITE);
    formatTime(_start, a, sizeof(a));
    formatTime(endF, b, sizeof(b));
    snprintf(t, sizeof(t), "TRIM %s - %s", a, b);
    formatTime(_info.totalFrames, a, sizeof(a));
    strncat(t, "   OF ", sizeof(t) - strlen(t) - 1);
    strncat(t, a, sizeof(t) - strlen(t) - 1);
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(SE_WAVE_X, SE_TIME_Y);
    _d.print(t);
}

void SampleEditPage::drawScrollbar() {
    bool zoomed = viewLen() < _info.totalFrames;
    _d.fillRect(SE_WAVE_X, SE_SCROLL_Y, SE_WAVE_W, SE_SCROLL_H, COL_WHITE);
    _d.drawRect(SE_WAVE_X, SE_SCROLL_Y, SE_WAVE_W, SE_SCROLL_H,
                zoomed ? COL_BLACK : COL_LTGREY);
    if (!zoomed || _info.totalFrames == 0) return;   // whole file visible

    int thumbW = (int)(((uint64_t)viewLen() * SE_WAVE_W) / _info.totalFrames);
    if (thumbW < 40) thumbW = 40;
    int thumbX = SE_WAVE_X +
                 (int)(((uint64_t)_viewStart * (SE_WAVE_W - thumbW)) /
                       (_info.totalFrames - viewLen()));
    _d.fillRect(thumbX, SE_SCROLL_Y + 4, thumbW, SE_SCROLL_H - 8, COL_BLACK);
}

void SampleEditPage::drawZoomRow() {
    _d.fillRect(0, SE_ZOOM_Y, 960, SE_ZOOM_H, COL_WHITE);
    int btnY = SE_ZOOM_Y + (SE_ZOOM_H - SE_BTN_H) / 2;
    bool canOut = viewLen() < _info.totalFrames;
    bool canIn  = viewLen() > SE_MIN_VIEW_FRAMES;
    uiButton(_d, SE_ZOUT_X, btnY, SE_ZBTN_W, SE_BTN_H, "ZOOM -",
             COL_WHITE, canOut ? COL_BLACK : COL_DKGREY, 2);
    uiButton(_d, SE_ZIN_X, btnY, SE_ZBTN_W, SE_BTN_H, "ZOOM +",
             COL_WHITE, canIn ? COL_BLACK : COL_DKGREY, 2);

    // Zoom factor readout.
    char z[24];
    uint32_t len = viewLen() ? viewLen() : 1;
    snprintf(z, sizeof(z), "x%lu", (unsigned long)(_info.totalFrames / len));
    _d.setTextSize(3);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(SE_ZIN_X + SE_ZBTN_W + 40, btnY + (SE_BTN_H - 24) / 2);
    _d.print(z);

    uiButton(_d, SE_SPEC_X, btnY, SE_SPEC_W, SE_BTN_H, "SPECTRUM",
             COL_WHITE, COL_BLACK, 2);
}

// Fairlight CMI "Page D" homage — 3D hidden-line waterfall of the trimmed
// region.  Slices back-to-front; each column is blacked below its curve so
// nearer traces occlude farther ones, then the curve drawn on top.  Inverted
// (white on black) for the CRT look.  Display only; any touch returns.
void SampleEditPage::drawSpectrogram() {
    _d.fillScreen(COL_BLACK);
    _d.setTextSize(2);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(330, 18);
    _d.print("*** WAVEFORM DISPLAY ***");
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(12, 516);
    _d.print(_name);
    _d.setCursor(760, 516);
    _d.print("TOUCH: BACK");

    const uint8_t* sp = gServerPairing.specData();
    const int plotX   = 90;    // front-left origin
    const int baseY   = 452;   // front slice floor
    const int dxSlice = 5;     // deeper slice → right + up
    const int dySlice = 6;
    const int span    = 550;   // pixel width of one slice trace
    const int maxH    = 150;

    // ── Scales ────────────────────────────────────────────────────────────────
    // Frequency along the front edge (log, 60 Hz .. fTop — mirror the server's
    // bin mapping); time recedes along the left edge (0 = front = sound start).
    {
        uint32_t rate = _info.rate ? _info.rate : 32000;
        float fTop = (rate / 2 > 8500) ? 8000.0f : (float)rate * 0.45f;
        _d.setTextSize(2);
        _d.setTextColor(COL_WHITE);

        static const struct { float hz; const char* lbl; } FT[] = {
            { 100, "100" }, { 300, "300" }, { 1000, "1K" },
            { 3000, "3K" }, { 8000, "8K" },
        };
        float lr = logf(fTop / 60.0f);
        for (unsigned i = 0; i < sizeof(FT) / sizeof(FT[0]); i++) {
            if (FT[i].hz > fTop) break;
            int x = plotX + (int)(span * logf(FT[i].hz / 60.0f) / lr);
            _d.fillRect(x, baseY + 4, 2, 8, COL_WHITE);            // tick
            _d.setCursor(x - (int)strlen(FT[i].lbl) * 6, baseY + 16);
            _d.print(FT[i].lbl);
        }
        _d.setCursor(plotX + span - 24, baseY + 40);
        _d.print("HZ");

        // Time axis: along the receding RIGHT edge, clear of the mountain
        // (no slice ever draws right of its own trace end).  0s at the front,
        // the trim's duration at the back.
        int fx = plotX + span + 14;                              // front-right
        int fy = baseY;
        int bx = fx + (SAMPLE_SPEC_SLICES - 1) * dxSlice;        // back-right
        int by = fy - (SAMPLE_SPEC_SLICES - 1) * dySlice;
        _d.drawLine(fx, fy, bx, by, COL_DKGREY);
        _d.fillRect(fx - 3, fy - 1, 6, 2, COL_WHITE);            // end ticks
        _d.fillRect(bx - 3, by - 1, 6, 2, COL_WHITE);
        uint32_t endF = _end ? _end : _info.totalFrames;
        char tbuf[16];
        _d.setCursor(fx + 10, fy - 14);
        _d.print("0S");
        formatTime((endF > _start) ? endF - _start : 0, tbuf, sizeof(tbuf));
        int lx = bx + 10;
        if (lx + (int)strlen(tbuf) * 12 > 956) lx = 956 - (int)strlen(tbuf) * 12;
        _d.setCursor(lx, by - 8);
        _d.print(tbuf);
        _d.setTextColor(COL_DKGREY);
        int mx2 = (fx + bx) / 2 + 12;
        int my2 = (fy + by) / 2;
        _d.setCursor(mx2, my2);
        _d.print("TIME");
    }

    for (int s = SAMPLE_SPEC_SLICES - 1; s >= 0; s--) {
        const uint8_t* row = sp + s * SAMPLE_SPEC_BINS;
        int ox = plotX + s * dxSlice;
        int oy = baseY - s * dySlice;
        int prevY = oy;
        for (int px = 0; px <= span; px++) {
            int32_t fb = (int32_t)px * (SAMPLE_SPEC_BINS - 1);
            int b  = fb / span;
            int fr = fb % span;
            int v  = (row[b] * (span - fr) +
                      row[(b + 1 < SAMPLE_SPEC_BINS) ? b + 1 : b] * fr) / span;
            int h  = v * maxH / 255;
            int x  = ox + px;
            int y  = oy - h;
            if (h > 0) _d.fillRect(x, y + 1, 1, h, COL_BLACK);   // occlude behind
            // Curve: connect to the previous column for steep segments.
            int y0 = (prevY < y) ? prevY : y;
            int y1 = (prevY > y) ? prevY : y;
            _d.fillRect(x, y0, 1, y1 - y0 + 1, COL_WHITE);
            prevY = y;
        }
    }
}

void SampleEditPage::draw() {
    _d.fillScreen(COL_WHITE);      // full FB wipe (clear() is the EPD cycle only)
    drawHeader();
    drawWave();
    drawScrollbar();
    drawZoomRow();
}

void SampleEditPage::zoom(int dir) {
    uint32_t total = _info.totalFrames;
    if (total == 0) return;
    uint32_t len = viewLen() ? viewLen() : total;
    uint64_t centre = (uint64_t)_viewStart + len / 2;

    if (dir > 0) {
        if (len <= SE_MIN_VIEW_FRAMES) return;
        len /= 2;
        if (len < SE_MIN_VIEW_FRAMES) len = SE_MIN_VIEW_FRAMES;
    } else {
        if (len >= total) return;
        len *= 2;
        if (len >= total) { _viewStart = 0; _viewEnd = total; fetchView(); return; }
    }
    int64_t vs = (int64_t)centre - (int64_t)len / 2;
    if (vs < 0) vs = 0;
    if ((uint64_t)vs + len > total) vs = (int64_t)total - (int64_t)len;
    _viewStart = (uint32_t)vs;
    _viewEnd   = _viewStart + len;
    fetchView();
}

void SampleEditPage::scrollTo(int tx) {
    uint32_t total = _info.totalFrames;
    uint32_t len   = viewLen();
    if (total == 0 || len >= total) return;
    // Centre the window on the track position under the finger.
    int px = tx - SE_WAVE_X;
    if (px < 0) px = 0;
    if (px > SE_WAVE_W) px = SE_WAVE_W;
    int64_t centre = ((int64_t)px * total) / SE_WAVE_W;
    int64_t vs = centre - (int64_t)len / 2;
    if (vs < 0) vs = 0;
    if ((uint64_t)vs + len > total) vs = (int64_t)total - (int64_t)len;
    if ((uint32_t)vs != _viewStart) {
        _viewStart = (uint32_t)vs;
        _viewEnd   = _viewStart + len;
        drawScrollbar();
        _d.paintLater();
    }
}

void SampleEditPage::auditionStop() {
    if (!_playing) return;
    _playing = false;
    gServerPairing.sendSampleEdit(SAMPLE_EDIT_STOP, _sampleId);
}

bool SampleEditPage::poll() {
    // Overview arrival — the initial load adopts the trim meta too; later
    // arrivals (zoom/scroll windows) just refresh the wave.
    if (gServerPairing.sampleInfoPending() &&
        gServerPairing.sampleInfo().sampleId == _sampleId) {
        bool first = _loading;
        gServerPairing.clearSampleInfo();
        if (first) {
            adopt();
        } else {
            _info = gServerPairing.sampleInfo();
        }
        _viewStart = _info.viewStart;
        _viewEnd   = _info.viewEnd;
        if (first) draw();
        else { drawWave(); drawScrollbar(); drawZoomRow(); }
        _d.paintLater();
    }

    // Spectrogram popup: render when the pages have all arrived; any touch
    // returns to the editor.
    if (_specMode == 1 && gServerPairing.specComplete()) {
        drawSpectrogram();
        _d.paintLater();
        _specMode = 2;
    }

    if (!_touch.read()) return false;   // no new touch event this frame
    bool down = _touch.isTouched;
    int  tx, ty;
    rawToScreen(_touch.x, _touch.y, tx, ty);

    if (_specMode > 0) {
        if (down && !_wasDown) {
            _wasDown  = true;
            _specMode = 0;
            _d.clear();               // popup was inverted — anti-ghost + wipe
            draw();
            _d.paintLater();
        } else if (!down) {
            _wasDown = false;
        }
        return false;
    }

    // ── Marker drag in progress: follow the finger ────────────────────────────
    if (_dragMarker >= 0) {
        if (down) {
            uint32_t f    = frameForX(tx);
            uint32_t endF = _end ? _end : _info.totalFrames;
            bool changed  = false;
            if (_dragMarker == 0) {
                uint32_t lim = (endF > 16) ? endF - 16 : 0;
                if (f > lim) f = lim;
                if (f != _start) { _start = f; changed = true; }
            } else {
                if (f < _start + 16) f = _start + 16;
                uint32_t nf = (f >= _info.totalFrames) ? 0 : f;
                if (nf != _end) { _end = nf; changed = true; }
            }
            if (changed) {
                drawWave();
                _d.paintLater();
            }
        } else {
            _dragMarker = -1;           // released: keep position, store it
            _wasDown    = false;
            sendMeta();
        }
        return false;
    }

    // ── Scrollbar drag in progress ────────────────────────────────────────────
    if (_dragScroll) {
        if (down) {
            scrollTo(tx);
        } else {
            _dragScroll = false;        // released: fetch the new window's peaks
            _wasDown    = false;
            fetchView();
        }
        return false;
    }

    if (down && !_wasDown) {
        _wasDown = true;

        if (ty < SE_HDR_H) {
            if (tx >= SE_BACK_X) {
                // Fire on RELEASE: the ColumnEditor reopens with its HOME
                // button in this same corner — acting on the press would let
                // the release of this very tap close it too.
                _backArmed = true;
                return false;
            }
            if (_loading) return false;
            if (tx >= SE_LOOP_X && tx < SE_LOOP_X + SE_LOOP_W) {
                _loop = !_loop;
                sendMeta();
                drawHeader();
                _d.paintLater();
            } else if (tx >= SE_PLAY_X && tx < SE_PLAY_X + SE_PLAY_W) {
                if (_playing) {
                    auditionStop();
                } else {
                    _playing = true;
                    gServerPairing.sendAuditionRawNote(SFX_CHANNEL, 60, 100, _col);
                }
                drawHeader();
                _d.paintLater();
            }
            return false;
        }
        if (_loading) return false;

        // Touch-down near a marker line grabs it; it then follows the drag
        // and locks in (meta stored) on release.
        if (ty >= SE_WAVE_Y - 20 && ty < SE_WAVE_Y + SE_WAVE_H + 20 &&
            tx >= SE_WAVE_X - 20 && tx < SE_WAVE_X + SE_WAVE_W + 20) {
            int dS = tx - markerX(0); if (dS < 0) dS = -dS;
            int dE = tx - markerX(1); if (dE < 0) dE = -dE;
            const int GRAB = 60;                   // finger-sized capture zone
            if (dS <= dE && dS <= GRAB)      _dragMarker = 0;
            else if (dE < dS && dE <= GRAB)  _dragMarker = 1;
            return false;
        }

        // Scrollbar: start dragging the window (only useful when zoomed).
        if (ty >= SE_SCROLL_Y && ty < SE_SCROLL_Y + SE_SCROLL_H &&
            viewLen() < _info.totalFrames) {
            _dragScroll = true;
            scrollTo(tx);
            return false;
        }

        // Zoom + spectrum buttons.
        int btnY = SE_ZOOM_Y + (SE_ZOOM_H - SE_BTN_H) / 2;
        if (ty >= btnY && ty < btnY + SE_BTN_H) {
            if (tx >= SE_ZOUT_X && tx < SE_ZOUT_X + SE_ZBTN_W)     zoom(-1);
            else if (tx >= SE_ZIN_X && tx < SE_ZIN_X + SE_ZBTN_W)  zoom(+1);
            else if (tx >= SE_SPEC_X && tx < SE_SPEC_X + SE_SPEC_W) {
                auditionStop();
                _specMode = 1;
                gServerPairing.clearSpec();
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_SPEC, _sampleId);
                _d.fillScreen(COL_BLACK);
                _d.setTextSize(3);
                _d.setTextColor(COL_WHITE);
                _d.setCursor(340, 260);
                _d.print("ANALYSING...");
                _d.paintLater();
            }
        }
    } else if (!down && _wasDown) {
        _wasDown = false;
        if (_backArmed) {
            _backArmed = false;
            auditionStop();
            return true;                           // back to the ColumnEditor
        }
    }
    return false;
}

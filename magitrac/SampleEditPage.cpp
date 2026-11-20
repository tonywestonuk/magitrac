#include "SampleEditPage.h"
#include "ServerPairing.h"
#include "UIHelpers.h"
#include "Constants.h"
#include "TrackerData.h"   // SFX_CHANNEL
#include "MagiLink.h"      // gMagiLink.isConnected() — gates the key lease
#include "SampleDebug.h"   // [SMPDBG] timing lines (off unless SAMPLE_DEBUG is 1)

static uint32_t sDbgOpenMs = 0;   // [SMPDBG] when open() started

void SampleEditPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

void SampleEditPage::open(uint8_t sampleId, const char* name, uint8_t col,
                          int16_t tuneCents) {
    _sampleId  = sampleId;
    _col       = col;
    _tuneCents = tuneCents;
    strncpy(_name, name ? name : "", sizeof(_name) - 1);
    _name[sizeof(_name) - 1] = '\0';
    _loading    = true;
    _playing    = false;
    _wasDown    = _touch.isTouched;
    _dragMarker = -1;
    _dragScroll = false;
    _dragWave   = false;
    _backArmed  = false;
    _specMode   = 0;       // a previous visit's spectrogram does not carry over
    _tuneMode   = false;
    _doneArmed  = false;
    _autoState  = 0;
    _levelDlg   = false;
    _lvlState   = 0;
    _level      = 0;       // until the server's meta arrives
    _deleting   = false;
    _deleted    = false;
    _viewStart  = 0;
    _viewEnd    = 0;   // whole file
    _sampleTune = 0;       // until the server's meta arrives
    _tuneHold.release();
    _tuneStep   = 1;

    sDbgOpenMs = millis();
    gServerPairing.clearSampleInfo();
    gServerPairing.sendSampleEdit(SAMPLE_EDIT_GET, _sampleId);
    if (standalone()) {
        // The MIDI-in keyboard now plays this sample (server side); poll()
        // keeps the lease alive.
        gServerPairing.sendSampleEdit(SAMPLE_EDIT_KEYS, _sampleId);
        _keysLastMs = millis();
    }
    uint32_t dbgSent = millis();

    _d.clear();                    // anti-ghost cycle only — doesn't touch the FB
    uint32_t dbgClear = millis();
    _d.fillScreen(COL_WHITE);      // ...so wipe the bitmap too
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(360, 260);
    _d.print("LOADING...");
    drawHeader();
    _d.paint();
    SMPDBG("editor open id=%u '%s': requests sent %lu ms, display clear %lu ms, "
           "LOADING paint %lu ms | open() total %lu ms", (unsigned)_sampleId, _name,
           (unsigned long)(dbgSent - sDbgOpenMs), (unsigned long)(dbgClear - dbgSent),
           (unsigned long)(millis() - dbgClear), (unsigned long)(millis() - sDbgOpenMs));
}

void SampleEditPage::adopt() {
    _info  = gServerPairing.sampleInfo();
    _start = _info.startFrame;
    _end   = _info.endFrame;          // 0 = end of file
    _loop  = _info.loop != 0;
    _velOff = _info.velOff != 0;
    _keepLen = _info.keepLen != 0;
    _sampleTune = _info.tuneCents;
    _level      = _info.levelDb10;
    if (_start >= _info.totalFrames) _start = 0;
    if (_end > _info.totalFrames)    _end   = 0;
    _loading = false;
}

void SampleEditPage::sendMeta() {
    gServerPairing.sendSampleEdit(SAMPLE_EDIT_SET, _sampleId,
                                  _start, _end, _loop ? 1 : 0, _sampleTune,
                                  60, _velOff ? 1 : 0, _keepLen ? 1 : 0, _level);
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
    if (_tuneMode) {
        const char* title = _levelDlg ? "LEVEL" : "TUNE";
        _d.setTextSize(3);
        _d.setTextColor(COL_WHITE);
        _d.setCursor(12, (SE_HDR_H - 24) / 2);
        _d.print(title);
        _d.setTextSize(2);
        _d.setTextColor(COL_LTGREY);
        _d.setCursor(12 + (int)strlen(title) * 18 + 18, (SE_HDR_H - 16) / 2);
        _d.print(_name);
        uiButton(_d, SE_PLAY_X, SE_BTN_Y, SE_PLAY_W, SE_BTN_H,
                 _playing ? "STOP" : "PLAY",
                 _playing ? COL_BLACK : COL_WHITE,
                 _playing ? COL_WHITE : COL_BLACK, 2);
        uiButton(_d, SE_BACK_X, SE_BTN_Y, SE_BACK_W, SE_BTN_H, "DONE",
                 COL_WHITE, COL_BLACK, 3);
        return;
    }
    // Big if it fits left of VEL, otherwise smaller (23 chars at size 2 do).
    int sz = ((int)strlen(_name) * 18 <= SE_NAME_MAXW) ? 3 : 2;
    _d.setTextSize(sz);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(12, (SE_HDR_H - 8 * sz) / 2);
    _d.print(_name);

    if (!_loading) {
        // VEL ON = keys follow velocity (normal, so drawn plain); VEL OFF =
        // always the sample's own level (the exception, so highlighted).
        uiButton(_d, SE_VEL_X, SE_BTN_Y, SE_VEL_W, SE_BTN_H,
                 _velOff ? "VEL OFF" : "VEL ON",
                 _velOff ? COL_BLACK : COL_WHITE,
                 _velOff ? COL_WHITE : COL_BLACK, 2);
        uiButton(_d, SE_LOOP_X, SE_BTN_Y, SE_LOOP_W, SE_BTN_H,
                 _loop ? "LOOP ON" : "LOOP OFF",
                 _loop ? COL_BLACK : COL_WHITE,
                 _loop ? COL_WHITE : COL_BLACK, 2);
        if (standalone())          // (from a column, the column still points at it)
            uiButton(_d, SE_DEL_X, SE_BTN_Y, SE_DEL_W, SE_BTN_H, "DELETE",
                     COL_WHITE, COL_BLACK, 2);
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

    // Each screen column maps to a frame, looked up in whatever window the
    // server last sent (_info.viewStart..viewEnd).  While drag-panning, the
    // view can slide beyond that window — unloaded regions draw as a flat
    // midline until the release-triggered fetch fills them in.
    uint64_t iVs = _info.viewStart, iVe = _info.viewEnd;
    bool haveData = (iVe > iVs);
    for (int c = 0; c < SAMPLE_OVERVIEW_N; c++) {
        uint64_t f = (uint64_t)_viewStart + ((uint64_t)c * len) / SAMPLE_OVERVIEW_N;
        bool have = false;
        int mn = 0, mx = 0;
        if (haveData && f >= iVs && f < iVe) {
            int sc = (int)(((f - iVs) * SAMPLE_OVERVIEW_N) / (iVe - iVs));
            if (sc >= 0 && sc < SAMPLE_OVERVIEW_N) {
                mn = _info.peaks[sc * 2];
                mx = _info.peaks[sc * 2 + 1];
                have = true;
            }
        }
        if (!have) {                               // not loaded yet: flat line
            _d.fillRect(SE_WAVE_X + c * 2, midY, 2, 1, COL_LTGREY);
            continue;
        }
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
    _d.fillRect(SE_WAVE_X, SE_TIME_Y, SE_TIME_W, 22, COL_WHITE);
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
    drawKeepLen();
}

void SampleEditPage::drawKeepLen() {
    _d.fillRect(SE_KEEP_HIT_X0, SE_KEEP_HIT_Y0, SE_KEEP_HIT_X1 - SE_KEEP_HIT_X0,
                SE_KEEP_HIT_Y1 - SE_KEEP_HIT_Y0 - 2, COL_WHITE);
    _d.drawRect(SE_KEEP_X, SE_KEEP_Y, SE_KEEP_BOX, SE_KEEP_BOX, COL_BLACK);
    _d.drawRect(SE_KEEP_X + 1, SE_KEEP_Y + 1, SE_KEEP_BOX - 2, SE_KEEP_BOX - 2, COL_BLACK);
    if (_keepLen)
        _d.fillRect(SE_KEEP_X + 6, SE_KEEP_Y + 6, SE_KEEP_BOX - 12, SE_KEEP_BOX - 12, COL_BLACK);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SE_KEEP_X + SE_KEEP_BOX + 10, SE_KEEP_Y + (SE_KEEP_BOX - 16) / 2);
    _d.print("KEEP LENGTH");
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
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(SE_ZIN_X + SE_ZBTN_W + 12, btnY + (SE_BTN_H - 16) / 2);
    _d.print(z);

    drawTune();
    drawLevel();

    uiButton(_d, SE_SPEC_X, btnY, SE_SPEC_W, SE_BTN_H, "SPECTRUM",
             COL_WHITE, COL_BLACK, 2);
}

// TUNE: the sample's own pitch offset, shown as semitones.cents ("+7.05").
static void fmtTune(int16_t t, char* v, int n) {
    int a = (t < 0) ? -t : t;
    snprintf(v, n, "%c%d.%02d", (t < 0) ? '-' : '+', a / 100, a % 100);
}

// LEVEL in dB, tenths shown: "+6.2".
static void fmtLevel(int16_t l, char* v, int n) {
    int a = (l < 0) ? -l : l;
    snprintf(v, n, "%c%d.%d", (l < 0) ? '-' : '+', a / 10, a % 10);
}

static void noteName(uint8_t note, char* out, int n) {
    static const char* NM[12] = { "C", "C#", "D", "D#", "E", "F",
                                  "F#", "G", "G#", "A", "A#", "B" };
    snprintf(out, n, "%s%d", NM[note % 12], (int)note / 12 - 1);
}

// The zoom row's [TUNE +7.05] button, or — in the dialog — the big value.
void SampleEditPage::drawTune() {
    char v[16];
    fmtTune(_sampleTune, v, sizeof(v));
    if (!_tuneMode) {
        int btnY = SE_ZOOM_Y + (SE_ZOOM_H - SE_BTN_H) / 2;
        char l[24];
        snprintf(l, sizeof(l), "TUNE %s", v);
        uiButton(_d, SE_TUNE_X, btnY, SE_TUNE_W, SE_BTN_H, l, COL_WHITE, COL_BLACK, 2);
        return;
    }
    if (_levelDlg) return;
    _d.fillRect(0, SE_TD_VAL_Y - 10, 960, 130, COL_WHITE);
    _d.setTextSize(8);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(480 - (int)strlen(v) * 48 / 2, SE_TD_VAL_Y);
    _d.print(v);
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    const char* cap = "SEMITONES . CENTS   (0 = as recorded)";
    _d.setCursor(480 - (int)strlen(cap) * 6, SE_TD_VAL_Y + 84);
    _d.print(cap);
}

// The zoom row's [LEVEL +6.2dB] button, or — in the LEVEL dialog — the big value.
void SampleEditPage::drawLevel() {
    char v[16];
    fmtLevel(_level, v, sizeof(v));
    if (!_tuneMode) {
        int btnY = SE_ZOOM_Y + (SE_ZOOM_H - SE_BTN_H) / 2;
        char l[24];
        snprintf(l, sizeof(l), "LEVEL %sdB", v);
        uiButton(_d, SE_LVL_X, btnY, SE_LVL_W, SE_BTN_H, l, COL_WHITE, COL_BLACK, 2);
        return;
    }
    if (!_levelDlg) return;
    strncat(v, " dB", sizeof(v) - strlen(v) - 1);
    _d.fillRect(0, SE_TD_VAL_Y - 10, 960, 130, COL_WHITE);
    _d.setTextSize(8);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(480 - (int)strlen(v) * 48 / 2, SE_TD_VAL_Y);
    _d.print(v);
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    const char* cap = "LOUDER / QUIETER, IN DECIBELS   (0 = as recorded)";
    _d.setCursor(480 - (int)strlen(cap) * 6, SE_TD_VAL_Y + 84);
    _d.print(cap);
}

void SampleEditPage::drawAutoStatus() {
    if (_levelDlg) {
        bool busy = _lvlState == 1;
        uiButton(_d, SE_TD_AUTO_X, SE_TD_AUTO_Y, SE_TD_AUTO_W, SE_TD_AUTO_H,
                 busy ? "MEASURING..." : "AUTO LEVEL",
                 COL_WHITE, busy ? COL_DKGREY : COL_BLACK, 3);
        char m[96];
        switch (_lvlState) {
            case 1:  snprintf(m, sizeof(m), "Finding its loudest point..."); break;
            case 2:  if (_level >= SAMPLE_LEVEL_MAX_DB10)
                         snprintf(m, sizeof(m), "Very quiet (loudest %u%%) - boosted the most it can",
                                  (unsigned)_lvlPeakPct);
                     else
                         snprintf(m, sizeof(m), "Loudest point was %u%% - now %d%%",
                                  (unsigned)_lvlPeakPct, SAMPLE_LEVEL_TARGET_PCT);
                     break;
            case 3:  snprintf(m, sizeof(m), "It's silent - level left as it was"); break;
            case 4:  snprintf(m, sizeof(m), "Couldn't read the sample - try again"); break;
            case 5:  snprintf(m, sizeof(m), "No answer from the server"); break;
            default: snprintf(m, sizeof(m), "AUTO LEVEL sets its loudest point to %d%%",
                              SAMPLE_LEVEL_TARGET_PCT); break;
        }
        _d.fillRect(0, SE_TD_MSG_Y - 8, 960, 40, COL_WHITE);
        _d.setTextSize(2);
        _d.setTextColor(_lvlState == 0 ? COL_DKGREY : COL_BLACK);
        _d.setCursor(480 - (int)strlen(m) * 6, SE_TD_MSG_Y);
        _d.print(m);
        return;
    }
    bool asking = _autoState == 1;
    uiButton(_d, SE_TD_AUTO_X, SE_TD_AUTO_Y, SE_TD_AUTO_W, SE_TD_AUTO_H,
             asking ? "LISTENING..." : "AUTO TUNE",
             COL_WHITE, asking ? COL_DKGREY : COL_BLACK, 3);
    char m[96], nn[8];
    switch (_autoState) {
        case 1:  snprintf(m, sizeof(m), "Finding its pitch..."); break;
        case 2:  noteName(_autoNote, nn, sizeof(nn));
                 snprintf(m, sizeof(m), "It is %s %+d cents - tuned so C4 plays middle C",
                          nn, (int)_autoCents); break;
        case 3:  snprintf(m, sizeof(m), "No clear pitch - tuning left as it was"); break;
        case 4:  snprintf(m, sizeof(m), "Couldn't read the sample - try again"); break;
        case 5:  snprintf(m, sizeof(m), "No answer from the server"); break;
        default: snprintf(m, sizeof(m), "AUTO TUNE finds its pitch and tunes it to middle C"); break;
    }
    _d.fillRect(0, SE_TD_MSG_Y - 8, 960, 40, COL_WHITE);
    _d.setTextSize(2);
    _d.setTextColor(_autoState == 0 ? COL_DKGREY : COL_BLACK);
    _d.setCursor(480 - (int)strlen(m) * 6, SE_TD_MSG_Y);
    _d.print(m);
}

void SampleEditPage::drawTuneDialog() {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    drawTune();
    drawLevel();
    static const char* TLBL[5] = { "-ST", "-", "+", "+ST", "ZERO" };
    static const char* LLBL[5] = { "-1dB", "-", "+", "+1dB", "ZERO" };
    const char* const* lbl = _levelDlg ? LLBL : TLBL;
    for (int i = 0; i < 5; i++)
        uiButton(_d, SE_TD_BTN_X0 + i * SE_TD_BTN_STEP, SE_TD_BTN_Y, SE_TD_BTN_W, SE_TD_BTN_H,
                 lbl[i], COL_WHITE, COL_BLACK, (i == 1 || i == 2) ? 4 : 3);
    drawAutoStatus();
}

// Repaint only — the value goes to the server when the finger lifts
// (sendMeta), so a held button doesn't send a message per step.
void SampleEditPage::stepTune(int cents) {
    int v = (int)_sampleTune + cents;
    if (v < -4800) v = -4800;
    if (v >  4800) v =  4800;
    if (v == _sampleTune) return;
    _sampleTune = (int16_t)v;
    drawTune();
    _d.paintLater();
}

void SampleEditPage::stepLevel(int db10) {
    int v = (int)_level + db10;
    if (v < SAMPLE_LEVEL_MIN_DB10) v = SAMPLE_LEVEL_MIN_DB10;
    if (v > SAMPLE_LEVEL_MAX_DB10) v = SAMPLE_LEVEL_MAX_DB10;
    if (v == _level) return;
    _level = (int16_t)v;
    drawLevel();
    _d.paintLater();
}

// The dialog's −/+: `big` = a semitone / a dB, else _tuneStep cents / tenths.
void SampleEditPage::stepValue(int dir, bool big) {
    if (_levelDlg) stepLevel(dir * (big ? 10 : _tuneStep));
    else           stepTune(dir * (big ? 100 : _tuneStep));
}

void SampleEditPage::drawDeleting(const char* line) {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(480 - (int)strlen(line) * 9, 260);
    _d.print(line);
    _d.paintLater();
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

void SampleEditPage::startAudition() {
    _playing = true;
    // Predict the end so the button resets itself: trimmed length at the
    // audition's playback speed (TUNE), plus a little margin for the wire +
    // the release fade.
    uint32_t endF   = _end ? _end : _info.totalFrames;
    uint32_t frames = (endF > _start) ? endF - _start : 0;
    uint32_t rate   = _info.rate ? _info.rate : 32000;
    float    spd    = _keepLen ? 1.0f      // KEEP LENGTH: same length at any pitch
                     : exp2f((float)(_tuneCents + _sampleTune) / 1200.0f);
    uint32_t ms     = (uint32_t)((float)frames * 1000.0f / ((float)rate * spd));
    _playEndMs = millis() + ms + 400;
    if (standalone())      // the bare sample, through its own TUNE
        gServerPairing.sendSampleEdit(SAMPLE_EDIT_PLAY, _sampleId);
    else                   // through the column's settings
        gServerPairing.sendAuditionRawNote(SFX_CHANNEL, 60, 100, _col);
}

// The TUNE dialog's touches.  The value is stored (sendMeta) when a −/+ is
// let go, on ZERO, and when AUTO TUNE's answer arrives.
bool SampleEditPage::pollTuneDialog(bool down, int tx, int ty) {
    if (down && !_wasDown) {
        _wasDown = true;
        if (ty < SE_HDR_H) {
            if (tx >= SE_BACK_X) { _doneArmed = true; return false; }   // on release
            if (tx >= SE_PLAY_X && tx < SE_PLAY_X + SE_PLAY_W) {
                if (_playing) auditionStop();
                else          startAudition();
                drawHeader();
                _d.paintLater();
            }
            return false;
        }
        if (ty >= SE_TD_BTN_Y && ty < SE_TD_BTN_Y + SE_TD_BTN_H && tx >= SE_TD_BTN_X0) {
            int i   = (tx - SE_TD_BTN_X0) / SE_TD_BTN_STEP;
            int off = (tx - SE_TD_BTN_X0) % SE_TD_BTN_STEP;
            if (i > 4 || off >= SE_TD_BTN_W) return false;
            _tuneStep = 1;
            switch (i) {
                case 0: stepValue(-1, true);  _tuneHold.start(1, -1); break;   // a semitone / dB
                case 1: stepValue(-1, false); _tuneHold.start(0, -1); break;   // a cent / tenth,
                case 2: stepValue(+1, false); _tuneHold.start(0, +1); break;   // faster held
                case 3: stepValue(+1, true);  _tuneHold.start(1, +1); break;
                case 4:
                    if (_levelDlg ? _level != 0 : _sampleTune != 0) {
                        if (_levelDlg) _level = 0; else _sampleTune = 0;
                        sendMeta();
                        drawTune();
                        drawLevel();
                        _d.paintLater();
                    }
                    break;
            }
            return false;
        }
        bool onAuto = tx >= SE_TD_AUTO_X && tx < SE_TD_AUTO_X + SE_TD_AUTO_W &&
                      ty >= SE_TD_AUTO_Y && ty < SE_TD_AUTO_Y + SE_TD_AUTO_H;
        if (onAuto && _levelDlg && _lvlState != 1) {
            MsgSampleResult stale;
            gServerPairing.takeSampleResult(stale);
            _lvlState  = 1;
            _lvlSentMs = millis();
            gServerPairing.sendSampleEdit(SAMPLE_EDIT_AUTOLEVEL, _sampleId);
            drawAutoStatus();
            _d.paintLater();
        } else if (onAuto && !_levelDlg && _autoState != 1) {
            MsgSampleTune stale;
            gServerPairing.takeSampleTune(stale);
            _autoState  = 1;
            _autoSentMs = millis();
            gServerPairing.sendSampleEdit(SAMPLE_EDIT_AUTOTUNE, _sampleId);
            drawAutoStatus();
            _d.paintLater();
        }
    } else if (!down && _wasDown) {
        _wasDown = false;
        if (_tuneHold.active()) {
            _tuneHold.release();
            sendMeta();
        }
        if (_doneArmed) {
            _doneArmed = false;
            _tuneMode  = false;
            _levelDlg  = false;
            _d.clear();
            draw();
            _d.paintLater();
        }
    }
    return false;
}

// AUTO TUNE's and AUTO LEVEL's answers: applied (and stored) even if the
// dialog was left.
void SampleEditPage::pollAnswers() {
    if (_autoState == 1) {
        MsgSampleTune at;
        bool got = gServerPairing.takeSampleTune(at) && at.sampleId == _sampleId;
        if (got || (uint32_t)(millis() - _autoSentMs) > SE_AUTO_TIMEOUT_MS) {
            if (!got) {
                _autoState = 5;
            } else if (at.result == SAMPLE_TUNE_FOUND) {
                _autoState  = 2;
                _autoNote   = at.note;
                _autoCents  = at.cents;
                _sampleTune = at.tuneCents;
                sendMeta();
            } else {
                _autoState = (at.result == SAMPLE_TUNE_UNCLEAR) ? 3 : 4;
            }
            if (_tuneMode) {
                drawTune();
                drawAutoStatus();
                _d.paintLater();
            } else if (_specMode == 0 && !_loading) {
                drawTune();
                _d.paintLater();
            }
        }
    }


    if (_lvlState == 1 && !_deleting) {
        MsgSampleResult r;
        bool got = gServerPairing.takeSampleResult(r) && r.sampleId == _sampleId &&
                   r.op == SAMPLE_EDIT_AUTOLEVEL;
        if (got || (uint32_t)(millis() - _lvlSentMs) > SE_LEVEL_TIMEOUT_MS) {
            if (!got) {
                _lvlState = 5;
            } else if (r.result == SAMPLE_RES_OK) {
                _lvlState   = 2;
                _lvlPeakPct = r.peakPct;
                _level      = r.levelDb10;
                sendMeta();
            } else {
                _lvlState = (r.result == SAMPLE_RES_SILENT) ? 3 : 4;
            }
            if (_tuneMode && _levelDlg) {
                drawLevel();
                drawAutoStatus();
                _d.paintLater();
            } else if (!_tuneMode && _specMode == 0 && !_loading) {
                drawLevel();
                _d.paintLater();
            }
        }
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
        // Sync the view to the echoed window (server may have clamped) —
        // unless the user is already panning again, in which case the
        // window-mapped renderer just uses the new data where it overlaps.
        if (!_dragWave && !_dragScroll) {
            _viewStart = _info.viewStart;
            _viewEnd   = _info.viewEnd;
        }
        uint32_t dbgDraw = millis();
        if (first) draw();
        else { drawWave(); drawScrollbar(); drawZoomRow(); }
        _d.paintLater();
        SMPDBG("editor: waveform (%s) noticed %lu ms after open(), drawn in %lu ms, "
               "paint queued", first ? "first" : "zoom/scroll",
               (unsigned long)(dbgDraw - sDbgOpenMs), (unsigned long)(millis() - dbgDraw));
    }

    // A non-looping audition ends on its own — flip STOP back to PLAY.
    // (Looping ones really do play until stopped, so they keep STOP.)
    if (_playing && !_loop && (int32_t)(millis() - _playEndMs) > 0) {
        _playing = false;
        if (_specMode == 0) {
            drawHeader();
            _d.paintLater();
        }
    }

    pollAnswers();

    // DELETE's answer: done = back to the squares (which fetch the list again).
    if (_deleting) {
        MsgSampleResult r;
        bool got = gServerPairing.takeSampleResult(r) && r.sampleId == _sampleId &&
                   r.op == SAMPLE_EDIT_DELETE;
        if (got && r.result == SAMPLE_RES_OK) {
            _deleting = false;
            _deleted  = true;
            gServerPairing.sendSampleEdit(SAMPLE_EDIT_KEYS, 0);   // (standalone only)
            return true;
        }
        if (got || (uint32_t)(millis() - _delSentMs) > SE_DELETE_TIMEOUT_MS) {
            _deleting = false;
            _d.clear();
            draw();
            _d.fillRect(SE_WAVE_X, SE_TIME_Y, SE_TIME_W, 22, COL_WHITE);
            _d.setTextSize(2);
            _d.setTextColor(COL_BLACK);
            _d.setCursor(SE_WAVE_X, SE_TIME_Y);
            _d.print(got ? "Couldn't delete it - the card wouldn't let go"
                         : "No answer from the server - not deleted");
            _d.paintLater();
        }
        _touch.read();                  // touches wait until it's answered
        _wasDown = _touch.isTouched;
        return false;
    }

    // Spectrogram popup: render when the pages have all arrived; any touch
    // returns to the editor.
    if (_specMode == 1 && gServerPairing.specComplete()) {
        drawSpectrogram();
        _d.paintLater();
        _specMode = 2;
    }

    // Keep the server's "MIDI keys play this sample" lease alive.  Skipped
    // while the link is down: a send into a dead link blocks for seconds.
    if (standalone() &&
        (uint32_t)(millis() - _keysLastMs) >= SAMPLE_KEYS_RENEW_MS) {
        _keysLastMs = millis();
        if (gMagiLink.isConnected())
            gServerPairing.sendSampleEdit(SAMPLE_EDIT_KEYS, _sampleId);
    }

    // DELETE's YES / NO owns the touches while it's up.
    if (_confirm.isOpen()) {
        if (_confirm.poll()) {
            if (_confirm.confirmed()) {
                MsgSampleResult stale;
                gServerPairing.takeSampleResult(stale);
                _lvlState  = 0;          // (an AUTO LEVEL in flight is moot)
                _deleting  = true;
                _delSentMs = millis();
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_DELETE, _sampleId);
                char l[48];
                snprintf(l, sizeof(l), "DELETING %s...", _name);
                drawDeleting(l);
            } else {
                _d.clear();
                draw();
                _d.paintLater();
            }
            _wasDown = _touch.isTouched;
        }
        return false;
    }

    // read() is true only on a state change; isTouched holds the last-seen
    // state either way, which is what the hold-repeat needs.
    bool ev   = _touch.read();
    bool down = _touch.isTouched;

    // TUNE −/+ held: repeat, accelerating to a semitone per step; −ST/+ST
    // held: a semitone per step.  Gated on the finger still being down so a
    // stalled loop can't run on after it.
    if (down && _tuneHold.active() && _tuneHold.tickFast()) {
        if (_tuneHold.field() == 1) {
            stepValue(_tuneHold.delta(), true);
        } else {
            int cap = _levelDlg ? 10 : 100;
            _tuneStep += _tuneStep / 4 + 1;
            if (_tuneStep > cap) _tuneStep = cap;
            stepValue(_tuneHold.delta(), false);
        }
    }

    if (!ev) return false;              // no new touch event this frame
    int  tx, ty;
    rawToScreen(_touch.x, _touch.y, tx, ty);

    if (_tuneMode) return pollTuneDialog(down, tx, ty);

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

    // ── Waveform pan in progress: content follows the finger ─────────────────
    if (_dragWave) {
        if (down) {
            uint32_t len   = viewLen();
            uint32_t total = _info.totalFrames;
            int64_t dFrames = ((int64_t)(_dragAnchorX - tx) * (int64_t)len) / SE_WAVE_W;
            int64_t vs = (int64_t)_dragAnchorVs + dFrames;
            if (vs < 0) vs = 0;
            if ((uint64_t)vs + len > total) vs = (int64_t)total - (int64_t)len;
            if ((uint32_t)vs != _viewStart) {
                _viewStart = (uint32_t)vs;
                _viewEnd   = _viewStart + len;
                drawWave();             // unloaded regions show as a flat line
                drawScrollbar();
                _d.paintLater();
            }
        } else {
            _dragWave = false;          // released: fill in the revealed region
            _wasDown  = false;
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
            if (tx >= SE_VEL_X && tx < SE_VEL_X + SE_VEL_W) {
                _velOff = !_velOff;
                sendMeta();
                drawHeader();
                _d.paintLater();
            } else if (tx >= SE_LOOP_X && tx < SE_LOOP_X + SE_LOOP_W) {
                _loop = !_loop;
                sendMeta();
                drawHeader();
                _d.paintLater();
            } else if (tx >= SE_PLAY_X && tx < SE_PLAY_X + SE_PLAY_W) {
                if (_playing) auditionStop();
                else          startAudition();
                drawHeader();
                _d.paintLater();
            } else if (standalone() && tx >= SE_DEL_X && tx < SE_DEL_X + SE_DEL_W) {
                auditionStop();
                char m[64];
                if (strlen(_name) <= 20) snprintf(m, sizeof(m), "Delete %s?", _name);
                else                     snprintf(m, sizeof(m), "Delete this sample?");
                _confirm.open(m);        // (it swallows this press's lift)
                _confirm.draw();
                _d.paintLater();
            }
            return false;
        }
        if (_loading) return false;

        if (tx >= SE_KEEP_HIT_X0 && tx < SE_KEEP_HIT_X1 &&
            ty >= SE_KEEP_HIT_Y0 && ty < SE_KEEP_HIT_Y1) {
            _keepLen = !_keepLen;
            sendMeta();
            drawKeepLen();
            _d.paintLater();
            return false;
        }

        // Touch-down near a marker line grabs it; it then follows the drag
        // and locks in (meta stored) on release.  Away from the markers (and
        // zoomed in), the same touch pans the view instead.
        if (ty >= SE_WAVE_Y - 20 && ty < SE_WAVE_Y + SE_WAVE_H + 20 &&
            tx >= SE_WAVE_X - 20 && tx < SE_WAVE_X + SE_WAVE_W + 20) {
            int dS = tx - markerX(0); if (dS < 0) dS = -dS;
            int dE = tx - markerX(1); if (dE < 0) dE = -dE;
            const int GRAB = 60;                   // finger-sized capture zone
            if (dS <= dE && dS <= GRAB)      _dragMarker = 0;
            else if (dE < dS && dE <= GRAB)  _dragMarker = 1;
            else if (viewLen() < _info.totalFrames) {
                _dragWave     = true;              // pan the zoomed view
                _dragAnchorX  = tx;
                _dragAnchorVs = _viewStart;
            }
            return false;
        }

        // Scrollbar: start dragging the window (only useful when zoomed).
        if (ty >= SE_SCROLL_Y && ty < SE_SCROLL_Y + SE_SCROLL_H &&
            viewLen() < _info.totalFrames) {
            _dragScroll = true;
            scrollTo(tx);
            return false;
        }

        // Zoom, tune + spectrum buttons.
        int btnY = SE_ZOOM_Y + (SE_ZOOM_H - SE_BTN_H) / 2;
        if (ty >= btnY && ty < btnY + SE_BTN_H) {
            if (tx >= SE_ZOUT_X && tx < SE_ZOUT_X + SE_ZBTN_W)     zoom(-1);
            else if (tx >= SE_ZIN_X && tx < SE_ZIN_X + SE_ZBTN_W)  zoom(+1);
            else if ((tx >= SE_TUNE_X && tx < SE_TUNE_X + SE_TUNE_W) ||
                     (tx >= SE_LVL_X && tx < SE_LVL_X + SE_LVL_W)) {
                _levelDlg  = tx >= SE_LVL_X;    // the LEVEL dialog, or TUNE's
                _tuneMode  = true;
                _doneArmed = false;
                if (_autoState != 1) _autoState = 0;
                if (_lvlState != 1) _lvlState = 0;
                _d.clear();
                drawTuneDialog();
                _d.paintLater();
            }
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
        if (_tuneHold.active()) {
            _tuneHold.release();
            sendMeta();                            // store the tuned value
        }
        if (_backArmed) {
            _backArmed = false;
            auditionStop();
            if (standalone())                      // keys go back to normal
                gServerPairing.sendSampleEdit(SAMPLE_EDIT_KEYS, 0);
            return true;                           // caller picks where to
        }
    }
    return false;
}

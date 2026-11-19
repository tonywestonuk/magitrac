#include "SampleRecordPage.h"
#include "ServerPairing.h"
#include "UIHelpers.h"
#include "Constants.h"
#include "MagiLink.h"
#include <string.h>

static const uint32_t SRC_REPLY_MS = 4000;   // a request with no answer by then failed

// Meter: the level bar and the seconds under it.
static const int MTR_X = 80, MTR_Y = 200, MTR_W = 800, MTR_H = 60;
static const int MTR_TIME_Y = MTR_Y + MTR_H + 24;
static const int MTR_AREA_H = MTR_TIME_Y + 40 - MTR_Y;

// Status lines.
static const int ST_TITLE_Y = SRC_HDR_H + 34;
static const int ST_MSG_Y   = ST_TITLE_Y + 50;
static const int ST_AREA_Y  = SRC_HDR_H;
static const int ST_AREA_H  = MTR_Y - 16 - SRC_HDR_H;

// Button rows: up to four slots across the bottom.
static const int BTN4_W = 210, BTN4_GAP = 20;
static const int BTN4_X0 = (960 - (4 * BTN4_W + 3 * BTN4_GAP)) / 2;
static const int BTN1_W = 400, BTN1_X = (960 - BTN1_W) / 2;

void SampleRecordPage::open() {
    _st        = St::IDLE;
    _wasDown   = _touch.isTouched;
    _backArmed = false;
    _frames    = 0;
    _peak      = 0;
    _clipped   = false;
    _savedId   = 0;
    _leave     = false;
    _name[0]   = '\0';
    _msg[0]    = '\0';
    MsgSampleRecStatus stale;
    while (gServerPairing.takeSampleRecStatus(stale)) {}   // from a previous visit
    _d.clear();
    draw();
    request(SAMPLE_REC_ARM, St::ARMING);     // the server puts up its hold screen
    _d.paint();
}

// ── Drawing ──────────────────────────────────────────────────────────────────

void SampleRecordPage::drawHeader() {
    _d.fillRect(0, 0, 960, SRC_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(16, (SRC_HDR_H - 24) / 2);
    _d.print("ADD SAMPLE");
    uiButton(_d, SRC_BACK_X, 8, SRC_BACK_W, 44, "BACK", COL_WHITE, COL_BLACK, 3);
}

static void centred(EPD_PainterAdafruit& d, const char* s, int y, int ts) {
    d.setTextSize(ts);
    d.setCursor((960 - (int)strlen(s) * 6 * ts) / 2, y);
    d.print(s);
}

void SampleRecordPage::drawStatus() {
    _d.fillRect(0, ST_AREA_Y, 960, ST_AREA_H, COL_WHITE);
    const char* title = "";
    const char* sub   = "";
    switch (_st) {
        case St::IDLE:      title = "Not recording";
                            sub   = "TRY AGAIN to record";                 break;
        case St::ARMING:    title = "Asking the server...";                break;
        case St::ARMED:     title = "Press and hold the server's screen";
                            sub   = "It records while you hold it - up to 30 seconds"; break;
        case St::RECORDING: title = "Recording";
                            sub   = "Let go of the server's screen to finish"; break;
        case St::REVIEW:    title = "Listen on the server";
                            sub   = "Your keys play it. KEEP, CANCEL or RESAMPLE on its screen"; break;
        case St::TAKE:      title = "Got it";
                            sub   = _clipped ? "It CLIPPED - too loud. Try AGAIN further from the mic"
                                             : "PLAY to listen, AGAIN to redo, KEEP to save it"; break;
        case St::SAVING:    title = "Saving...";                           break;
        case St::NAMING:    break;
    }
    _d.setTextColor(COL_BLACK);
    centred(_d, title, ST_TITLE_Y, 4);
    // An error replaces the hint.
    _d.setTextColor(_msg[0] ? COL_BLACK : COL_DKGREY);
    centred(_d, _msg[0] ? _msg : sub, ST_MSG_Y, 2);
}

void SampleRecordPage::drawMeter() {
    _d.fillRect(0, MTR_Y - 4, 960, MTR_AREA_H + 8, COL_WHITE);
    bool live = (_st == St::RECORDING || _st == St::REVIEW || _st == St::TAKE);
    _d.drawRect(MTR_X,     MTR_Y,     MTR_W,     MTR_H,     COL_BLACK);
    _d.drawRect(MTR_X + 1, MTR_Y + 1, MTR_W - 2, MTR_H - 2, COL_BLACK);
    if (live && _peak) {
        int w = (MTR_W - 8) * (_peak > 100 ? 100 : _peak) / 100;
        _d.fillRect(MTR_X + 4, MTR_Y + 4, w, MTR_H - 8,
                    (_clipped || _peak >= 99) ? COL_BLACK : COL_DKGREY);
    }
    char t[40];
    if (live) {
        float secs = _rate ? (float)_frames / _rate : 0;
        snprintf(t, sizeof(t), "%.1f s of %d%s", secs, SAMPLE_REC_MAX_SECONDS,
                 _clipped ? "   CLIPPED" : "");
    } else {
        snprintf(t, sizeof(t), "up to %d s", SAMPLE_REC_MAX_SECONDS);
    }
    _d.setTextColor(COL_BLACK);
    centred(_d, t, MTR_TIME_Y, 3);
}

// The labels of the current button row; none while waiting on the server.
int SampleRecordPage::rowLabels(const char* out[4]) const {
    switch (_st) {
        case St::IDLE:      out[0] = "TRY AGAIN"; return 1;
        case St::TAKE:      out[0] = "PLAY"; out[1] = "STOP"; out[2] = "AGAIN"; out[3] = "KEEP"; return 4;
        default:            return 0;
    }
}

void SampleRecordPage::drawButtons() {
    _d.fillRect(0, SRC_BTN_Y - 4, 960, SRC_BTN_H + 8, COL_WHITE);
    const char* lab[4];
    int n = rowLabels(lab);
    if (n == 1) {
        uiButton(_d, BTN1_X, SRC_BTN_Y, BTN1_W, SRC_BTN_H, lab[0], COL_BLACK, COL_WHITE, 4);
    } else {
        for (int i = 0; i < n; i++)
            uiButton(_d, BTN4_X0 + i * (BTN4_W + BTN4_GAP), SRC_BTN_Y, BTN4_W, SRC_BTN_H,
                     lab[i], i == 3 ? COL_BLACK : COL_WHITE, i == 3 ? COL_WHITE : COL_BLACK, 4);
    }
}

void SampleRecordPage::draw() {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    drawStatus();
    drawMeter();
    drawButtons();
}

int SampleRecordPage::buttonAt(int sx, int sy) const {
    if (sy < SRC_BTN_Y || sy >= SRC_BTN_Y + SRC_BTN_H) return -1;
    const char* lab[4];
    int n = rowLabels(lab);
    if (n == 1) return (sx >= BTN1_X && sx < BTN1_X + BTN1_W) ? 0 : -1;
    for (int i = 0; i < n; i++) {
        int x = BTN4_X0 + i * (BTN4_W + BTN4_GAP);
        if (sx >= x && sx < x + BTN4_W) return i;
    }
    return -1;
}

// ── Requests and replies ─────────────────────────────────────────────────────

void SampleRecordPage::request(uint8_t op, St waitState, const char* name) {
    _msg[0] = '\0';
    if (!gMagiLink.isConnected() || !gServerPairing.sendSampleRec(op, name)) {
        strcpy(_msg, "Not connected to the server");
        drawStatus();
        _d.paintLater();
        return;
    }
    _st     = waitState;
    _waitMs = millis();
    draw();
    _d.paintLater();
}

static const char* errText(uint8_t err) {
    switch (err) {
        case SAMPLE_REC_ERR_SONG:   return "Stop the song first";
        case SAMPLE_REC_ERR_ORGAN:  return "Close the organ first";
        case SAMPLE_REC_ERR_FULL:   return "No room - the sample list is full";
        case SAMPLE_REC_ERR_MEMORY: return "The server is out of memory - try again";
        case SAMPLE_REC_ERR_SHORT:  return "Too short - hold the server's screen while it plays";
        case SAMPLE_REC_ERR_SD:     return "SD card error on the server";
        case SAMPLE_REC_ERR_NAME:   return "That name is taken - KEEP again for another";
        case SAMPLE_REC_ERR_CANCEL: return "Cancelled on the server";
    }
    return "";
}

// The server's pitch analysis of the take, for the message line.
static void pitchText(const MsgSampleRecStatus& s, char* out, size_t len) {
    static const char* const NAMES[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    out[0] = '\0';
    if (s.pitchState == SAMPLE_REC_PITCH_FOUND)
        snprintf(out, len, "%s%d %+d cents, tuned to middle C - your keys play it",
                 NAMES[s.pitchNote % 12], s.pitchNote / 12 - 1, (int)s.pitchCents);
    else if (s.pitchState == SAMPLE_REC_PITCH_UNCLEAR)
        snprintf(out, len, "No clear pitch, left as recorded - your keys play it");
}

void SampleRecordPage::handleStatus(const MsgSampleRecStatus& s) {
    if (s.rate) _rate = s.rate;

    if (s.state == SAMPLE_REC_SAVED) {
        if (_st != St::SAVING) return;
        if (s.err == SAMPLE_REC_OK && s.sampleId) {
            _savedId = s.sampleId;
            return;                         // poll() hands over to the editor
        }
        // Saved but the manifest has no id for it: it is in the list, just
        // not openable from here.
        _st = St::IDLE;
        strcpy(_msg, "Saved - find it on the samples page");
        draw();
        _d.paintLater();
        return;
    }

    if (s.state == SAMPLE_REC_ARMED) {
        // Armed, or a hold too short to keep (still armed).
        bool redraw = (_st != St::ARMED) || s.err;
        _st = St::ARMED;
        _frames = 0; _peak = 0; _clipped = false;
        if (s.err) strncpy(_msg, errText(s.err), sizeof(_msg) - 1);
        else if (redraw) _msg[0] = '\0';
        if (redraw) { draw(); _d.paintLater(); }
        return;
    }

    if (s.state == SAMPLE_REC_RECORDING) {
        bool first = (_st != St::RECORDING);
        _st      = St::RECORDING;
        _frames  = s.frames;
        _peak    = s.peak;
        _clipped = s.clipped;
        if (first) { _msg[0] = '\0'; draw(); }
        else       drawMeter();
        _d.paintLater();
        return;
    }

    if (s.state == SAMPLE_REC_REVIEW) {
        // The server is playing the take back (or a RESAMPLE was too short),
        // and reports its pitch once found.
        _st      = St::REVIEW;
        _frames  = s.frames;
        _peak    = s.peak;
        _clipped = s.clipped;
        pitchText(s, _msg, sizeof(_msg));
        draw();
        _d.paintLater();
        return;
    }

    if (s.state == SAMPLE_REC_TAKE) {
        if (_st == St::REVIEW && !s.err) {
            // KEEP on the server: straight on to naming it.
            _frames  = s.frames;
            _peak    = s.peak;
            _clipped = s.clipped;
            _msg[0]  = '\0';
            _d.clear();
            keepTake();
            return;
        }
        // A request the server refused (still holding the take).
        _st      = St::TAKE;
        _frames  = s.frames;
        _peak    = s.peak;
        _clipped = s.clipped;
        if (s.err) strncpy(_msg, errText(s.err), sizeof(_msg) - 1);
        else       _msg[0] = '\0';
        draw();
        _d.paintLater();
        return;
    }

    // IDLE: refused, cancelled on the server, discarded.
    if (s.err == SAMPLE_REC_ERR_CANCEL) { _leave = true; return; }
    if (_st == St::IDLE && !s.err) return;
    _st = St::IDLE;
    _frames = 0; _peak = 0; _clipped = false;
    if (s.err) strncpy(_msg, errText(s.err), sizeof(_msg) - 1);
    draw();
    _d.paintLater();
}

// ── Naming ───────────────────────────────────────────────────────────────────

bool SampleRecordPage::nameTaken(const char* base) const {
    int n = gServerPairing.sampleListCount();
    for (int i = 0; i < n; i++) {
        const SampleCacheEntry* e = gServerPairing.sampleListEntry(i);
        if (!e) continue;
        size_t bl = strlen(base), el = strlen(e->name);
        // "<base>.wav", any case — the card's names aren't case-sensitive.
        if (el == bl + 4 && strncasecmp(e->name, base, bl) == 0 &&
            strcasecmp(e->name + bl, ".wav") == 0) return true;
    }
    return false;
}

void SampleRecordPage::defaultName() {
    for (int i = 1; i < 1000; i++) {
        snprintf(_name, sizeof(_name), "rec%02d", i);
        if (!nameTaken(_name)) return;
    }
}

void SampleRecordPage::keepTake() {
    if (!_name[0]) defaultName();
    _st = St::NAMING;
    _d.fillScreen(COL_WHITE);
    _keyboard.open(_name, sizeof(_name));
    _keyboard.draw();
    _d.paint();
}

// Same rule as the server's (sample_record.cpp makeFileName), so the name the
// editor shows is the file's.
static void sanitise(char* s) {
    for (char* c = s; *c; c++) {
        char ch = *c;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == ' ' || ch == '.';
        if (!ok) *c = '_';
    }
    size_t n = strlen(s);
    if (n >= 4 && strcasecmp(s + n - 4, ".wav") == 0) s[n -= 4] = '\0';
    while (n && (s[n - 1] == ' ' || s[n - 1] == '.')) s[--n] = '\0';
    size_t lead = strspn(s, " .");
    if (lead) memmove(s, s + lead, strlen(s + lead) + 1);
}

void SampleRecordPage::nameDone() {
    sanitise(_name);
    _st = St::TAKE;
    if (!_name[0]) {
        strcpy(_msg, "It needs a name - KEEP again");
    } else if (nameTaken(_name)) {
        snprintf(_msg, sizeof(_msg), "\"%s\" is taken - KEEP again", _name);
    } else {
        _d.clear();
        request(SAMPLE_REC_SAVE, St::SAVING, _name);
        return;
    }
    _d.clear();
    draw();
    _d.paint();
}

// ── Poll ─────────────────────────────────────────────────────────────────────

SampleRecordPage::Result SampleRecordPage::poll() {
    if (_st == St::NAMING) {
        if (_keyboard.poll()) {
            _wasDown = _touch.isTouched;
            if (_keyboard.isDone()) {
                nameDone();
            } else {
                _st = St::TAKE;
                _d.clear();
                draw();
                _d.paint();
            }
        }
        return Result::NONE;
    }

    MsgSampleRecStatus s;
    if (gServerPairing.takeSampleRecStatus(s)) handleStatus(s);
    if (_savedId) return Result::SAVED;
    if (_leave)   { _leave = false; return Result::BACK; }

    // A request the server never answered.
    bool waiting = (_st == St::ARMING || _st == St::SAVING);
    if (waiting && (uint32_t)(millis() - _waitMs) >= SRC_REPLY_MS) {
        _st = (_st == St::SAVING) ? St::TAKE : St::IDLE;
        strcpy(_msg, "No reply from the server");
        draw();
        _d.paintLater();
    }

    if (!_touch.read()) return Result::NONE;
    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (down && !_wasDown) {
        _wasDown   = true;
        _backArmed = (sy < SRC_HDR_H && sx >= SRC_BACK_X);
        int b = buttonAt(sx, sy);
        if (b < 0) return Result::NONE;
        switch (_st) {
            case St::IDLE:
                request(SAMPLE_REC_ARM, St::ARMING);
                break;
            case St::TAKE:
                if      (b == 0) gServerPairing.sendSampleRec(SAMPLE_REC_PLAY);
                else if (b == 1) gServerPairing.sendSampleRec(SAMPLE_REC_PLAY_STOP);
                else if (b == 2) request(SAMPLE_REC_ARM, St::ARMING);
                else             keepTake();
                break;
            default: break;
        }
    } else if (!down && _wasDown) {
        _wasDown = false;
        if (_backArmed && sy < SRC_HDR_H && sx >= SRC_BACK_X) {
            _backArmed = false;
            // Anything not kept goes, and the server's hold screen with it.
            // Sent even from IDLE: an ARM we gave up waiting on may yet land.
            gServerPairing.sendSampleRec(SAMPLE_REC_DISCARD);
            return Result::BACK;
        }
        _backArmed = false;
    }
    return Result::NONE;
}

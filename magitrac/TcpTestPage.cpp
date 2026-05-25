#include "TcpTestPage.h"
#include "Constants.h"
#include <WiFi.h>

extern ServerPairing gServerPairing;

// ── Layout ────────────────────────────────────────────────────────────────────
static const int TT_W       = 960;
static const int TT_H       = 540;
static const int TT_HDR_H   = 50;
static const int TT_EXIT_X  = 830;
static const int TT_EXIT_W  = 130;

// Repaint cadence — counters update once per second, not per blob.
static const uint32_t REPAINT_PERIOD_MS = 1000;

TcpTestPage::TcpTestPage(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display), _touch(touch),
      _state(TcpTestState::RUNNING), _wasDown(false),
      _framesInBurst(0), _burstNumber(0),
      _totalBytes(0), _totalFrames(0),
      _patternErrors(0), _wedgeCount(0),
      _waitingBlob(false), _requestSentMs(0),
      _lastBurstAtMs(0), _wedgeReported(false),
      _expected(0)
{}

void TcpTestPage::open() {
    _state         = TcpTestState::RUNNING;
    _wasDown       = _touch.isTouched;
    _totalFrames   = 0;
    _totalBytes    = 0;
    _lastBurstAtMs = millis();
    _requestSentMs = _lastBurstAtMs;  // re-used as "lastRepaint" timestamp

    gServerPairing.tcpTestResetCounters();
    if (gServerPairing.startTcpTest()) {
        Serial.println("[TCPTEST] START sent — server should begin streaming");
    } else {
        Serial.println("[TCPTEST] START send failed (not paired?)");
    }
}

void TcpTestPage::draw() {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    drawCounters();
    drawStatus();
    drawExitButton();
}

void TcpTestPage::drawHeader() {
    _d.fillRect(0, 0, TT_W, TT_HDR_H, COL_BLACK);
    _d.setTextColor(COL_WHITE);
    _d.setTextSize(3);
    _d.setCursor(20, 14);
    _d.print("TCP/IP TEST");
}

void TcpTestPage::drawCounters() {
    _d.setTextColor(COL_BLACK);
    _d.setTextSize(3);

    char line[80];
    int  y = TT_HDR_H + 30;

    snprintf(line, sizeof(line), "Blobs:  %u",
             (unsigned)gServerPairing.tcpTestBlobCount());
    _d.setCursor(40, y); _d.print(line); y += 60;

    snprintf(line, sizeof(line), "Bytes:  %llu",
             (unsigned long long)gServerPairing.tcpTestByteCount());
    _d.setCursor(40, y); _d.print(line); y += 60;

    snprintf(line, sizeof(line), "Rate:   %u KB/s",
             (unsigned)((_totalBytes > 0 && _lastBurstAtMs > 0)
                        ? (_totalBytes / 1024) /
                          ((millis() - _lastBurstAtMs) / 1000 + 1)
                        : 0));
    _d.setCursor(40, y); _d.print(line); y += 60;
}

void TcpTestPage::drawStatus() {
    _d.setTextColor(COL_BLACK);
    _d.setTextSize(2);
    char line[80];
    int  y = TT_HDR_H + 260;

    snprintf(line, sizeof(line), "Heap: %u   PSRAM: %u",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    _d.setCursor(40, y); _d.print(line); y += 30;

    snprintf(line, sizeof(line), "RSSI: %d dBm   WiFi: %d",
             (int)WiFi.RSSI(), (int)WiFi.status());
    _d.setCursor(40, y); _d.print(line); y += 30;

    snprintf(line, sizeof(line), "Paired/Connected: %s",
             gServerPairing.isPaired() ? "yes" : "no");
    _d.setCursor(40, y); _d.print(line);
}

void TcpTestPage::drawExitButton() {
    uiButton(_d, TT_EXIT_X, 0, TT_EXIT_W, TT_HDR_H, "EXIT",
             COL_BLACK, COL_WHITE, 3);
}

bool TcpTestPage::poll() {
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);
    bool down = _touch.isTouched;
    if (down && !_wasDown) {
        if (hitExit(sx, sy)) {
            gServerPairing.stopTcpTest();
            Serial.printf("[TCPTEST] EXIT — total blobs=%u bytes=%llu\n",
                          (unsigned)gServerPairing.tcpTestBlobCount(),
                          (unsigned long long)gServerPairing.tcpTestByteCount());
            _wasDown = down;
            return true;
        }
    }
    _wasDown = down;

    uint32_t now = millis();

    // Periodic repaint — once per second, not per blob.  Mirrors the
    // "minimal magitrac UI activity" we want during the test.
    if (now - _requestSentMs >= REPAINT_PERIOD_MS) {
        _requestSentMs = now;

        uint32_t blobs = gServerPairing.tcpTestBlobCount();
        uint64_t bytes = gServerPairing.tcpTestByteCount();
        uint64_t deltaBytes = bytes - _totalBytes;
        _totalBytes  = bytes;
        _totalFrames = blobs;

        Serial.printf("[TCPTEST] blobs=%u bytes=%llu (+%u this sec) "
                      "heap=%u rssi=%d wifiStatus=%d paired=%d\n",
                      (unsigned)blobs, (unsigned long long)bytes,
                      (unsigned)deltaBytes,
                      ESP.getFreeHeap(), WiFi.RSSI(),
                      (int)WiFi.status(),
                      gServerPairing.isPaired() ? 1 : 0);

        draw();
        _d.paint();
    }
    return false;
}

// Stubs — no longer used, kept so the header doesn't change too much.
void TcpTestPage::verifyBlob()      {}
void TcpTestPage::burstCompleted()  {}

bool TcpTestPage::hitExit(int sx, int sy) const {
    return (sx >= TT_EXIT_X && sx < TT_EXIT_X + TT_EXIT_W &&
            sy >= 0 && sy < TT_HDR_H);
}

void TcpTestPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

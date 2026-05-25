#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "UIHelpers.h"
#include "ServerPairing.h"

// ── TcpTestPage — diagnostic full-screen TCP/IP loop ──────────────────────────
//
// Mirrors the standalone wifi_repro client/server but runs inside the real
// magitrac runtime — so EPD background paint task, MIDI processing, touch
// polling, etc. are all alive while the test runs.  Sends a steady stream of
// MSG_TCP_TEST_REQUEST and receives MSG_TCP_TEST_BLOB responses (4096-byte
// incrementing-u8 payload).
//
// Repaints progress every BURST_SIZE frames (mirrors backup's 28-file
// granularity).  Logs to serial every burst too.  Exit button stops the test.
//
//  y=  0  ┌─ Header: "TCP/IP TEST"  [EXIT]
//  y= 60  ├─ Big counters: frames, bytes, errors
//  y=300  ├─ heap / RSSI / status
//  y=460  └─ EXIT button

enum class TcpTestState : uint8_t {
    RUNNING,     // sending requests / receiving blobs
    PAUSED,      // user tapped pause — kept around in case we want it later
    DONE,
};

class TcpTestPage {
public:
    TcpTestPage(EPD_PainterAdafruit& display, GT911_Lite& touch);

    void open();
    void draw();
    bool poll();   // returns true when page should close

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    TcpTestState         _state;
    bool                 _wasDown;

    // Per-test counters
    uint32_t _framesInBurst;
    uint32_t _burstNumber;
    uint64_t _totalBytes;
    uint32_t _totalFrames;
    uint32_t _patternErrors;
    uint32_t _wedgeCount;

    // Pacing / wedge detection
    bool     _waitingBlob;
    uint32_t _requestSentMs;
    uint32_t _lastBurstAtMs;
    bool     _wedgeReported;

    // Pattern verification
    uint8_t  _expected;

    // Inter-frame pacing — matches BackupRestorePage delay(80) exactly so
    // the test load matches a real backup.
    static const uint32_t INTER_FRAME_DELAY_MS = 80;
    static const uint32_t BURST_SIZE           = 28;
    static const uint32_t WEDGE_MS             = 5000;   // matches repro
    static const uint32_t REQUEST_TIMEOUT_MS   = 30000;  // matches repro

    void drawHeader();
    void drawCounters();
    void drawStatus();
    void drawExitButton();
    void verifyBlob();
    void burstCompleted();

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    bool hitExit(int sx, int sy) const;
};

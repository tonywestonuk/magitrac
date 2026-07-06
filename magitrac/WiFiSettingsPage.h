#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "KeyboardPopup.h"

// ── WiFiSettingsPage — user-managed WiFi credentials + apMode + channel ─────
//
//  y=  0  ┌─ Header: "WIFI" [BACK x=830,w=130]  (50px) ──────────────────────┐
//  y= 50  ├─ "MODE" label strip (20px) ───────────────────────────────────────┤
//  y= 70  ├─ Mode row: "MODE" [Server hosts AP] [External AP] (60px) ────────┤
//  y=130  ├─ "CREDENTIALS" label strip (20px) ────────────────────────────────┤
//  y=150  ├─ SSID row: "SSID" [value tap-to-edit] [SCAN] (60px) ─────────────┤
//  y=210  ├─ PSK row:  "PSK"  [value tap-to-edit]        (60px) ─────────────┤
//  y=270  ├─ "CHANNEL" label strip (20px) ─────────────────────────────────────┤
//  y=290  ├─ Channel row: "CHANNEL" [1] [6] [11]  (60px, Server-AP only) ────┤
//  y=350  ├─ "ACTIONS" label strip (20px) ─────────────────────────────────────┤
//  y=370  └─ Save (large button) ───────────────────────────────────────────────┘
//
// SCAN button: only enabled in External-AP mode (Server-AP picks its own
// SSID, no scan needed).  CHANNEL row: only enabled in Server-AP mode
// (channel is determined by the external AP otherwise).
//
// SCAN flow: WiFi.scanNetworks() runs synchronously (~2-5s blocking).
// Results displayed as a tappable list overlay; tap an entry to copy
// its SSID into the SSID field.  Back to return without selecting.

#define WP_SCAN_MAX 16

class WiFiSettingsPage {
public:
    WiFiSettingsPage(EPD_PainterAdafruit& display, GT911_Lite& touch);

    void open();         // load NVS values into edit buffers
    void draw();         // full repaint
    bool poll();         // returns true when BACK tapped (caller closes us)

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    KeyboardPopup        _keyboard;

    char    _ssid[33];
    char    _psk[64];
    uint8_t _apMode;     // 0 = SERVER_AP, 1 = EXTERNAL_AP
    uint8_t _channelIdx; // 0/1/2 → 1/6/11 (only meaningful in SERVER_AP)

    bool    _wasDown;
    uint8_t _editing;        // 0 = none, 1 = ssid, 2 = psk
    bool    _dirty;          // edit happened — needs save+apply

    // Scan results overlay state.
    bool    _scanResultsOpen;
    int     _scanCount;
    char    _scanSsids[WP_SCAN_MAX][33];
    int8_t  _scanRssi[WP_SCAN_MAX];
    uint8_t _scanChan[WP_SCAN_MAX];

    void drawHeader();
    void drawModeSection();
    void drawCredsSection();
    void drawCredRow(int y, const char* label, const char* value,
                     bool hasScanButton, bool disabled = false);
    void drawChannelSection();
    void drawActionsSection();

    void drawScanResults();    // full overlay
    void runScan();            // synchronous WiFi.scanNetworks + result capture

    bool hitBack(int sx, int sy) const;
    int  hitModeBtn(int sx, int sy) const;       // 0 / 1 / -1
    bool hitSsid(int sx, int sy) const;
    bool hitScan(int sx, int sy) const;          // SCAN button next to SSID
    bool hitPsk(int sx, int sy)  const;
    int  hitChannelBtn(int sx, int sy) const;    // 0/1/2 (channels 1/6/11) or -1
    bool hitSave(int sx, int sy) const;

    // Scan results list hit tests
    int  hitScanRow(int sx, int sy) const;       // 0..count-1 or -1
    bool hitScanBack(int sx, int sy) const;

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;

    // Persist current edit buffers to NVS and (if WiFi is currently up)
    // do an in-place reconnect with the new creds.
    void applyAndSave();
};

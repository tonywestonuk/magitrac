#include "WiFiSettingsPage.h"
#include "PairNVS.h"
#include "MagiMsg.h"
#include "MagiLink.h"
#include "Constants.h"
#include "UIHelpers.h"
#include "SettingsPage.h"     // extern gWifiChannelIdx + save/load
#include <WiFi.h>
#include <string.h>

#define CLI_NVS_NS "magitrac_cli"

// ── Layout ───────────────────────────────────────────────────────────────────
static const int WP_W       = 960;
static const int WP_H       = 540;

static const int WP_HDR_H   = 50;
static const int WP_BACK_X  = 830;
static const int WP_BACK_W  = 130;

static const int WP_LBL_H   = 20;

// MODE
static const int WP_MODE_LBL_Y = WP_HDR_H;                       // 50
static const int WP_MODE_Y     = WP_MODE_LBL_Y + WP_LBL_H;       // 70
static const int WP_MODE_ROW_H = 60;
static const int WP_MODE_BTN_W = 280;
static const int WP_MODE_BTN_H = 44;
static const int WP_MODE_BTN0_X = 360;
static const int WP_MODE_BTN1_X = 660;

// CREDENTIALS
static const int WP_CRED_LBL_Y = WP_MODE_Y + WP_MODE_ROW_H;      // 130
static const int WP_SSID_Y     = WP_CRED_LBL_Y + WP_LBL_H;       // 150
static const int WP_PSK_Y      = WP_SSID_Y + 60;                 // 210
static const int WP_CRED_ROW_H = 60;
static const int WP_VAL_X      = 280;
static const int WP_VAL_W      = 530;
static const int WP_VAL_H      = 44;
static const int WP_SCAN_X     = 820;
static const int WP_SCAN_W     = 130;

// CHANNEL
static const int WP_CHAN_LBL_Y = WP_PSK_Y + WP_CRED_ROW_H;       // 270
static const int WP_CHAN_Y     = WP_CHAN_LBL_Y + WP_LBL_H;       // 290
static const int WP_CHAN_ROW_H = 60;
static const int WP_CHAN_BTN_W = 100;
static const int WP_CHAN_BTN_H = 44;
static const int WP_CHAN_BTN_X[3] = { 280, 410, 540 };

// ACTIONS
static const int WP_ACT_LBL_Y  = WP_CHAN_Y + WP_CHAN_ROW_H;      // 350
static const int WP_SAVE_Y     = WP_ACT_LBL_Y + WP_LBL_H + 10;   // 380
static const int WP_SAVE_X     = 360;
static const int WP_SAVE_W     = 240;
static const int WP_SAVE_H     = 60;

static const int WP_LABEL_X    = 40;

// Scan results overlay
static const int WP_SR_ROW_H   = 50;
static const int WP_SR_LIST_Y  = WP_HDR_H + 20;   // below "SCAN RESULTS" header strip

// ── ctor ─────────────────────────────────────────────────────────────────────
WiFiSettingsPage::WiFiSettingsPage(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _keyboard(display, touch)
    , _apMode(MAGI_AP_MODE_SERVER)
    , _channelIdx(0)
    , _wasDown(false)
    , _editing(0)
    , _dirty(false)
    , _scanResultsOpen(false)
    , _scanCount(0)
{
    _ssid[0] = '\0';
    _psk[0]  = '\0';
}

// ── open / draw ──────────────────────────────────────────────────────────────
void WiFiSettingsPage::open() {
    memset(_ssid, 0, sizeof(_ssid));
    memset(_psk,  0, sizeof(_psk));
    _apMode  = MAGI_AP_MODE_SERVER;
    _dirty   = false;
    _editing = 0;
    _wasDown = _touch.isTouched;
    _scanResultsOpen = false;
    _scanCount       = 0;

    uint8_t loadedMode = 0;
    if (pairNvsLoadCreds(CLI_NVS_NS, _ssid, _psk, &loadedMode)) {
        _apMode = loadedMode;
    }
    _channelIdx = gWifiChannelIdx;   // populated by loadWifiChannel() at boot
}

void WiFiSettingsPage::draw() {
    _d.fillScreen(COL_WHITE);
    drawHeader();
    drawModeSection();
    drawCredsSection();
    drawChannelSection();
    drawActionsSection();
}

// ── Header ───────────────────────────────────────────────────────────────────
void WiFiSettingsPage::drawHeader() {
    _d.fillRect(0, 0, WP_W, WP_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(20, (WP_HDR_H - 24) / 2);
    _d.print("WIFI");
    uiButton(_d, WP_BACK_X, 0, WP_BACK_W, WP_HDR_H, "BACK", COL_BLACK, COL_WHITE, 3);
}

// ── Mode section ─────────────────────────────────────────────────────────────
void WiFiSettingsPage::drawModeSection() {
    _d.fillRect(0, WP_MODE_LBL_Y, WP_W, WP_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, WP_MODE_LBL_Y + (WP_LBL_H - 16) / 2);
    _d.print("MODE");

    _d.fillRect(0, WP_MODE_Y, WP_W, WP_MODE_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, WP_MODE_Y + WP_MODE_ROW_H - 1, WP_W, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(WP_LABEL_X, WP_MODE_Y + (WP_MODE_ROW_H - 24) / 2);
    _d.print("MODE");

    int btnY = WP_MODE_Y + (WP_MODE_ROW_H - WP_MODE_BTN_H) / 2;
    bool sel0 = (_apMode == MAGI_AP_MODE_SERVER);
    bool sel1 = (_apMode == MAGI_AP_MODE_EXTERNAL);
    uiButton(_d, WP_MODE_BTN0_X, btnY, WP_MODE_BTN_W, WP_MODE_BTN_H,
             "Server AP",
             sel0 ? COL_BLACK : COL_WHITE,
             sel0 ? COL_WHITE : COL_BLACK, 2);
    uiButton(_d, WP_MODE_BTN1_X, btnY, WP_MODE_BTN_W, WP_MODE_BTN_H,
             "External AP",
             sel1 ? COL_BLACK : COL_WHITE,
             sel1 ? COL_WHITE : COL_BLACK, 2);
}

// ── Creds section ────────────────────────────────────────────────────────────
// In SERVER_AP mode the rows are read-only: the server owns its softAP
// SSID + PSK (generated as Magitrac_XXXX on first pair) and the client
// learns them via the pair-challenge.  The label flips to flag this and
// the rows render as a single informational placeholder so the user can't
// type into them.
void WiFiSettingsPage::drawCredsSection() {
    _d.fillRect(0, WP_CRED_LBL_Y, WP_W, WP_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, WP_CRED_LBL_Y + (WP_LBL_H - 16) / 2);
    if (_apMode == MAGI_AP_MODE_SERVER) {
        _d.print("CREDENTIALS  (server-managed)");
    } else {
        _d.print("CREDENTIALS");
    }

    bool serverAp = (_apMode == MAGI_AP_MODE_SERVER);
    drawCredRow(WP_SSID_Y, "SSID",     _ssid, /*hasScanButton=*/true,  serverAp);
    drawCredRow(WP_PSK_Y,  "PASSWORD", _psk,  /*hasScanButton=*/false, serverAp);
}

void WiFiSettingsPage::drawCredRow(int y, const char* label,
                                   const char* value, bool hasScanButton,
                                   bool disabled) {
    _d.fillRect(0, y, WP_W, WP_CRED_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, y + WP_CRED_ROW_H - 1, WP_W, COL_BLACK);

    _d.setTextSize(3);
    _d.setTextColor(disabled ? COL_DKGREY : COL_BLACK);
    _d.setCursor(WP_LABEL_X, y + (WP_CRED_ROW_H - 24) / 2);
    _d.print(label);

    int boxY = y + (WP_CRED_ROW_H - WP_VAL_H) / 2;
    _d.drawRect(WP_VAL_X, boxY, WP_VAL_W, WP_VAL_H,
                disabled ? COL_DKGREY : COL_BLACK);
    _d.setTextSize(2);
    _d.setCursor(WP_VAL_X + 8, boxY + (WP_VAL_H - 16) / 2);
    if (disabled) {
        _d.setTextColor(COL_DKGREY);
        _d.print("(set by server at pair)");
    } else if (value[0] == '\0') {
        _d.setTextColor(COL_DKGREY);
        _d.print("(tap to set)");
    } else {
        _d.setTextColor(COL_BLACK);
        _d.print(value);
    }

    if (hasScanButton) {
        bool enabled = (_apMode == MAGI_AP_MODE_EXTERNAL);
        uint8_t bg = enabled ? COL_BLACK : COL_WHITE;
        uint8_t fg = enabled ? COL_WHITE : COL_DKGREY;
        if (!enabled) {
            // Disabled appearance: border-only with grey text.
            _d.fillRect(WP_SCAN_X, boxY, WP_SCAN_W, WP_VAL_H, COL_WHITE);
            _d.drawRect(WP_SCAN_X, boxY, WP_SCAN_W, WP_VAL_H, COL_DKGREY);
            _d.setTextSize(2);
            _d.setTextColor(COL_DKGREY);
            int tw = 4 * 12;   // 4 chars × (2*6) px
            _d.setCursor(WP_SCAN_X + (WP_SCAN_W - tw) / 2,
                         boxY + (WP_VAL_H - 16) / 2);
            _d.print("SCAN");
        } else {
            uiButton(_d, WP_SCAN_X, boxY, WP_SCAN_W, WP_VAL_H,
                     "SCAN", bg, fg, 2);
        }
    }
}

// ── Channel section ──────────────────────────────────────────────────────────
void WiFiSettingsPage::drawChannelSection() {
    _d.fillRect(0, WP_CHAN_LBL_Y, WP_W, WP_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, WP_CHAN_LBL_Y + (WP_LBL_H - 16) / 2);
    _d.print("CHANNEL");

    _d.fillRect(0, WP_CHAN_Y, WP_W, WP_CHAN_ROW_H, COL_WHITE);
    _d.drawFastHLine(0, WP_CHAN_Y + WP_CHAN_ROW_H - 1, WP_W, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(WP_LABEL_X, WP_CHAN_Y + (WP_CHAN_ROW_H - 24) / 2);
    _d.print("CHANNEL");

    bool enabled = (_apMode == MAGI_AP_MODE_SERVER);
    int btnY = WP_CHAN_Y + (WP_CHAN_ROW_H - WP_CHAN_BTN_H) / 2;
    static const char* LBL[3] = { "1", "6", "11" };
    for (int i = 0; i < 3; i++) {
        bool sel = (_channelIdx == (uint8_t)i);
        uint8_t bg, fg;
        if (!enabled) {
            // Disabled: greyed border + grey text, no fill.
            _d.fillRect(WP_CHAN_BTN_X[i], btnY, WP_CHAN_BTN_W, WP_CHAN_BTN_H, COL_WHITE);
            _d.drawRect(WP_CHAN_BTN_X[i], btnY, WP_CHAN_BTN_W, WP_CHAN_BTN_H, COL_DKGREY);
            _d.setTextSize(3);
            _d.setTextColor(COL_DKGREY);
            int tw = (int)strlen(LBL[i]) * 18;
            _d.setCursor(WP_CHAN_BTN_X[i] + (WP_CHAN_BTN_W - tw) / 2,
                         btnY + (WP_CHAN_BTN_H - 24) / 2);
            _d.print(LBL[i]);
        } else {
            bg = sel ? COL_BLACK : COL_WHITE;
            fg = sel ? COL_WHITE : COL_BLACK;
            uiButton(_d, WP_CHAN_BTN_X[i], btnY, WP_CHAN_BTN_W, WP_CHAN_BTN_H,
                     LBL[i], bg, fg, 3);
        }
    }
}

// ── Actions section ──────────────────────────────────────────────────────────
void WiFiSettingsPage::drawActionsSection() {
    _d.fillRect(0, WP_ACT_LBL_Y, WP_W, WP_LBL_H, COL_LTGREY);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(10, WP_ACT_LBL_Y + (WP_LBL_H - 16) / 2);
    _d.print("ACTIONS");

    uiButton(_d, WP_SAVE_X, WP_SAVE_Y, WP_SAVE_W, WP_SAVE_H,
             _dirty ? "SAVE *" : "SAVE",
             COL_BLACK, COL_WHITE, 3);
}

// ── Scan results overlay ─────────────────────────────────────────────────────
void WiFiSettingsPage::drawScanResults() {
    _d.fillScreen(COL_WHITE);
    _d.fillRect(0, 0, WP_W, WP_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(20, (WP_HDR_H - 24) / 2);
    _d.printf("SCAN RESULTS (%d)", _scanCount);
    uiButton(_d, WP_BACK_X, 0, WP_BACK_W, WP_HDR_H, "BACK", COL_BLACK, COL_WHITE, 3);

    if (_scanCount == 0) {
        _d.setTextSize(2);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(40, 100);
        _d.print("No networks found.");
        return;
    }

    int rows = _scanCount;
    if (rows > WP_SCAN_MAX) rows = WP_SCAN_MAX;
    for (int i = 0; i < rows; i++) {
        int y = WP_SR_LIST_Y + i * WP_SR_ROW_H;
        _d.drawFastHLine(0, y + WP_SR_ROW_H - 1, WP_W, COL_BLACK);
        _d.setTextSize(2);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(20, y + (WP_SR_ROW_H - 16) / 2);
        _d.print(_scanSsids[i]);
        // Right-align ch + RSSI
        char rhs[24];
        snprintf(rhs, sizeof(rhs), "ch%-2u  %4ddBm",
                 (unsigned)_scanChan[i], (int)_scanRssi[i]);
        _d.setCursor(WP_W - 200, y + (WP_SR_ROW_H - 16) / 2);
        _d.print(rhs);
    }
}

void WiFiSettingsPage::runScan() {
    // Show "Scanning..." flash so the user knows what's happening.
    _d.fillScreen(COL_WHITE);
    _d.fillRect(0, 0, WP_W, WP_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    _d.setCursor(20, (WP_HDR_H - 24) / 2);
    _d.print("SCANNING...");
    _d.setTextColor(COL_BLACK);
    _d.setCursor(40, 100);
    _d.setTextSize(2);
    _d.print("This may take a few seconds.");
    _d.paint();

    // Bring up WIFI_STA so scan works (we're in AP_STA already).  Async
    // mode false → blocks until done.  Returns count or negative on error.
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
    if (n < 0) n = 0;
    if (n > WP_SCAN_MAX) n = WP_SCAN_MAX;
    _scanCount = n;
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        strncpy(_scanSsids[i], s.c_str(), sizeof(_scanSsids[i]) - 1);
        _scanSsids[i][sizeof(_scanSsids[i]) - 1] = '\0';
        _scanRssi[i] = (int8_t)WiFi.RSSI(i);
        _scanChan[i] = (uint8_t)WiFi.channel(i);
    }
    WiFi.scanDelete();
    Serial.printf("[WIFI-SCAN] found %d networks\n", _scanCount);
}

// ── Hit tests ────────────────────────────────────────────────────────────────
bool WiFiSettingsPage::hitBack(int sx, int sy) const {
    return sy < WP_HDR_H && sx >= WP_BACK_X && sx < WP_BACK_X + WP_BACK_W;
}

int WiFiSettingsPage::hitModeBtn(int sx, int sy) const {
    int btnY = WP_MODE_Y + (WP_MODE_ROW_H - WP_MODE_BTN_H) / 2;
    if (sy < btnY || sy >= btnY + WP_MODE_BTN_H) return -1;
    if (sx >= WP_MODE_BTN0_X && sx < WP_MODE_BTN0_X + WP_MODE_BTN_W) return 0;
    if (sx >= WP_MODE_BTN1_X && sx < WP_MODE_BTN1_X + WP_MODE_BTN_W) return 1;
    return -1;
}

bool WiFiSettingsPage::hitSsid(int sx, int sy) const {
    if (sy < WP_SSID_Y || sy >= WP_SSID_Y + WP_CRED_ROW_H) return false;
    return sx < WP_SCAN_X;
}

bool WiFiSettingsPage::hitScan(int sx, int sy) const {
    int boxY = WP_SSID_Y + (WP_CRED_ROW_H - WP_VAL_H) / 2;
    return sy >= boxY && sy < boxY + WP_VAL_H &&
           sx >= WP_SCAN_X && sx < WP_SCAN_X + WP_SCAN_W;
}

bool WiFiSettingsPage::hitPsk(int sx, int sy) const {
    return sy >= WP_PSK_Y && sy < WP_PSK_Y + WP_CRED_ROW_H;
}

int WiFiSettingsPage::hitChannelBtn(int sx, int sy) const {
    int btnY = WP_CHAN_Y + (WP_CHAN_ROW_H - WP_CHAN_BTN_H) / 2;
    if (sy < btnY || sy >= btnY + WP_CHAN_BTN_H) return -1;
    for (int i = 0; i < 3; i++) {
        if (sx >= WP_CHAN_BTN_X[i] && sx < WP_CHAN_BTN_X[i] + WP_CHAN_BTN_W) return i;
    }
    return -1;
}

bool WiFiSettingsPage::hitSave(int sx, int sy) const {
    return sy >= WP_SAVE_Y && sy < WP_SAVE_Y + WP_SAVE_H &&
           sx >= WP_SAVE_X && sx < WP_SAVE_X + WP_SAVE_W;
}

int WiFiSettingsPage::hitScanRow(int sx, int sy) const {
    if (sy < WP_SR_LIST_Y) return -1;
    int idx = (sy - WP_SR_LIST_Y) / WP_SR_ROW_H;
    if (idx < 0 || idx >= _scanCount) return -1;
    return idx;
}

bool WiFiSettingsPage::hitScanBack(int sx, int sy) const {
    return sy < WP_HDR_H && sx >= WP_BACK_X && sx < WP_BACK_X + WP_BACK_W;
}

void WiFiSettingsPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

// ── Persist + apply ──────────────────────────────────────────────────────────
void WiFiSettingsPage::applyAndSave() {
    pairNvsSaveCreds(CLI_NVS_NS, _ssid, _psk, _apMode);

    // Persist channel and tell the server (if currently connected).
    // Channel only meaningful in SERVER_AP mode but harmless to persist
    // always (so the value sticks across mode flips).
    if (_channelIdx != gWifiChannelIdx) {
        gWifiChannelIdx = _channelIdx;
        saveWifiChannel();
        if (_apMode == MAGI_AP_MODE_SERVER && gMagiLink.isConnected()) {
            MsgSetWifiChannel msg;
            msg.idx = _channelIdx;
            gMagiLink.acquireMutex();
            gMagiLink.send(&msg, sizeof(msg));
            gMagiLink.releaseMutex();
        }
    }

    Serial.printf("[WIFI] saved ssid=%s apMode=%u ch=%u\n",
                  _ssid, (unsigned)_apMode,
                  (unsigned)magiWifiChannelFromIdx(_channelIdx));
    _dirty = false;

    // In-place reconnect with the new creds.  MagiLink's connect-loop
    // re-associates automatically on the new association.
    if (_ssid[0] != '\0') {
        WiFi.disconnect(false, false);
        WiFi.config(IPAddress(MAGI_CLIENT_IP_0, MAGI_CLIENT_IP_1,
                              MAGI_CLIENT_IP_2, MAGI_CLIENT_IP_3),
                    IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                              MAGI_SERVER_IP_2, MAGI_SERVER_IP_3),
                    IPAddress(255, 255, 255, 0));
        WiFi.begin(_ssid, _psk);
        Serial.println("[WIFI] STA reconnecting with new creds");
    }
}

// ── Poll ─────────────────────────────────────────────────────────────────────
bool WiFiSettingsPage::poll() {
    // Scan results overlay
    if (_scanResultsOpen) {
        if (!_touch.read()) return false;
        bool down = _touch.isTouched;
        int sx, sy;
        rawToScreen(_touch.x, _touch.y, sx, sy);
        bool falling = (!down && _wasDown);
        if (down) _wasDown = true;
        if (!falling) return false;
        _wasDown = false;

        if (hitScanBack(sx, sy)) {
            _scanResultsOpen = false;
            _d.fillScreen(COL_WHITE);
            draw();
            _d.paintLater();
            return false;
        }
        int row = hitScanRow(sx, sy);
        if (row >= 0) {
            strncpy(_ssid, _scanSsids[row], sizeof(_ssid) - 1);
            _ssid[sizeof(_ssid) - 1] = '\0';
            _dirty = true;
            _scanResultsOpen = false;
            _d.fillScreen(COL_WHITE);
            draw();
            _d.paintLater();
        }
        return false;
    }

    // Keyboard active — delegate to it
    if (_editing > 0) {
        if (_keyboard.poll()) {
            if (_keyboard.isDone()) _dirty = true;
            _editing = 0;
            _d.fillScreen(COL_WHITE);
            draw();
            _d.paintLater();
        }
        return false;
    }

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    bool falling = (!down && _wasDown);
    if (down) _wasDown = true;
    if (!falling) return false;
    _wasDown = false;

    if (hitBack(sx, sy)) return true;

    int mode = hitModeBtn(sx, sy);
    if (mode >= 0) {
        uint8_t newMode = (mode == 0) ? MAGI_AP_MODE_SERVER : MAGI_AP_MODE_EXTERNAL;
        if (newMode != _apMode) {
            _apMode = newMode;
            _dirty  = true;
            // Mode flip re-enables/disables both SCAN and CHANNEL rows.
            drawModeSection();
            drawCredsSection();
            drawChannelSection();
            drawActionsSection();
            _d.paintLater();
        }
        return false;
    }

    if (hitScan(sx, sy)) {
        if (_apMode != MAGI_AP_MODE_EXTERNAL) return false;   // disabled
        runScan();
        _scanResultsOpen = true;
        drawScanResults();
        _d.paint();
        return false;
    }

    if (hitSsid(sx, sy)) {
        if (_apMode == MAGI_AP_MODE_SERVER) return false;   // server-managed
        _editing = 1;
        _keyboard.open(_ssid, sizeof(_ssid));
        _keyboard.draw();
        _d.paint();
        return false;
    }

    if (hitPsk(sx, sy)) {
        if (_apMode == MAGI_AP_MODE_SERVER) return false;   // server-managed
        _editing = 2;
        _keyboard.open(_psk, sizeof(_psk));
        _keyboard.draw();
        _d.paint();
        return false;
    }

    int chan = hitChannelBtn(sx, sy);
    if (chan >= 0 && _apMode == MAGI_AP_MODE_SERVER) {
        if ((uint8_t)chan != _channelIdx) {
            _channelIdx = (uint8_t)chan;
            _dirty      = true;
            drawChannelSection();
            drawActionsSection();
            _d.paintLater();
        }
        return false;
    }

    if (hitSave(sx, sy)) {
        applyAndSave();
        drawActionsSection();
        _d.paintLater();
        return false;
    }

    return false;
}

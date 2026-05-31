// pairing.ino — server-side pairing state machine.
//
// Client owns the WiFi credentials (set via its WiFi settings page).
// Pairing ceremony (over ESP-NOW, always on channel 1):
//   1. User long-presses BtnC on server → enterPairingMode → server
//      forces ch1 and listens for MSG_PAIR_PROBE.
//   2. Client (in its own pair mode) sits on ch1 broadcasting probes.
//      Server receives a probe, generates a 4-digit PIN, unicasts
//      MSG_PAIR_CHALLENGE back, displays PIN on LCD → SERVER_PAIRING_CONFIRM.
//   3. User compares PIN on both screens; if matched, taps Confirm on
//      the magitrac touch screen → client unicasts MSG_PAIR_OFFER
//      { apMode, apSsid, apPsk }.
//   4. Server persists creds + apMode + paired-client MAC to NVS, then
//      ESP.restart()s.  Next boot brings WiFi up per apMode:
//        SERVER_AP   → softAP at MAGI_SERVER_IP
//        EXTERNAL_AP → STA, config(MAGI_SERVER_IP, ...) + begin(ssid, psk)
//
// In SERVER_STANDALONE the server always plays; MSG_CONNECT from the
// paired client (over TCP, once WiFi is up) transitions to SERVER_CONNECTED.

#include "midi_player.h"
#include "PairNVS.h"
#include <esp_random.h>
#include <esp_wifi.h>

extern bool needsFullRedraw;   // defined in magitrac_server.ino

// ── State ─────────────────────────────────────────────────────────────────────
enum ServerMode {
    SERVER_STANDALONE,        // playing; MagiLink listening for paired client
    SERVER_PAIRING,           // listening for MSG_PAIR_PROBE on ch1
    SERVER_PAIRING_CONFIRM,   // sent CHALLENGE; PIN displayed; waiting for OFFER
    SERVER_CONNECTED,         // MagiLink session active with paired client
};

static ServerMode  serverMode   = SERVER_STANDALONE;
       uint8_t     clientMac[6] = {};   // exposed for commands_server.ino

// ── NVS-stored pairing ────────────────────────────────────────────────────────
#define SRV_NVS_NS "magitrac_srv"

static bool    sHasPairing         = false;
static uint8_t sStoredClientMac[6] = {};
static uint8_t sStoredSecret[16]   = {};

// ── Pairing ceremony ──────────────────────────────────────────────────────────
static uint8_t  sPairPendingMac[6] = {};   // MAC of the client we sent CHALLENGE to
static uint8_t  sPairCode[4]       = {};   // PIN we generated
static uint32_t sPairingWindowMs   = 0;
static const uint32_t PAIR_WINDOW_MS = 60000;

// Deferred-send flag for CHALLENGE.  Cannot send from inside the ESP-NOW
// recv callback (blocks waiting for send-cb on the same WiFi task → ACK
// times out).  PROBE handler sets this; pairingLoop() sends.
static bool     sChallengePending  = false;

// ── Song push (set here on connect; consumed by commandsTick in commands_server.ino)
bool sSongPushPending = false;

static bool sCancelArmed = false;

// ── Screen ────────────────────────────────────────────────────────────────────
static void drawClientServerScreen(const char* line1, const char* line2,
                                   const char* footer) {
    lcd.fillScreen(lgfx::color565(0, 80, 0));

    lcd.fillRect(0, 0, 240, 36, lgfx::color565(0, 110, 0));
    lcd.setTextColor(TFT_WHITE, lgfx::color565(0, 110, 0));
    lcd.setTextSize(2);
    lcd.setCursor(4, 10);
    lcd.print("CLIENT SERVER MODE");

    lcd.setTextColor(TFT_WHITE, lgfx::color565(0, 80, 0));
    lcd.setTextSize(2);
    lcd.setCursor(8, 48);
    lcd.print(line1);

    if (line2 && line2[0]) {
        lcd.setTextSize(line2[1] == '\0' ? 4 : 2);
        lcd.setCursor(8, 76);
        lcd.print(line2);
    }

    uint8_t mac[6];
    gTransportEspNow.localAddr(mac);
    lcd.setTextSize(1);
    lcd.setCursor(8, 130);
    lcd.printf("Server: %02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    lcd.fillRect(0, 204, 240, 36, lgfx::color565(0, 60, 0));
    lcd.setTextColor(TFT_WHITE, lgfx::color565(0, 60, 0));
    lcd.setTextSize(1);
    lcd.setCursor(8, 216);
    lcd.print(footer);
}

// ── Init (call from setup() after the active transport is up) ────────────────
void pairingInit() {
    sHasPairing = pairNvsLoad(SRV_NVS_NS, sStoredClientMac, sStoredSecret);
    if (sHasPairing) {
        Serial.printf("[PAIR] stored client: %02X:%02X:%02X:%02X:%02X:%02X\n",
            sStoredClientMac[0], sStoredClientMac[1], sStoredClientMac[2],
            sStoredClientMac[3], sStoredClientMac[4], sStoredClientMac[5]);
    } else {
        Serial.println("[PAIR] no stored pairing — long-press BtnC to pair");
    }
}

// ── Enter / exit ──────────────────────────────────────────────────────────────
void enterPairingMode() {
    if (serverMode != SERVER_STANDALONE && serverMode != SERVER_CONNECTED) return;
    serverMode         = SERVER_PAIRING;
    sPairingWindowMs   = millis();
    sChallengePending  = false;
    BtnA.wasPressed(); BtnB.wasPressed(); BtnC.wasPressed();
    sCancelArmed       = false;

    // Force radio to ch1 — the only pairing channel.  Stop softAP +
    // STA aggressively: if STA is in a retry loop for an unreachable
    // AP it will starve esp_now_send with "wifi:sta is connecting".
    // After a successful OFFER we ESP.restart() so the AP comes back
    // up on the persisted channel anyway.
    WiFi.setAutoReconnect(false);
    WiFi.softAPdisconnect(false);
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
    esp_wifi_disconnect();
    for (int i = 0; i < 10; i++) {
        wl_status_t st = WiFi.status();
        if (st != WL_CONNECTED && st != WL_IDLE_STATUS) break;
        delay(50);
    }
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    delay(50);

    drawClientServerScreen("PAIR MODE", "Listening", "C: Cancel");
    uint8_t mac[6];
    gTransportEspNow.localAddr(mac);
    Serial.printf("[PAIR] pair window open (60 s) on ch1; my MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ── Public queries ────────────────────────────────────────────────────────────
bool pairingIsActive()    { return serverMode == SERVER_PAIRING ||
                                   serverMode == SERVER_PAIRING_CONFIRM; }
bool pairingIsConnected() { return serverMode == SERVER_CONNECTED; }
bool pairingIsPaired()    { return sHasPairing; }

// ── MagiLink session hooks ──────────────────────────────────────────────────
void pairingOnMagiLinkConnected() {
    if (serverMode == SERVER_CONNECTED) return;
    serverMode       = SERVER_CONNECTED;
    sSongPushPending = true;
    sCancelArmed     = false;
    needsFullRedraw  = true;
    Serial.println("[PAIR] MagiLink session established");
}

void pairingOnMagiLinkDisconnected() {
    if (serverMode != SERVER_CONNECTED) return;
    serverMode      = SERVER_STANDALONE;
    needsFullRedraw = true;
    Serial.println("[PAIR] MagiLink session ended");
}

// ── Handle incoming messages ─────────────────────────────────────────────────
// Pairing-ceremony messages only — the data session is owned by MagiLink.
void pairingHandleMessage(const uint8_t* data, int len) {
    if (len < 1) return;
    MagiMsgType type = (MagiMsgType)data[0];
    const uint8_t* senderMac = gTransportEspNow.lastSenderAddr();

    switch (type) {

        case MSG_PAIR_PROBE:
            // Client is in pair mode on ch1 broadcasting probes.  Generate
            // a PIN and queue a CHALLENGE (deferred to pairingLoop so the
            // ESP-NOW send isn't blocked by the recv-callback context).
            if (serverMode != SERVER_PAIRING) return;
            if (len < (int)sizeof(MsgPairProbe)) return;
            if (sChallengePending) return;   // already queued from an earlier probe
            {
                const MsgPairProbe* p = (const MsgPairProbe*)data;
                if (memcmp(p->magic, MAGI_PAIR_MAGIC, 8) != 0) return;
                uint32_t r = esp_random();
                sPairCode[0] = '0' + (r % 10); r /= 10;
                sPairCode[1] = '0' + (r % 10); r /= 10;
                sPairCode[2] = '0' + (r % 10); r /= 10;
                sPairCode[3] = '0' + (r % 10);
                memcpy(sPairPendingMac, senderMac, 6);
                sChallengePending = true;
            }
            break;

        case MSG_PAIR_OFFER:
            // Client confirmed.  Persist creds + apMode + paired-client
            // MAC, then reboot into the chosen WiFi mode.
            if (serverMode != SERVER_PAIRING_CONFIRM) return;
            if (memcmp(senderMac, sPairPendingMac, 6) != 0) return;
            if (len < (int)sizeof(MsgPairOffer)) return;
            {
                const MsgPairOffer* o = (const MsgPairOffer*)data;
                if (o->apMode == MAGI_AP_MODE_SERVER) {
                    size_t pl = strlen(o->apPsk);
                    if (pl > 0 && pl < 8) {
                        Serial.printf("[PAIR] WARNING: PSK length %u — WPA2 needs 8+ chars; softAP will fail\n",
                            (unsigned)pl);
                    }
                }

                uint8_t zeroSecret[16] = {};
                pairNvsSave(SRV_NVS_NS, sPairPendingMac, zeroSecret);
                pairNvsSaveCreds(SRV_NVS_NS, o->apSsid, o->apPsk, o->apMode);

                drawClientServerScreen("Paired!", "Restarting", "");
                Serial.println("[PAIR] restarting into configured WiFi mode");
                Serial.flush();
                delay(200);
                ESP.restart();
            }
            break;

        default:
            break;
    }
}

// ── Main loop tick ────────────────────────────────────────────────────────────
void pairingLoop() {
    uint32_t now = millis();

    switch (serverMode) {

        case SERVER_STANDALONE:
            break;

        case SERVER_PAIRING: {
            if (!sCancelArmed && !BtnC.isDown()) sCancelArmed = true;
            if (sCancelArmed && BtnC.wasPressed()) {
                serverMode = SERVER_STANDALONE;
                needsFullRedraw = true;
                return;
            }
            if (now - sPairingWindowMs >= PAIR_WINDOW_MS) {
                Serial.println("[PAIR] pair window timed out");
                serverMode = SERVER_STANDALONE;
                needsFullRedraw = true;
                return;
            }

            // Deferred CHALLENGE send.
            if (sChallengePending) {
                sChallengePending = false;
                gTransportEspNow.addPeer(sPairPendingMac);
                MsgPairChallenge ch;
                ch.type = MSG_PAIR_CHALLENGE;
                memcpy(ch.pin, sPairCode, 4);
                bool ok = gTransportEspNow.sendRaw(&ch, sizeof(ch));
                Serial.printf("[PAIR] got PROBE → PIN %c%c%c%c, CHALLENGE send=%s\n",
                    sPairCode[0], sPairCode[1], sPairCode[2], sPairCode[3],
                    ok ? "OK" : "FAIL");
                char codeStr[6];
                snprintf(codeStr, sizeof(codeStr), "%c%c%c%c",
                    sPairCode[0], sPairCode[1], sPairCode[2], sPairCode[3]);
                drawClientServerScreen("Confirm code:", codeStr, "C: Cancel");
                serverMode = SERVER_PAIRING_CONFIRM;
            }
            break;
        }

        case SERVER_PAIRING_CONFIRM: {
            if (!sCancelArmed && !BtnC.isDown()) sCancelArmed = true;
            if (sCancelArmed && BtnC.wasPressed()) {
                serverMode = SERVER_STANDALONE;
                needsFullRedraw = true;
                return;
            }
            if (now - sPairingWindowMs >= PAIR_WINDOW_MS) {
                Serial.println("[PAIR] pair window timed out (after CHALLENGE)");
                serverMode = SERVER_STANDALONE;
                needsFullRedraw = true;
            }
            break;
        }

        case SERVER_CONNECTED:
            // Liveness driven by MagiLink TCP keepalive.
            break;
    }
}

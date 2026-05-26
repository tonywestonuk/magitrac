// pairing.ino — Client-Server session state machine
//
// Pairing ceremony (post-2026-05-24 redesign):
//   1. Server enters pair mode (BtnC long-press 2 s).  ESP-NOW transport
//      is already up.
//   2. SERVER_PAIRING: round-robin set channel {1, 6, 11} every 250 ms and
//      broadcast MSG_PAIR_PROBE on each.  Magitrac client (in its own pair
//      mode) receives the probe when our scan channel matches its AP
//      channel, and replies with MSG_PAIR_CHALLENGE carrying a 4-digit PIN.
//   3. On CHALLENGE: stop scanning, display the PIN, → SERVER_PAIRING_CONFIRM.
//      User compares against the PIN shown on the magitrac and taps Confirm
//      *on the magitrac* (one-sided confirm).
//   4. Magitrac unicasts MSG_PAIR_OFFER with AP creds + assigned IP.
//      Server saves to NVS and reboots into TCP-STA mode.
//
// Normal operation (SERVER_STANDALONE) is unchanged — server always plays;
// MSG_CONNECT from the paired client (over TCP after pairing) transitions
// to SERVER_CONNECTED.  C button ends an active session.

#include "midi_player.h"
#include "PairNVS.h"
#include <esp_random.h>
#include <esp_wifi.h>

extern bool needsFullRedraw;   // defined in magitrac_server.ino

// ── State ─────────────────────────────────────────────────────────────────────
enum ServerMode {
    SERVER_STANDALONE,        // playing; accepts authenticated MSG_CONNECT from paired client
    SERVER_PAIRING,           // scanning channels 1/6/11 broadcasting PROBE
    SERVER_PAIRING_CONFIRM,   // got CHALLENGE; PIN displayed; waiting for OFFER
    SERVER_CONNECTED,         // client active
};

static ServerMode  serverMode   = SERVER_STANDALONE;
       uint8_t     clientMac[6] = {};   // exposed so commands_server.ino can use it

// ── NVS-stored pairing ────────────────────────────────────────────────────────
// Secret slot is preserved in the NVS schema but written as zeros — no
// per-frame signing currently.  Kept so signing can be reintroduced later
// without a flash-wipe.
#define SRV_NVS_NS "magitrac_srv"

static bool    sHasPairing         = false;
static uint8_t sStoredClientMac[6] = {};
static uint8_t sStoredSecret[16]   = {};

// ── Pairing ceremony ──────────────────────────────────────────────────────────
static uint8_t  sPairPendingMac[6] = {};   // wire-source MAC of whoever sent CHALLENGE
static uint8_t  sPairCode[4]       = {};   // PIN received in CHALLENGE
static uint32_t sPairingWindowMs   = 0;
static const uint32_t PAIR_WINDOW_MS = 60000;

// Channel scan (SERVER_PAIRING)
static const uint8_t  PAIR_SCAN_CHANNELS[3]   = { 1, 6, 11 };
static const uint32_t PAIR_SCAN_DWELL_MS      = 250;
static uint32_t       sScanLastProbeMs        = 0;
static uint8_t        sScanIdx                = 0;

// ── Song push (set here on connect; consumed by commandsTick in commands_server.ino)
bool sSongPushPending = false;

// Liveness is driven by TCP keepalive on the data socket (set in
// MagiCommsTcp).  No app-layer heartbeat — used to be ESP-NOW ping/pong
// but ESP-NOW shares the radio with TCP and got starved during heavy
// backup traffic, causing false "client gone" tearndowns.
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
        lcd.setTextSize(line2[1] == '\0' ? 4 : 2);   // bigger text for short strings (e.g. code)
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
        // Register the paired-client MAC on the ESP-NOW transport so the
        // pairing handler's MAC check resolves correctly when a PAIR_OFFER
        // arrives during a re-pair attempt.
        gTransportEspNow.addPeer(sStoredClientMac);
    } else {
        Serial.println("[PAIR] no stored pairing — use BtnC long-press to pair");
    }
}

// ── Enter / exit ──────────────────────────────────────────────────────────────
void enterPairingMode() {
    if (serverMode != SERVER_STANDALONE) return;
    serverMode       = SERVER_PAIRING;
    sPairingWindowMs = millis();
    sScanLastProbeMs = 0;       // force immediate probe on first tick
    sScanIdx         = 0;
    BtnA.wasPressed(); BtnB.wasPressed(); BtnC.wasPressed();
    sCancelArmed = false;
    drawClientServerScreen("PAIR MODE", "Scanning...", "C: Cancel");
    uint8_t mac[6];
    gTransportEspNow.localAddr(mac);
    Serial.printf("[PAIR] pair window open (60 s); my MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ── Public queries ────────────────────────────────────────────────────────────
bool pairingIsActive()    { return serverMode != SERVER_STANDALONE; }
bool pairingIsConnected() { return serverMode == SERVER_CONNECTED; }
bool pairingIsPaired()    { return sHasPairing; }

// Drop both pair-flag and TCP creds, then reboot — we'll come back up
// in ESP-NOW mode, ready for a fresh pairing ceremony.  Called from the
// magitrac_server.ino BtnC long-press handler when already paired so the
// user can re-pair without flashing.
void pairingClearAndRestart() {
    Serial.println("[PAIR] clearing creds + restarting for re-pair");
    pairNvsClear(SRV_NVS_NS);
    pairNvsClearCreds(SRV_NVS_NS);
    drawClientServerScreen("Cleared.", "Restarting", "");
    Serial.flush();
    delay(200);
    ESP.restart();
}

// ── MagiLink session hooks ──────────────────────────────────────────────────
// Called from the MagiLink session task in magitrac_server.ino after the
// MSG_CONNECT / MSG_CONNECT_ACK handshake completes (or fails).  These
// own the SERVER_STANDALONE ↔ SERVER_CONNECTED transition.
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
// Pairing-ceremony messages only — the data session is owned by MagiLink
// (CONNECT/DISCONNECT + command dispatch handled via registered callbacks
// in magitrac_server.ino).
void pairingHandleMessage(const uint8_t* data, int len) {
    if (len < 1) return;
    MagiMsgType type = (MagiMsgType)data[0];
    const uint8_t* senderMac = gTransportEspNow.lastSenderAddr();

    switch (type) {

        case MSG_PAIR_CHALLENGE:
            // Reply to our PROBE — magitrac generated a PIN and is
            // showing it.  Lock the channel we received this on (just
            // halt the scan) and mirror the PIN to our screen.
            if (serverMode != SERVER_PAIRING) return;
            if (len < (int)sizeof(MsgPairChallenge)) return;
            {
                const MsgPairChallenge* c = (const MsgPairChallenge*)data;
                memcpy(sPairCode,       c->pin,   4);
                memcpy(sPairPendingMac, senderMac, 6);
                serverMode = SERVER_PAIRING_CONFIRM;

                char codeStr[6];
                snprintf(codeStr, sizeof(codeStr), "%c%c%c%c",
                    sPairCode[0], sPairCode[1], sPairCode[2], sPairCode[3]);
                drawClientServerScreen("Confirm code:", codeStr, "C: Cancel");

                uint8_t ch = 0; wifi_second_chan_t sec;
                esp_wifi_get_channel(&ch, &sec);
                Serial.printf("[PAIR] got CHALLENGE PIN=%s on ch=%u from %02X:%02X:%02X:%02X:%02X:%02X\n",
                    codeStr, (unsigned)ch,
                    senderMac[0], senderMac[1], senderMac[2],
                    senderMac[3], senderMac[4], senderMac[5]);
            }
            break;

        case MSG_PAIR_OFFER:
            // Magitrac user tapped Confirm.  Save creds + paired-client MAC
            // and reboot into TCP-STA mode.
            if (serverMode != SERVER_PAIRING_CONFIRM) return;
            if (memcmp(senderMac, sPairPendingMac, 6) != 0) return;
            if (len < (int)sizeof(MsgPairOffer)) return;
            {
                const MsgPairOffer* o = (const MsgPairOffer*)data;
                uint8_t zeroSecret[16] = {};
                pairNvsSave(SRV_NVS_NS, sPairPendingMac, zeroSecret);
                pairNvsSaveCreds(SRV_NVS_NS, o->apSsid, o->apPsk,
                                 o->assignedIp, o->gatewayIp);
                Serial.printf("[PAIR] OFFER: ssid=%s ip=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
                    o->apSsid,
                    o->assignedIp[0], o->assignedIp[1], o->assignedIp[2], o->assignedIp[3],
                    o->gatewayIp[0],  o->gatewayIp[1],  o->gatewayIp[2],  o->gatewayIp[3]);
                drawClientServerScreen("Paired!", "Restarting", "");
                Serial.println("[PAIR] restarting into TCP mode");
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
            // Cancel via BtnC (debounced: arm after BtnC released first)
            if (!sCancelArmed && digitalRead(BTN_C) == HIGH) sCancelArmed = true;
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

            // Channel scan + broadcast PROBE on each dwell tick
            if (now - sScanLastProbeMs >= PAIR_SCAN_DWELL_MS) {
                sScanLastProbeMs = now;
                uint8_t ch = PAIR_SCAN_CHANNELS[sScanIdx];
                sScanIdx = (sScanIdx + 1) % 3;
                esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
                delay(10);   // brief radio-settle

                MsgPairProbe probe;
                probe.type = MSG_PAIR_PROBE;
                memcpy(probe.magic, MAGI_PAIR_MAGIC, sizeof(probe.magic));
                gTransportEspNow.localAddr(probe.senderMac);
                gTransportEspNow.sendBroadcast(&probe, sizeof(probe));
            }
            break;
        }

        case SERVER_PAIRING_CONFIRM: {
            // Locked on a channel; just waiting for the OFFER.  BtnC still
            // cancels.
            if (!sCancelArmed && digitalRead(BTN_C) == HIGH) sCancelArmed = true;
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
            // Liveness now driven by the MagiLink session task in
            // magitrac_server.ino — it watches gMagiLink.isConnected()
            // and calls pairingOnMagiLinkDisconnected() on falling edge.
            break;
    }
}

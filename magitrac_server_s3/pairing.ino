// pairing.ino — server-side pairing state machine.
//
// WiFi credentials: for SERVER_AP the server generates its own Magitrac_XXXX
// SSID + random PSK and ships them to the client in the CHALLENGE; for
// EXTERNAL_AP the client supplies the creds of the network to join (from its
// WiFi settings page).  Either way the server stores whatever the client's
// OFFER carries.
// Pairing ceremony (over ESP-NOW, always on channel 1):
//   1. User holds the bezel strip for 2 s on the server → enterPairingMode →
//      server forces ch1 and listens for MSG_PAIR_PROBE.
//   2. Client (in its own pair mode) sits on ch1 broadcasting probes.
//      Server receives a probe, generates a 4-digit PIN, unicasts
//      MSG_PAIR_CHALLENGE { pin, apSsid, apPsk } back, displays PIN on LCD
//      → SERVER_PAIRING_CONFIRM.
//   3. User compares PIN on both screens; if matched, taps Confirm on
//      the magitrac touch screen → client unicasts MSG_PAIR_OFFER
//      { apMode, apSsid, apPsk }.
//   4. Server persists creds + apMode + paired-client MAC to NVS, then
//      ESP.restart()s.  Next boot brings WiFi up per apMode
//      (serverWifiBringUp in magitrac_server_s3.ino):
//        SERVER_AP   → hidden softAP at MAGI_SERVER_IP
//        EXTERNAL_AP → STA, begin(ssid, psk), address from that AP's DHCP
//   A bezel tap or the 60 s window running out abandons the ceremony and
//   brings WiFi back up from the stored pairing (pairingAbandon).
//
// In SERVER_STANDALONE the server always plays; MSG_CONNECT from the
// paired client (over TCP, once WiFi is up) transitions to SERVER_CONNECTED.

#include "midi_player.h"
#include "PairNVS.h"
#include <esp_random.h>
#include <esp_wifi.h>

extern bool needsFullRedraw;   // defined in magitrac_server_s3.ino

// The bezel is one logical strip (BtnA/B/C share the capacitive zone); cancel
// should respond to a tap anywhere on it, not just the BtnC sub-zone.
static bool bezelDownAny()   { return BtnA.isDown()     || BtnB.isDown()     || BtnC.isDown(); }
static bool bezelTappedAny() { return BtnA.wasPressed() || BtnB.wasPressed() || BtnC.wasPressed(); }

// ── State ─────────────────────────────────────────────────────────────────────
enum ServerMode {
    SERVER_STANDALONE,        // playing; MagiLink listening for paired client
    SERVER_PAIRING,           // listening for MSG_PAIR_PROBE on ch1
    SERVER_PAIRING_CONFIRM,   // sent CHALLENGE; PIN displayed; waiting for OFFER
    SERVER_CONNECTED,         // MagiLink session active with paired client
};

static ServerMode  serverMode   = SERVER_STANDALONE;

// ── NVS-stored pairing ────────────────────────────────────────────────────────
// SRV_NVS_NS ("magitrac_srv": pairing record + WiFi creds) is defined in
// magitrac_server_s3.ino, which also reads it.
#define SRV_SELF_NVS_NS "magitrac_srv_self"   // server-owned Magitrac_XXXX creds

static bool    sHasPairing         = false;
static uint8_t sStoredClientMac[6] = {};
static uint8_t sStoredSecret[16]   = {};

// Load (or generate-on-first-need) the server's own Magitrac_XXXX softAP
// SSID + random PSK.  Persisted in NVS so the same pair survives reboots;
// regenerates only after a factory-reset / NVS wipe.  SSID format
// "Magitrac_XXXX" where XXXX is 4 random hex digits.  PSK is 12 random
// ASCII chars from a safe alphabet (no ambiguous 0/O/l/1).
static void srvSelfCredsLoadOrGenerate(char ssid_out[MAGI_OFFER_SSID_LEN],
                                       char psk_out [MAGI_OFFER_PSK_LEN]) {
    Preferences prefs;
    prefs.begin(SRV_SELF_NVS_NS, /*readOnly=*/false);
    bool have = (prefs.getUChar("ready", 0) == 1);
    if (have) {
        prefs.getString("ssid", ssid_out, MAGI_OFFER_SSID_LEN);
        prefs.getString("psk",  psk_out,  MAGI_OFFER_PSK_LEN);
        // Defensive: if either is empty for some reason, regenerate.
        if (ssid_out[0] && psk_out[0]) {
            prefs.end();
            return;
        }
    }
    const char* alpha = "ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    size_t alphaN = strlen(alpha);
    uint32_t r = esp_random();
    snprintf(ssid_out, MAGI_OFFER_SSID_LEN, "Magitrac_%04X",
             (unsigned)(r & 0xFFFF));
    for (int i = 0; i < 12; i++) {
        psk_out[i] = alpha[esp_random() % alphaN];
    }
    psk_out[12] = '\0';
    prefs.putString("ssid", ssid_out);
    prefs.putString("psk",  psk_out);
    prefs.putUChar ("ready", 1);
    prefs.end();
    Serial.printf("[PAIR] generated self-creds ssid='%s' psk='%s'\n",
                  ssid_out, psk_out);
}

// ── Pairing ceremony ──────────────────────────────────────────────────────────
static uint8_t  sPairPendingMac[6] = {};   // MAC of the client we sent CHALLENGE to
static uint8_t  sPairCode[4]       = {};   // PIN we generated
static uint32_t sPairingWindowMs   = 0;
static const uint32_t PAIR_WINDOW_MS = 60000;

// Deferred-send flag for CHALLENGE.  Cannot send from inside the ESP-NOW
// recv callback (blocks waiting for send-cb on the same WiFi task → ACK
// times out).  PROBE handler sets this; pairingLoop() sends.
static bool     sChallengePending  = false;

// Latched OFFER.  The ESP-NOW recv callback runs on the WiFi task — no place
// for NVS writes, LCD drawing, a delay and a restart.  The OFFER handler
// checks the frame and copies its fields here; pairingLoop() does the work.
// sOfferPending is written last in the callback, so the loop task never sees
// half-copied fields.
static volatile bool sOfferPending = false;
static uint8_t       sOfferApMode  = 0;
static char          sOfferSsid[MAGI_OFFER_SSID_LEN] = {};
static char          sOfferPsk [MAGI_OFFER_PSK_LEN]  = {};

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
        Serial.println("[PAIR] no stored pairing — hold the bezel 2 s to pair");
    }
}

// ── Enter / exit ──────────────────────────────────────────────────────────────
void enterPairingMode() {
    if (serverMode != SERVER_STANDALONE && serverMode != SERVER_CONNECTED) return;
    serverMode         = SERVER_PAIRING;
    sPairingWindowMs   = millis();
    sChallengePending  = false;
    sOfferPending      = false;
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

    drawClientServerScreen("PAIR MODE", "Listening", "Bezel tap: cancel");
    uint8_t mac[6];
    gTransportEspNow.localAddr(mac);
    Serial.printf("[PAIR] pair window open (60 s) on ch1; my MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ── Public queries ────────────────────────────────────────────────────────────
bool pairingIsActive()    { return serverMode == SERVER_PAIRING ||
                                   serverMode == SERVER_PAIRING_CONFIRM; }
bool pairingIsConnected() { return serverMode == SERVER_CONNECTED; }

// ── MagiLink session hooks ──────────────────────────────────────────────────
void pairingOnMagiLinkConnected() {
    if (serverMode == SERVER_CONNECTED) return;
    serverMode       = SERVER_CONNECTED;
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
            // Client confirmed.  Check the frame here, latch its contents,
            // and let pairingLoop() persist them and reboot (see
            // sOfferPending) — none of that belongs in this callback.
            if (serverMode != SERVER_PAIRING_CONFIRM) return;
            if (memcmp(senderMac, sPairPendingMac, 6) != 0) return;
            if (len < (int)sizeof(MsgPairOffer)) return;
            if (sOfferPending) return;   // already latched from an earlier copy
            {
                const MsgPairOffer* o = (const MsgPairOffer*)data;
                sOfferApMode = o->apMode;
                memcpy(sOfferSsid, o->apSsid, sizeof(sOfferSsid));
                memcpy(sOfferPsk,  o->apPsk,  sizeof(sOfferPsk));
                sOfferSsid[sizeof(sOfferSsid) - 1] = '\0';
                sOfferPsk [sizeof(sOfferPsk)  - 1] = '\0';
                sOfferPending = true;
            }
            break;

        default:
            break;
    }
}

// Leave pairing mode without a completed pairing.  enterPairingMode() took
// the softAP down, turned auto-reconnect off and parked the radio on ch1 — put
// all of that back, or the already-paired client can never reconnect.
static void pairingAbandon(const char* why) {
    Serial.printf("[PAIR] %s — restoring WiFi\n", why);
    serverMode      = SERVER_STANDALONE;
    sOfferPending   = false;   // an OFFER that raced the cancel is void
    needsFullRedraw = true;
    WiFi.setAutoReconnect(true);
    serverWifiBringUp();
}

// ── Main loop tick ────────────────────────────────────────────────────────────
void pairingLoop() {
    uint32_t now = millis();

    switch (serverMode) {

        case SERVER_STANDALONE:
            break;

        case SERVER_PAIRING: {
            if (!sCancelArmed && !bezelDownAny()) sCancelArmed = true;
            if (sCancelArmed && bezelTappedAny()) {
                pairingAbandon("cancelled");
                return;
            }
            if (now - sPairingWindowMs >= PAIR_WINDOW_MS) {
                pairingAbandon("pair window timed out");
                return;
            }

            // Deferred CHALLENGE send.
            if (sChallengePending) {
                sChallengePending = false;
                gTransportEspNow.addPeer(sPairPendingMac);
                MsgPairChallenge ch = {};
                ch.type = MSG_PAIR_CHALLENGE;
                memcpy(ch.pin, sPairCode, 4);
                srvSelfCredsLoadOrGenerate(ch.apSsid, ch.apPsk);
                bool ok = gTransportEspNow.sendRaw(&ch, sizeof(ch));
                Serial.printf("[PAIR] got PROBE → PIN %c%c%c%c ssid='%s' CHALLENGE send=%s\n",
                    sPairCode[0], sPairCode[1], sPairCode[2], sPairCode[3],
                    ch.apSsid, ok ? "OK" : "FAIL");
                char codeStr[6];
                snprintf(codeStr, sizeof(codeStr), "%c%c%c%c",
                    sPairCode[0], sPairCode[1], sPairCode[2], sPairCode[3]);
                drawClientServerScreen("Confirm code:", codeStr, "Bezel tap: cancel");
                serverMode = SERVER_PAIRING_CONFIRM;
            }
            break;
        }

        case SERVER_PAIRING_CONFIRM: {
            // Deferred OFFER (latched by the recv callback, which has already
            // checked mode, sender and length).  First in this state: the
            // client has confirmed, so a cancel tap or the window running out
            // in the same pass no longer applies.  Persist creds + apMode +
            // paired-client MAC, then reboot into the chosen WiFi mode.
            if (sOfferPending) {
                sOfferPending = false;
                if (sOfferApMode == MAGI_AP_MODE_SERVER) {
                    size_t pl = strlen(sOfferPsk);
                    if (pl > 0 && pl < 8) {
                        Serial.printf("[PAIR] WARNING: PSK length %u — WPA2 needs 8+ chars; softAP will fail\n",
                            (unsigned)pl);
                    }
                }

                uint8_t zeroSecret[16] = {};
                pairNvsSave(SRV_NVS_NS, sPairPendingMac, zeroSecret);
                pairNvsSaveCreds(SRV_NVS_NS, sOfferSsid, sOfferPsk, sOfferApMode);

                drawClientServerScreen("Paired!", "Restarting", "");
                Serial.println("[PAIR] restarting into configured WiFi mode");
                Serial.flush();
                delay(200);
                ESP.restart();
            }
            if (!sCancelArmed && !bezelDownAny()) sCancelArmed = true;
            if (sCancelArmed && bezelTappedAny()) {
                pairingAbandon("cancelled");
                return;
            }
            if (now - sPairingWindowMs >= PAIR_WINDOW_MS) {
                pairingAbandon("pair window timed out (after CHALLENGE)");
            }
            break;
        }

        case SERVER_CONNECTED:
            // Liveness driven by MagiLink TCP keepalive.
            break;
    }
}

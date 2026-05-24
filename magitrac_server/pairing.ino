// pairing.ino — Client-Server session state machine
//
// Pure ESP-NOW unicast: pairing exchanges MACs (and a now-unused secret
// kept in NVS for future signing), then sessions rely on the link-layer
// ACK for reliability and on MAC filtering for "is this our peer".  No
// per-frame HMAC, no LMK encryption.
//
// Normal operation (SERVER_STANDALONE):
//   Server always plays.  When the paired client sends MSG_CONNECT the
//   server matches the sender MAC against the stored paired MAC and
//   transitions to SERVER_CONNECTED.
//
// One-time pairing ceremony (SERVER_PAIRING):
//   A+C held 2 s → 60-second window for the client to broadcast MSG_PAIR_REQUEST.
//   Server shows a 4-digit code; operator confirms both screens match.
//   Random 16-byte secret persisted to NVS on both sides (currently unused).
//
// C button ends an active session; also cancels a pairing ceremony.

#include "midi_player.h"
#include "PairNVS.h"
#include <esp_random.h>

extern bool needsFullRedraw;   // defined in magitrac_server.ino

// ── State ─────────────────────────────────────────────────────────────────────
enum ServerMode {
    SERVER_STANDALONE,  // playing; accepts authenticated MSG_CONNECT from paired client
    SERVER_PAIRING,     // one-time ceremony window open (60 s)
    SERVER_CONNECTED,   // client active
};

static ServerMode  serverMode   = SERVER_STANDALONE;
static uint32_t    serverModeMs = 0;
       uint8_t     clientMac[6] = {};   // exposed so commands_server.ino can use it

// ── NVS-stored pairing ────────────────────────────────────────────────────────
// sStoredSecret is loaded but unused — kept on disk so packet signing can be
// re-added later without re-pairing every device.
#define SRV_NVS_NS "magitrac_srv"

static bool    sHasPairing         = false;
static uint8_t sStoredClientMac[6] = {};
static uint8_t sStoredSecret[16]   = {};

// ── Pairing ceremony ──────────────────────────────────────────────────────────
static uint8_t  sPairPendingMac[6] = {};
static uint8_t  sPairCode[4]       = {};
static uint32_t sPairingWindowMs   = 0;
static const uint32_t PAIR_WINDOW_MS = 60000;

// ── Song push (set here on connect; consumed by commandsTick in commands_server.ino)
bool sSongPushPending = false;

// ── Keepalive ─────────────────────────────────────────────────────────────────
static const uint32_t PING_INTERVAL_MS = 2000;
static const uint32_t PONG_TIMEOUT_MS  = 10000;
static uint32_t lastPingMs  = 0;
static uint32_t lastPongMs  = 0;
static bool     sCancelArmed = false;

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
    gComms.localAddr(mac);
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

// ── Init (call from setup() after gComms.begin()) ────────────────────────────
void pairingInit() {
    sHasPairing = pairNvsLoad(SRV_NVS_NS, sStoredClientMac, sStoredSecret);
    if (sHasPairing)
        Serial.printf("[PAIR] stored client: %02X:%02X:%02X:%02X:%02X:%02X\n",
            sStoredClientMac[0], sStoredClientMac[1], sStoredClientMac[2],
            sStoredClientMac[3], sStoredClientMac[4], sStoredClientMac[5]);
    else
        Serial.println("[PAIR] no stored pairing — use A+C hold to pair");
}

// ── Enter / exit ──────────────────────────────────────────────────────────────
void enterPairingMode() {
    if (serverMode != SERVER_STANDALONE) return;
    serverMode       = SERVER_PAIRING;
    serverModeMs     = millis();
    sPairingWindowMs = millis();
    BtnA.wasPressed(); BtnB.wasPressed(); BtnC.wasPressed();
    sCancelArmed = false;
    drawClientServerScreen("PAIR MODE", "Waiting...", "C: Cancel");
    Serial.println("[PAIR] pair window open (60 s)");
}

static void endSession(bool sendDisconnect) {
    if (serverMode == SERVER_CONNECTED) {
        if (sendDisconnect) {
            MsgDisconnect msg;
            msg.type = MSG_DISCONNECT;
            gComms.send(&msg, sizeof(msg));
        }
        gComms.removePeer(clientMac);  // always clean up peer (may have LMK)
    }
    serverMode = SERVER_STANDALONE;
    needsFullRedraw = true;
}

// ── Public queries ────────────────────────────────────────────────────────────
bool pairingIsActive()    { return serverMode != SERVER_STANDALONE; }
bool pairingIsConnected() { return serverMode == SERVER_CONNECTED; }

void pairingSendToClient(const void* data, size_t len) {
    if (serverMode != SERVER_CONNECTED) return;
    gComms.send(data, len);
}

// ── Handle incoming messages ─────────────────────────────────────────────────
void pairingHandleMessage(const uint8_t* data, int len) {
    if (len < 1) return;
    MagiMsgType type = (MagiMsgType)data[0];
    const uint8_t* senderMac = gComms.lastSenderAddr();

    switch (type) {

        case MSG_CONNECT:
            // Accept only from the stored paired client.  Pure ESP-NOW —
            // sender MAC is the only authentication.
            if (!sHasPairing) return;
            if (len < (int)sizeof(MsgConnect)) return;
            if (memcmp(senderMac, sStoredClientMac, 6) != 0) return;
            if (serverMode == SERVER_CONNECTED) return;
            {
                memcpy(clientMac, senderMac, 6);
                gComms.addPeer(clientMac);

                MsgConnectAck ack;
                ack.type = MSG_CONNECT_ACK;
                memset(ack.nonce, 0, sizeof(ack.nonce));   // reserved
                gComms.send(&ack, sizeof(ack));

                serverMode       = SERVER_CONNECTED;
                serverModeMs     = millis();
                lastPingMs       = millis();
                lastPongMs       = millis();
                sSongPushPending = true;
                sCancelArmed     = false;

                needsFullRedraw  = true;
                Serial.printf("[PAIR] client connected: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    clientMac[0], clientMac[1], clientMac[2],
                    clientMac[3], clientMac[4], clientMac[5]);
            }
            break;

        case MSG_PAIR_REQUEST:
            // Only accepted during the 60-second pairing window
            if (serverMode != SERVER_PAIRING) return;
            if (len < (int)sizeof(MsgPairRequest)) return;
            {
                const MsgPairRequest* req = (const MsgPairRequest*)data;
                memcpy(sPairPendingMac, req->senderMac, 6);

                // Generate 4-digit display code
                uint32_t r = esp_random();
                sPairCode[0] = '0' + (r % 10); r /= 10;
                sPairCode[1] = '0' + (r % 10); r /= 10;
                sPairCode[2] = '0' + (r % 10); r /= 10;
                sPairCode[3] = '0' + (r % 10);

                // Register temp peer so we can unicast back
                gComms.addPeer(sPairPendingMac);

                MsgPairConfirm confirm;
                confirm.type = MSG_PAIR_CONFIRM;
                memcpy(confirm.code, sPairCode, 4);
                gComms.send(&confirm, sizeof(confirm));

                char codeStr[6];
                snprintf(codeStr, sizeof(codeStr), "%c%c%c%c",
                    sPairCode[0], sPairCode[1], sPairCode[2], sPairCode[3]);
                drawClientServerScreen("Code:", codeStr, "C: Cancel");
                Serial.printf("[PAIR] sent code %s\n", codeStr);
            }
            break;

        case MSG_PAIR_ACCEPT:
            if (serverMode != SERVER_PAIRING) return;
            if (memcmp(senderMac, sPairPendingMac, 6) != 0) return;
            {
                // Generate random shared secret and persist
                uint8_t newSecret[16];
                esp_fill_random(newSecret, sizeof(newSecret));

                pairNvsSave(SRV_NVS_NS, sPairPendingMac, newSecret);
                memcpy(sStoredClientMac, sPairPendingMac, 6);
                memcpy(sStoredSecret,    newSecret,       sizeof(newSecret));
                sHasPairing = true;

                MsgPairComplete complete;
                complete.type = MSG_PAIR_COMPLETE;
                memcpy(complete.secret,    newSecret, 16);
                gComms.localAddr(complete.serverMac);
                gComms.send(&complete, sizeof(complete));

                Serial.println("[PAIR] ceremony complete");
                needsFullRedraw = true;

                // Remove temp peer; client will reconnect via MSG_CONNECT.
                gComms.removePeer(sPairPendingMac);
                serverMode   = SERVER_STANDALONE;
                serverModeMs = millis();
            }
            break;

        case MSG_PONG:
            if (serverMode != SERVER_CONNECTED) return;
            if (memcmp(senderMac, clientMac, 6) != 0) return;
            lastPongMs = millis();
            break;

        case MSG_DISCONNECT:
            if (serverMode != SERVER_CONNECTED) return;
            if (memcmp(senderMac, clientMac, 6) != 0) return;
            gComms.removePeer(clientMac);
            serverMode = SERVER_STANDALONE;
            needsFullRedraw = true;
            break;

        default:
            // Route commands from the connected client
            if (serverMode == SERVER_CONNECTED &&
                memcmp(senderMac, clientMac, 6) == 0) {
                handleCommand(type, data, len);
            }
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
            }
            // Also broadcast a pixel_post pairing beacon every 1 s while
            // the magitrac pair screen is open. Any pixel_post that's in its
            // own BOOT-hold pairing mode at the same time will capture our
            // MAC as its secondary controller.
            static uint32_t sLastPixelPostPairMs = 0;
            if (now - sLastPixelPostPairMs >= 1000) {
                sLastPixelPostPairMs = now;
                pixelpostSendPairingBeacon();
            }
            break;
        }

        case SERVER_CONNECTED:
            // Keepalive ping
            if (now - lastPingMs >= PING_INTERVAL_MS) {
                lastPingMs = now;
                uint8_t ping = (uint8_t)MSG_PING;
                gComms.send(&ping, 1);
            }
            // Pong timeout — client gone
            if (now - lastPongMs >= PONG_TIMEOUT_MS) {
                Serial.println("[PAIR] pong timeout — client gone");
                endSession(false);
            }
            break;
    }
}

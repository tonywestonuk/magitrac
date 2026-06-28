#include "ServerPairing.h"
#include "Autosave.h"
#include "NoteGrid.h"
#include "PairNVS.h"
#include "MagiCommsEspNow.h"
#include "MagiUdpLink.h"
#include "MagiLink.h"
#include "MagiMsg.h"
#include <WiFi.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <string.h>

#define CLI_NVS_NS "magitrac_cli"

// PROBE broadcast cadence while in PAIRING_REQUEST.  Both sides are on
// ch1 — no channel hopping needed.
static const uint32_t PAIR_SCAN_DWELL_MS = 250;

// ── Global instances ─────────────────────────────────────────────────────────
// Three links running on the magitrac client (AP/STA roles inverted
// 2026-05-27 — the server is now the AP):
//   • gMagiLink (TCP)          — reliable data path; STA role.  Connects
//                                to the server at 192.168.0.1:MAGI_PORT.
//   • gPairTransport (ESP-NOW) — pairing ceremony only; coexist mode.
//   • gUdpLink (UDP)           — best-effort datagrams for loss-tolerant
//                                updates (row position, preview playhead,
//                                MIDI note-in).  Listens on MAGI_PORT.
static MagiCommsEspNow  gPairTransport;
static MagiUdpLink      gUdpLink;
ServerPairing gServerPairing;

static void onUdpRecv(const uint8_t* data, int len, const IPAddress& src) {
    gServerPairing._onReceive(data, len, src);
}

static void onPairRecv(const uint8_t* data, int len) {
    gServerPairing._onPairReceive(data, len);
}

// ── begin() ───────────────────────────────────────────────────────────────────
//
// WiFi mode setup + STA association live in magitrac.ino's setup() because
// they're ordering-sensitive (must happen around display.begin()).  This
// function handles the rest: pairing-NVS load, ESP-NOW pair transport,
// UDP listener, MagiLink callback registrations.
void ServerPairing::begin() {
    if (!_songBuf) _songBuf = new uint8_t[SONG_TRANSFER_MAX]();
    if (!_instBuf) _instBuf = new uint8_t[MAX_INSTRUMENTS * sizeof(Instrument)]();
    if (_ready) return;

    // Load existing pairing (server MAC + secret).
    _hasPairing = pairNvsLoad(CLI_NVS_NS, _storedServerMac, _storedSecret);

    // ESP-NOW transport — pairing ceremony only.  Coexist mode keeps it
    // from clobbering the STA's WiFi setup.
    gPairTransport.setCoexistMode(true);
    gPairTransport.setOnReceive(onPairRecv);
    gPairTransport.begin();

    // UDP listener — bound to MAGI_PORT for SEQ_POS / MIDI_NOTE_IN /
    // PREVIEW_ROW updates from the server.
    gUdpLink.setOnReceive(onUdpRecv);
    gUdpLink.beginListener(MAGI_PORT);

    // MagiLink: register MSG_CONNECT handler.  Server (STA) initiates the
    // session handshake the moment its TCP connect succeeds; we reply
    // with ACK and transition to SUCCESS.  The session ends when
    // gMagiLink.isConnected() flips false — handled in tick().
    gMagiLink.registerCallback(MSG_CONNECT,
        [](const uint8_t* /*msg*/, size_t /*len*/, void* ctx) {
            static_cast<ServerPairing*>(ctx)->_onMagiLinkConnect();
        }, this);

    // MagiLink: MSG_DISCONNECT from the server — explicit teardown.
    // Currently never invoked (no UI calls disconnect()), but registered
    // for symmetry so the protocol works if either side ever issues it.
    gMagiLink.registerCallback(MSG_DISCONNECT,
        [](const uint8_t* /*msg*/, size_t /*len*/, void* ctx) {
            static_cast<ServerPairing*>(ctx)->_onMagiLinkDisconnect();
        }, this);

    // MagiLink: song push (HEADER + N×BODY) and NO_SONG.  All funnel
    // into the same dispatcher which switches on the id byte.
    auto songCb = [](const uint8_t* msg, size_t len, void* ctx) {
        static_cast<ServerPairing*>(ctx)->_onMagiLinkSongMessage(msg, len);
    };
    gMagiLink.registerCallback(MSG_SONG_PUSH_HEADER, songCb, this);
    gMagiLink.registerCallback(MSG_SONG_PUSH_BODY,   songCb, this);
    gMagiLink.registerCallback(MSG_NO_SONG,          songCb, this);
    gMagiLink.registerCallback(MSG_SONG_LIST_RESP,   songCb, this);

    // MagiLink: server → client play/stop notification.  Same id as the
    // outbound request controls — receivers know by direction.  Updates
    // _serverPlaying so the UI play/stop button reflects sequencer state.
    auto playStopCb = [](const uint8_t* msg, size_t /*len*/, void* ctx) {
        static_cast<ServerPairing*>(ctx)->_onMagiLinkServerState(msg[0] == MSG_PLAY);
    };
    gMagiLink.registerCallback(MSG_PLAY, playStopCb, this);
    gMagiLink.registerCallback(MSG_STOP, playStopCb, this);

    // MagiLink: instruments push (HEADER + N×BODY) from server.
    auto instCb = [](const uint8_t* msg, size_t len, void* ctx) {
        static_cast<ServerPairing*>(ctx)->_onMagiLinkInstrumentsMessage(msg, len);
    };
    gMagiLink.registerCallback(MSG_INSTRUMENTS_PUSH_HEADER, instCb, this);
    gMagiLink.registerCallback(MSG_INSTRUMENTS_PUSH_BODY,   instCb, this);

    // MagiLink: sample list response (paged).
    gMagiLink.registerCallback(MSG_SAMPLE_LIST_RESP,
        [](const uint8_t* msg, size_t len, void* ctx) {
            static_cast<ServerPairing*>(ctx)->_onMagiLinkSampleList(msg, len);
        }, this);

    // MagiLink: generic file list + load.  Load is a 3-part stream (HEADER,
    // N×BODY, EndOfData) so we route all three IDs into one dispatcher.
    gMagiLink.registerCallback(MSG_FILE_LIST_RESP,
        [](const uint8_t* msg, size_t len, void* ctx) {
            static_cast<ServerPairing*>(ctx)->_onMagiLinkFileList(msg, len);
        }, this);
    auto fileLoadCb = [](const uint8_t* msg, size_t len, void* ctx) {
        static_cast<ServerPairing*>(ctx)->_onMagiLinkFileLoad(msg, len);
    };
    gMagiLink.registerCallback(MSG_FILE_LOAD_HEADER, fileLoadCb, this);
    gMagiLink.registerCallback(MSG_FILE_LOAD_BODY,   fileLoadCb, this);
    gMagiLink.registerCallback(MSG_END_OF_DATA,      fileLoadCb, this);

    _ready = true;

    if (_hasPairing) {
        Serial.printf("[SP] stored server: %02X:%02X:%02X:%02X:%02X:%02X\n",
            _storedServerMac[0], _storedServerMac[1], _storedServerMac[2],
            _storedServerMac[3], _storedServerMac[4], _storedServerMac[5]);
        memcpy(_serverMac, _storedServerMac, 6);
        _tryAutoConnect();
    } else {
        Serial.println("[SP] no stored pairing");
    }
}

// ── Auto-connect ─────────────────────────────────────────────────────────────
//
// Post-MagiLink: the server (STA) is the one that initiates the session
// handshake — it sends MSG_CONNECT the moment its TCP connect succeeds,
// and our registered callback (set up in begin()) replies with ACK and
// transitions to SUCCESS.  So _tryAutoConnect just parks us in the
// AUTO_CONNECTING state and waits for that to happen.
void ServerPairing::_tryAutoConnect() {
    _setPairState(PairClientState::AUTO_CONNECTING);
}

// ── Pairing ceremony (over ESP-NOW; always on ch1) ──────────────────────────
//
// Client owns the WiFi credentials (via WiFiSettingsPage → magitrac_cli
// NVS).  Pairing's job is to deliver them to the server.
//
//   1. User enters pair mode → force radio to ch1, broadcast PROBE every
//      PAIR_SCAN_DWELL_MS.
//   2. Server (in its own pair mode, also on ch1) replies with CHALLENGE
//      carrying a 4-digit PIN.
//   3. _onPairReceive captures PIN + server MAC → PAIRING_CONFIRM.
//   4. User taps Confirm on the touch screen → confirmPairCode() unicasts
//      MSG_PAIR_OFFER { apMode, apSsid, apPsk } and persists the server
//      MAC + the creds we just shipped.
//   5. Server saves + reboots.  We stay up; in-place WiFi reconnect to
//      the (possibly new) AP picks the session back up via MagiLink.
void ServerPairing::startPairCeremony() {
    Serial.println("[SP] entering pair mode on ch1");
    // STA may be in a tight retry loop trying to associate with a
    // non-existent or out-of-range AP (e.g. user just changed creds on
    // the WiFi page).  That hogs the radio and makes unicast ESP-NOW
    // sends fail with "wifi:sta is connecting, return error".  Kill
    // auto-reconnect first, then disconnect, then poll until the STA
    // is actually idle (or give up after ~500 ms).
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
    esp_wifi_disconnect();
    for (int i = 0; i < 10; i++) {
        wl_status_t st = WiFi.status();
        if (st != WL_CONNECTED && st != WL_IDLE_STATUS) break;
        delay(50);
    }
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    delay(50);
    _scanLastProbeMs  = 0;
    _setPairState(PairClientState::PAIRING_REQUEST);
}

void ServerPairing::confirmPairCode() {
    if (_pairState != PairClientState::PAIRING_CONFIRM) return;

    // Load the user-configured WiFi creds + apMode.  If unset, refuse.
    char    ssid[33] = {};
    char    psk[64]  = {};
    uint8_t apMode   = 0;
    bool    haveCreds = pairNvsLoadCreds(CLI_NVS_NS, ssid, psk, &apMode);

    // SERVER_AP is the supported architecture, and the server always advertises
    // its own Magitrac_XXXX creds in the CHALLENGE.  So a client with no
    // configured apMode — e.g. a freshly NVS-erased one — defaults to SERVER_AP
    // and pairs out of the box; no WiFi-page setup needed first.  Only an
    // explicit EXTERNAL_AP choice (which persists apMode + external creds) takes
    // the other path.
    if (!haveCreds) apMode = MAGI_AP_MODE_SERVER;

    // SERVER_AP: the server owns its Magitrac_XXXX SSID/PSK — we use what
    // came in MSG_PAIR_CHALLENGE, not what the user typed on the WiFi page.
    // Re-persist into client NVS so STA can find/join the AP after reboot.
    if (apMode == MAGI_AP_MODE_SERVER) {
        if (_pairOfferedSsid[0] == '\0') {
            Serial.println("[SP] confirmPairCode: no SERVER_AP creds in challenge — abort");
            _setPairState(PairClientState::IDLE);
            return;
        }
        strncpy(ssid, _pairOfferedSsid, sizeof(ssid) - 1); ssid[sizeof(ssid) - 1] = '\0';
        strncpy(psk,  _pairOfferedPsk,  sizeof(psk)  - 1); psk [sizeof(psk)  - 1] = '\0';
        pairNvsSaveCreds(CLI_NVS_NS, ssid, psk, apMode);
        Serial.printf("[SP] adopted server-supplied SERVER_AP creds ssid='%s'\n", ssid);
    }

    MsgPairOffer offer;
    memset(&offer, 0, sizeof(offer));
    offer.type   = MSG_PAIR_OFFER;
    offer.apMode = apMode;
    strncpy(offer.apSsid, ssid, sizeof(offer.apSsid) - 1);
    strncpy(offer.apPsk,  psk,  sizeof(offer.apPsk)  - 1);

    // Retry — the WiFi radio may be in a brief scan window where
    // unicast esp_now_send is rejected.  150 ms × 6 covers most.
    bool ok = false;
    for (int attempt = 0; attempt < 6 && !ok; attempt++) {
        ok = gPairTransport.sendRaw(&offer, sizeof(offer));
        if (!ok) {
            Serial.printf("[SP-DBG] OFFER attempt %d FAIL, retrying\n", attempt);
            delay(150);
        }
    }
    Serial.printf("[SP] sent MSG_PAIR_OFFER apMode=%u size=%u (%s)\n",
                  (unsigned)apMode, (unsigned)sizeof(offer),
                  ok ? "OK" : "FAIL_ALL_RETRIES");

    // Persist server MAC for our own NVS records.
    uint8_t zeroSecret[16] = {};
    pairNvsSave(CLI_NVS_NS, _serverMac, zeroSecret);
    memcpy(_storedServerMac, _serverMac, 6);
    _hasPairing = true;
    gPairTransport.removePeer(_serverMac);

    // Server is rebooting; reconnect our STA on top of the (possibly
    // new) AP creds.  WiFi.disconnect + WiFi.begin handles both
    // "same network as before" and "switched networks" cases.
    WiFi.disconnect(false, false);
    WiFi.config(IPAddress(MAGI_CLIENT_IP_0, MAGI_CLIENT_IP_1,
                          MAGI_CLIENT_IP_2, MAGI_CLIENT_IP_3),
                IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                          MAGI_SERVER_IP_2, MAGI_SERVER_IP_3),
                IPAddress(255, 255, 255, 0));
    WiFi.begin(ssid, psk);
    WiFi.setAutoReconnect(true);

    _tryAutoConnect();
}

void ServerPairing::cancelPairing() {
    WiFi.setAutoReconnect(true);
    if (_hasPairing) {
        memcpy(_serverMac, _storedServerMac, 6);
        _tryAutoConnect();
    } else {
        _setPairState(PairClientState::IDLE);
    }
}

void ServerPairing::clearPairing() {
    pairNvsClear(CLI_NVS_NS);
    _hasPairing = false;
    memset(_storedServerMac, 0, 6);
    memset(_storedSecret,    0, 16);
}

// ── disconnect() ──────────────────────────────────────────────────────────────
void ServerPairing::disconnect() {
    if (_pairState != PairClientState::SUCCESS) return;
    MsgDisconnect msg;
    gMagiLink.acquireMutex();
    gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    _serverName[0] = '\0';
    _setBrowseState(BrowseState::IDLE);
    if (_hasPairing) _tryAutoConnect();
    else             _setPairState(PairClientState::IDLE);
}

// ── sendControl / seek / midi ─────────────────────────────────────────────────
//
// sendControl is for the empty-payload controls PLAY/STOP/PAUSE/UNPAUSE — all
// share the 3-byte {id, length} wire layout, so a single MsgPlay-shaped
// buffer carries any of them.  CANCEL_QUEUE and friends still go through
// the legacy path (Phase 3 of the migration).
bool ServerPairing::sendControl(MagiMsgType type) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPlay msg;          // any 3-byte control type shares this layout
    msg.id = (uint8_t)type;   // override NSDMI default — could be any of PLAY/STOP/PAUSE/UNPAUSE
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendSeek(uint8_t pattern, uint8_t row) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgSeek msg;
    msg.pattern = pattern;
    msg.row     = row;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendGoto(uint8_t pattern, uint8_t row) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgGoto msg;
    msg.pattern = pattern;
    msg.row     = row;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendQueueBlock(uint8_t pattern) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgQueueBlock msg;
    msg.pattern = pattern;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPixelpostEffect(uint8_t effectIdx) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostSetEffect msg;
    msg.effectIdx = effectIdx;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPixelpostSlider(uint8_t value) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostSetSlider msg;
    msg.value = value;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPixelpostTouchpad(uint8_t x, uint8_t y, bool touched) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostSetTouchpad msg;
    msg.x       = x;
    msg.y       = y;
    msg.touched = touched ? 1 : 0;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPixelpostPowerOff(bool off) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostPowerOff msg;
    msg.off = off ? 1 : 0;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPixelpostOverride(uint8_t op) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostOverride msg;
    msg.op = op;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPixelpostPostCount(uint8_t count) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostSetPostCount msg;
    msg.count = count;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPixelpostFirmwareUpdate() {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostFirmwareUpdate msg;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPixelpostFlashCtrl(uint8_t value) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostSetFlashCtrl msg;
    msg.flashCtrl = value;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendCancelQueue() {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgCancelQueue msg;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPreviewStart(uint8_t pattern, uint8_t col) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPreviewStart msg;
    msg.pattern = pattern;
    msg.col     = col;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendPreviewStop() {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPreviewStop msg;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::pollPreviewRow(uint8_t* row) {
    if (!_previewRowPending) return false;
    *row                = _previewRow;
    _previewRowPending  = false;
    return true;
}

bool ServerPairing::sendMidi(const uint8_t* bytes, uint8_t len) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (len == 0 || len > 3) return false;
    MsgMidiData msg;
    msg.midiLen = len;
    memcpy(msg.data, bytes, len);
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendSongPatch(const Song& song, const void* fieldPtr, uint8_t length) {
    // Any song mutation routes through here — that's our autosave trigger.
    markSongDirty();
    if (_pairState != PairClientState::SUCCESS) return false;
    if (length == 0 || length > SONG_PATCH_MAX) return false;
    MsgSetSongData msg;
    msg.offset  = (uint16_t)((const uint8_t*)fieldPtr - (const uint8_t*)&song);
    msg.dataLen = length;
    memcpy(msg.data, fieldPtr, length);
    // Variable-length send: override NSDMI wire length, send only the bytes
    // carrying meaningful payload (6-byte header + dataLen bytes).
    uint16_t wireLen = (uint16_t)(6 + length);
    msg.length = wireLen;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, wireLen);
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendNoteSet(const Song& song, uint8_t pattern, uint8_t row, uint8_t col) {
    markSongDirty();
    if (_pairState != PairClientState::SUCCESS) return false;
    if (pattern >= MAX_PATTERNS) return false;
    MsgNoteSet msg;
    msg.pattern = pattern;
    msg.row     = row;
    msg.col     = col;
    NoteGrid grid(song.notePool, &song.patterns[pattern].noteHead);
    msg.note = grid.get(row, col);
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendAuditionNote(uint8_t pattern, uint8_t row, uint8_t col) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgNoteAudition msg;
    msg.pattern = pattern;
    msg.row     = row;
    msg.col     = col;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendAuditionRawNote(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgAuditionRawNote msg;
    msg.channel  = channel;
    msg.note     = note;
    msg.velocity = velocity;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendAuditionProgram(uint8_t channel, uint8_t program) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgAuditionProgram msg;
    msg.channel = channel;
    msg.program = program;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendNoteSetReliable(const Song& song, uint8_t pattern, uint8_t row, uint8_t col) {
    // TCP delivery is already reliable, so this is identical to sendNoteSet now.
    // The legacy "sendReliable" had a row-keyed retransmit ring on top of
    // ESP-NOW; with MagiLink it's a no-op distinction.
    return sendNoteSet(song, pattern, row, col);
}

bool ServerPairing::sendSaveActive(const char* name, bool isAutosave) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;
    if (!name || !name[0]) return false;
    MsgSaveActive msg;
    memset(msg.name, 0, sizeof(msg.name));
    strncpy(msg.name, name, sizeof(msg.name) - 1);
    msg.is_autosave = isAutosave ? 1 : 0;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendNewSong() {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;
    MsgNewSong msg;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendSetOff() {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;
    MsgSetNoSong msg;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendSongToServer(const char* name, const Song* song) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;

    // Build the save header.  Empty name means "push to server's active
    // buffer without writing to SD" — used after in-memory edits to keep
    // the server's playback copy in sync.
    MsgSongSaveHeader hdr;
    memset(hdr.name, 0, sizeof(hdr.name));
    if (name) strncpy(hdr.name, name, sizeof(hdr.name) - 1);
    hdr.song_bytes               = sizeof(Song);
    hdr.song_file_header.magic   = SONG_FILE_MAGIC;
    hdr.song_file_header.version = SONG_FILE_VERSION;
    memset(hdr.song_file_header._pad, 0, sizeof(hdr.song_file_header._pad));

    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&hdr, sizeof(hdr));

    // Stream Song bytes in 1024-byte chunks.  Body is 1029 bytes — fits
    // in the main-loop task's 8 KB stack with headroom; declaring it
    // here scopes it to the function and avoids polluting RAM.
    MsgSongSaveBody body;
    const uint8_t* p = (const uint8_t*)song;
    uint32_t remaining = sizeof(Song);
    while (ok && remaining > 0) {
        uint16_t chunk = remaining > 1024 ? 1024 : (uint16_t)remaining;
        body.data_len = chunk;
        memcpy(body.data, p, chunk);
        ok = gMagiLink.send(&body, sizeof(body));
        p         += chunk;
        remaining -= chunk;
    }

    gMagiLink.releaseMutex();
    Serial.printf("[SP] song save: name='%s' %u bytes (%s)\n",
                  hdr.name, (unsigned)sizeof(Song), ok ? "OK" : "FAIL");
    return ok;
}

void ServerPairing::requestInstruments() {
    if (_pairState != PairClientState::SUCCESS) return;
    _instReady       = false;
    _instBufLen      = 0;
    _instChunksGot   = 0;
    _instChunksTotal = 0;
    MsgInstrumentsReq req;
    gMagiLink.acquireMutex();
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
}

bool ServerPairing::sendInstrumentPatch(const Instrument* instruments, int idx) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (idx < 0 || idx >= MAX_INSTRUMENTS) return false;
    MsgInstrumentsPatch msg;
    msg.offset  = (uint16_t)((uint32_t)idx * sizeof(Instrument));
    msg.dataLen = (uint8_t)sizeof(Instrument);
    memcpy(msg.data, &instruments[idx], sizeof(Instrument));
    uint16_t wireLen = (uint16_t)(6 + sizeof(Instrument));
    msg.length = wireLen;
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, wireLen);
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::copyInstruments(Instrument* out) const {
    if (!_instReady) return false;
    uint32_t needed = MAX_INSTRUMENTS * sizeof(Instrument);
    if (_instBufLen < needed) return false;
    memcpy(out, _instBuf, needed);
    return true;
}

void ServerPairing::resetInstruments() {
    _instReady       = false;
    _instBufLen      = 0;
    _instChunksGot   = 0;
    _instChunksTotal = 0;
}

// ── Backup / Restore ─────────────────────────────────────────────────────────

bool ServerPairing::sendRestoreFile(const char* name, bool isInstruments,
                                     const uint8_t* data, uint32_t dataLen) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;

    MsgRestoreHeader hdr;
    memset(hdr.name, 0, sizeof(hdr.name));
    if (name) strncpy(hdr.name, name, sizeof(hdr.name) - 1);
    hdr.isInstruments = isInstruments ? 1 : 0;
    hdr.total_size    = dataLen;

    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&hdr, sizeof(hdr));

    MsgRestoreBody body;
    const uint8_t* p = data;
    uint32_t remaining = dataLen;
    while (ok && remaining > 0) {
        uint16_t chunk = remaining > 1024 ? 1024 : (uint16_t)remaining;
        body.data_len = chunk;
        memcpy(body.data, p, chunk);
        ok = gMagiLink.send(&body, sizeof(body));
        p         += chunk;
        remaining -= chunk;
    }

    gMagiLink.releaseMutex();
    Serial.printf("[SP] restore '%s' %u bytes (%s)\n",
                  hdr.name, (unsigned)dataLen, ok ? "OK" : "FAIL");
    return ok;
}

bool ServerPairing::sendFileSave(uint8_t kind, const char* name,
                                 const uint8_t* data, uint32_t dataLen) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;

    MsgFileSaveHeader hdr;
    hdr.kind = kind;
    memset(hdr.name, 0, sizeof(hdr.name));
    if (name) strncpy(hdr.name, name, sizeof(hdr.name) - 1);
    hdr.total_size = dataLen;

    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&hdr, sizeof(hdr));

    MsgFileSaveBody body;
    const uint8_t* p = data;
    uint32_t remaining = dataLen;
    while (ok && remaining > 0) {
        uint16_t chunk = remaining > 1024 ? 1024 : (uint16_t)remaining;
        body.data_len = chunk;
        memcpy(body.data, p, chunk);
        ok = gMagiLink.send(&body, sizeof(body));
        p         += chunk;
        remaining -= chunk;
    }

    gMagiLink.releaseMutex();
    Serial.printf("[SP] file save kind=%u '%s' %u bytes (%s)\n",
                  (unsigned)kind, name ? name : "", (unsigned)dataLen,
                  ok ? "OK" : "FAIL");
    return ok;
}

bool ServerPairing::deleteSongOnServer(const char* name) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgSongDelete msg;
    memset(msg.name, 0, sizeof(msg.name));
    if (name) strncpy(msg.name, name, sizeof(msg.name) - 1);
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

// ── tick() ────────────────────────────────────────────────────────────────────
void ServerPairing::tick() {
    // Drain any pending UDP datagrams (row/preview/note-in updates).  No-op
    // when nothing's been received.
    gUdpLink.poll();

    uint32_t now = millis();
    switch (_pairState) {

        case PairClientState::AUTO_CONNECTING:
            // Server is the one that initiates the MagiLink handshake.
            // Nothing to retry from here — we just wait for the
            // registered MSG_CONNECT callback to fire.
            break;

        case PairClientState::REQUESTING:
            if (now - _pairStateMs >= CONNECT_TIMEOUT_MS) {
                Serial.println("[SP] connect timeout");
                if (_hasPairing)
                    _tryAutoConnect();
                else
                    _setPairState(PairClientState::IDLE);
            }
            break;

        case PairClientState::PAIRING_REQUEST: {
            // Both sides are on ch1.  Broadcast PROBE every PAIR_SCAN_DWELL_MS
            // until the server (in pair mode) replies with CHALLENGE.
            if (now - _scanLastProbeMs >= PAIR_SCAN_DWELL_MS) {
                _scanLastProbeMs = now;
                MsgPairProbe probe;
                probe.type = MSG_PAIR_PROBE;
                memcpy(probe.magic, MAGI_PAIR_MAGIC, sizeof(probe.magic));
                gPairTransport.localAddr(probe.senderMac);
                gPairTransport.sendBroadcast(&probe, sizeof(probe));
            }
            if (now - _pairStateMs >= PAIR_REQUEST_TIMEOUT_MS) {
                Serial.println("[SP] pair window timeout");
                _setPairState(PairClientState::IDLE);
            }
            break;
        }

        case PairClientState::PAIRING_CONFIRM:
            if (now - _pairStateMs >= PAIR_REQUEST_TIMEOUT_MS) {
                Serial.println("[SP] pair window timeout");
                _setPairState(PairClientState::IDLE);
            }
            break;

        case PairClientState::SUCCESS:
            // MagiLink's TCP keepalive drives liveness — when the socket
            // dies, isConnected() flips false and we go back to waiting
            // for the server to re-initiate the handshake.
            if (!gMagiLink.isConnected()) {
                Serial.println("[SP] MagiLink disconnected — back to AUTO_CONNECTING");
                _serverName[0] = '\0';
                _serverPlaying = false;
                _setBrowseState(BrowseState::IDLE);
                if (_hasPairing) _tryAutoConnect();
                else             _setPairState(PairClientState::IDLE);
            }
            break;

        default:
            break;
    }
}

// ── Server browse ──────────────────────────────────────────────────────────────
void ServerPairing::requestSongList(uint8_t page) {
    if (_pairState != PairClientState::SUCCESS) return;
    _listCount = 0;
    _listPage  = page;
    MsgSongListReq req;
    req.page = page;
    gMagiLink.acquireMutex();
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
    _setBrowseState(BrowseState::WAITING_LIST);
}

void ServerPairing::requestSongLoad(uint8_t page, uint8_t index) {
    if (_pairState != PairClientState::SUCCESS) return;
    _loadPage     = page;
    _loadIndex    = index;
    _songBufLen   = 0;
    _chunksGot    = 0;
    _chunksTotal  = 0;
    MsgSongLoadReq req;
    req.page  = page;
    req.index = index;
    gMagiLink.acquireMutex();
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
    _setBrowseState(BrowseState::WAITING_SONG);
}

void ServerPairing::requestSongLoadByName(const char* name) {
    if (_pairState != PairClientState::SUCCESS) return;
    _songBufLen   = 0;
    _chunksGot    = 0;
    _chunksTotal  = 0;
    MsgSongLoadNameReq req;
    memset(req.name, 0, sizeof(req.name));
    if (name) strncpy(req.name, name, sizeof(req.name) - 1);
    gMagiLink.acquireMutex();
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
    _setBrowseState(BrowseState::WAITING_SONG);
}

void ServerPairing::resetBrowse() {
    _setBrowseState(BrowseState::IDLE);
    _listCount   = 0;
    _songBufLen  = 0;
    _chunksGot   = 0;
    _chunksTotal = 0;
}

// ── Sample list ────────────────────────────────────────────────────────────
void ServerPairing::resetSampleList() {
    _sampleListState  = SampleListState::IDLE;
    _sampleCount      = 0;
    _sampleTotal      = 0;
    _samplePage       = 0;
    _sampleTotalPages = 0;
}

void ServerPairing::requestSampleList() {
    if (_pairState != PairClientState::SUCCESS) return;
    resetSampleList();
    MsgSampleListReq req;
    req.page = 0;
    gMagiLink.acquireMutex();
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
    _sampleListState = SampleListState::WAITING;
}

const char* ServerPairing::sampleListNameFor(uint8_t id) const {
    if (id == 0) return nullptr;
    for (int i = 0; i < _sampleCount; i++) {
        if (_sampleCache[i].id == id) return _sampleCache[i].name;
    }
    return nullptr;
}

// ── Generic file list + load ───────────────────────────────────────────────
void ServerPairing::resetFileList() {
    _fileListState      = FileListState::IDLE;
    _fileListKind       = 0;
    _fileListCount      = 0;
    _fileListPage       = 0;
    _fileListTotalPages = 0;
}

void ServerPairing::requestFileList(uint8_t kind, uint8_t page) {
    if (_pairState != PairClientState::SUCCESS) return;
    if (page == 0) resetFileList();
    _fileListKind = kind;
    MsgFileListReq req;
    req.kind = kind;
    req.page = page;
    gMagiLink.acquireMutex();
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
    _fileListState = FileListState::WAITING;
}

void ServerPairing::resetFileLoad() {
    _fileLoadState    = FileLoadState::IDLE;
    _fileLoadKind     = 0;
    _fileLoadBufLen   = 0;
    _fileLoadExpected = 0;
    _fileLoadName[0]  = '\0';
}

void ServerPairing::requestFileLoad(uint8_t kind, const char* name) {
    if (_pairState != PairClientState::SUCCESS || !name) return;
    resetFileLoad();
    _fileLoadKind = kind;
    strncpy(_fileLoadName, name, FILE_NAME_LEN - 1);
    _fileLoadName[FILE_NAME_LEN - 1] = '\0';
    MsgFileLoadReq req;
    req.kind = kind;
    memset(req.name, 0, sizeof(req.name));
    strncpy(req.name, _fileLoadName, FILE_NAME_LEN - 1);
    gMagiLink.acquireMutex();
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
    _fileLoadState = FileLoadState::WAITING_HEADER;
}

bool ServerPairing::copySong(Song* out) const {
    if (_browseState != BrowseState::SONG_READY) return false;
    const SongFileHeader* hdr = (const SongFileHeader*)_songBuf;
    if (hdr->magic != SONG_FILE_MAGIC) return false;
    if (hdr->version != SONG_FILE_VERSION) return false;
    if (_songBufLen < sizeof(SongFileHeader) + sizeof(Song)) return false;
    memcpy(out, _songBuf + sizeof(SongFileHeader), sizeof(Song));
    return true;
}

// ── Receive ───────────────────────────────────────────────────────────────────
// Receives UDP datagrams only — the TCP/MagiLink reliable path is dispatched
// directly by MagiLink-registered callbacks.  UDP delivers loss-tolerant
// updates from the server: row position, performer-MIDI passthrough, and the
// column-preview playhead.
void ServerPairing::_onReceive(const uint8_t* data, int len, const IPAddress& src) {
    if (len < 1) return;
    MagiMsgType type = (MagiMsgType)data[0];

    switch (type) {

        case MSG_SERVER_ANNOUNCE: {
            if (len < (int)sizeof(MsgServerAnnounce)) return;
            const MsgServerAnnounce* a = (const MsgServerAnnounce*)data;
            // First beacon, or address changed → record + flag
            bool changed = !_discoveredServerIPValid
                        || (_discoveredServerIP   != src)
                        || (_discoveredServerPort != a->tcpPort);
            _discoveredServerIP     = src;
            _discoveredServerPort   = a->tcpPort;
            strncpy(_discoveredServerName, a->name, MAGI_ANNOUNCE_NAME_LEN - 1);
            _discoveredServerName[MAGI_ANNOUNCE_NAME_LEN - 1] = '\0';
            _discoveredServerIPValid = true;
            if (changed) {
                Serial.printf("[SP] discovered server '%s' at %s:%u\n",
                              _discoveredServerName,
                              _discoveredServerIP.toString().c_str(),
                              (unsigned)_discoveredServerPort);
            }
            break;
        }

        case MSG_SEQ_POS:
            if (_pairState != PairClientState::SUCCESS) return;
            if (len < (int)sizeof(MsgSeqPos)) return;
            {
                const MsgSeqPos* p = (const MsgSeqPos*)data;
                _remotePattern  = p->pattern;
                _remoteRow      = p->row;
                _remotePosDirty = true;
            }
            break;

        case MSG_MIDI_NOTE_IN:
            if (_pairState != PairClientState::SUCCESS) return;
            if (len < (int)sizeof(MsgMidiNoteIn)) return;
            {
                const MsgMidiNoteIn* m = (const MsgMidiNoteIn*)data;
                _midiNoteIn        = m->midiNote;
                _midiVelocityIn    = m->velocity;
                _midiNoteInPending = true;
            }
            break;

        case MSG_PREVIEW_ROW:
            if (_pairState != PairClientState::SUCCESS) return;
            if (len < (int)sizeof(MsgPreviewRow)) return;
            {
                const MsgPreviewRow* m = (const MsgPreviewRow*)data;
                _previewRow        = m->row;
                _previewRowPending = true;
            }
            break;

        default:
            break;
    }
}

bool ServerPairing::remoteSeqPos(uint8_t* pattern, uint8_t* row) {
    if (!_remotePosDirty) return false;
    *pattern        = _remotePattern;
    *row            = _remoteRow;
    _remotePosDirty = false;
    return true;
}

bool ServerPairing::pollMidiNoteIn(uint8_t* midiNote, uint8_t* velocity) {
    if (!_midiNoteInPending) return false;
    *midiNote          = _midiNoteIn;
    if (velocity) *velocity = _midiVelocityIn;
    _midiNoteInPending = false;
    return true;
}

// ── Pair-ceremony receive callback (off the ESP-NOW transport) ──────────────
void ServerPairing::_onPairReceive(const uint8_t* data, int len) {
    if (len < 1) return;
    MagiMsgType type = (MagiMsgType)data[0];
    const uint8_t* senderMac = gPairTransport.lastSenderAddr();

    switch (type) {

        case MSG_PAIR_CHALLENGE:
            // Server's reply to our PROBE.  Lock the PIN + server MAC and
            // move to PAIRING_CONFIRM so the user can read the code and
            // tap Confirm.  Register the server as ESP-NOW peer so the
            // subsequent OFFER unicast can reach it.
            if (_pairState != PairClientState::PAIRING_REQUEST) return;
            if (len < (int)sizeof(MsgPairChallenge)) return;
            {
                const MsgPairChallenge* c = (const MsgPairChallenge*)data;
                memcpy(_pairCode, c->pin, 4);
                strncpy(_pairOfferedSsid, c->apSsid, sizeof(_pairOfferedSsid) - 1);
                _pairOfferedSsid[sizeof(_pairOfferedSsid) - 1] = '\0';
                strncpy(_pairOfferedPsk,  c->apPsk,  sizeof(_pairOfferedPsk)  - 1);
                _pairOfferedPsk[sizeof(_pairOfferedPsk) - 1] = '\0';
                memcpy(_serverMac, senderMac, 6);
                gPairTransport.addPeer(_serverMac);
                Serial.printf("[SP] got CHALLENGE PIN=%c%c%c%c ssid='%s' from %02X:%02X:%02X:%02X:%02X:%02X\n",
                    _pairCode[0], _pairCode[1], _pairCode[2], _pairCode[3],
                    _pairOfferedSsid,
                    _serverMac[0], _serverMac[1], _serverMac[2],
                    _serverMac[3], _serverMac[4], _serverMac[5]);
                _setPairState(PairClientState::PAIRING_CONFIRM);
            }
            break;

        default:
            break;
    }
}

// ── MagiLink session handshake ──────────────────────────────────────────────
//
// Called from the worker task with the transaction mutex held — handler is
// re-entrant-safe to call gMagiLink.send() (same task → mutex re-take is a
// no-op).  Server has already established TCP and just sent MsgConnect;
// we reply with MsgConnectAck and flip state to SUCCESS.
void ServerPairing::_onMagiLinkConnect() {
    MsgConnectAck ack;
    bool ok = gMagiLink.send(&ack, sizeof(ack));
    Serial.printf("[SP] MagiLink: got MSG_CONNECT, ack send=%s\n",
                  ok ? "OK" : "FAIL");
    _setPairState(PairClientState::SUCCESS);
}

void ServerPairing::_onMagiLinkDisconnect() {
    Serial.println("[SP] MagiLink: MSG_DISCONNECT from server");
    _serverName[0] = '\0';
    _serverPlaying = false;
    _setBrowseState(BrowseState::IDLE);
    if (_hasPairing) _tryAutoConnect();
    else             _setPairState(PairClientState::IDLE);
}

// ── MagiLink song messages — push header / push body / no song ──────────────
//
// Runs on the worker task with the mutex held.  The push protocol is:
//   server: HEADER(total_size) → N×BODY(data_len,data) until total_size bytes sent
//   client: HEADER → reset counters + record expected size + WAITING_SONG
//           BODY  → append data into _songBuf; when bytes == expected → SONG_READY
//
// No end marker — the header's total_size is authoritative.  Stray bodies
// (received with _songRecvExpected == 0) are dropped; oversize headers are
// rejected.
void ServerPairing::_onMagiLinkSongMessage(const uint8_t* msg, size_t len) {
    switch (msg[0]) {
        case MSG_SONG_PUSH_HEADER: {
            if (len < sizeof(MsgSongPushHeader)) return;
            const MsgSongPushHeader* h = (const MsgSongPushHeader*)msg;
            if (h->total_size > SONG_TRANSFER_MAX) {
                Serial.printf("[SP] song push too big: %u (max %u)\n",
                              (unsigned)h->total_size,
                              (unsigned)SONG_TRANSFER_MAX);
                _songRecvExpected = 0;
                return;
            }
            _songBufLen       = 0;
            _songRecvExpected = h->total_size;
            _setBrowseState(BrowseState::WAITING_SONG);
            Serial.printf("[SP] song push start: %u bytes\n",
                          (unsigned)_songRecvExpected);
            break;
        }

        case MSG_SONG_PUSH_BODY: {
            if (_songRecvExpected == 0) return;       // stray body
            if (len < sizeof(MsgSongPushBody))  return;
            const MsgSongPushBody* b = (const MsgSongPushBody*)msg;
            if (b->data_len > 1024) return;
            if (_songBufLen + b->data_len > _songRecvExpected) {
                Serial.println("[SP] song push: overshoot — dropping");
                _songRecvExpected = 0;
                return;
            }
            memcpy(_songBuf + _songBufLen, b->data, b->data_len);
            _songBufLen += b->data_len;
            if (_songBufLen == _songRecvExpected) {
                Serial.printf("[SP] song push done: %u bytes\n",
                              (unsigned)_songBufLen);
                _setBrowseState(BrowseState::SONG_READY);
                _songRecvExpected = 0;
            }
            break;
        }

        case MSG_NO_SONG:
            _noSongPending = true;
            Serial.println("[SP] NO_SONG from server");
            break;

        case MSG_SONG_LIST_RESP: {
            if (_browseState != BrowseState::WAITING_LIST) return;
            if (len < sizeof(MsgSongListResp)) return;
            const MsgSongListResp* r = (const MsgSongListResp*)msg;
            _listPage       = r->page;
            _listTotalPages = r->totalPages > 0 ? r->totalPages : 1;
            _listCount      = r->count <= SL_PER_PKT ? r->count : SL_PER_PKT;
            memset(_listNames, 0, sizeof(_listNames));
            for (int i = 0; i < _listCount; i++) {
                strncpy(_listNames[i], r->names[i], SL_NAME_LEN - 1);
                _listNames[i][SL_NAME_LEN - 1] = '\0';
            }
            _setBrowseState(BrowseState::LIST_READY);
            Serial.printf("[SP] song list page %u: %d entries\n",
                          (unsigned)_listPage, _listCount);
            break;
        }
    }
}

void ServerPairing::_onMagiLinkServerState(bool playing) {
    _serverPlaying = playing;
    Serial.printf("[SP] server sequencer %s\n", playing ? "PLAY" : "STOP");
}

// ── MagiLink sample list response ───────────────────────────────────────────
// Runs on the worker task with the mutex held.  If more pages remain we
// send the next request inline — same task already holds the mutex so the
// re-acquire is a no-op.
void ServerPairing::_onMagiLinkSampleList(const uint8_t* msg, size_t len) {
    if (_pairState != PairClientState::SUCCESS) return;
    if (len < sizeof(MsgSampleListResp)) return;
    if (_sampleListState != SampleListState::WAITING &&
        _sampleListState != SampleListState::PARTIAL) return;
    const MsgSampleListResp* r = (const MsgSampleListResp*)msg;
    _samplePage       = r->page;
    _sampleTotalPages = r->totalPages > 0 ? r->totalPages : 1;
    _sampleTotal      = r->totalEntries;

    int n = r->count <= SAMPLES_PER_PKT ? r->count : SAMPLES_PER_PKT;
    for (int i = 0; i < n && _sampleCount < SAMPLE_CACHE_MAX; i++) {
        _sampleCache[_sampleCount].id = r->entries[i].id;
        strncpy(_sampleCache[_sampleCount].name,
                r->entries[i].name, SAMPLE_NAME_LEN - 1);
        _sampleCache[_sampleCount].name[SAMPLE_NAME_LEN - 1] = '\0';
        _sampleCount++;
    }

    if (_samplePage + 1 < _sampleTotalPages) {
        _sampleListState = SampleListState::PARTIAL;
        MsgSampleListReq req;
        req.page = (uint8_t)(_samplePage + 1);
        gMagiLink.send(&req, sizeof(req));
    } else {
        _sampleListState = SampleListState::READY;
        Serial.printf("[SP] sample list ready: %d entries\n", _sampleCount);
    }
}

// ── MagiLink generic file list response ───────────────────────────────────
void ServerPairing::_onMagiLinkFileList(const uint8_t* msg, size_t len) {
    if (_pairState != PairClientState::SUCCESS) return;
    if (len < sizeof(MsgFileListResp)) return;
    if (_fileListState != FileListState::WAITING &&
        _fileListState != FileListState::PARTIAL) return;

    const MsgFileListResp* r = (const MsgFileListResp*)msg;
    if (r->kind != _fileListKind) return;   // ignore stray response

    _fileListPage       = r->page;
    _fileListTotalPages = r->totalPages > 0 ? r->totalPages : 1;

    int n = r->count <= FILE_LIST_PER_PKT ? r->count : FILE_LIST_PER_PKT;
    for (int i = 0; i < n && _fileListCount < FILE_LIST_CACHE_MAX; i++) {
        strncpy(_fileListNames[_fileListCount], r->names[i], FILE_NAME_LEN - 1);
        _fileListNames[_fileListCount][FILE_NAME_LEN - 1] = '\0';
        _fileListCount++;
    }

    if (_fileListPage + 1 < _fileListTotalPages) {
        _fileListState = FileListState::PARTIAL;
        MsgFileListReq req;
        req.kind = _fileListKind;
        req.page = (uint8_t)(_fileListPage + 1);
        gMagiLink.send(&req, sizeof(req));
    } else {
        _fileListState = FileListState::READY;
        Serial.printf("[SP] file list ready: kind=%u, %d entries\n",
                      (unsigned)_fileListKind, _fileListCount);
    }
}

// ── MagiLink generic file load: HEADER → BODY × N → EndOfData ─────────────
void ServerPairing::_onMagiLinkFileLoad(const uint8_t* msg, size_t len) {
    if (_pairState != PairClientState::SUCCESS) return;
    uint8_t id = msg[0];

    if (id == MSG_FILE_LOAD_HEADER) {
        if (_fileLoadState != FileLoadState::WAITING_HEADER) return;
        if (len < sizeof(MsgFileLoadHeader)) {
            _fileLoadState = FileLoadState::ERROR;
            return;
        }
        const MsgFileLoadHeader* h = (const MsgFileLoadHeader*)msg;
        if (h->kind != _fileLoadKind) return;
        if (!h->found) {
            _fileLoadState = FileLoadState::NOT_FOUND;
            return;
        }
        if (h->total_size > FILE_LOAD_BUF_MAX) {
            Serial.printf("[SP] file '%s' too big (%u > %u)\n",
                          _fileLoadName, (unsigned)h->total_size,
                          (unsigned)FILE_LOAD_BUF_MAX);
            _fileLoadState = FileLoadState::ERROR;
            return;
        }
        _fileLoadExpected = h->total_size;
        _fileLoadBufLen   = 0;
        _fileLoadState    = FileLoadState::WAITING_BODY;
    }
    else if (id == MSG_FILE_LOAD_BODY) {
        if (_fileLoadState != FileLoadState::WAITING_BODY) return;
        if (len < sizeof(MsgFileLoadBody)) return;
        const MsgFileLoadBody* b = (const MsgFileLoadBody*)msg;
        uint16_t n = b->data_len > 1024 ? 1024 : b->data_len;
        if (_fileLoadBufLen + n > FILE_LOAD_BUF_MAX) {
            _fileLoadState = FileLoadState::ERROR;
            return;
        }
        memcpy(_fileLoadBuf + _fileLoadBufLen, b->data, n);
        _fileLoadBufLen += n;
    }
    else if (id == MSG_END_OF_DATA) {
        // BackupRestorePage takes the MagiLink mutex and consumes EndOfData
        // directly — that path bypasses this dispatcher.  Only react if
        // we're actively expecting one.
        if (_fileLoadState != FileLoadState::WAITING_BODY) return;
        if (_fileLoadBufLen != _fileLoadExpected) {
            Serial.printf("[SP] file short read: got %u expected %u\n",
                          (unsigned)_fileLoadBufLen, (unsigned)_fileLoadExpected);
            _fileLoadState = FileLoadState::ERROR;
            return;
        }
        _fileLoadState = FileLoadState::READY;
        Serial.printf("[SP] file '%s' ready (%u bytes)\n",
                      _fileLoadName, (unsigned)_fileLoadBufLen);
    }
}

// ── MagiLink instruments push ───────────────────────────────────────────────
// Same pattern as song push but writes into _instBuf and flips _instReady.
void ServerPairing::_onMagiLinkInstrumentsMessage(const uint8_t* msg, size_t len) {
    switch (msg[0]) {
        case MSG_INSTRUMENTS_PUSH_HEADER: {
            if (!_instBuf) return;
            if (len < sizeof(MsgInstrumentsPushHeader)) return;
            const MsgInstrumentsPushHeader* h = (const MsgInstrumentsPushHeader*)msg;
            uint32_t cap = MAX_INSTRUMENTS * (uint32_t)sizeof(Instrument);
            if (h->total_size > cap) {
                Serial.printf("[SP] inst push too big: %u (max %u)\n",
                              (unsigned)h->total_size, (unsigned)cap);
                _instRecvExpected = 0;
                return;
            }
            _instReady        = false;
            _instBufLen       = 0;
            _instRecvExpected = h->total_size;
            Serial.printf("[SP] inst push start: %u bytes\n",
                          (unsigned)_instRecvExpected);
            break;
        }
        case MSG_INSTRUMENTS_PUSH_BODY: {
            if (_instRecvExpected == 0) return;
            if (len < sizeof(MsgInstrumentsPushBody)) return;
            const MsgInstrumentsPushBody* b = (const MsgInstrumentsPushBody*)msg;
            if (b->data_len > 1024) return;
            if (_instBufLen + b->data_len > _instRecvExpected) {
                Serial.println("[SP] inst push overshoot — dropping");
                _instRecvExpected = 0;
                return;
            }
            memcpy(_instBuf + _instBufLen, b->data, b->data_len);
            _instBufLen += b->data_len;
            if (_instBufLen == _instRecvExpected) {
                _instReady        = true;
                _instRecvExpected = 0;
                Serial.printf("[SP] inst push done: %u bytes\n",
                              (unsigned)_instBufLen);
            }
            break;
        }
    }
}

// ── Internal helpers ──────────────────────────────────────────────────────────
void ServerPairing::_setPairState(PairClientState s) {
    _pairState   = s;
    _pairStateMs = millis();
}

void ServerPairing::_setBrowseState(BrowseState s) {
    _browseState = s;
}

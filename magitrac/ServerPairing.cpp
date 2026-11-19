#include "ServerPairing.h"
#include "Autosave.h"
#include "NoteGrid.h"
#include "PairNVS.h"
#include "MagiCommsEspNow.h"
#include "MagiUdpLink.h"
#include "MagiLink.h"
#include "MagiMsg.h"
#include "SampleDebug.h"   // [SMPDBG] timing lines (off unless SAMPLE_DEBUG is 1)
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

// Guards _recStatus between the link worker (writer) and the UI task.
static portMUX_TYPE sRecStatusMux = portMUX_INITIALIZER_UNLOCKED;

#define CLI_NVS_NS "magitrac_cli"

// PROBE broadcast cadence while in PAIRING_REQUEST.  Both sides are on
// ch1 — no channel hopping needed.
static const uint32_t PAIR_SCAN_DWELL_MS = 250;

// ── Global instances ─────────────────────────────────────────────────────────
// Three links running on the magitrac client.  The client is always a WiFi
// STA — of the server's own softAP (SERVER_AP mode) or of a third-party AP
// that both devices join (EXTERNAL_AP mode):
//   • gMagiLink (TCP)          — reliable data path; we are the connecting
//                                side, the server listens on MAGI_PORT.
//                                Started from magitrac.ino, not here.
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
    // List caches to PSRAM (ps_malloc forces it — under the 4 KB new→PSRAM
    // threshold).  Control-rate, and guarded null-safe at the population sites.
    if (!_sampleCache)
        _sampleCache = (SampleCacheEntry*)ps_malloc(SAMPLE_CACHE_MAX * sizeof(SampleCacheEntry));
    if (!_fileListNames)
        _fileListNames = (char(*)[FILE_NAME_LEN])ps_malloc(FILE_LIST_CACHE_MAX * FILE_NAME_LEN);
    // File-load buffer too (24 KB) — inside the global object it sat in the
    // internal RAM the radios need.  If this fails, a file load ends in ERROR.
    if (!_fileLoadBuf)
        _fileLoadBuf = (uint8_t*)ps_malloc(FILE_LOAD_BUF_MAX);
    if (!_fileLoadBuf)
        Serial.println("[SP] no PSRAM for the file-load buffer — file loads will fail");
    if (_ready) return;

    // Load existing pairing (server MAC + secret).
    _hasPairing = pairNvsLoad(CLI_NVS_NS, _storedServerMac, _storedSecret);

    // ESP-NOW transport — pairing ceremony only.  Coexist mode keeps it
    // from clobbering the STA's WiFi setup.
    gPairTransport.setCoexistMode(true);
    gPairTransport.setOnReceive(onPairRecv);
    gPairTransport.begin();

    // UDP listener — bound to MAGI_PORT for SEQ_POS / MIDI_NOTE_IN /
    // PREVIEW_ROW updates and the SERVER_ANNOUNCE discovery beacon.
    gUdpLink.setOnReceive(onUdpRecv);
    gUdpLink.beginListener(MAGI_PORT);

    // MagiLink: register MSG_CONNECT handler.  We open the TCP connection;
    // the server starts the session handshake by sending MSG_CONNECT as
    // soon as it has accepted it.  We reply with ACK and transition to
    // SUCCESS.  The session ends when gMagiLink.isConnected() flips false —
    // handled in tick().
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
    gMagiLink.registerCallback(MSG_ACTIVE_SONG_CHANGED, songCb, this);
    gMagiLink.registerCallback(MSG_NO_SONG,          songCb, this);
    gMagiLink.registerCallback(MSG_SONG_LOAD_FAILED, songCb, this);
    gMagiLink.registerCallback(MSG_SONG_LIST_RESP,   songCb, this);
    gMagiLink.registerCallback(MSG_SAVE_RESULT,      songCb, this);

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

    // MagiLink: sample editor info — kind 0 = waveform overview, kind 1..N =
    // spectrogram pages (accumulated here so back-to-back pages can't race
    // the main loop's consumption of a single pending slot).
    gMagiLink.registerCallback(MSG_SAMPLE_INFO,
        [](const uint8_t* msg, size_t len, void* ctx) {
            ServerPairing* sp = static_cast<ServerPairing*>(ctx);
            if (len < sizeof(MsgSampleInfo)) return;
            const MsgSampleInfo* mi = (const MsgSampleInfo*)msg;
            if (mi->kind == 0) {
                SMPDBG("link: waveform for id=%u arrived from the server",
                       (unsigned)mi->sampleId);
                memcpy(&sp->_sampleInfo, msg, sizeof(MsgSampleInfo));
                sp->_sampleInfoPending = true;
            } else if (mi->kind <= SAMPLE_SPEC_PAGES) {
                if (!sp->_specData)
                    sp->_specData = (uint8_t*)ps_malloc(SAMPLE_SPEC_BYTES);
                if (!sp->_specData) return;   // no PSRAM → popup never completes
                memcpy(sp->_specData + (mi->kind - 1) * SAMPLE_SPEC_PAGE_BYTES,
                       mi->peaks, SAMPLE_SPEC_PAGE_BYTES);
                sp->_specMask |= (uint8_t)(1u << (mi->kind - 1));
            }
        }, this);

    // MagiLink: ADD SAMPLE statuses (level while recording, the take, the
    // result of a save).  The page takes the newest; any in between are
    // superseded.
    gMagiLink.registerCallback(MSG_SAMPLE_REC_STATUS,
        [](const uint8_t* msg, size_t len, void* ctx) {
            ServerPairing* sp = static_cast<ServerPairing*>(ctx);
            if (len < sizeof(MsgSampleRecStatus)) return;
            portENTER_CRITICAL(&sRecStatusMux);
            memcpy(&sp->_recStatus, msg, sizeof(MsgSampleRecStatus));
            sp->_recStatusPending = true;
            portEXIT_CRITICAL(&sRecStatusMux);
        }, this);

    // MagiLink: AUTO TUNE's answer (sample editor's TUNE dialog).
    gMagiLink.registerCallback(MSG_SAMPLE_TUNE,
        [](const uint8_t* msg, size_t len, void* ctx) {
            ServerPairing* sp = static_cast<ServerPairing*>(ctx);
            if (len < sizeof(MsgSampleTune)) return;
            portENTER_CRITICAL(&sRecStatusMux);
            memcpy(&sp->_sampleTune, msg, sizeof(MsgSampleTune));
            sp->_sampleTunePending = true;
            portEXIT_CRITICAL(&sRecStatusMux);
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
// Nothing is actually "tried" here.  MagiLink's worker makes (and remakes)
// the TCP connection on its own; the server then sends MSG_CONNECT on the
// new socket, and our registered callback (set up in begin()) replies with
// ACK and transitions to SUCCESS.  So _tryAutoConnect just parks us in the
// AUTO_CONNECTING state and waits for that to happen.
void ServerPairing::_tryAutoConnect() {
    _setPairState(PairClientState::AUTO_CONNECTING);
}

// ── Pairing ceremony (over ESP-NOW; always on ch1) ──────────────────────────
//
// Pairing's job is to get both devices onto the same WiFi network.  In
// SERVER_AP mode (the default) that network is the server's own softAP: its
// SSID/PSK arrive in the CHALLENGE and we echo them back.  In EXTERNAL_AP
// mode it is the network the user entered on WiFiSettingsPage (magitrac_cli
// NVS), which we deliver to the server.
//
//   1. User enters pair mode → force radio to ch1, broadcast PROBE every
//      PAIR_SCAN_DWELL_MS.
//   2. Server (in its own pair mode, also on ch1) replies with CHALLENGE
//      carrying a 4-digit PIN + its softAP SSID/PSK.
//   3. _onPairReceive captures PIN + creds + server MAC → PAIRING_CONFIRM.
//   4. User taps Confirm on the touch screen → confirmPairCode() unicasts
//      MSG_PAIR_OFFER { apMode, apSsid, apPsk } and, only once that send has
//      succeeded, persists the server MAC (and, in SERVER_AP mode, the creds
//      we just adopted).  If the send fails we stay in PAIRING_CONFIRM with
//      nothing saved, and the user can tap Confirm again.
//   5. Server saves + reboots.  We stay up and re-join WiFi in place; the
//      session comes back via MagiLink once the server is listening again.
//      NB: MagiLink::beginConnect() is one-shot, so this only works if the
//      link was never started (magitrac.ino starts it from the server's
//      announce beacon) or was already aimed at the address the server
//      comes back on.  A changed server address needs a client reboot.
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

    // Load the user-configured WiFi creds + apMode.  If unset, fall back to
    // the SERVER_AP default below.
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
    // (They are written to client NVS further down, once the OFFER is out.)
    if (apMode == MAGI_AP_MODE_SERVER) {
        if (_pairOfferedSsid[0] == '\0') {
            Serial.println("[SP] confirmPairCode: no SERVER_AP creds in challenge — abort");
            _leavePairMode();
            return;
        }
        strncpy(ssid, _pairOfferedSsid, sizeof(ssid) - 1); ssid[sizeof(ssid) - 1] = '\0';
        strncpy(psk,  _pairOfferedPsk,  sizeof(psk)  - 1); psk [sizeof(psk)  - 1] = '\0';
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

    // The server never got the offer, so it is not going to switch networks.
    // Commit nothing — no NVS write, no WiFi change — and stay in
    // PAIRING_CONFIRM (peer still registered, pair-window clock still running
    // from the CHALLENGE) so the user can tap Confirm again.
    if (!ok) {
        Serial.println("[SP] OFFER not delivered — nothing saved, still waiting for Confirm");
        return;
    }

    // Offer is out: now commit.  SERVER_AP creds go to client NVS so the STA
    // can find/join the server's AP after a reboot.
    if (apMode == MAGI_AP_MODE_SERVER) {
        pairNvsSaveCreds(CLI_NVS_NS, ssid, psk, apMode);
        Serial.printf("[SP] adopted server-supplied SERVER_AP creds ssid='%s'\n", ssid);
    }

    // Persist server MAC for our own NVS records.
    uint8_t zeroSecret[16] = {};
    pairNvsSave(CLI_NVS_NS, _serverMac, zeroSecret);
    memcpy(_storedServerMac, _serverMac, 6);
    _hasPairing = true;
    gPairTransport.removePeer(_serverMac);

    // Server is rebooting; reconnect our STA on top of the (possibly
    // new) AP creds.  WiFi.disconnect + WiFi.begin handles both
    // "same network as before" and "switched networks" cases.
    // NB (review note, behaviour unchanged): the static MAGI_CLIENT_IP
    // config below is applied whatever apMode is, whereas setup() in
    // magitrac.ino leaves an EXTERNAL_AP client on DHCP — so an EXTERNAL_AP
    // pairing runs on the static address until the next reboot.
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
    _leavePairMode();
}

// Every way out of pair mode that did not produce a new pairing comes through
// here.  startPairCeremony() switched WiFi auto-reconnect off; put it back, and
// return to the stored pairing if there is one.
void ServerPairing::_leavePairMode() {
    WiFi.setAutoReconnect(true);
    if (_hasPairing) {
        memcpy(_serverMac, _storedServerMac, 6);
        _tryAutoConnect();
    } else {
        _setPairState(PairClientState::IDLE);
    }
}

// ── One complete message, sent under the link mutex ──────────────────────────
// The common case: take the mutex, send one frame, give it back.  Multi-frame
// streams (song / restore / file save) hold the mutex across the whole stream
// themselves and do not use this.
bool ServerPairing::_sendLocked(const void* msg, size_t len) {
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(msg, len);
    gMagiLink.releaseMutex();
    return ok;
}

// ── disconnect() ──────────────────────────────────────────────────────────────
void ServerPairing::disconnect() {
    if (_pairState != PairClientState::SUCCESS) return;
    MsgDisconnect msg;
    _sendLocked(&msg, sizeof(msg));
    _setBrowseState(BrowseState::IDLE);
    if (_hasPairing) _tryAutoConnect();
    else             _setPairState(PairClientState::IDLE);
}

// ── sendControl / seek / midi ─────────────────────────────────────────────────
//
// sendControl is for the empty-payload controls PLAY/STOP/PAUSE/UNPAUSE — all
// share the 3-byte {id, length} wire layout, so a single MsgPlay-shaped
// buffer carries any of them.
bool ServerPairing::sendControl(MagiMsgType type) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPlay msg;          // any 3-byte control type shares this layout
    msg.id = (uint8_t)type;   // override NSDMI default — could be any of PLAY/STOP/PAUSE/UNPAUSE
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendSeek(uint8_t pattern, uint8_t row) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgSeek msg;
    msg.pattern = pattern;
    msg.row     = row;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendGoto(uint8_t pattern, uint8_t row) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgGoto msg;
    msg.pattern = pattern;
    msg.row     = row;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendQueueBlock(uint8_t pattern) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgQueueBlock msg;
    msg.pattern = pattern;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendPixelpostEffect(uint8_t effectIdx) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostSetEffect msg;
    msg.effectIdx = effectIdx;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendPixelpostSlider(uint8_t value) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostSetSlider msg;
    msg.value = value;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendPixelpostTouchpad(uint8_t x, uint8_t y, bool touched) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostSetTouchpad msg;
    msg.x       = x;
    msg.y       = y;
    msg.touched = touched ? 1 : 0;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendPixelpostPowerOff(bool off) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostPowerOff msg;
    msg.off = off ? 1 : 0;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendPixelpostOverride(uint8_t op) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostOverride msg;
    msg.op = op;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendOrgan(uint8_t op, uint8_t index, uint8_t value) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgOrgan msg;
    msg.op    = op;
    msg.index = index;
    msg.value = value;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendPixelpostFirmwareUpdate() {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostFirmwareUpdate msg;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendPixelpostFlashCtrl(uint8_t value) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPixelpostSetFlashCtrl msg;
    msg.flashCtrl = value;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendCancelQueue() {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgCancelQueue msg;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendPreviewStart(uint8_t pattern, uint8_t col) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPreviewStart msg;
    msg.pattern = pattern;
    msg.col     = col;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendPreviewStop() {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPreviewStop msg;
    return _sendLocked(&msg, sizeof(msg));
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
    return _sendLocked(&msg, sizeof(msg));
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
    return _sendLocked(&msg, wireLen);
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
    // get() hands back all-zero for a cell that isn't there, but all-zero is
    // not "empty" to NoteGrid::set on the far side (velocity 0 is an explicit
    // value — a PixelPost "slider 0" row).  Send the real empty encoding, or
    // every cleared cell is stored on the server as a phantom note.
    if (!grid.has(row, col)) msg.note.velocity = VEL_DEFAULT;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendAuditionNote(uint8_t pattern, uint8_t row, uint8_t col) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgNoteAudition msg;
    msg.pattern = pattern;
    msg.row     = row;
    msg.col     = col;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendAuditionRawNote(uint8_t channel, uint8_t note, uint8_t velocity,
                                        uint8_t col) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgAuditionRawNote msg;
    msg.channel  = channel;
    msg.note     = note;
    msg.velocity = velocity;
    msg.col      = col;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendSampleEdit(uint8_t op, uint8_t sampleId,
                                   uint32_t startFrame, uint32_t endFrame,
                                   uint8_t loop, int16_t tuneCents,
                                   uint8_t note, uint8_t velOff, uint8_t keepLen) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgSampleEdit msg;
    msg.op         = op;
    msg.sampleId   = sampleId;
    msg.startFrame = startFrame;
    msg.endFrame   = endFrame;
    msg.loop       = loop;
    msg.tuneCents  = tuneCents;
    msg.note       = note;
    msg.velOff     = velOff;
    msg.keepLen    = keepLen;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendSampleRec(uint8_t op, const char* name) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgSampleRec msg;
    msg.op = op;
    memset(msg.name, 0, sizeof(msg.name));
    if (name) strncpy(msg.name, name, sizeof(msg.name) - 1);
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::takeSampleTune(MsgSampleTune& out) {
    bool got = false;
    portENTER_CRITICAL(&sRecStatusMux);
    if (_sampleTunePending) {
        out = _sampleTune;
        _sampleTunePending = false;
        got = true;
    }
    portEXIT_CRITICAL(&sRecStatusMux);
    return got;
}

bool ServerPairing::takeSampleRecStatus(MsgSampleRecStatus& out) {
    bool got = false;
    portENTER_CRITICAL(&sRecStatusMux);
    if (_recStatusPending) {
        out = _recStatus;
        _recStatusPending = false;
        got = true;
    }
    portEXIT_CRITICAL(&sRecStatusMux);
    return got;
}

bool ServerPairing::sendAuditionProgram(uint8_t channel, uint8_t program) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgAuditionProgram msg;
    msg.channel = channel;
    msg.program = program;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendSaveActive(const char* name, bool isAutosave) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;
    if (!name || !name[0]) return false;
    MsgSaveActive msg;
    memset(msg.name, 0, sizeof(msg.name));
    strncpy(msg.name, name, sizeof(msg.name) - 1);
    msg.is_autosave = isAutosave ? 1 : 0;
    // Drop any stale result under the mutex, before the frame goes out: the
    // worker cannot dispatch the server's MSG_SAVE_RESULT until we release.
    gMagiLink.acquireMutex();
    _saveResultPending = false;
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendNewSong() {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;
    MsgNewSong msg;
    return _sendLocked(&msg, sizeof(msg));
}

bool ServerPairing::sendSetOff() {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;
    MsgSetNoSong msg;
    return _sendLocked(&msg, sizeof(msg));
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
    // A named save is answered with MSG_SAVE_RESULT — drop any stale result
    // first (see sendSaveActive).  An un-named push gets no reply, so it must
    // not wipe a result the UI has not read yet.
    if (hdr.name[0] != '\0') _saveResultPending = false;
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
    MsgInstrumentsReq req;
    _sendLocked(&req, sizeof(req));
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
    return _sendLocked(&msg, wireLen);
}

// Same locking contract as copySong() — do not call with the mutex held.
bool ServerPairing::copyInstruments(Instrument* out) const {
    gMagiLink.acquireMutex();
    bool ok = false;
    uint32_t needed = MAX_INSTRUMENTS * sizeof(Instrument);
    if (_instReady && _instBufLen >= needed) {
        memcpy(out, _instBuf, needed);
        ok = true;
    }
    gMagiLink.releaseMutex();
    return ok;
}

void ServerPairing::resetInstruments() {
    _instReady       = false;
    _instBufLen      = 0;
}

// ── Backup / Restore ─────────────────────────────────────────────────────────

bool ServerPairing::sendRestoreFile(const char* name, bool isInstruments,
                                     fs::File& f, uint32_t size) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (!gMagiLink.isConnected()) return false;

    MsgRestoreHeader hdr;
    memset(hdr.name, 0, sizeof(hdr.name));
    if (name) strncpy(hdr.name, name, sizeof(hdr.name) - 1);
    hdr.isInstruments = isInstruments ? 1 : 0;
    hdr.total_size    = size;

    MsgRestoreBody body;

    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&hdr, sizeof(hdr));
    bool readOk = true;
    uint32_t remaining = size;
    while (ok && remaining > 0) {
        uint16_t chunk = remaining > sizeof(body.data) ? sizeof(body.data) : (uint16_t)remaining;
        if (f.read(body.data, chunk) != chunk) {
            // The server is owed `remaining` more bytes; it keeps waiting and
            // drops the half-file when the next upload's header arrives.
            readOk = false;
            break;
        }
        body.data_len = chunk;
        ok = gMagiLink.send(&body, sizeof(body));
        remaining -= chunk;
    }
    gMagiLink.releaseMutex();

    Serial.printf("[SP] restore '%s' %u bytes (%s)\n", hdr.name, (unsigned)size,
                  !readOk ? "SD READ FAILED" : ok ? "OK" : "FAIL");
    return ok && readOk;
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
    return _sendLocked(&msg, sizeof(msg));
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
                _leavePairMode();
            }
            break;
        }

        case PairClientState::PAIRING_CONFIRM:
            if (now - _pairStateMs >= PAIR_REQUEST_TIMEOUT_MS) {
                Serial.println("[SP] pair window timeout");
                _leavePairMode();
            }
            break;

        case PairClientState::SUCCESS:
            // MagiLink's TCP keepalive drives liveness — when the socket
            // dies, isConnected() flips false and we go back to waiting
            // for the server to re-initiate the handshake.
            if (!gMagiLink.isConnected()) {
                Serial.println("[SP] MagiLink disconnected — back to AUTO_CONNECTING");
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

// ── Requests that wait for a reply ─────────────────────────────────────────────
//
// The reply is handled on the MagiLink worker task, and each handler drops it
// unless the matching WAITING state is already set.  So every request below
// resets its counters and sets WAITING *before* the frame goes out, all under
// the link mutex: the worker cannot dispatch anything until we release, by
// which time the state is in place however fast the server answers.  (As
// before, the state is set whether or not the send succeeds.)

// ── Server browse ──────────────────────────────────────────────────────────────
void ServerPairing::requestSongList(uint8_t page) {
    if (_pairState != PairClientState::SUCCESS) return;
    MsgSongListReq req;
    req.page = page;
    gMagiLink.acquireMutex();
    _listCount = 0;
    _listPage  = page;
    _setBrowseState(BrowseState::WAITING_LIST);
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
}

void ServerPairing::requestSongLoad(uint8_t page, uint8_t index) {
    if (_pairState != PairClientState::SUCCESS) return;
    MsgSongLoadReq req;
    req.page  = page;
    req.index = index;
    gMagiLink.acquireMutex();
    _songBufLen      = 0;
    _loadFailPending = false;
    _setBrowseState(BrowseState::WAITING_SONG);
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
}

void ServerPairing::requestSongLoadByName(const char* name) {
    if (_pairState != PairClientState::SUCCESS) return;
    MsgSongLoadNameReq req;
    memset(req.name, 0, sizeof(req.name));
    if (name) strncpy(req.name, name, sizeof(req.name) - 1);
    gMagiLink.acquireMutex();
    _songBufLen      = 0;
    _loadFailPending = false;
    _setBrowseState(BrowseState::WAITING_SONG);
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
}

const char* ServerPairing::loadFailText() const {
    switch (_loadFailReason) {
        case SONG_LOAD_FAIL_NOT_FOUND: return "NO FILE";
        case SONG_LOAD_FAIL_READ:      return "READ ERR";
        case SONG_LOAD_FAIL_BAD_FILE:  return "BAD FILE";
        case SONG_LOAD_FAIL_NO_MEMORY: return "NO MEMORY";
        default:                       return "UNKNOWN";
    }
}

// ── Save result (MSG_SAVE_RESULT) ────────────────────────────────────────────
bool ServerPairing::saveResultPending() const {
    return _saveResultPending;
}

bool ServerPairing::saveResultOk() const {
    return _saveResultOk;
}

bool ServerPairing::saveResultIsAutosave() const {
    return _saveResultIsAutosave;
}

const char* ServerPairing::saveResultName() const {
    return _saveResultName;
}

void ServerPairing::clearSaveResult() {
    _saveResultPending = false;
}

const char* ServerPairing::pushedSongFile() const {
    return _pushedSongFile;
}

void ServerPairing::resetBrowse() {
    _setBrowseState(BrowseState::IDLE);
    _listCount   = 0;
    _songBufLen  = 0;
}

// ── Sample list ────────────────────────────────────────────────────────────
void ServerPairing::resetSampleList() {
    _sampleListState  = SampleListState::IDLE;
    _sampleCount      = 0;
    _samplePage       = 0;
    _sampleTotalPages = 0;
}

void ServerPairing::requestSampleList() {
    if (_pairState != PairClientState::SUCCESS) return;
    MsgSampleListReq req;
    req.page = 0;
    gMagiLink.acquireMutex();
    resetSampleList();
    _sampleListState = SampleListState::WAITING;
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
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
    MsgFileListReq req;
    req.kind = kind;
    req.page = page;
    gMagiLink.acquireMutex();
    if (page == 0) resetFileList();
    _fileListKind  = kind;
    _fileListState = FileListState::WAITING;
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
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
    MsgFileLoadReq req;
    req.kind = kind;
    memset(req.name, 0, sizeof(req.name));
    strncpy(req.name, name, FILE_NAME_LEN - 1);
    gMagiLink.acquireMutex();
    resetFileLoad();
    _fileLoadKind = kind;
    strncpy(_fileLoadName, req.name, FILE_NAME_LEN - 1);
    _fileLoadName[FILE_NAME_LEN - 1] = '\0';
    _fileLoadState = FileLoadState::WAITING_HEADER;
    gMagiLink.send(&req, sizeof(req));
    gMagiLink.releaseMutex();
}

// Callers must NOT hold the link mutex (UI-poll helper; it takes and
// releases the mutex itself).  The lock makes the state-check + copy atomic
// against the worker task: with back-to-back song streams (connect pull +
// changed-event pull), a second stream's HEADER/BODY could otherwise start
// overwriting _songBuf mid-memcpy and hand the caller a torn song.
bool ServerPairing::copySong(Song* out) const {
    gMagiLink.acquireMutex();
    bool ok = false;
    if (_browseState == BrowseState::SONG_READY &&
        _songBufLen >= sizeof(SongFileHeader) + sizeof(Song)) {
        const SongFileHeader* hdr = (const SongFileHeader*)_songBuf;
        if (hdr->magic == SONG_FILE_MAGIC && hdr->version == SONG_FILE_VERSION) {
            memcpy(out, _songBuf + sizeof(SongFileHeader), sizeof(Song));
            ok = true;
        }
    }
    gMagiLink.releaseMutex();
    return ok;
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
// no-op).  Our TCP connection is up and the server has just sent MsgConnect
// on it; we reply with MsgConnectAck and flip state to SUCCESS.
void ServerPairing::_onMagiLinkConnect() {
    MsgConnectAck ack;
    bool ok = gMagiLink.send(&ack, sizeof(ack));
    Serial.printf("[SP] MagiLink: got MSG_CONNECT, ack send=%s\n",
                  ok ? "OK" : "FAIL");
    _setPairState(PairClientState::SUCCESS);
    // Pull the active song as our first command.  This replaces the old
    // server push-on-connect: the server never volunteers the song stream,
    // we ask for it — which also makes every reconnect recover the active
    // song through the ordinary request path.
    if (ok) {
        MsgActiveSongReq req;
        gMagiLink.send(&req, sizeof(req));
    }
}

void ServerPairing::_onMagiLinkDisconnect() {
    Serial.println("[SP] MagiLink: MSG_DISCONNECT from server");
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
            memcpy(_pushIncomingFile, h->name, sizeof(_pushIncomingFile));
            _pushIncomingFile[sizeof(_pushIncomingFile) - 1] = '\0';
            _setBrowseState(BrowseState::WAITING_SONG);
            Serial.printf("[SP] song push start: %u bytes, file '%s'\n",
                          (unsigned)_songRecvExpected, _pushIncomingFile);
            break;
        }

        case MSG_SONG_PUSH_BODY: {
            if (_songRecvExpected == 0) return;       // stray body
            // Bodies arrive truncated to header + data_len bytes (server
            // shrinks frames for RF robustness) — validate against
            // data_len, not the full struct size.
            if (len < offsetof(MsgSongPushBody, data)) return;
            const MsgSongPushBody* b = (const MsgSongPushBody*)msg;
            if (b->data_len > 1024 ||
                len < offsetof(MsgSongPushBody, data) + b->data_len) return;
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
                // Name first, then SONG_READY — the UI reads it on that edge.
                memcpy(_pushedSongFile, _pushIncomingFile, sizeof(_pushedSongFile));
                _pushedSongFile[sizeof(_pushedSongFile) - 1] = '\0';
                _setBrowseState(BrowseState::SONG_READY);
                _songRecvExpected = 0;
            }
            break;
        }

        case MSG_NO_SONG:
            _noSongPending = true;
            Serial.println("[SP] NO_SONG from server");
            break;

        case MSG_SONG_LOAD_FAILED: {
            // The server could not serve our load and has kept whatever it
            // had active — so nothing here touches the current song.  Stop
            // waiting; the page that asked picks the flag up and says why.
            if (len < sizeof(MsgSongLoadFailed)) return;
            _loadFailReason   = ((const MsgSongLoadFailed*)msg)->reason;
            _loadFailPending  = true;
            _songRecvExpected = 0;
            if (_browseState == BrowseState::WAITING_SONG)
                _setBrowseState(BrowseState::IDLE);
            Serial.printf("[SP] load failed on server: %s (reason %u)\n",
                          loadFailText(), (unsigned)_loadFailReason);
            break;
        }

        case MSG_ACTIVE_SONG_CHANGED: {
            // Server-side event (song loaded on the server screen, restore
            // reload).  Tiny notification — we pull the actual stream.
            Serial.println("[SP] active song changed — pulling");
            MsgActiveSongReq req;
            gMagiLink.send(&req, sizeof(req));
            break;
        }

        case MSG_SAVE_RESULT: {
            // Fields first, flag last — the UI task reads them once it sees
            // the flag (see ServerPairing.h).
            if (len < sizeof(MsgSaveResult)) return;
            const MsgSaveResult* r = (const MsgSaveResult*)msg;
            _saveResultOk         = (r->ok != 0);
            _saveResultIsAutosave = (r->is_autosave != 0);
            memcpy(_saveResultName, r->name, sizeof(_saveResultName));
            _saveResultName[sizeof(_saveResultName) - 1] = '\0';
            _saveResultPending    = true;
            Serial.printf("[SP] save result: '%s' %s%s\n", _saveResultName,
                          _saveResultOk ? "OK" : "FAILED",
                          _saveResultIsAutosave ? " (autosave)" : "");
            break;
        }

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

    int n = r->count <= SAMPLES_PER_PKT ? r->count : SAMPLES_PER_PKT;
    for (int i = 0; i < n && _sampleCount < SAMPLE_CACHE_MAX && _sampleCache; i++) {
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
    for (int i = 0; i < n && _fileListCount < FILE_LIST_CACHE_MAX && _fileListNames; i++) {
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
        // A reply for a different file is the late answer to a request we
        // already timed out of — its body must not be taken for this one.
        // (State stays WAITING_HEADER, so its BODY / EndOfData are ignored.)
        if (strncmp(h->name, _fileLoadName, FILE_NAME_LEN) != 0) {
            Serial.printf("[SP] ignoring late file reply '%.47s' (waiting for '%s')\n",
                          h->name, _fileLoadName);
            return;
        }
        if (!h->found) {
            _fileLoadState = FileLoadState::NOT_FOUND;
            return;
        }
        if (!_fileLoadBuf) {
            Serial.println("[SP] file load: no buffer (PSRAM alloc failed at boot)");
            _fileLoadState = FileLoadState::ERROR;
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
        if (!_fileLoadBuf) {
            _fileLoadState = FileLoadState::ERROR;
            return;
        }
        // Bodies may arrive truncated to header + data_len bytes (as the
        // song-push bodies do) or full-size — validate against data_len, not
        // the full struct size.  A frame that fails is dropped, and the byte
        // count check at EndOfData then ends the load in ERROR.
        if (len < offsetof(MsgFileLoadBody, data)) return;
        const MsgFileLoadBody* b = (const MsgFileLoadBody*)msg;
        if (b->data_len > sizeof(b->data) ||
            len < offsetof(MsgFileLoadBody, data) + b->data_len) return;
        uint16_t n = b->data_len;
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
            // Truncated frames — see MSG_SONG_PUSH_BODY.
            if (len < offsetof(MsgInstrumentsPushBody, data)) return;
            const MsgInstrumentsPushBody* b = (const MsgInstrumentsPushBody*)msg;
            if (b->data_len > 1024 ||
                len < offsetof(MsgInstrumentsPushBody, data) + b->data_len) return;
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

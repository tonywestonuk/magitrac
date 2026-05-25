#include "ServerPairing.h"
#include "NoteGrid.h"
#include "PairNVS.h"
#include "MagiCommsEspNow.h"
#include "MagiCommsTcp.h"
#include "MagiUdpLink.h"
#include "MagiLink.h"
#include "MagiMsg.h"          // magiWifiChannelFromIdx()
#include "SettingsPage.h"     // extern gWifiChannelIdx
#include <WiFi.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <string.h>

#define CLI_NVS_NS "magitrac_cli"

// ── Global instances ─────────────────────────────────────────────────────────
// Three links running simultaneously on the magitrac client:
//   • gTransport (TCP)     — main reliable data path; AP role at 192.168.0.1.
//   • gPairTransport (ESP-NOW) — pairing ceremony only; coexist mode.
//   • gUdpLink (UDP)       — best-effort datagrams for loss-tolerant updates
//                            (row position, preview playhead, MIDI note-in).
//                            Listener bound to MAGI_PORT alongside the TCP
//                            listener (TCP/UDP port namespaces are disjoint).
// Only the TCP transport is wrapped in MagiComms — pairing code and UDP
// callers go through their own direct APIs.
MagiCommsTcp            gTransport;          // exposed for streamBegin/streamMore from save/restore paths
static MagiCommsEspNow  gPairTransport;
static MagiUdpLink      gUdpLink;
MagiComms gComms(gTransport);
ServerPairing gServerPairing;

// ── Receive callbacks — data vs pairing, kept fully separate ────────────────
static void onCommsRecv(const uint8_t* data, int len) {
    gServerPairing._onReceive(data, len);
}

static void onPairRecv(const uint8_t* data, int len) {
    gServerPairing._onPairReceive(data, len);
}

// ── begin() ───────────────────────────────────────────────────────────────────
void ServerPairing::begin() {
    if (!_songBuf) _songBuf = new uint8_t[SONG_TRANSFER_MAX]();
    if (!_instBuf) _instBuf = new uint8_t[MAX_INSTRUMENTS * sizeof(Instrument)]();
    if (_ready) return;

    // DEBUG: dump raw NVS state at boot — temporary, for NVS-corruption hunt.
    pairNvsDump(CLI_NVS_NS);

    // Load existing pairing (server MAC + secret).
    _hasPairing = pairNvsLoad(CLI_NVS_NS, _storedServerMac, _storedSecret);

    // Load or generate the AP info.  PSK is generated once on first boot
    // and persisted forever — it's our identity as an AP, not part of any
    // particular pairing.  Survives clearPairing().
    //
    // Also regenerate if a previous build saved a degenerate SSID (all-zero
    // MAC bytes, e.g. "magitrac-000000") — that came from calling
    // WiFi.macAddress() before WiFi was initialised; we now use
    // esp_read_mac() which works pre-init.
    bool loaded = pairNvsLoadApInfo(CLI_NVS_NS, _apSsid, _apPsk, &_nextHostOctet);
    bool bogus  = loaded && (strstr(_apSsid, "000000") != nullptr);
    if (!loaded || bogus) {
        uint8_t myMac[6] = {};
        esp_read_mac(myMac, ESP_MAC_WIFI_STA);   // factory MAC from eFuse — no WiFi needed
        snprintf(_apSsid, sizeof(_apSsid), "magitrac-%02X%02X%02X",
                 myMac[3], myMac[4], myMac[5]);
        for (int i = 0; i < 16; i++) {
            uint8_t v = esp_random() & 0xF;
            _apPsk[i] = "0123456789abcdef"[v];
        }
        _apPsk[16] = 0;
        _nextHostOctet = 3;
        pairNvsSaveApInfo(CLI_NVS_NS, _apSsid, _apPsk, _nextHostOctet);
        Serial.printf("[SP] %s AP: SSID=%s PSK=%s\n",
                      bogus ? "regenerated" : "generated",
                      _apSsid, _apPsk);
    } else {
        Serial.printf("[SP] AP ready: SSID=%s next-IP=.%u\n",
                      _apSsid, _nextHostOctet);
    }

    // TCP transport — main data path.  AP role on 192.168.0.1, always on
    // the user-configured channel (one of 1/6/11).  The server, when in
    // pair mode, scans those three channels broadcasting MSG_PAIR_PROBE
    // — so wherever our AP lives, the server will find it.  Once paired,
    // the server STA-joins by SSID and tracks channel changes automatically.
    const uint8_t channel = magiWifiChannelFromIdx(gWifiChannelIdx);
    Serial.printf("[SP] AP channel: %u\n", channel);
    gTransport.configureAp(_apSsid, _apPsk, MAGI_PORT, channel);
    gComms.setOnReceive(onCommsRecv);
    gComms.begin();

    // ESP-NOW transport — pairing ceremony only.  Coexist mode keeps it
    // from clobbering the TCP transport's WiFi setup.
    gPairTransport.setCoexistMode(true);
    gPairTransport.setOnReceive(onPairRecv);
    gPairTransport.begin();

    // UDP listener — shares MAGI_PORT with the TCP listener.  Loss-tolerant
    // position/preview updates come in here.  Dispatches via the same
    // _onReceive handler as TCP — the existing MAC check trivially passes
    // because `lastSenderAddr()` returns the static `_storedServerMac`.
    gUdpLink.setOnReceive(onCommsRecv);
    gUdpLink.beginListener(MAGI_PORT);

    // Streaming receive — song-load and instruments-load blobs come in as
    // single ~35 KB frames; we read them straight into the in-memory
    // buffers without a giant rxBuf in the TCP transport.
    gTransport.registerStreamRecv((uint8_t)MSG_SONG_BLOB,
                                  &ServerPairing::_onSongBlobStreamTrampoline, this);
    gTransport.registerStreamRecv((uint8_t)MSG_INSTRUMENTS_BLOB,
                                  &ServerPairing::_onInstrumentsBlobStreamTrampoline, this);

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

    _ready = true;

    if (_hasPairing) {
        Serial.printf("[SP] stored server: %02X:%02X:%02X:%02X:%02X:%02X\n",
            _storedServerMac[0], _storedServerMac[1], _storedServerMac[2],
            _storedServerMac[3], _storedServerMac[4], _storedServerMac[5]);
        memcpy(_serverMac, _storedServerMac, 6);
        gComms.addPeer(_storedServerMac);   // register MAC for TCP-side lastSenderAddr
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

// ── Pairing ceremony (over ESP-NOW; AP stays on user channel) ───────────────
//
// Flow (new, post-2026-05-24 redesign):
//   1. User enters pair mode here → state = PAIRING_REQUEST (listening).
//   2. Server (in its own pair mode) scans channels 1/6/11 broadcasting
//      MSG_PAIR_PROBE.  When the server's current scan channel matches
//      our AP channel, we receive the probe — see _onPairReceive.
//   3. We generate a 4-digit PIN, register the server as ESP-NOW peer,
//      unicast MSG_PAIR_CHALLENGE back with the PIN, show PIN on screen,
//      → PAIRING_CONFIRM.
//   4. User taps Confirm → confirmPairCode() unicasts MSG_PAIR_OFFER with
//      AP creds + assigned static IP, persists the server MAC, → AUTO_CONNECTING.
//   5. Server saves creds and reboots into TCP-STA mode.  TCP-side
//      MSG_CONNECT closes the loop and we go to SUCCESS.
void ServerPairing::startPairCeremony() {
    Serial.println("[SP] entering pair mode — listening for PROBE");
    _challengePending = false;
    _setPairState(PairClientState::PAIRING_REQUEST);
}

void ServerPairing::confirmPairCode() {
    if (_pairState != PairClientState::PAIRING_CONFIRM) return;

    MsgPairOffer offer;
    memset(&offer, 0, sizeof(offer));
    offer.type = MSG_PAIR_OFFER;
    strncpy(offer.apSsid, _apSsid, sizeof(offer.apSsid) - 1);
    strncpy(offer.apPsk,  _apPsk,  sizeof(offer.apPsk)  - 1);
    offer.assignedIp[0] = 192; offer.assignedIp[1] = 168;
    offer.assignedIp[2] = 0;   offer.assignedIp[3] = 2;
    offer.gatewayIp[0]  = 192; offer.gatewayIp[1]  = 168;
    offer.gatewayIp[2]  = 0;   offer.gatewayIp[3]  = 1;
    bool ok = gPairTransport.sendRaw(&offer, sizeof(offer));
    Serial.printf("[SP] sent MSG_PAIR_OFFER unicast (%s)\n", ok ? "OK" : "FAIL");

    // Persist server MAC — needed for TCP-side `lastSenderAddr` matching
    // after the server reboots into STA mode and connects.  Secret is
    // unused (no per-frame signing) but kept in the NVS schema.
    uint8_t zeroSecret[16] = {};
    pairNvsSave(CLI_NVS_NS, _serverMac, zeroSecret);
    memcpy(_storedServerMac, _serverMac, 6);
    _hasPairing = true;

    // Hand off to the auto-connect path.  Server will be unreachable for
    // a few seconds (reboot + WiFi join), then it sends MSG_CONNECT over
    // TCP and we transition to SUCCESS.
    gPairTransport.removePeer(_serverMac);    // ESP-NOW peer no longer needed
    gComms.addPeer(_storedServerMac);         // register MAC label on TCP transport
    _tryAutoConnect();
}

void ServerPairing::cancelPairing() {
    _challengePending = false;
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
    msg.id     = MSG_DISCONNECT;
    msg.length = sizeof(msg);
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
    MsgPlay msg;
    msg.id     = (uint8_t)type;
    msg.length = sizeof(msg);
    gMagiLink.acquireMutex();
    bool ok = gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
    return ok;
}

bool ServerPairing::sendSeek(uint8_t pattern, uint8_t row) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgSeek msg;
    msg.id      = MSG_SEEK;
    msg.length  = sizeof(msg);
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
    msg.id      = MSG_GOTO;
    msg.length  = sizeof(msg);
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
    msg.type    = MSG_QUEUE_BLOCK;
    msg.pattern = pattern;
    return gComms.send(&msg, sizeof(msg));
}

bool ServerPairing::sendCancelQueue() {
    if (_pairState != PairClientState::SUCCESS) return false;
    uint8_t msg = (uint8_t)MSG_CANCEL_QUEUE;
    return gComms.send(&msg, 1);
}

bool ServerPairing::sendPreviewStart(uint8_t pattern, uint8_t col) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgPreviewStart msg;
    msg.type    = MSG_PREVIEW_START;
    msg.pattern = pattern;
    msg.col     = col;
    return gComms.send(&msg, sizeof(msg));
}

bool ServerPairing::sendPreviewStop() {
    if (_pairState != PairClientState::SUCCESS) return false;
    uint8_t msg = (uint8_t)MSG_PREVIEW_STOP;
    return gComms.send(&msg, 1);
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
    msg.type = MSG_MIDI_DATA;
    msg.len  = len;
    memcpy(msg.data, bytes, len);
    return gComms.send(&msg, sizeof(msg));
}

bool ServerPairing::sendSongPatch(const Song& song, const void* fieldPtr, uint8_t length) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (length == 0 || length > SONG_PATCH_MAX) return false;
    MsgSetSongData msg;
    msg.type   = MSG_SET_SONG_DATA;
    msg.offset = (uint16_t)((const uint8_t*)fieldPtr - (const uint8_t*)&song);
    msg.length = length;
    memcpy(msg.data, fieldPtr, length);
    return gComms.send(&msg, 4 + length);
}

bool ServerPairing::sendNoteSet(const Song& song, uint8_t pattern, uint8_t row, uint8_t col) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (pattern >= MAX_PATTERNS) return false;
    MsgNoteSet msg;
    msg.type    = MagiMsgType::MSG_NOTE_SET;
    msg.pattern = pattern;
    msg.row     = row;
    msg.col     = col;
    NoteGrid grid(song.notePool, &song.patterns[pattern].noteHead);
    msg.note = grid.get(row, col);
    return gComms.send(&msg, sizeof(msg));
}

bool ServerPairing::sendAuditionNote(uint8_t pattern, uint8_t row, uint8_t col) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgNoteAudition msg;
    msg.type    = MSG_NOTE_AUDITION;
    msg.pattern = pattern;
    msg.row     = row;
    msg.col     = col;
    return gComms.send(&msg, sizeof(msg));
}

bool ServerPairing::sendNoteSetReliable(const Song& song, uint8_t pattern, uint8_t row, uint8_t col) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (pattern >= MAX_PATTERNS) return false;
    MsgNoteSet msg;
    msg.type    = MagiMsgType::MSG_NOTE_SET;
    msg.pattern = pattern;
    msg.row     = row;
    msg.col     = col;
    NoteGrid grid(song.notePool, &song.patterns[pattern].noteHead);
    msg.note = grid.get(row, col);
    return gComms.sendReliable(&msg, sizeof(msg), row);
}

bool ServerPairing::sendSongToServer(const char* name, const Song* song) {
    if (_pairState != PairClientState::SUCCESS) return false;

    // Wire payload: type(1) + name(SRV_NAME_MAX) + SongFileHeader + Song.
    // Streamed straight from our in-memory buffers — no chunk-buffer copy,
    // no per-chunk loop, just three streamMore calls.
    SongFileHeader hdr;
    hdr.magic   = SONG_FILE_MAGIC;
    hdr.version = SONG_FILE_VERSION;
    memset(hdr._pad, 0, sizeof(hdr._pad));

    char namePadded[SRV_NAME_MAX] = {};
    strncpy(namePadded, name, SRV_NAME_MAX - 1);

    size_t totalLen = 1 + SRV_NAME_MAX + sizeof(SongFileHeader) + sizeof(Song);
    if (!gTransport.streamBegin(totalLen)) return false;
    uint8_t type = (uint8_t)MSG_SONG_SAVE_BLOB;
    bool ok = true;
    ok &= gTransport.streamMore(&type, 1);
    ok &= gTransport.streamMore(namePadded, SRV_NAME_MAX);
    ok &= gTransport.streamMore(&hdr, sizeof(hdr));
    ok &= gTransport.streamMore(song, sizeof(Song));
    gTransport.streamEnd();
    return ok;
}

void ServerPairing::requestInstruments() {
    if (_pairState != PairClientState::SUCCESS) return;
    uint8_t msg = (uint8_t)MSG_INSTRUMENTS_REQ;
    gComms.send(&msg, 1);
    _instReady       = false;
    _instBufLen      = 0;
    _instChunksGot   = 0;
    _instChunksTotal = 0;
}

bool ServerPairing::sendInstrumentPatch(const Instrument* instruments, int idx) {
    if (_pairState != PairClientState::SUCCESS) return false;
    if (idx < 0 || idx >= MAX_INSTRUMENTS) return false;
    MsgInstrumentsPatch msg;
    msg.type   = MSG_INSTRUMENTS_PATCH;
    msg.offset = (uint16_t)((uint32_t)idx * sizeof(Instrument));
    msg.length = (uint8_t)sizeof(Instrument);
    memcpy(msg.data, &instruments[idx], sizeof(Instrument));
    return gComms.send(&msg, 4 + sizeof(Instrument));
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

void ServerPairing::requestBackupFileList(uint8_t /*unused*/) {
    if (_pairState != PairClientState::SUCCESS) return;
    MsgBackupListReq msg;
    msg.type = MSG_BACKUP_LIST_REQ;
    bool ok = gComms.send(&msg, sizeof(msg));
    Serial.printf("[SP] requestBackupFileList send=%s\n", ok ? "OK" : "FAIL");
    _backupState = BackupState::WAITING_FILE_LIST;
}

void ServerPairing::requestBackupFile(const char* name) {
    if (_pairState != PairClientState::SUCCESS) return;
    MsgBackupFileReq msg;
    msg.type = MSG_BACKUP_FILE_REQ;
    strncpy(msg.name, name, sizeof(msg.name) - 1);
    msg.name[sizeof(msg.name) - 1] = '\0';
    _songBufLen  = 0;
    _chunksGot   = 0;
    _chunksTotal = 0;
    _backupState = BackupState::WAITING_FILE;
    gComms.send(&msg, sizeof(msg));
}

void ServerPairing::resetBackup() {
    _backupState = BackupState::IDLE;
    _bkFileCount = 0;
}

bool ServerPairing::sendRestoreFile(const char* name, bool isInstruments,
                                     const uint8_t* data, uint32_t dataLen) {
    if (_pairState != PairClientState::SUCCESS) return false;

    // Wire payload: type(1) + name(SRV_FNAME_MAX) + isInstruments(1) + size(u32) + bytes.
    char namePadded[SRV_FNAME_MAX] = {};
    strncpy(namePadded, name, SRV_FNAME_MAX - 1);
    uint8_t isInstrByte = isInstruments ? 1 : 0;

    size_t totalLen = 1 + SRV_FNAME_MAX + 1 + 4 + dataLen;
    if (!gTransport.streamBegin(totalLen)) return false;
    uint8_t type = (uint8_t)MSG_RESTORE_FILE_BLOB;
    bool ok = true;
    ok &= gTransport.streamMore(&type, 1);
    ok &= gTransport.streamMore(namePadded, SRV_FNAME_MAX);
    ok &= gTransport.streamMore(&isInstrByte, 1);
    ok &= gTransport.streamMore(&dataLen, 4);
    if (dataLen > 0) ok &= gTransport.streamMore(data, dataLen);
    gTransport.streamEnd();
    return ok;
}

bool ServerPairing::deleteSongOnServer(const char* name) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgSongDelete msg;
    msg.type = MSG_SONG_DELETE;
    strncpy(msg.name, name, sizeof(msg.name) - 1);
    msg.name[sizeof(msg.name) - 1] = '\0';
    return gComms.send(&msg, sizeof(msg));
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

        case PairClientState::PAIRING_REQUEST:
            if (_challengePending) {
                _challengePending = false;
                gPairTransport.addPeer(_serverMac);
                MsgPairChallenge ch;
                ch.type = MSG_PAIR_CHALLENGE;
                memcpy(ch.pin, _pairCode, 4);
                bool ok = gPairTransport.sendRaw(&ch, sizeof(ch));
                Serial.printf("[SP] got PROBE → PIN %c%c%c%c, CHALLENGE send=%s\n",
                    _pairCode[0], _pairCode[1], _pairCode[2], _pairCode[3],
                    ok ? "OK" : "FAIL");
                _setPairState(PairClientState::PAIRING_CONFIRM);
                // We just transitioned — skip the timeout check this tick
                // (else if).  Otherwise `now` (sampled before _setPairState)
                // is older than the freshly-stamped _pairStateMs and the
                // uint32_t subtraction underflows, firing the timeout
                // immediately.
            } else if (now - _pairStateMs >= PAIR_REQUEST_TIMEOUT_MS) {
                Serial.println("[SP] pair window timeout");
                _setPairState(PairClientState::IDLE);
            }
            break;

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
    req.type = MSG_SONG_LIST_REQ;
    req.page = page;
    gComms.send(&req, sizeof(req));
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
    req.type  = MSG_SONG_LOAD_REQ;
    req.page  = page;
    req.index = index;
    gComms.send(&req, sizeof(req));
    _setBrowseState(BrowseState::WAITING_SONG);
}

void ServerPairing::requestSongLoadByName(const char* name) {
    if (_pairState != PairClientState::SUCCESS) return;
    _songBufLen   = 0;
    _chunksGot    = 0;
    _chunksTotal  = 0;
    MsgSongLoadNameReq req;
    req.type = MSG_SONG_LOAD_NAME;
    memset(req.name, 0, sizeof(req.name));
    if (name) strncpy(req.name, name, sizeof(req.name) - 1);
    gComms.send(&req, sizeof(req));
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
    req.type = MSG_SAMPLE_LIST_REQ;
    req.page = 0;
    gComms.send(&req, sizeof(req));
    _sampleListState = SampleListState::WAITING;
}

const char* ServerPairing::sampleListNameFor(uint8_t id) const {
    if (id == 0) return nullptr;
    for (int i = 0; i < _sampleCount; i++) {
        if (_sampleCache[i].id == id) return _sampleCache[i].name;
    }
    return nullptr;
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
void ServerPairing::_onReceive(const uint8_t* data, int len) {
    if (len < 1) return;
    MagiMsgType type = (MagiMsgType)data[0];
    const uint8_t* senderMac = gComms.lastSenderAddr();

    switch (type) {

        case MSG_CONNECT_ACK:
            if (_pairState != PairClientState::REQUESTING &&
                _pairState != PairClientState::AUTO_CONNECTING) return;
            if (len < (int)sizeof(MsgConnectAck)) return;
            {
                gComms.addPeer(_serverMac);
                Serial.printf("[SP] connected to %02X:%02X:%02X:%02X:%02X:%02X\n",
                    _serverMac[0], _serverMac[1], _serverMac[2],
                    _serverMac[3], _serverMac[4], _serverMac[5]);
                _setPairState(PairClientState::SUCCESS);
            }
            break;

        case MSG_DISCONNECT:
            if (_pairState != PairClientState::SUCCESS) return;
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
            gComms.removePeer(_serverMac);
            _setBrowseState(BrowseState::IDLE);
            _serverPlaying = false;
            Serial.println("[SP] server ended session");
            if (_hasPairing) {
                memcpy(_serverMac, _storedServerMac, 6);
                _tryAutoConnect();
            } else {
                _setPairState(PairClientState::IDLE);
            }
            break;

        case MSG_SONG_LIST_RESP:
            if (_browseState != BrowseState::WAITING_LIST) return;
            if (len < (int)sizeof(MsgSongListResp)) return;
            {
                const MsgSongListResp* r = (const MsgSongListResp*)data;
                _listPage       = r->page;
                _listTotalPages = r->totalPages > 0 ? r->totalPages : 1;
                _listCount      = r->count <= SL_PER_PKT ? r->count : SL_PER_PKT;
                memset(_listNames, 0, sizeof(_listNames));
                for (int i = 0; i < _listCount; i++) {
                    strncpy(_listNames[i], r->names[i], SL_NAME_LEN - 1);
                    _listNames[i][SL_NAME_LEN - 1] = '\0';
                }
                _setBrowseState(BrowseState::LIST_READY);
            }
            break;

        // MSG_SONG_DATA (chunked song download) retired — replaced by the
        // streaming MSG_SONG_BLOB path which lands in _onSongBlobStream.

        case MSG_SAMPLE_LIST_RESP:
            if (_pairState != PairClientState::SUCCESS) return;
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
            if (len < (int)sizeof(MsgSampleListResp)) return;
            if (_sampleListState != SampleListState::WAITING &&
                _sampleListState != SampleListState::PARTIAL) return;
            {
                const MsgSampleListResp* r = (const MsgSampleListResp*)data;
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
                    // Request next page
                    _sampleListState = SampleListState::PARTIAL;
                    MsgSampleListReq req;
                    req.type = MSG_SAMPLE_LIST_REQ;
                    req.page = (uint8_t)(_samplePage + 1);
                    gComms.send(&req, sizeof(req));
                } else {
                    _sampleListState = SampleListState::READY;
                }
            }
            break;

        // MSG_INSTRUMENTS_DATA (chunked) retired — replaced by streaming
        // MSG_INSTRUMENTS_BLOB which lands in _onInstrumentsBlobStream.

        case MSG_SEQ_POS:
            if (_pairState != PairClientState::SUCCESS) return;
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
            if (len < (int)sizeof(MsgSeqPos)) return;
            {
                const MsgSeqPos* p = (const MsgSeqPos*)data;
                _remotePattern  = p->pattern;
                _remoteRow      = p->row;
                _remotePosDirty = true;
            }
            break;

        case MSG_PLAY:
            if (_pairState != PairClientState::SUCCESS) return;
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
            _serverPlaying = true;
            break;

        case MSG_STOP:
            if (_pairState != PairClientState::SUCCESS) return;
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
            _serverPlaying = false;
            break;

        case MSG_NO_SONG:
            if (_pairState != PairClientState::SUCCESS) return;
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
            _noSongPending = true;
            break;

        case MSG_MIDI_NOTE_IN:
            if (_pairState != PairClientState::SUCCESS) return;
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
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
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
            if (len < (int)sizeof(MsgPreviewRow)) return;
            {
                const MsgPreviewRow* m = (const MsgPreviewRow*)data;
                _previewRow        = m->row;
                _previewRowPending = true;
            }
            break;

        case MSG_BACKUP_LIST_BLOB:
            // Wire: type(1) + numFiles(1) + BkFileEntry × numFiles.
            // The whole list arrives in one frame — UI just iterates
            // backupFileCount() entries and goes.
            if (_backupState != BackupState::WAITING_FILE_LIST) return;
            if (len < 2) return;
            {
                int numFiles = data[1];
                int hdrLen   = 2;
                int expected = hdrLen + numFiles * (int)sizeof(BkFileEntry);
                if (len != expected) return;
                int cap = (int)(sizeof(_bkEntries) / sizeof(_bkEntries[0]));
                if (numFiles > cap) numFiles = cap;
                memcpy(_bkEntries, data + hdrLen,
                       numFiles * sizeof(BkFileEntry));
                _bkFileCount = numFiles;
                _backupState = BackupState::FILE_LIST_READY;
            }
            break;

        case MSG_BACKUP_FILE_BLOB:
            // Layout: type(1) + name(SRV_FNAME_MAX=24) + size(u32 LE) + bytes.
            // The whole file arrives as a single framed message — TCP did
            // the segmentation/reassembly transparently.
            Serial.printf("[SP] BACKUP_FILE_BLOB arrived len=%d bkState=%d\n",
                          len, (int)_backupState);
            if (_backupState != BackupState::WAITING_FILE) return;
            if (len < 1 + (int)SRV_FNAME_MAX + 4) return;
            {
                uint32_t fileSize;
                memcpy(&fileSize, data + 1 + SRV_FNAME_MAX, 4);
                int hdrLen = 1 + SRV_FNAME_MAX + 4;
                Serial.printf("[SP] BLOB fileSize=%u expectLen=%d\n",
                              (unsigned)fileSize, hdrLen + (int)fileSize);
                if (len != hdrLen + (int)fileSize) return;
                if (fileSize > SONG_TRANSFER_MAX) return;
                memcpy(_songBuf, data + hdrLen, fileSize);
                _songBufLen  = fileSize;
                _backupState = BackupState::FILE_RECEIVED;
            }
            break;

        case MSG_TCP_TEST_BLOB:
            // Pure streaming test — just count the blob and its bytes.
            // No verification, no buffering, no per-blob handshake.
            _tcpTestBlobCount++;
            _tcpTestByteCount += (uint64_t)len;
            break;

        default:
            break;
    }
}

bool ServerPairing::startTcpTest() {
    if (_pairState != PairClientState::SUCCESS) return false;
    uint8_t msg = (uint8_t)MSG_TCP_TEST_START;
    return gComms.send(&msg, 1);
}

bool ServerPairing::stopTcpTest() {
    if (_pairState != PairClientState::SUCCESS) return false;
    uint8_t msg = (uint8_t)MSG_TCP_TEST_STOP;
    return gComms.send(&msg, 1);
}

// ── Streaming receive: song load (server → client) ──────────────────────────
//
// Frame payload is SongFileHeader + Song (~35 KB) — too big for the 8 KB
// reader buffer.  Pump it straight into _songBuf, advance the browse state
// machine on completion.  Runs on the TCP reader task.
void ServerPairing::_onSongBlobStream(size_t remainingLen) {
    // Drain anything we can't store cleanly so the next frame stays
    // aligned to the wire.
    if (remainingLen > SONG_TRANSFER_MAX) {
        uint8_t junk[256];
        while (remainingLen > 0) {
            size_t n = remainingLen > sizeof(junk) ? sizeof(junk) : remainingLen;
            size_t got = gTransport.streamReadRecv(junk, n);
            if (got == 0) break;
            remainingLen -= got;
        }
        Serial.println("[SP] SONG_BLOB too big — dropped");
        return;
    }

    size_t got = gTransport.streamReadRecv(_songBuf, remainingLen);
    if (got != remainingLen) {
        Serial.printf("[SP] SONG_BLOB short read: got=%u want=%u\n",
                      (unsigned)got, (unsigned)remainingLen);
        return;
    }
    _songBufLen  = got;
    _chunksGot   = 0;
    _chunksTotal = 0;
    _setBrowseState(BrowseState::SONG_READY);
}

// ── Streaming receive: instruments load (server → client) ───────────────────
void ServerPairing::_onInstrumentsBlobStream(size_t remainingLen) {
    if (!_instBuf) return;
    uint32_t cap = MAX_INSTRUMENTS * (uint32_t)sizeof(Instrument);
    if (remainingLen > cap) {
        uint8_t junk[256];
        while (remainingLen > 0) {
            size_t n = remainingLen > sizeof(junk) ? sizeof(junk) : remainingLen;
            size_t got = gTransport.streamReadRecv(junk, n);
            if (got == 0) break;
            remainingLen -= got;
        }
        Serial.println("[SP] INSTRUMENTS_BLOB too big — dropped");
        return;
    }
    size_t got = gTransport.streamReadRecv(_instBuf, remainingLen);
    if (got != remainingLen) {
        Serial.printf("[SP] INSTRUMENTS_BLOB short read: got=%u want=%u\n",
                      (unsigned)got, (unsigned)remainingLen);
        return;
    }
    _instBufLen      = got;
    _instChunksGot   = 0;
    _instChunksTotal = 0;
    _instReady       = true;
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

        case MSG_PAIR_PROBE:
            if (_pairState != PairClientState::PAIRING_REQUEST) return;
            if (len < (int)sizeof(MsgPairProbe)) return;
            if (_challengePending) return;   // already queued from an earlier probe
            {
                // Generate 4-digit PIN; record the server MAC.  The
                // CHALLENGE itself is sent from tick() — we're on the
                // WiFi task here and esp_now_send's send-cb runs on the
                // same task, so blocking on the ACK semaphore inline
                // would time out every time.
                uint32_t r = esp_random();
                _pairCode[0] = '0' + (r % 10); r /= 10;
                _pairCode[1] = '0' + (r % 10); r /= 10;
                _pairCode[2] = '0' + (r % 10); r /= 10;
                _pairCode[3] = '0' + (r % 10);
                memcpy(_serverMac, senderMac, 6);
                _challengePending = true;
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
    ack.id     = MSG_CONNECT_ACK;
    ack.length = sizeof(ack);
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

// ── Internal helpers ──────────────────────────────────────────────────────────
void ServerPairing::_setPairState(PairClientState s) {
    _pairState   = s;
    _pairStateMs = millis();
}

void ServerPairing::_setBrowseState(BrowseState s) {
    _browseState = s;
}

#include "ServerPairing.h"
#include "NoteGrid.h"
#include "PairNVS.h"
#include "MagiCommsEspNow.h"
#include <string.h>

#define CLI_NVS_NS "magitrac_cli"

// ── Global instances ─────────────────────────────────────────────────────────
static MagiCommsEspNow gTransport;
MagiComms gComms(gTransport);
ServerPairing gServerPairing;

// ── Receive callback — bridges MagiComms → ServerPairing ─────────────────────
static void onCommsRecv(const uint8_t* data, int len) {
    gServerPairing._onReceive(data, len);
}

// ── begin() ───────────────────────────────────────────────────────────────────
void ServerPairing::begin() {
    if (!_songBuf) _songBuf = new uint8_t[SONG_TRANSFER_MAX]();
    if (!_instBuf) _instBuf = new uint8_t[MAX_INSTRUMENTS * sizeof(Instrument)]();
    if (_ready) return;

    gComms.setOnReceive(onCommsRecv);
    gComms.begin();
    _ready = true;

    // Load stored pairing and attempt auto-connect immediately
    _hasPairing = pairNvsLoad(CLI_NVS_NS, _storedServerMac, _storedSecret);
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

// ── Auto-connect (pure ESP-NOW unicast — no HMAC) ─────────────────────────────
void ServerPairing::_tryAutoConnect() {
    uint8_t myMac[6];
    gComms.localAddr(myMac);

    MsgConnect msg;
    msg.type = MSG_CONNECT;
    memcpy(msg.senderMac, myMac, 6);
    memset(msg.hmac8, 0, sizeof(msg.hmac8));   // reserved — formerly HMAC

    gComms.addPeer(_storedServerMac);

    bool ok = gComms.send(&msg, sizeof(msg));
    Serial.printf("[SP] auto-connect send=%s\n", ok ? "OK" : "FAIL");

    _setPairState(PairClientState::AUTO_CONNECTING);
}

// ── Pairing ceremony ──────────────────────────────────────────────────────────
void ServerPairing::startPairCeremony() {
    MsgPairRequest req;
    req.type = MSG_PAIR_REQUEST;
    memcpy(req.magic, MAGI_PAIR_MAGIC, sizeof(req.magic));
    gComms.localAddr(req.senderMac);
    gComms.sendBroadcast(&req, sizeof(req));
    Serial.println("[SP] sent MSG_PAIR_REQUEST");
    _setPairState(PairClientState::PAIRING_REQUEST);
}

void ServerPairing::confirmPairCode() {
    if (_pairState != PairClientState::PAIRING_CONFIRM) return;
    MsgPairAccept accept;
    accept.type = MSG_PAIR_ACCEPT;
    gComms.send(&accept, sizeof(accept));
    Serial.println("[SP] sent MSG_PAIR_ACCEPT");
    _setPairState(PairClientState::PAIRING_WAITING);
}

void ServerPairing::cancelPairing() {
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
    msg.type = MSG_DISCONNECT;
    gComms.send(&msg, sizeof(msg));
    gComms.removePeer(_serverMac);
    _serverName[0] = '\0';
    _setBrowseState(BrowseState::IDLE);
    if (_hasPairing)
        _tryAutoConnect();
    else
        _setPairState(PairClientState::IDLE);
}

// ── sendControl / seek / midi ─────────────────────────────────────────────────
bool ServerPairing::sendControl(MagiMsgType type) {
    if (_pairState != PairClientState::SUCCESS) return false;
    uint8_t msg = (uint8_t)type;
    return gComms.send(&msg, 1);
}

bool ServerPairing::sendSeek(uint8_t pattern, uint8_t row) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgSeek msg;
    msg.type    = MSG_SEEK;
    msg.pattern = pattern;
    msg.row     = row;
    return gComms.send(&msg, sizeof(msg));
}

bool ServerPairing::sendGoto(uint8_t pattern, uint8_t row) {
    if (_pairState != PairClientState::SUCCESS) return false;
    MsgGoto msg;
    msg.type    = MSG_GOTO;
    msg.pattern = pattern;
    msg.row     = row;
    return gComms.send(&msg, sizeof(msg));
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
    static uint8_t buf[SONG_TRANSFER_MAX];
    SongFileHeader* hdr = (SongFileHeader*)buf;
    hdr->magic   = SONG_FILE_MAGIC;
    hdr->version = SONG_FILE_VERSION;
    memset(hdr->_pad, 0, sizeof(hdr->_pad));
    memcpy(buf + sizeof(SongFileHeader), song, sizeof(Song));
    uint32_t totalSize   = sizeof(SongFileHeader) + sizeof(Song);
    uint8_t  totalChunks = (uint8_t)((totalSize + SONG_CHUNK_SIZE - 1) / SONG_CHUNK_SIZE);

    MsgSongSaveStart start;
    start.type = MSG_SONG_SAVE;
    strncpy(start.name, name, sizeof(start.name) - 1);
    start.name[sizeof(start.name) - 1] = '\0';
    start.totalChunks = totalChunks;
    gComms.send(&start, sizeof(start));
    delay(10);

    MsgSongSaveData chunk;
    chunk.type        = MSG_SONG_SAVE_DATA;
    chunk.totalChunks = totalChunks;
    for (uint8_t i = 0; i < totalChunks; i++) {
        uint32_t offset    = (uint32_t)i * SONG_CHUNK_SIZE;
        uint32_t remaining = totalSize - offset;
        chunk.chunk   = i;
        chunk.dataLen = (uint8_t)(remaining < SONG_CHUNK_SIZE ? remaining : SONG_CHUNK_SIZE);
        memcpy(chunk.payload, buf + offset, chunk.dataLen);
        gComms.sendReliable(&chunk, 4 + chunk.dataLen, i);
    }
    return true;
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

void ServerPairing::requestBackupFileList(uint8_t page) {
    if (_pairState != PairClientState::SUCCESS) return;
    MsgBackupListReq msg;
    msg.type = MSG_BACKUP_LIST_REQ;
    msg.page = page;
    gComms.send(&msg, sizeof(msg));
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
    _bkTotalFiles = 0;
}

bool ServerPairing::sendRestoreFile(const char* name, bool isInstruments,
                                     const uint8_t* data, uint32_t dataLen) {
    if (_pairState != PairClientState::SUCCESS) return false;
    uint8_t totalChunks = (uint8_t)((dataLen + SONG_CHUNK_SIZE - 1) / SONG_CHUNK_SIZE);

    MsgRestoreFileStart start;
    start.type = MSG_RESTORE_FILE_START;
    strncpy(start.name, name, sizeof(start.name) - 1);
    start.name[sizeof(start.name) - 1] = '\0';
    start.totalChunks  = totalChunks;
    start.isInstruments = isInstruments ? 1 : 0;
    gComms.send(&start, sizeof(start));
    delay(10);

    MsgRestoreFileData chunk;
    chunk.type        = MSG_RESTORE_FILE_DATA;
    chunk.totalChunks = totalChunks;
    for (uint8_t i = 0; i < totalChunks; i++) {
        uint32_t offset    = (uint32_t)i * SONG_CHUNK_SIZE;
        uint32_t remaining = dataLen - offset;
        chunk.chunk   = i;
        chunk.dataLen = (uint8_t)(remaining < SONG_CHUNK_SIZE ? remaining : SONG_CHUNK_SIZE);
        memcpy(chunk.payload, data + offset, chunk.dataLen);
        gComms.sendReliable(&chunk, 4 + chunk.dataLen, i);
    }
    return true;
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
    uint32_t now = millis();
    switch (_pairState) {

        case PairClientState::AUTO_CONNECTING:
            if (now - _pairStateMs >= AUTO_CONNECT_RETRY_MS) {
                Serial.println("[SP] auto-connect retry");
                _tryAutoConnect();
            }
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
            if (now - _pairStateMs >= PAIR_REQUEST_TIMEOUT_MS) {
                Serial.println("[SP] pair request timeout");
                _setPairState(PairClientState::IDLE);
            }
            break;

        case PairClientState::PAIRING_WAITING:
            if (now - _pairStateMs >= CONNECT_TIMEOUT_MS) {
                Serial.println("[SP] pair complete timeout");
                _setPairState(PairClientState::IDLE);
            }
            break;

        case PairClientState::SUCCESS:
            if (now - _lastPingMs >= PING_TIMEOUT_MS) {
                Serial.println("[SP] ping timeout — server gone, retrying");
                gComms.removePeer(_serverMac);
                _serverName[0] = '\0';
                _serverPlaying = false;
                _setBrowseState(BrowseState::IDLE);
                if (_hasPairing) {
                    memcpy(_serverMac, _storedServerMac, 6);
                    _tryAutoConnect();
                } else {
                    _setPairState(PairClientState::IDLE);
                }
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

        case MSG_PAIR_CONFIRM:
            if (_pairState != PairClientState::PAIRING_REQUEST) return;
            if (len < (int)sizeof(MsgPairConfirm)) return;
            {
                const MsgPairConfirm* c = (const MsgPairConfirm*)data;
                memcpy(_pairCode, c->code, 4);
                // Store server MAC from sender so we can send back to it
                memcpy(_serverMac, senderMac, 6);
                gComms.addPeer(_serverMac);
                Serial.printf("[SP] pair code: %c%c%c%c\n",
                    _pairCode[0], _pairCode[1], _pairCode[2], _pairCode[3]);
                _setPairState(PairClientState::PAIRING_CONFIRM);
            }
            break;

        case MSG_PAIR_COMPLETE:
            if (_pairState != PairClientState::PAIRING_WAITING) return;
            if (len < (int)sizeof(MsgPairComplete)) return;
            {
                const MsgPairComplete* c = (const MsgPairComplete*)data;
                pairNvsSave(CLI_NVS_NS, c->serverMac, c->secret);
                memcpy(_storedServerMac, c->serverMac, 6);
                memcpy(_storedSecret,    c->secret,    16);
                _hasPairing = true;
                Serial.println("[SP] pairing complete — secret saved");
                gComms.removePeer(_serverMac);
                memcpy(_serverMac, c->serverMac, 6);
                _tryAutoConnect();
            }
            break;

        case MSG_PING:
            if (_pairState != PairClientState::SUCCESS) return;
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
            _lastPingMs = millis();
            {
                uint8_t pong = (uint8_t)MSG_PONG;
                gComms.send(&pong, 1);
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

        case MSG_SONG_DATA:
            if (_backupState != BackupState::IDLE) return;
            if (_browseState == BrowseState::IDLE && _pairState == PairClientState::SUCCESS) {
                if (len >= 4 && ((const MsgSongData*)data)->chunk == 0)
                    _setBrowseState(BrowseState::WAITING_SONG);
                else
                    return;
            }
            if (_browseState != BrowseState::WAITING_SONG) return;
            if (len < 4) return;
            {
                const MsgSongData* d = (const MsgSongData*)data;
                if (d->chunk == 0) {
                    _songBufLen  = 0;
                    _chunksGot   = 0;
                    _chunksTotal = d->totalChunks;
                }
                uint32_t offset = (uint32_t)d->chunk * SONG_CHUNK_SIZE;
                if (offset + d->dataLen <= SONG_TRANSFER_MAX) {
                    memcpy(_songBuf + offset, d->payload, d->dataLen);
                    _chunksGot++;
                    if (offset + d->dataLen > _songBufLen)
                        _songBufLen = offset + d->dataLen;
                }
                if (_chunksGot >= _chunksTotal && _chunksTotal > 0)
                    _setBrowseState(BrowseState::SONG_READY);
            }
            break;

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

        case MSG_INSTRUMENTS_DATA:
            if (_pairState != PairClientState::SUCCESS) return;
            if (memcmp(senderMac, _serverMac, 6) != 0) return;
            if (len < 4) return;
            {
                const MsgInstrumentsData* d = (const MsgInstrumentsData*)data;
                if (d->chunk == 0) {
                    _instBufLen      = 0;
                    _instChunksGot   = 0;
                    _instChunksTotal = d->totalChunks;
                }
                uint32_t instMax = MAX_INSTRUMENTS * sizeof(Instrument);
                uint32_t offset  = (uint32_t)d->chunk * SONG_CHUNK_SIZE;
                if (offset + d->dataLen <= instMax) {
                    memcpy(_instBuf + offset, d->payload, d->dataLen);
                    _instChunksGot++;
                    if (offset + d->dataLen > _instBufLen)
                        _instBufLen = offset + d->dataLen;
                }
                if (_instChunksGot >= _instChunksTotal && _instChunksTotal > 0)
                    _instReady = true;
            }
            break;

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

        case MSG_BACKUP_LIST_RESP:
            if (_backupState != BackupState::WAITING_FILE_LIST) return;
            if (len < 5) return;
            {
                const MsgBackupListResp* r = (const MsgBackupListResp*)data;
                _bkPage       = r->page;
                _bkTotalPages = r->totalPages;
                _bkTotalFiles = r->totalFiles;
                _bkFileCount  = r->count;
                for (int i = 0; i < r->count && i < BK_PER_PKT; i++)
                    _bkEntries[i] = r->entries[i];
                _backupState = BackupState::FILE_LIST_READY;
            }
            break;

        case MSG_BACKUP_FILE_START:
            if (_backupState != BackupState::WAITING_FILE) return;
            if (len < (int)sizeof(MsgBackupFileStart)) return;
            {
                const MsgBackupFileStart* s = (const MsgBackupFileStart*)data;
                _songBufLen  = 0;
                _chunksGot   = 0;
                _chunksTotal = s->totalChunks;
            }
            break;

        case MSG_BACKUP_FILE_DATA:
            if (_backupState != BackupState::WAITING_FILE) return;
            if (len < 4) return;
            {
                const MsgBackupFileData* d = (const MsgBackupFileData*)data;
                uint32_t offset = (uint32_t)d->chunk * SONG_CHUNK_SIZE;
                if (offset + d->dataLen <= SONG_TRANSFER_MAX) {
                    memcpy(_songBuf + offset, d->payload, d->dataLen);
                    _chunksGot++;
                    if (offset + d->dataLen > _songBufLen)
                        _songBufLen = offset + d->dataLen;
                }
                if (_chunksGot >= _chunksTotal && _chunksTotal > 0)
                    _backupState = BackupState::FILE_RECEIVED;
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

// ── Internal helpers ──────────────────────────────────────────────────────────
void ServerPairing::_setPairState(PairClientState s) {
    _pairState   = s;
    _pairStateMs = millis();
    if (s == PairClientState::SUCCESS)
        _lastPingMs = millis();
}

void ServerPairing::_setBrowseState(BrowseState s) {
    _browseState = s;
}

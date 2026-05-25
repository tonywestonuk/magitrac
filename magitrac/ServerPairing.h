#pragma once
#include <stdint.h>
#include <stddef.h>
#include "MagiMsg.h"
#include "MagiComms.h"
#include "TrackerData.h"   // for Song / SongFileHeader

enum class PairClientState : uint8_t {
    IDLE,
    AUTO_CONNECTING,   // has stored pairing; silently retrying connection
    PAIRING_REQUEST,   // in pair mode; listening for MSG_PAIR_PROBE
    PAIRING_CONFIRM,   // got probe + generated PIN; waiting for user to tap Confirm
    REQUESTING,        // sent MSG_CONNECT; waiting for MSG_CONNECT_ACK
    SUCCESS,           // connected — session active
    TIMEOUT,
};

enum class BrowseState : uint8_t {
    IDLE,
    WAITING_LIST,   // waiting for MSG_SONG_LIST_RESP
    LIST_READY,     // list available, user can select
    WAITING_SONG,   // waiting for MSG_SONG_BLOB stream
    SONG_READY,     // song fully received and validated
    ERROR,
};

enum class BackupState : uint8_t {
    IDLE,
    WAITING_FILE_LIST,  // waiting for MSG_BACKUP_LIST_BLOB
    FILE_LIST_READY,    // list received
    WAITING_FILE,       // waiting for MSG_BACKUP_FILE_BLOB
    FILE_RECEIVED,      // all chunks received for current file
    ERROR,
};

enum class SampleListState : uint8_t {
    IDLE,
    WAITING,    // request sent, waiting for first MSG_SAMPLE_LIST_RESP
    PARTIAL,    // got some pages, more to come
    READY,      // all pages received
    ERROR,
};

// Upper bound on samples we cache client-side.  Sized for the editor picker;
// matches PROG range (1..127) + 1 reserved slot for "no sample".
#define SAMPLE_CACHE_MAX 128

struct SampleCacheEntry {
    uint8_t id;
    char    name[SAMPLE_NAME_LEN];  // includes .wav
};

// Max file transfer size: SongFileHeader (8) + Song struct (~35 KB)
#define SONG_TRANSFER_MAX (sizeof(SongFileHeader) + sizeof(Song) + 64)

class ServerPairing {
public:
    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void begin();   // init transport, load NVS pairing, auto-connect if paired

    // ── Pairing ceremony ──────────────────────────────────────────────────────
    void startPairCeremony();           // enter listen mode for MSG_PAIR_PROBE
    void confirmPairCode();             // user tapped Confirm → send MSG_PAIR_OFFER
    void cancelPairing();               // abort ceremony or auto-connect
    bool hasPairing()    const { return _hasPairing; }
    void clearPairing();                // forget stored pairing (NVS clear)
    const uint8_t* pairCode() const { return _pairCode; }  // 4 bytes (ASCII digits)

    // ── Connection ────────────────────────────────────────────────────────────
    void disconnect();
    void tick();              // call each loop()

    PairClientState    pairState()   const { return _pairState; }
    bool           isPaired()    const { return _pairState == PairClientState::SUCCESS; }
    const uint8_t* serverMac()  const { return _serverMac; }
    const char*    serverName() const { return _serverName; }

    // ── Server file browse ────────────────────────────────────────────────────
    void requestSongList(uint8_t page = 0);
    void requestSongLoad(uint8_t page, uint8_t index);
    void requestSongLoadByName(const char* name);   // bare name, no extension
    void resetBrowse();

    // ── Server file write / delete ────────────────────────────────────────────
    bool sendSongToServer(const char* name, const Song* song);
    bool deleteSongOnServer(const char* name);

    // ── Live song sync ────────────────────────────────────────────────────────
    bool sendSongPatch(const Song& song, const void* fieldPtr, uint8_t length);
    bool sendNoteSet(const Song& song, uint8_t pattern, uint8_t row, uint8_t col);
    bool sendNoteSetReliable(const Song& song, uint8_t pattern, uint8_t row, uint8_t col);
    bool sendAuditionNote(uint8_t pattern, uint8_t row, uint8_t col);

    // ── Instruments sync ─────────────────────────────────────────────────────
    void requestInstruments();
    bool sendInstrumentPatch(const Instrument* instruments, int idx);
    bool instrumentsReady() const { return _instReady; }
    bool copyInstruments(Instrument* out) const;
    void resetInstruments();

    // ── Sample list (SFX column) ─────────────────────────────────────────────
    // Server-side this triggers a manifest sync (scan + samples.txt update),
    // then pages results back.  Cache is paged in incrementally; check
    // sampleListState() == READY before consuming.
    void requestSampleList();
    void resetSampleList();
    SampleListState     sampleListState()        const { return _sampleListState; }
    int                 sampleListCount()        const { return _sampleCount; }
    int                 sampleListTotalExpected() const { return _sampleTotal; }
    const SampleCacheEntry* sampleListEntry(int i) const {
        return (i >= 0 && i < _sampleCount) ? &_sampleCache[i] : nullptr;
    }
    // Look up filename for a stable id (1..127) in the cache.  Returns nullptr
    // if not present (id 0 = "no sample", or cache not fetched).
    const char* sampleListNameFor(uint8_t id) const;

    // ── Backup / Restore ──────────────────────────────────────────────────────
    void requestBackupFileList(uint8_t page = 0);
    void requestBackupFile(const char* name);
    void resetBackup();
    bool sendRestoreFile(const char* name, bool isInstruments,
                         const uint8_t* data, uint32_t dataLen);

    BackupState backupState()       const { return _backupState; }
    int         backupFileCount()   const { return _bkFileCount; }
    uint8_t     backupTotalFiles()  const { return (uint8_t)_bkFileCount; }
    const char* backupFileName(int i) const { return _bkEntries[i].name; }
    uint32_t    backupFileSize(int i) const {
        return ((uint32_t)_bkEntries[i].sizeHi << 16) | _bkEntries[i].sizeLo;
    }
    const uint8_t* receivedFileData() const { return _songBuf; }
    uint32_t       receivedFileLen()  const { return _songBufLen; }

    // ── TCP/IP diagnostic test ───────────────────────────────────────────────
    // startTcpTest() tells the server to start emitting MSG_TCP_TEST_BLOB
    // continuously (one per server main-loop tick, paced by TCP backpressure).
    // stopTcpTest() tells it to stop.  Each arriving blob increments
    // _tcpTestBlobCount and adds to _tcpTestByteCount — the page just
    // reads those counters; no per-blob handshake.
    bool startTcpTest();
    bool stopTcpTest();
    uint32_t tcpTestBlobCount() const { return _tcpTestBlobCount; }
    uint64_t tcpTestByteCount() const { return _tcpTestByteCount; }
    void     tcpTestResetCounters() { _tcpTestBlobCount = 0; _tcpTestByteCount = 0; }

    // ── MIDI passthrough ──────────────────────────────────────────────────────
    bool sendMidi(const uint8_t* bytes, uint8_t len);

    // ── Transport control ─────────────────────────────────────────────────────
    bool sendControl(MagiMsgType type);

    // ── Scrub seek ────────────────────────────────────────────────────────────
    bool sendSeek(uint8_t pattern, uint8_t row);
    bool sendGoto(uint8_t pattern, uint8_t row);

    // ── Performance mode — block queueing ────────────────────────────────────
    bool sendQueueBlock(uint8_t pattern);
    bool sendCancelQueue();

    // ── Column preview ───────────────────────────────────────────────────────
    bool sendPreviewStart(uint8_t pattern, uint8_t col);
    bool sendPreviewStop();
    // Drain the latest preview-row update.  Returns false if no update pending.
    bool pollPreviewRow(uint8_t* row);

    // ── Sequencer position (received from server) ─────────────────────────────
    bool remoteSeqPos(uint8_t* pattern, uint8_t* row);

    // ── Sequencer play state (received from server) ────────────────────────────
    bool serverPlaying() const { return _serverPlaying; }

    // ── MIDI note-in (received from server while stopped) ─────────────────────
    bool pollMidiNoteIn(uint8_t* midiNote, uint8_t* velocity = nullptr);

    bool        noSongPending()    const { return _noSongPending; }
    void        clearNoSong()            { _noSongPending = false; }

    BrowseState browseState()      const { return _browseState; }
    int         listCount()        const { return _listCount; }
    uint8_t     listPage()         const { return _listPage; }
    uint8_t     listTotalPages()   const { return _listTotalPages; }
    const char* listName(int i)    const { return _listNames[i]; }
    bool        copySong(Song* out) const;

    // ── Internal (called by receive callbacks) ────────────────────────────────
    // _onReceive  — data path, comes off MagiCommsTcp via gComms.
    // _onPairReceive — pairing ceremony, comes off MagiCommsEspNow directly
    //                  (bypassing MagiComms so the two recv streams stay
    //                  segregated).
    // _onSongBlobStream / _onInstrumentsBlobStream — streaming-receive
    //                  handlers invoked by MagiCommsTcp's reader task when
    //                  a MSG_*_BLOB arrives.  They pump bytes straight into
    //                  the in-memory buffers via streamReadRecv().
    void _onReceive(const uint8_t* data, int len);
    void _onPairReceive(const uint8_t* data, int len);
    void _onSongBlobStream(size_t remainingLen);
    void _onInstrumentsBlobStream(size_t remainingLen);
    // MagiLink session handshake — called from the registered callback
    // (worker task, mutex held).  Sends MsgConnectAck and transitions
    // _pairState to SUCCESS.
    void _onMagiLinkConnect();
    // MagiLink: server sent MSG_DISCONNECT.  Transition out of SUCCESS.
    void _onMagiLinkDisconnect();
    static void _onSongBlobStreamTrampoline(size_t n, void* ctx) {
        static_cast<ServerPairing*>(ctx)->_onSongBlobStream(n);
    }
    static void _onInstrumentsBlobStreamTrampoline(size_t n, void* ctx) {
        static_cast<ServerPairing*>(ctx)->_onInstrumentsBlobStream(n);
    }

private:
    void _setPairState(PairClientState s);
    void _setBrowseState(BrowseState s);
    void _tryAutoConnect();

    // Session
    PairClientState _pairState   = PairClientState::IDLE;
    uint32_t        _pairStateMs = 0;
    uint8_t         _serverMac[6]   = {};
    char            _serverName[17] = {};
    bool            _ready          = false;

    // NVS-stored pairing.  _storedSecret is loaded but unused — kept on
    // disk so packet signing can be re-added later without re-pairing.
    bool    _hasPairing          = false;
    uint8_t _storedServerMac[6]  = {};
    uint8_t _storedSecret[16]    = {};

    // Client-owned AP info — generated once on first boot, persisted forever.
    // Handed to the server inside MsgPairOffer so it can STA-join the AP.
    // _nextHostOctet is the next static IP to allocate (starts at 3 — .1 is
    // the AP itself, .2 is reserved for magitrac_server).
    char    _apSsid[33]    = {};
    char    _apPsk[64]     = {};
    uint8_t _nextHostOctet = 3;

    // TCP/IP diagnostic test — receiver just counts, no buffering needed
    uint32_t _tcpTestBlobCount = 0;
    uint64_t _tcpTestByteCount = 0;

    // Pairing ceremony
    uint8_t _pairCode[4] = {};
    // Set by the PROBE handler; consumed by tick().  Sending the CHALLENGE
    // from inside the ESP-NOW recv callback would block waiting for the
    // send-cb on the same WiFi task, so the ACK always times out.  Defer
    // to the main loop instead.
    bool    _challengePending = false;

    // Browse
    BrowseState _browseState      = BrowseState::IDLE;
    char        _listNames[SL_PER_PKT][SL_NAME_LEN];
    int         _listCount        = 0;
    uint8_t     _listPage         = 0;
    uint8_t     _listTotalPages   = 1;
    uint8_t     _loadPage         = 0;
    uint8_t     _loadIndex        = 0;

    // Remote sequencer position and play state
    uint8_t  _remotePattern  = 0;
    uint8_t  _remoteRow      = 0;
    bool     _remotePosDirty = false;
    bool     _serverPlaying  = false;
    bool     _noSongPending  = false;
    uint8_t  _midiNoteIn     = 0;
    uint8_t  _midiVelocityIn = 0;
    bool     _midiNoteInPending = false;

    // Latest preview-row position from server
    uint8_t  _previewRow         = 0;
    bool     _previewRowPending  = false;

    // Song receive buffer
    uint8_t* _songBuf      = nullptr;
    uint32_t _songBufLen   = 0;
    uint8_t  _chunksGot    = 0;
    uint8_t  _chunksTotal  = 0;

    // Backup state — single-blob list (no paging) holds every entry the
    // server reported.  Sized to match the server's SRV_MAX_FILES upper
    // bound plus one for the instruments file.
    BackupState _backupState    = BackupState::IDLE;
    BkFileEntry _bkEntries[SRV_MAX_FILES + 1];
    int         _bkFileCount    = 0;

    // Instruments receive buffer
    uint8_t* _instBuf         = nullptr;
    uint32_t _instBufLen      = 0;
    uint8_t  _instChunksGot   = 0;
    uint8_t  _instChunksTotal = 0;
    bool     _instReady       = false;

    // Sample list cache (SFX column picker)
    SampleListState  _sampleListState = SampleListState::IDLE;
    SampleCacheEntry _sampleCache[SAMPLE_CACHE_MAX];
    int              _sampleCount     = 0;
    int              _sampleTotal     = 0;
    uint8_t          _samplePage      = 0;
    uint8_t          _sampleTotalPages = 0;

    static const uint32_t CONNECT_TIMEOUT_MS    = 10000;
    static const uint32_t AUTO_CONNECT_RETRY_MS  = 5000;
    static const uint32_t PAIR_REQUEST_TIMEOUT_MS = 60000;   // match the server's 60 s window
};

extern ServerPairing gServerPairing;
extern MagiComms gComms;

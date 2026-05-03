#pragma once
#include <stdint.h>
#include <stddef.h>
#include "MagiMsg.h"
#include "MagiComms.h"
#include "TrackerData.h"   // for Song / SongFileHeader

enum class PairClientState : uint8_t {
    IDLE,
    AUTO_CONNECTING,   // has stored pairing; silently retrying connection
    PAIRING_REQUEST,   // broadcast MSG_PAIR_REQUEST; waiting for MSG_PAIR_CONFIRM
    PAIRING_CONFIRM,   // received code; waiting for user confirmation
    PAIRING_WAITING,   // sent MSG_PAIR_ACCEPT; waiting for MSG_PAIR_COMPLETE
    REQUESTING,        // sent MSG_CONNECT; waiting for MSG_CONNECT_ACK
    SUCCESS,           // connected — session active
    TIMEOUT,
};

enum class BrowseState : uint8_t {
    IDLE,
    WAITING_LIST,   // waiting for MSG_SONG_LIST_RESP
    LIST_READY,     // list available, user can select
    WAITING_SONG,   // waiting for MSG_SONG_DATA chunks
    SONG_READY,     // song fully received and validated
    ERROR,
};

enum class BackupState : uint8_t {
    IDLE,
    WAITING_FILE_LIST,  // waiting for MSG_BACKUP_LIST_RESP
    FILE_LIST_READY,    // list received
    WAITING_FILE,       // waiting for MSG_BACKUP_FILE_START + DATA chunks
    FILE_RECEIVED,      // all chunks received for current file
    ERROR,
};

// Max file transfer size: SongFileHeader (8) + Song struct (~35 KB)
#define SONG_TRANSFER_MAX (sizeof(SongFileHeader) + sizeof(Song) + 64)

class ServerPairing {
public:
    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void begin();   // init transport, load NVS pairing, auto-connect if paired

    // ── Pairing ceremony ──────────────────────────────────────────────────────
    void startPairCeremony();           // broadcast MSG_PAIR_REQUEST
    void confirmPairCode();             // user confirmed code → send MSG_PAIR_ACCEPT
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
    void resetBrowse();

    // ── Server file write / delete ────────────────────────────────────────────
    bool sendSongToServer(const char* name, const Song* song);
    bool deleteSongOnServer(const char* name);

    // ── Live song sync ────────────────────────────────────────────────────────
    bool sendSongPatch(const Song& song, const void* fieldPtr, uint8_t length);
    bool sendNoteSet(const Song& song, uint8_t pattern, uint8_t row, uint8_t col);
    bool sendNoteSetReliable(const Song& song, uint8_t pattern, uint8_t row, uint8_t col);

    // ── Instruments sync ─────────────────────────────────────────────────────
    void requestInstruments();
    bool sendInstrumentPatch(const Instrument* instruments, int idx);
    bool instrumentsReady() const { return _instReady; }
    bool copyInstruments(Instrument* out) const;
    void resetInstruments();

    // ── Backup / Restore ──────────────────────────────────────────────────────
    void requestBackupFileList(uint8_t page = 0);
    void requestBackupFile(const char* name);
    void resetBackup();
    bool sendRestoreFile(const char* name, bool isInstruments,
                         const uint8_t* data, uint32_t dataLen);

    BackupState backupState()       const { return _backupState; }
    int         backupFileCount()   const { return _bkFileCount; }
    uint8_t     backupTotalFiles()  const { return _bkTotalFiles; }
    uint8_t     backupPage()        const { return _bkPage; }
    uint8_t     backupTotalPages()  const { return _bkTotalPages; }
    const char* backupFileName(int i) const { return _bkEntries[i].name; }
    uint32_t    backupFileSize(int i) const {
        return ((uint32_t)_bkEntries[i].sizeHi << 16) | _bkEntries[i].sizeLo;
    }
    const uint8_t* receivedFileData() const { return _songBuf; }
    uint32_t       receivedFileLen()  const { return _songBufLen; }

    // ── MIDI passthrough ──────────────────────────────────────────────────────
    bool sendMidi(const uint8_t* bytes, uint8_t len);

    // ── Transport control ─────────────────────────────────────────────────────
    bool sendControl(MagiMsgType type);

    // ── Scrub seek ────────────────────────────────────────────────────────────
    bool sendSeek(uint8_t pattern, uint8_t row);

    // ── Performance mode — block queueing ────────────────────────────────────
    bool sendQueueBlock(uint8_t pattern);
    bool sendCancelQueue();

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

    // ── Internal (called by receive callback) ─────────────────────────────────
    void _onReceive(const uint8_t* data, int len);

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

    // NVS-stored pairing
    bool    _hasPairing       = false;
    uint8_t _storedServerMac[6]  = {};
    uint8_t _storedSecret[16]    = {};
    uint8_t _sessionNonce[8]     = {};

    // Pairing ceremony
    uint8_t _pairCode[4] = {};

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

    // Song receive buffer
    uint8_t* _songBuf      = nullptr;
    uint32_t _songBufLen   = 0;
    uint8_t  _chunksGot    = 0;
    uint8_t  _chunksTotal  = 0;

    // Backup state
    BackupState _backupState    = BackupState::IDLE;
    BkFileEntry _bkEntries[BK_PER_PKT];
    int         _bkFileCount    = 0;
    uint8_t     _bkPage         = 0;
    uint8_t     _bkTotalPages   = 1;
    uint8_t     _bkTotalFiles   = 0;

    // Instruments receive buffer
    uint8_t* _instBuf         = nullptr;
    uint32_t _instBufLen      = 0;
    uint8_t  _instChunksGot   = 0;
    uint8_t  _instChunksTotal = 0;
    bool     _instReady       = false;

    // Keepalive
    uint32_t _lastPingMs = 0;

    static const uint32_t CONNECT_TIMEOUT_MS    = 10000;
    static const uint32_t PING_TIMEOUT_MS        = 15000;
    static const uint32_t AUTO_CONNECT_RETRY_MS  = 5000;
    static const uint32_t PAIR_REQUEST_TIMEOUT_MS = 15000;
};

extern ServerPairing gServerPairing;
extern MagiComms gComms;

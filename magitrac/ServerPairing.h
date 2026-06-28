#pragma once
#include <stdint.h>
#include <stddef.h>
#include <IPAddress.h>
#include "MagiMsg.h"
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

enum class SampleListState : uint8_t {
    IDLE,
    WAITING,    // request sent, waiting for first MSG_SAMPLE_LIST_RESP
    PARTIAL,    // got some pages, more to come
    READY,      // all pages received
    ERROR,
};

enum class FileListState : uint8_t {
    IDLE,
    WAITING,    // request sent, waiting for first MSG_FILE_LIST_RESP
    PARTIAL,    // got some pages, more to come
    READY,      // all pages received
    ERROR,
};

enum class FileLoadState : uint8_t {
    IDLE,
    WAITING_HEADER,  // request sent, waiting for MSG_FILE_LOAD_HEADER
    WAITING_BODY,    // got HEADER, accumulating BODY chunks
    READY,           // EndOfData received, buffer ready to consume
    NOT_FOUND,       // server returned found=0
    ERROR,
};

#define FILE_LIST_CACHE_MAX 64    // ceiling on names cached client-side
#define FILE_LOAD_BUF_MAX   24576 // ceiling on a single fetched file
                                  //   (the master catalog master.txt, 80 songs
                                  //    × name/file/notes, is the largest ~21 KB)

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
    // Tell the server to write its current in-memory song to SD under `name`.
    // Cheap (one small message) — server already has the song bytes thanks
    // to the patch / note-set stream.  Used by autosave.
    // isAutosave=true routes the server's write to /autosave/<name>.mgt
    // (power-loss recovery); false writes to the canonical /songs/<name>.mgt
    // and clears any pending /autosave/<name>.mgt.
    bool sendSaveActive(const char* name, bool isAutosave = false);
    // Tell the server to call initSong() on its in-memory copy — keeps
    // client and server in sync after the user creates a new blank song.
    bool sendNewSong();

    // Tell the server to drop to OFF / No Song (mirrors its local "-- OFF --").
    // Server replies with MsgNoSong, which clears the client via noSongPending().
    bool sendSetOff();

    // ── Live song sync ────────────────────────────────────────────────────────
    bool sendSongPatch(const Song& song, const void* fieldPtr, uint8_t length);
    bool sendNoteSet(const Song& song, uint8_t pattern, uint8_t row, uint8_t col);
    bool sendNoteSetReliable(const Song& song, uint8_t pattern, uint8_t row, uint8_t col);
    bool sendAuditionNote(uint8_t pattern, uint8_t row, uint8_t col);
    // Fire a raw MIDI note-on (no note-off).  Used by DrumTrackImportPage to
    // audition drum blocks before import.
    bool sendAuditionRawNote(uint8_t channel, uint8_t note, uint8_t velocity);
    // Select an audition program (e.g. drum kit on ch10).  Emitted on the
    // server's MIDI task so it stays coherent with running status.
    bool sendAuditionProgram(uint8_t channel, uint8_t program);

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

    // ── Generic server-SD file fetch ────────────────────────────────────────
    // `kind` is a FileKind enum value; the server holds the actual SD path
    // whitelist.  Used by DrumTrackImportPage and (planned) any future PC
    // tool that wants to enumerate / download server files.  List is paged;
    // load is a chunked stream terminated by MsgEndOfData.  Buffer caps at
    // FILE_LOAD_BUF_MAX bytes — anything larger is rejected ERROR.
    void requestFileList(uint8_t kind, uint8_t page = 0);
    void resetFileList();
    FileListState fileListState()      const { return _fileListState; }
    int           fileListCount()      const { return _fileListCount; }
    const char*   fileListName(int i)  const {
        return (i >= 0 && i < _fileListCount) ? _fileListNames[i] : nullptr;
    }

    void requestFileLoad(uint8_t kind, const char* name);
    void resetFileLoad();
    FileLoadState   fileLoadState() const { return _fileLoadState; }
    const uint8_t*  fileLoadData()  const { return _fileLoadBuf; }
    uint32_t        fileLoadLen()   const { return _fileLoadBufLen; }

    // Upload a file to the server SD under `kind` (FileKind).  `name` is the
    // destination filename with extension.  Chunked, fire-and-forget (the
    // server ACKs but the send path doesn't block on it).  Returns the
    // send result.  Mirrors sendRestoreFile.
    bool sendFileSave(uint8_t kind, const char* name,
                      const uint8_t* data, uint32_t dataLen);

    // ── Restore ───────────────────────────────────────────────────────────────
    bool sendRestoreFile(const char* name, bool isInstruments,
                         const uint8_t* data, uint32_t dataLen);

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

    // ── Manual PixelPost control ─────────────────────────────────────────────
    // Each call sends a single MagiLink message; the server updates its PPOST
    // state cache and broadcasts (on-change + 2 s heartbeat coverage).
    bool sendPixelpostEffect        (uint8_t effectIdx);
    bool sendPixelpostSlider        (uint8_t value);
    bool sendPixelpostTouchpad      (uint8_t x, uint8_t y, bool touched);
    bool sendPixelpostPowerOff      (bool off);
    bool sendPixelpostOverride      (uint8_t op);   // PpOverrideOp (perf-page light strip)
    bool sendPixelpostPostCount     (uint8_t count);
    bool sendPixelpostFirmwareUpdate();
    bool sendPixelpostFlashCtrl     (uint8_t value);

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

    // ── Server discovery (MSG_SERVER_ANNOUNCE) ───────────────────────────
    // True once we've seen at least one beacon since boot.  Used by the
    // EXTERNAL_AP setup path to know when it can call beginConnect().
    bool        hasDiscoveredServerIP() const { return _discoveredServerIPValid; }
    IPAddress   discoveredServerIP()    const { return _discoveredServerIP; }
    uint16_t    discoveredServerPort()  const { return _discoveredServerPort; }

    // ── Internal (called by receive callbacks) ────────────────────────────────
    // _onReceive — UDP datagrams only (row position, performer MIDI, preview
    //              playhead, server announce).  TCP/MagiLink delivery goes via
    //              registered MagiLink callbacks below.
    // _onPairReceive — pairing ceremony, off the ESP-NOW transport.
    void _onReceive(const uint8_t* data, int len, const IPAddress& src);
    void _onPairReceive(const uint8_t* data, int len);
    // MagiLink session handshake — called from the registered callback
    // (worker task, mutex held).  Sends MsgConnectAck and transitions
    // _pairState to SUCCESS.
    void _onMagiLinkConnect();
    // MagiLink: server sent MSG_DISCONNECT.  Transition out of SUCCESS.
    void _onMagiLinkDisconnect();
    // MagiLink dispatcher for song-related messages: PUSH_HEADER, PUSH_BODY,
    // NO_SONG.  Switches on msg[0] and updates browse state / _songBuf.
    void _onMagiLinkSongMessage(const uint8_t* msg, size_t len);
    // MagiLink server-state notification: MSG_PLAY / MSG_STOP from server
    // tell us whether its sequencer is running.  Updates _serverPlaying.
    void _onMagiLinkServerState(bool playing);
    // MagiLink instruments push (server → client).  PUSH_HEADER allocates +
    // resets receive counters; PUSH_BODY appends into _instBuf; completion
    // sets _instReady.
    void _onMagiLinkInstrumentsMessage(const uint8_t* msg, size_t len);
    // MagiLink sample list response (server → client).  Appends to the
    // cache; if more pages are pending, sends a request for the next.
    void _onMagiLinkSampleList(const uint8_t* msg, size_t len);
    // MagiLink generic file list response.  Same paging pattern as samples.
    void _onMagiLinkFileList(const uint8_t* msg, size_t len);
    // MagiLink generic file load dispatcher: handles HEADER, BODY chunks,
    // and the trailing MsgEndOfData (id byte routed in).
    void _onMagiLinkFileLoad(const uint8_t* msg, size_t len);

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

    // Pairing ceremony — PIN + server-supplied softAP creds received in
    // MSG_PAIR_CHALLENGE.  For SERVER_AP mode the client uses these
    // instead of the user-entered WiFi page creds.
    uint8_t _pairCode[4]                     = {};
    char    _pairOfferedSsid[MAGI_OFFER_SSID_LEN] = {};
    char    _pairOfferedPsk [MAGI_OFFER_PSK_LEN]  = {};

    // PROBE broadcast pacing.  Pairing is always on ch1 — no channel hop.
    uint32_t _scanLastProbeMs  = 0;

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

    // Server discovery — captured from MSG_SERVER_ANNOUNCE UDP broadcasts.
    // Source IP comes from the datagram itself (passed by MagiUdpLink).
    bool     _discoveredServerIPValid = false;
    IPAddress _discoveredServerIP     = IPAddress((uint32_t)0);
    uint16_t _discoveredServerPort    = 0;
    char     _discoveredServerName[MAGI_ANNOUNCE_NAME_LEN] = {};

    // Song receive buffer
    uint8_t* _songBuf      = nullptr;
    uint32_t _songBufLen   = 0;
    uint8_t  _chunksGot    = 0;
    uint8_t  _chunksTotal  = 0;
    // Set by MSG_SONG_PUSH_HEADER, cleared when the body bytes drain.
    // 0 means "not currently receiving a push" — stray body messages are
    // dropped if this is 0.
    uint32_t _songRecvExpected = 0;
    // Same for instruments push.
    uint32_t _instRecvExpected = 0;

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

    // Generic file-server list cache + per-file load buffer.
    FileListState _fileListState  = FileListState::IDLE;
    uint8_t       _fileListKind   = 0;
    char          _fileListNames[FILE_LIST_CACHE_MAX][FILE_NAME_LEN];
    int           _fileListCount  = 0;
    uint8_t       _fileListPage   = 0;
    uint8_t       _fileListTotalPages = 0;

    FileLoadState _fileLoadState   = FileLoadState::IDLE;
    uint8_t       _fileLoadKind    = 0;
    uint8_t       _fileLoadBuf[FILE_LOAD_BUF_MAX] = {};
    uint32_t      _fileLoadBufLen  = 0;
    uint32_t      _fileLoadExpected = 0;
    char          _fileLoadName[FILE_NAME_LEN] = {};

    static const uint32_t CONNECT_TIMEOUT_MS    = 10000;
    static const uint32_t AUTO_CONNECT_RETRY_MS  = 5000;
    static const uint32_t PAIR_REQUEST_TIMEOUT_MS = 60000;   // match the server's 60 s window
};

extern ServerPairing gServerPairing;

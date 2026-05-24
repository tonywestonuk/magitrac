// MagiMsg.h — shared ESP-NOW message definitions
#pragma once
#include <stdint.h>
#include "TrackerData.h"

// ── SD / file constants (shared between main sketch and commands) ─────────────
#define SRV_FNAME_MAX  24
#define SRV_MAX_FILES  32

// ── Message types ─────────────────────────────────────────────────────────────
enum MagiMsgType : uint8_t {
    // Session
    MSG_BEACON       = 0x01,  // (legacy — no longer sent; kept to avoid reusing the value)
    MSG_CONNECT      = 0x02,  // client → server unicast: "connect to me" (includes HMAC)
    MSG_CONNECT_ACK  = 0x03,  // server → client: "connected" (includes session nonce)
    MSG_DISCONNECT   = 0x04,  // either → other:  "ending session"
    MSG_PING         = 0x05,  // server → client: keepalive ping
    MSG_PONG         = 0x06,  // client → server: keepalive pong

    // Pairing ceremony (one-time setup)
    MSG_PAIR_REQUEST  = 0x07,  // client → broadcast: start one-time pairing
    MSG_PAIR_CONFIRM  = 0x08,  // server → client:    4-digit confirmation code
    MSG_PAIR_ACCEPT   = 0x09,  // client → server:    user confirmed code match
    MSG_PAIR_COMPLETE = 0x0A,  // server → client:    shared secret + server MAC

    // Playback control
    MSG_PLAY             = 0x10,
    MSG_STOP             = 0x11,
    MSG_SET_BPM          = 0x12,
    MSG_SET_SONG_DATA    = 0x13,  // client → server: generic song memory patch
    MSG_PAUSE            = 0x14,  // client → server: freeze position, keep state
    MSG_UNPAUSE          = 0x15,  // client → server: resume from frozen position
    MSG_SET_WIFI_CHANNEL = 0x16,  // client → server: switch WiFi channel (idx 0/1/2 → 1/6/11)

    // Song transfer — server → client (load)
    MSG_SONG_LIST_REQ  = 0x20,
    MSG_SONG_LIST_RESP = 0x21,
    MSG_SONG_LOAD_REQ  = 0x22,
    MSG_SONG_DATA      = 0x23,  // server → client: one download chunk

    // Song transfer — client → server (save)
    MSG_SONG_SAVE      = 0x24,  // client → server: save-start (name + total chunks)
    MSG_SONG_SAVE_DATA = 0x25,  // client → server: one upload chunk
    MSG_SONG_DELETE    = 0x26,  // client → server: delete file by name
    MSG_CHUNK_ACK      = 0x27,  // reserved — formerly per-chunk app ACK; link-layer ACK is the only reliability now
    MSG_SONG_LOAD_NAME = 0x28,  // client → server: request song by bare name (no .mgt)

    // MIDI passthrough — client → server
    MSG_MIDI_DATA      = 0x30,  // client → server: raw MIDI bytes to forward to MIDI out

    // Sequencer position — server → client
    MSG_SEQ_POS        = 0x31,  // server → client: current pattern + row

    // Scrub seek — client → server
    MSG_SEEK           = 0x32,  // client → server: jump to pattern+row and play it (audible scrub)
    MSG_GOTO           = 0x33,  // client → server: jump to pattern+row, do NOT play (block switch)

    // Performance mode — block queueing
    MSG_QUEUE_BLOCK    = 0x35,  // client → server: queue block to play after current ends
    MSG_CANCEL_QUEUE   = 0x36,  // client → server: cancel queued block

    // Column preview — single-column looping playback for the column-note editor
    MSG_PREVIEW_START  = 0x37,  // client → server: start preview of (pattern, column)
    MSG_PREVIEW_STOP   = 0x38,  // client → server: stop preview
    MSG_PREVIEW_ROW    = 0x39,  // server → client: preview playhead is at this row

    // Instruments — bidirectional
    MSG_INSTRUMENTS_REQ   = 0x40,  // client → server: request full instruments array
    MSG_INSTRUMENTS_DATA  = 0x41,  // server → client: one chunk of instruments data
    MSG_INSTRUMENTS_PATCH = 0x42,  // client → server: patch bytes in instruments array

    // Note edit
    MSG_NOTE_SET = 0x50,  // client → server: set or clear one note by (pattern, row, col)

    // Audition — play a freshly-entered note through MIDI for ~500ms
    MSG_NOTE_AUDITION = 0x3A,  // client → server: play (pattern, row, col) briefly

    // Backup/Restore — bidirectional
    MSG_BACKUP_LIST_REQ    = 0x70,  // client → server: request file list with sizes
    MSG_BACKUP_LIST_RESP   = 0x71,  // server → client: one page of file entries
    MSG_BACKUP_FILE_REQ    = 0x72,  // client → server: request raw file by name
    MSG_BACKUP_FILE_START  = 0x73,  // server → client: file header before chunks
    MSG_BACKUP_FILE_DATA   = 0x74,  // server → client: one chunk of raw file bytes
    MSG_RESTORE_FILE_START = 0x75,  // client → server: begin restore upload
    MSG_RESTORE_FILE_DATA  = 0x76,  // client → server: one chunk of restore data

    // Server → client: server set to OFF (no song loaded)
    MSG_NO_SONG  = 0x60,

    // Server → client: MIDI note-on received while sequencer is stopped
    MSG_MIDI_NOTE_IN = 0x61,

    // Sample list (SFX column support) — server enumerates /samples/, maintains
    // /samples/samples.txt manifest with stable IDs, returns paged list.
    MSG_SAMPLE_LIST_REQ  = 0x62,  // client → server: request page of sample list
    MSG_SAMPLE_LIST_RESP = 0x63,  // server → client: one page of (id, filename) entries
};

#pragma pack(push, 1)

// Server broadcasts this while in Client-Server mode (waiting for client)
struct MsgBeacon {
    MagiMsgType type;     // MSG_BEACON
    char        name[16]; // e.g. "MagiTrac"
};

// Client → server unicast: connect request with authentication
// HMAC = HMAC-SHA256(sharedSecret, "CONNECT" || senderMac)[0..7]
struct MsgConnect {
    MagiMsgType type;         // MSG_CONNECT
    uint8_t     senderMac[6]; // client's own MAC
    uint8_t     hmac8[8];     // first 8 bytes of HMAC-SHA256 for authentication
};

// Server → client: session established, includes per-session nonce
struct MsgConnectAck {
    MagiMsgType type;     // MSG_CONNECT_ACK
    uint8_t     nonce[8]; // server-issued session nonce (anti-replay)
};

// ── Pairing ceremony messages ─────────────────────────────────────────────────

// 8-byte magic prefix that pair-ceremony frames carry to distinguish a real
// magitrac MsgPairRequest from a stray 7+8-byte ESP-NOW broadcast that
// happens to start with the right type byte.  Not security — just a sanity
// guard against random traffic on shared channels.
#define MAGI_PAIR_MAGIC  "MAGITRAC"   // 8 chars, no terminator

// Client → broadcast: initiate one-time pairing
struct MsgPairRequest {
    MagiMsgType type;         // MSG_PAIR_REQUEST
    char        magic[8];     // must equal MAGI_PAIR_MAGIC
    uint8_t     senderMac[6]; // client's own MAC (broadcast has no sender info)
};

// Server → client: 4-digit confirmation code to display
struct MsgPairConfirm {
    MagiMsgType type;    // MSG_PAIR_CONFIRM
    uint8_t     code[4]; // 4 ASCII digit bytes, e.g. {'4','7','1','9'}
};

// Client → server: user confirmed codes match (no payload beyond type)
struct MsgPairAccept {
    MagiMsgType type;    // MSG_PAIR_ACCEPT
};

// Server → client: pairing complete — shared secret and server MAC
struct MsgPairComplete {
    MagiMsgType type;         // MSG_PAIR_COMPLETE
    uint8_t     secret[16];   // randomly generated 16-byte shared secret
    uint8_t     serverMac[6]; // server's own MAC address
};

// Either side → other: session ended
struct MsgDisconnect {
    MagiMsgType type;     // MSG_DISCONNECT
};

// Client → server: switch WiFi channel.
// idx is encoded the same way as pixel_post's MSG_CHG_WIFI_CHANNEL so we can
// later send the same payload bytes to both magitrac server and pixel_post.
struct MsgSetWifiChannel {
    MagiMsgType type;     // MSG_SET_WIFI_CHANNEL
    uint8_t     idx;      // 0, 1, 2 → channels 1, 6, 11
};

// Map a channel index (0..2) to the WiFi channel number (1, 6, or 11).
// Any other input maps to 1 as a safe fallback.
inline uint8_t magiWifiChannelFromIdx(uint8_t idx) {
    return (idx == 1) ? 6 : (idx == 2) ? 11 : 1;
}

// Client → server: patch any bytes in the Song struct
// Sent as 4 header bytes + length data bytes (not the full SONG_PATCH_MAX payload).
#define SONG_PATCH_MAX 64

struct MsgSetSongData {
    MagiMsgType type;              // MSG_SET_SONG_DATA
    uint16_t    offset;            // byte offset into Song struct
    uint8_t     length;            // bytes to patch (1..SONG_PATCH_MAX)
    uint8_t     data[SONG_PATCH_MAX];
};

// ── Song list ──────────────────────────────────────────────────────────────────
#define SL_NAME_LEN  16   // max song name chars including null
#define SL_PER_PKT    7   // song names per list-response packet

// Client → Server: request page of song list
struct MsgSongListReq {
    MagiMsgType type;   // MSG_SONG_LIST_REQ
    uint8_t     page;   // 0-based page
};

// Server → Client: one page of the song list
struct MsgSongListResp {
    MagiMsgType type;
    uint8_t     page;
    uint8_t     totalPages;
    uint8_t     count;                      // entries in this packet (0..SL_PER_PKT)
    char        names[SL_PER_PKT][SL_NAME_LEN]; // 7 × 16 = 112 bytes
};

// Client → Server: request a specific song by page + index-within-page
struct MsgSongLoadReq {
    MagiMsgType type;
    uint8_t     page;
    uint8_t     index;
};

// Client → Server: request a specific song by name (no extension).
// Server appends ".mgt" to build the SD path.  Used by the setlist feature
// where the client stores a bare song name rather than a positional ref.
struct MsgSongLoadNameReq {
    MagiMsgType type;
    char        name[SL_NAME_LEN];   // null-terminated, no extension
};

// Server → Client: one chunk of song file data (download)
#define SONG_CHUNK_SIZE 240

struct MsgSongData {
    MagiMsgType type;        // MSG_SONG_DATA
    uint8_t     chunk;       // 0-based
    uint8_t     totalChunks;
    uint8_t     dataLen;     // bytes in payload (1..SONG_CHUNK_SIZE)
    uint8_t     payload[SONG_CHUNK_SIZE];
};

// Client → Server: begin song upload
#define SRV_NAME_MAX 24

struct MsgSongSaveStart {
    MagiMsgType type;              // MSG_SONG_SAVE
    char        name[SRV_NAME_MAX]; // destination filename (no extension)
    uint8_t     totalChunks;
};

// Client → Server: one chunk of song file data (upload) — same layout as MsgSongData
struct MsgSongSaveData {
    MagiMsgType type;        // MSG_SONG_SAVE_DATA
    uint8_t     chunk;
    uint8_t     totalChunks;
    uint8_t     dataLen;
    uint8_t     payload[SONG_CHUNK_SIZE];
};

// Client → Server: delete a file by name
struct MsgSongDelete {
    MagiMsgType type;              // MSG_SONG_DELETE
    char        name[SRV_NAME_MAX]; // filename to delete (no extension)
};

// Client → Server: raw MIDI bytes to forward directly to MIDI out
struct MsgMidiData {
    MagiMsgType type;    // MSG_MIDI_DATA
    uint8_t     len;     // number of MIDI bytes (1–3)
    uint8_t     data[3]; // raw MIDI bytes
};

// Server → Client: current sequencer position
struct MsgSeqPos {
    MagiMsgType type;    // MSG_SEQ_POS
    uint8_t     pattern; // current pattern index
    uint8_t     row;     // current row within that pattern
};

// Client → Server: scrub to position and play
struct MsgSeek {
    MagiMsgType type;    // MSG_SEEK
    uint8_t     pattern;
    uint8_t     row;
};

// Client → Server: position only (no audible play).  When running, tick
// fires on the next iteration and the existing WAIT/play logic kicks in.
struct MsgGoto {
    MagiMsgType type;    // MSG_GOTO
    uint8_t     pattern;
    uint8_t     row;
};

// Client → Server: queue a block to play after current block ends
struct MsgQueueBlock {
    MagiMsgType type;    // MSG_QUEUE_BLOCK
    uint8_t     pattern; // block index to queue
};

// Client → Server: start column preview
struct MsgPreviewStart {
    MagiMsgType type;     // MSG_PREVIEW_START
    uint8_t     pattern;  // pattern to loop
    uint8_t     col;      // 1..MAX_COLUMNS-1 — col 0 makes no MIDI sound
};

// Server → Client: preview playhead position (row within the previewed pattern)
struct MsgPreviewRow {
    MagiMsgType type;  // MSG_PREVIEW_ROW
    uint8_t     row;
};

// Server → Client: one chunk of instruments array (same wire layout as MsgSongData)
struct MsgInstrumentsData {
    MagiMsgType type;        // MSG_INSTRUMENTS_DATA
    uint8_t     chunk;
    uint8_t     totalChunks;
    uint8_t     dataLen;
    uint8_t     payload[SONG_CHUNK_SIZE];
};

// Client → Server: patch bytes in instruments array (same wire layout as MsgSetSongData)
struct MsgInstrumentsPatch {
    MagiMsgType type;    // MSG_INSTRUMENTS_PATCH
    uint16_t    offset;  // byte offset into Instrument[MAX_INSTRUMENTS] array
    uint8_t     length;  // bytes (1..SONG_PATCH_MAX)
    uint8_t     data[SONG_PATCH_MAX];
};

// Client → Server: set or clear a single note
struct MsgNoteSet {
    MagiMsgType type;       // MSG_NOTE_SET
    uint8_t     pattern;    // pattern index
    uint8_t     row;
    uint8_t     col;
    Note        note;       // all-zero = clear the cell
};

// Client → Server: audition the note at (pattern, row, col) for ~500ms.
// Server reads the cell from the active song so transpose/velocity/program
// stay in sync without duplicating logic on the client.
struct MsgNoteAudition {
    MagiMsgType type;       // MSG_NOTE_AUDITION
    uint8_t     pattern;
    uint8_t     row;
    uint8_t     col;
};

// ── Backup / Restore ──────────────────────────────────────────────────────────

#define BK_PER_PKT  5   // backup list entries per response packet

struct BkFileEntry {
    char     name[SRV_FNAME_MAX];  // 24 bytes — filename with extension
    uint16_t sizeHi;               // file size >> 16
    uint16_t sizeLo;               // file size & 0xFFFF
};

struct MsgBackupListReq {
    MagiMsgType type;      // MSG_BACKUP_LIST_REQ
    uint8_t     page;      // 0-based page
};

struct MsgBackupListResp {
    MagiMsgType type;          // MSG_BACKUP_LIST_RESP
    uint8_t     page;
    uint8_t     totalPages;
    uint8_t     count;         // entries this packet (0..BK_PER_PKT)
    uint8_t     totalFiles;    // total across all pages
    BkFileEntry entries[BK_PER_PKT];
};
// 5 + 5*28 = 145 bytes — fits ESP-NOW 250-byte limit

struct MsgBackupFileReq {
    MagiMsgType type;              // MSG_BACKUP_FILE_REQ
    char        name[SRV_FNAME_MAX]; // filename to fetch
};

struct MsgBackupFileStart {
    MagiMsgType type;              // MSG_BACKUP_FILE_START
    char        name[SRV_FNAME_MAX];
    uint16_t    totalSize;         // total file bytes
    uint8_t     totalChunks;
};

struct MsgBackupFileData {
    MagiMsgType type;        // MSG_BACKUP_FILE_DATA
    uint8_t     chunk;
    uint8_t     totalChunks;
    uint8_t     dataLen;
    uint8_t     payload[SONG_CHUNK_SIZE];
};

struct MsgRestoreFileStart {
    MagiMsgType type;              // MSG_RESTORE_FILE_START
    char        name[SRV_FNAME_MAX];
    uint8_t     totalChunks;
    uint8_t     isInstruments;     // 1 = /instruments.mgt, 0 = /songs/<name>
};

struct MsgRestoreFileData {
    MagiMsgType type;        // MSG_RESTORE_FILE_DATA
    uint8_t     chunk;
    uint8_t     totalChunks;
    uint8_t     dataLen;
    uint8_t     payload[SONG_CHUNK_SIZE];
};

// Server → Client: MIDI note-on received while sequencer is stopped
struct MsgMidiNoteIn {
    MagiMsgType type;        // MSG_MIDI_NOTE_IN
    uint8_t     midiNote;    // raw MIDI note number 0–127
    uint8_t     velocity;    // MIDI velocity 0–127
};

// ── Sample list (SFX column) ──────────────────────────────────────────────────
//
// IDs are 1-based, stable, and persisted in /samples/samples.txt on the server.
// On each MSG_SAMPLE_LIST_REQ the server scans /samples/*.wav, appends any new
// files to the manifest with the next free ID, then returns paged results.

#define SAMPLE_NAME_LEN  24    // includes .wav extension, null-terminated
#define SAMPLES_PER_PKT   8    // entries per response packet

struct MsgSampleListReq {
    MagiMsgType type;    // MSG_SAMPLE_LIST_REQ
    uint8_t     page;    // 0-based page
};

struct MsgSampleListEntry {
    uint8_t id;                          // 1..127, stable
    char    name[SAMPLE_NAME_LEN];       // filename including .wav
};

struct MsgSampleListResp {
    MagiMsgType        type;             // MSG_SAMPLE_LIST_RESP
    uint8_t            page;
    uint8_t            totalPages;
    uint8_t            count;            // entries in this packet
    uint8_t            totalEntries;     // total across all pages
    MsgSampleListEntry entries[SAMPLES_PER_PKT];
};
// 5 + 8 * 25 = 205 bytes — fits ESP-NOW 250-byte limit

#pragma pack(pop)

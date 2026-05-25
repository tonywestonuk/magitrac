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
    MSG_PING         = 0x05,  // retired — liveness now via TCP keepalive (id reserved)
    MSG_PONG         = 0x06,  // retired — liveness now via TCP keepalive (id reserved)

    // Pairing ceremony (one-time setup).  Server scans channels 1/6/11
    // broadcasting MSG_PAIR_PROBE; magitrac client (when in pair mode)
    // replies with MSG_PAIR_CHALLENGE carrying a 4-digit PIN it generated.
    // The PIN is shown on both screens; user taps Confirm on magitrac;
    // magitrac then unicasts MSG_PAIR_OFFER with the AP creds + static IP.
    // Server saves and reboots into TCP-STA mode.
    MSG_PAIR_PROBE     = 0x07,  // server → broadcast: "magitrac, are you there?"
    MSG_PAIR_CHALLENGE = 0x08,  // client → server:    PIN to display + confirm
    MSG_PAIR_OFFER     = 0x0B,  // client → server:    AP creds + assigned IP (TCP transport handoff)

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
    MSG_SONG_DATA      = 0x23,  // (retired — superseded by MSG_SONG_BLOB)

    // Song transfer — client → server (save)
    MSG_SONG_SAVE      = 0x24,  // (retired — superseded by MSG_SONG_SAVE_BLOB)
    MSG_SONG_SAVE_DATA = 0x25,  // (retired — superseded by MSG_SONG_SAVE_BLOB)
    MSG_SONG_DELETE    = 0x26,  // client → server: delete file by name
    MSG_CHUNK_ACK      = 0x27,  // reserved — formerly per-chunk app ACK; link-layer ACK is the only reliability now
    MSG_SONG_LOAD_NAME = 0x28,  // client → server: request song by bare name (no .mgt)
    MSG_SONG_BLOB      = 0x29,  // server → client: whole song in one streamed message
    MSG_SONG_SAVE_BLOB = 0x2A,  // client → server: whole song save in one streamed message

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
    MSG_INSTRUMENTS_DATA  = 0x41,  // (retired — superseded by MSG_INSTRUMENTS_BLOB)
    MSG_INSTRUMENTS_PATCH = 0x42,  // client → server: patch bytes in instruments array
    MSG_INSTRUMENTS_BLOB  = 0x43,  // server → client: whole instruments array in one streamed message

    // Note edit
    MSG_NOTE_SET = 0x50,  // client → server: set or clear one note by (pattern, row, col)

    // Audition — play a freshly-entered note through MIDI for ~500ms
    MSG_NOTE_AUDITION = 0x3A,  // client → server: play (pattern, row, col) briefly

    // Backup/Restore — bidirectional
    MSG_BACKUP_LIST_REQ    = 0x70,  // client → server: request full file list (no paging)
    MSG_BACKUP_LIST_RESP   = 0x71,  // (retired — superseded by MSG_BACKUP_LIST_BLOB)
    MSG_BACKUP_FILE_REQ    = 0x72,  // client → server: request raw file by name
    MSG_BACKUP_FILE_START  = 0x73,  // (retired — superseded by MSG_BACKUP_FILE_BLOB)
    MSG_BACKUP_FILE_DATA   = 0x74,  // (retired — superseded by MSG_BACKUP_FILE_BLOB)
    MSG_RESTORE_FILE_START = 0x75,  // (retired — superseded by MSG_RESTORE_FILE_BLOB)
    MSG_RESTORE_FILE_DATA  = 0x76,  // (retired — superseded by MSG_RESTORE_FILE_BLOB)
    MSG_BACKUP_FILE_BLOB   = 0x77,  // server → client: whole file in one streamed message
    MSG_BACKUP_LIST_BLOB   = 0x78,  // server → client: all file entries in one streamed message
    MSG_RESTORE_FILE_BLOB  = 0x79,  // client → server: whole restore file in one streamed message

    // TCP/IP diagnostic test — minimal continuous streamer.  After START,
    // server emits MSG_TCP_TEST_BLOB on every main-loop tick (paced only
    // by TCP backpressure) until STOP.  Client just counts bytes.  No
    // SD, no request/response per frame — pure WiFi+TCP throughput test.
    MSG_TCP_TEST_START     = 0x7A,  // client → server: start streaming blobs
    MSG_TCP_TEST_BLOB      = 0x7B,  // server → client: 4096-byte payload
    MSG_TCP_TEST_STOP      = 0x7C,  // client → server: stop streaming

    // New struct-driven backup transport (v2).  Client sends START_BACKUP
    // once; server streams a continuous sequence of (HEADER + N×BODY) per
    // file, finishing with END_OF_DATA.  Every message is a fixed-size
    // struct in MagiMsg.h — receiver dispatches by id alone, no per-file
    // request/response handshake.
    MSG_START_BACKUP       = 0x80,  // client → server: start the backup
    MSG_BACKUP_HEADER      = 0x81,  // server → client: filename + size + index/total
    MSG_BACKUP_BODY        = 0x82,  // server → client: 1024-byte data slot + data_len
    MSG_END_OF_DATA        = 0x83,  // server → client: marks end of a multi-message stream (generic)

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

// 8-byte magic prefix on the PROBE broadcast to distinguish a real
// magitrac pairing frame from a stray ESP-NOW broadcast that happens to
// start with the right type byte.  Not security — just a sanity guard
// against random traffic on shared channels.
#define MAGI_PAIR_MAGIC  "MAGITRAC"   // 8 chars, no terminator

// Server → broadcast (on channels 1, 6, 11 in turn): "magitrac, are you
// there?"  Magitrac client replies (if in pair mode) with MSG_PAIR_CHALLENGE.
struct MsgPairProbe {
    MagiMsgType type;         // MSG_PAIR_PROBE
    char        magic[8];     // must equal MAGI_PAIR_MAGIC
    uint8_t     senderMac[6]; // server's own MAC (informational; wire src has it too)
};

// Client → server unicast: PIN to display on both screens.  Magitrac
// generates the PIN locally, sends it here, and shows it to the user.
// Server displays the received PIN.  User taps Confirm on magitrac.
struct MsgPairChallenge {
    MagiMsgType type;    // MSG_PAIR_CHALLENGE
    uint8_t     pin[4];  // 4 ASCII digit bytes, e.g. {'4','7','1','9'}
};

// Client → server unicast: AP credentials and assigned IP — sent only
// after the user confirms on the magitrac side.  Server saves to NVS and
// reboots into TCP-STA mode to join the AP.
//
// SSID/PSK lengths are tight to fit the format we generate
// ("magitrac-XXXXXX" + 16 hex chars).  Bump if longer creds are ever
// needed.
#define MAGI_OFFER_SSID_LEN 20
#define MAGI_OFFER_PSK_LEN  20
struct MsgPairOffer {
    MagiMsgType type;                             // MSG_PAIR_OFFER
    char        apSsid[MAGI_OFFER_SSID_LEN];      // null-terminated, ≤ 19 chars
    char        apPsk [MAGI_OFFER_PSK_LEN];       // null-terminated, ≤ 19 chars
    uint8_t     assignedIp[4];                    // {192,168,0,2}
    uint8_t     gatewayIp [4];                    // {192,168,0,1}
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

struct BkFileEntry {
    char     name[SRV_FNAME_MAX];  // 24 bytes — filename with extension
    uint16_t sizeHi;               // file size >> 16
    uint16_t sizeLo;               // file size & 0xFFFF
};

struct MsgBackupListReq {
    MagiMsgType type;      // MSG_BACKUP_LIST_REQ
};

// MSG_BACKUP_LIST_BLOB wire layout (streamed via MagiCommsTcp):
//   type(1) + numFiles(1) + BkFileEntry × numFiles
// No struct definition because the body is variable-length.  At
// SRV_MAX_FILES+1 = 33 entries the blob is 1+1+33*28 = 926 bytes — well
// under the reader buffer.  Old MsgBackupListResp + paging retired.

struct MsgBackupFileReq {
    MagiMsgType type;              // MSG_BACKUP_FILE_REQ
    char        name[SRV_FNAME_MAX]; // filename to fetch
};

// MsgBackupFileStart / MsgBackupFileData (ESP-NOW-era chunked transfer)
// retired — replaced by a single streamed MSG_BACKUP_FILE_BLOB whose wire
// layout is: type(1) + name(SRV_FNAME_MAX) + size(u32 LE) + bytes.  No
// struct definition because the body is variable-length raw bytes; the
// sender uses MagiCommsTcp::streamMore() to shovel SD→socket directly.

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

// ── New struct-driven backup messages (v2 transport) ──────────────────────────
// Every message starts with {id, length} so the receiver always knows how
// many bytes follow, even for types it doesn't recognise.

struct MsgStartBackup {
    uint8_t  id;          // MSG_START_BACKUP
    uint16_t length;      // sizeof(MsgStartBackup) — always 3
};

struct MsgBackupHeader {
    uint8_t  id;          // MSG_BACKUP_HEADER
    uint16_t length;      // sizeof(MsgBackupHeader)
    char     filename[24];
    uint32_t file_size;   // total bytes in this file
    uint16_t file_index;  // 1-based: this is the Nth file
    uint16_t file_total;  // total files in this backup (for progress UI)
};
// 1 + 2 + 24 + 4 + 2 + 2 = 35 bytes

struct MsgBackupBody {
    uint8_t  id;          // MSG_BACKUP_BODY
    uint16_t length;      // sizeof(MsgBackupBody) — always 1029
    uint16_t data_len;    // 0..1024 — meaningful bytes in `data`
    uint8_t  data[1024];  // always sent in full, trailing bytes undefined
};
// 1 + 2 + 2 + 1024 = 1029 bytes

struct MsgEndOfData {
    uint8_t  id;          // MSG_END_OF_DATA
    uint16_t length;      // sizeof(MsgEndOfData) — always 3
};

#pragma pack(pop)

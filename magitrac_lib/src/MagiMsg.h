// MagiMsg.h — shared ESP-NOW message definitions
#pragma once
#include <stdint.h>
#include "TrackerData.h"

// ── SD / file constants (shared between main sketch and commands) ─────────────
#define SRV_FNAME_MAX  24
#define SRV_MAX_FILES  32

// Magitrac default port — used for the MagiLink TCP listener and the
// MagiUdpLink datagram listener (TCP/UDP namespaces are disjoint so they
// coexist).
#define MAGI_PORT 4242

// ── Network topology constants ──────────────────────────────────────────────
//
// Server and client always sit at these IPs regardless of who hosts the AP.
// In SERVER_AP mode the server is also the AP (so the softAP IP == server IP).
// In EXTERNAL_AP mode both devices are STAs against an external AP that must
// live on the 192.168.0.0/24 subnet and avoid .1/.2.
//
// Octet arrays for use with PairNVS / WiFi.config (which take 4-byte arrays).
#define MAGI_SERVER_IP_0  192
#define MAGI_SERVER_IP_1  168
#define MAGI_SERVER_IP_2  0
#define MAGI_SERVER_IP_3  1
#define MAGI_CLIENT_IP_0  192
#define MAGI_CLIENT_IP_1  168
#define MAGI_CLIENT_IP_2  0
#define MAGI_CLIENT_IP_3  2

// ── AP topology mode ────────────────────────────────────────────────────────
//
// Lives in MsgPairOffer + per-device NVS.  Tells the server whether it
// should host an AP itself or join an external one.  The client is
// always a STA — it doesn't branch on this value at boot.
enum MagiApMode : uint8_t {
    MAGI_AP_MODE_SERVER   = 0,  // server hosts the AP at MAGI_SERVER_IP
    MAGI_AP_MODE_EXTERNAL = 1,  // external AP; server joins as STA
};

// ── Message types ─────────────────────────────────────────────────────────────
enum MagiMsgType : uint8_t {
    // MagiLink session
    MSG_CONNECT      = 0x02,  // server → client (over TCP): "session up"
    MSG_CONNECT_ACK  = 0x03,  // client → server: "ack"
    MSG_DISCONNECT   = 0x04,  // either → other:  "ending session"

    // Server presence beacon — UDP broadcast on MAGI_PORT every ~2 s.
    // Lets clients discover the server's IP without a fixed-address
    // convention (e.g. when both devices take DHCP on an external AP).
    // Source IP comes from the datagram itself; payload carries only TCP
    // port + a friendly name.  See MsgServerAnnounce below.
    MSG_SERVER_ANNOUNCE = 0x09,

    // Pairing ceremony (one-time setup, over ESP-NOW, always on ch1).
    // The client owns the WiFi credentials (set via the WiFi settings
    // page on the touch screen) and ships them to the server in OFFER.
    //
    //   1. Client (pair mode) sits on ch1, broadcasts MSG_PAIR_PROBE.
    //   2. Server (pair mode) sits on ch1 listening.  On PROBE: gen PIN,
    //      unicast MSG_PAIR_CHALLENGE back, display PIN on LCD.
    //   3. Client displays the received PIN on the touch screen.  User
    //      compares + taps Confirm on the client.
    //   4. Client unicasts MSG_PAIR_OFFER {apMode, apSsid, apPsk}.
    //   5. Server persists creds + reboots into the chosen WiFi mode.
    //
    // No channel scan — both sides force ch1 on enterPairing.
    MSG_PAIR_PROBE     = 0x07,  // client → broadcast: "magitrac server, are you there?"
    MSG_PAIR_CHALLENGE = 0x08,  // server → client:    PIN to display + confirm
    MSG_PAIR_OFFER     = 0x0B,  // client → server:    WiFi creds + apMode

    // Playback control
    MSG_PLAY             = 0x10,
    MSG_STOP             = 0x11,
    MSG_SET_SONG_DATA    = 0x13,  // client → server: generic song memory patch
    MSG_PAUSE            = 0x14,  // client → server: freeze position, keep state
    MSG_UNPAUSE          = 0x15,  // client → server: resume from frozen position
    MSG_SET_WIFI_CHANNEL = 0x16,  // client → server: switch WiFi channel (idx 0/1/2 → 1/6/11)

    // Song transfer
    MSG_SONG_LIST_REQ  = 0x20,
    MSG_SONG_LIST_RESP = 0x21,
    MSG_SONG_LOAD_REQ  = 0x22,
    MSG_SONG_DELETE    = 0x26,  // client → server: delete file by name
    MSG_SONG_LOAD_NAME = 0x28,  // client → server: request song by bare name (no .mgt)
    MSG_SET_NO_SONG    = 0x29,  // client → server: set server to OFF / No Song
    MSG_ORGAN          = 0x2A,  // client → server: drawbar organ control (enter/exit/set bar)

    // MIDI passthrough — client → server
    MSG_MIDI_DATA      = 0x30,  // client → server: raw MIDI bytes to forward to MIDI out

    // Sequencer position — server → client (UDP, loss-tolerant)
    MSG_SEQ_POS        = 0x31,

    // Scrub seek — client → server
    MSG_SEEK           = 0x32,  // client → server: jump to pattern+row and play it (audible scrub)
    MSG_GOTO           = 0x33,  // client → server: jump to pattern+row, do NOT play (block switch)

    // Performance mode — block queueing
    MSG_QUEUE_BLOCK    = 0x35,  // client → server: queue block to play after current ends
    MSG_CANCEL_QUEUE   = 0x36,  // client → server: cancel queued block

    // Column preview — single-column looping playback for the column-note editor
    MSG_PREVIEW_START  = 0x37,  // client → server: start preview of (pattern, column)
    MSG_PREVIEW_STOP   = 0x38,  // client → server: stop preview
    MSG_PREVIEW_ROW    = 0x39,  // server → client: preview playhead is at this row (UDP)

    // Instruments
    MSG_INSTRUMENTS_REQ   = 0x40,  // client → server: request full instruments array
    MSG_INSTRUMENTS_PATCH = 0x42,  // client → server: patch bytes in instruments array

    // Note edit
    MSG_NOTE_SET = 0x50,  // client → server: set or clear one note by (pattern, row, col)

    // Audition — play a freshly-entered note through MIDI for ~500ms
    MSG_NOTE_AUDITION = 0x3A,

    // Backup transport (client → server one-shot trigger, then server
    // streams a continuous sequence of (HEADER + N×BODY) per file,
    // finishing with END_OF_DATA).  Every message is a fixed-size struct
    // — receiver dispatches by id alone, no per-file handshake.
    MSG_START_BACKUP       = 0x80,  // client → server: start the backup
    MSG_BACKUP_HEADER      = 0x81,  // server → client: filename + size + index/total
    MSG_BACKUP_BODY        = 0x82,  // server → client: 1024-byte data slot + data_len
    MSG_END_OF_DATA        = 0x83,  // server → client: marks end of a multi-message stream (generic)

    // Song push / load (server → client, MagiLink).  Server streams the
    // active song's bytes (SongFileHeader + Song) as HEADER + N×BODY.
    // No end marker — header carries total_size; receiver knows when
    // bytes_received == total_size that the stream is complete.
    MSG_SONG_PUSH_HEADER   = 0x84,
    MSG_SONG_PUSH_BODY     = 0x85,

    // Song save (client → server).  Same streaming pattern but reversed:
    // client streams its in-memory Song to the server.  Header carries
    // destination filename (empty = "push, don't write to SD") and the
    // SongFileHeader; bodies carry the raw Song bytes.
    MSG_SAVE_SONG_HEADER   = 0x86,
    MSG_SAVE_SONG_BODY     = 0x87,

    // Instruments push (server → client).  HEADER + N×BODY, same shape
    // as song push but the receiver writes into _instBuf instead of
    // _songBuf.  Triggered by client sending MSG_INSTRUMENTS_REQ.
    MSG_INSTRUMENTS_PUSH_HEADER = 0x88,
    MSG_INSTRUMENTS_PUSH_BODY   = 0x89,

    // Restore (client → server).  HEADER carries filename + isInstruments
    // flag + total_size; BODY carries raw file bytes.  Server accumulates
    // into a side buffer and main-loop tick writes to SD.
    MSG_RESTORE_HEADER          = 0x8A,
    MSG_RESTORE_BODY            = 0x8B,

    // Save active (client → server).  Tells the server to write its
    // current in-memory song to SD under `name`.  No payload beyond the
    // name — the server already has the song bytes in srvActiveBuf
    // (kept in sync by patch / note-set messages), so this is much
    // cheaper than streaming the full song.
    MSG_SAVE_ACTIVE             = 0x8C,

    // New song (client → server).  Tells the server to initSong() its
    // own in-memory copy — keeps server / client in sync after the
    // client wipes the song without streaming any bytes.
    MSG_NEW_SONG                = 0x8D,

    // ── Generic file server ─────────────────────────────────────────────
    // Used by anything that wants to enumerate / fetch a known kind of
    // file on the server SD without needing its own dedicated message
    // type.  The `kind` byte (FileKind enum) names the directory; the
    // server holds a whitelist mapping kinds to actual SD paths.  Current
    // users: drum-track imports (FK_DRUMTRACKS), performer setlists
    // (FK_SETLISTS).  Future: presets, palettes, the planned Java/Swing PC
    // tool for SD upload/download.  ID 0x96 reserved for DELETE once needed.
    MSG_FILE_LIST_REQ           = 0x8E,
    MSG_FILE_LIST_RESP          = 0x8F,
    MSG_FILE_LOAD_REQ           = 0x90,
    MSG_FILE_LOAD_HEADER        = 0x91,
    MSG_FILE_LOAD_BODY          = 0x92,
    MSG_FILE_SAVE_HEADER        = 0x93,   // client → server: begin chunked upload
    MSG_FILE_SAVE_BODY          = 0x94,   // client → server: one chunk
    MSG_FILE_SAVE_ACK           = 0x95,   // server → client: save result

    // Client → server: fire-and-forget MIDI note-on for audition (no note-off,
    // assumes percussion-style one-shot envelope on the receiving channel).
    // Used by DrumTrackImportPage to play through a drum block before import.
    MSG_AUDITION_RAW_NOTE       = 0x97,

    // Client → server: program change for audition (e.g. drum-kit select on
    // ch10).  Emitted on the server's MIDI task so it shares the sequencer's
    // running-status cache — a raw MIDI_DATA write would desync it.
    MSG_AUDITION_PROGRAM        = 0x98,

    // Manual PixelPost control — client → server.  The server keeps a state
    // cache that drives the PPOST broadcast (see pixelpost_send.ino); these
    // messages just call the matching pixelpostSetX() setter so the next
    // change-or-heartbeat PPOST reflects the new state.
    MSG_PIXELPOST_SET_EFFECT     = 0x99,
    MSG_PIXELPOST_SET_SLIDER     = 0x9A,
    MSG_PIXELPOST_SET_TOUCHPAD   = 0x9B,
    MSG_PIXELPOST_POWER_OFF      = 0x9C,
    MSG_PIXELPOST_SET_POST_COUNT = 0x9D,
    MSG_PIXELPOST_FIRMWARE_UPDATE = 0x9E,
    MSG_PIXELPOST_SET_FLASH_CTRL  = 0x9F,
    // Perf-page live light override — grab effect control away from the PXL
    // POST track (NEXT/PREV/POW) or hand it back (RELEASE).
    MSG_PIXELPOST_OVERRIDE        = 0xA0,

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

// MagiLink session handshake.  Server (STA) sends MsgConnect once TCP
// is up; client (AP) replies with MsgConnectAck.  No payload — identity
// is established by TCP itself, auth by the WPA2-PSK on the AP.  Both
// structs follow the new {id, length} framing.
//
// All MagiLink structs from here on use NSDMI for id and length so the
// caller never needs to fill them in:
//   MsgFoo msg;                            // id + length already set
//   msg.payload_field = ...;               // set whatever varies
//   gMagiLink.send(&msg, sizeof(msg));
struct MsgConnect {
    uint8_t  id     = MSG_CONNECT;
    uint16_t length = sizeof(MsgConnect);
};

struct MsgConnectAck {
    uint8_t  id     = MSG_CONNECT_ACK;
    uint16_t length = sizeof(MsgConnectAck);
};

// ── Pairing ceremony messages ─────────────────────────────────────────────────

// 8-byte magic prefix on the PROBE broadcast to distinguish a real
// magitrac pairing frame from a stray ESP-NOW broadcast that happens to
// start with the right type byte.  Not security — just a sanity guard
// against random traffic on shared channels.
#define MAGI_PAIR_MAGIC  "MAGITRAC"   // 8 chars, no terminator

// Client → broadcast (on channels 1, 6, 11 in turn): "magitrac server,
// are you there?"  Server (if in pair mode) replies on its AP channel
// with MSG_PAIR_CHALLENGE.
struct MsgPairProbe {
    MagiMsgType type;         // MSG_PAIR_PROBE
    char        magic[8];     // must equal MAGI_PAIR_MAGIC
    uint8_t     senderMac[6]; // client's own MAC (informational; wire src has it too)
};

// Server → client unicast: PIN to display on both screens + the server's
// own softAP credentials.  Server generates a Magitrac_XXXX SSID + random
// PSK on first pair-mode entry and stores them in NVS so the same pair of
// strings comes back every challenge until factory-reset.  Client echoes
// them in MsgPairOffer (so the server can write a single NVS record) AND
// stores them in its own NVS so STA can join the softAP after reboot.
#define MAGI_OFFER_SSID_LEN 33
#define MAGI_OFFER_PSK_LEN  64
struct MsgPairChallenge {
    MagiMsgType type;                          // MSG_PAIR_CHALLENGE
    uint8_t     pin[4];                        // 4 ASCII digit bytes
    char        apSsid[MAGI_OFFER_SSID_LEN];   // server-generated, null-padded
    char        apPsk [MAGI_OFFER_PSK_LEN];    // server-generated, null-padded
};

// Client → server unicast: WiFi credentials + apMode.  Sent after the
// user confirms the PIN on the touch screen.  Server persists to NVS
// and reboots into the chosen WiFi mode (host AP or join external AP).
//
// SSID/PSK fields sized to the WPA2 maxima (32/63 chars + null) so the
// user can pick any normal home/venue network in EXTERNAL_AP mode.
// In SERVER_AP mode the apSsid/apPsk fields are the server-generated
// Magitrac_XXXX creds that the client received in MsgPairChallenge and
// is echoing back (the server already has them; the echo is just so the
// existing single-save NVS path covers both modes).
struct MsgPairOffer {
    MagiMsgType type;                             // MSG_PAIR_OFFER
    uint8_t     apMode;                           // MagiApMode enum
    char        apSsid[MAGI_OFFER_SSID_LEN];      // null-terminated, ≤ 32 chars
    char        apPsk [MAGI_OFFER_PSK_LEN];       // null-terminated, ≤ 63 chars
};

// Either side → other: session ended
struct MsgDisconnect {
    uint8_t  id     = MSG_DISCONNECT;
    uint16_t length = sizeof(MsgDisconnect);
};

// UDP server presence beacon.  Broadcast by the magitrac server every ~2 s
// to UDP MAGI_PORT.  Receivers discover the server's IP from the source
// address of the datagram — payload carries only the TCP port + a friendly
// name (typically the SSID or "magitrac-srv").  This is a UDP-only message;
// it does NOT use MagiLink's {id, length} TCP framing — the UDP datagram
// boundary is the framing.  Total wire size: 19 bytes.
#define MAGI_ANNOUNCE_NAME_LEN 16
struct MsgServerAnnounce {
    MagiMsgType type;                              // MSG_SERVER_ANNOUNCE
    uint16_t    tcpPort;                           // typically MAGI_PORT
    char        name[MAGI_ANNOUNCE_NAME_LEN];      // null-terminated friendly name
};

// ── Fire-and-forget playback controls (MagiLink) ──────────────────────────
// All client → server.  No payload — id alone is the instruction.  Server
// registers one MagiLink callback per id.
struct MsgPlay {
    uint8_t  id     = MSG_PLAY;
    uint16_t length = sizeof(MsgPlay);
};

struct MsgStop {
    uint8_t  id     = MSG_STOP;
    uint16_t length = sizeof(MsgStop);
};

struct MsgPause {
    uint8_t  id     = MSG_PAUSE;
    uint16_t length = sizeof(MsgPause);
};

struct MsgUnpause {
    uint8_t  id     = MSG_UNPAUSE;
    uint16_t length = sizeof(MsgUnpause);
};

// Client → server: switch WiFi channel.
// idx is encoded the same way as pixel_post's MSG_CHG_WIFI_CHANNEL so we can
// later send the same payload bytes to both magitrac server and pixel_post.
struct MsgSetWifiChannel {
    uint8_t  id     = MSG_SET_WIFI_CHANNEL;
    uint16_t length = sizeof(MsgSetWifiChannel);
    uint8_t  idx;   // 0, 1, 2 → channels 1, 6, 11
};

// Drawbar organ control — client → server.  One message type, three ops, so it
// costs a single MagiLink callback slot (the table is nearly full).
//  ORGAN_OP_ENTER → server switches to the organ screen + starts the synth
//  ORGAN_OP_EXIT  → server leaves the organ screen + stops the synth
//  ORGAN_OP_SET   → set drawbar `index` (0..8) to `value` (0..8)
//  ORGAN_OP_TYPE      → select voice model: `value` = organ type index
//  ORGAN_OP_VIBCHORUS → scanner: `value` 0=off,1-3=V1-3,4-6=C1-3
//  ORGAN_OP_LESLIE    → rotor: `value` 0=stop,1=slow,2=fast
//  ORGAN_OP_DRIVE     → tube drive: `value` 0=off,1=on
//  ORGAN_OP_PARAM     → per-type knob: `index` = knob 0..2, `value` 0..8
//  ORGAN_OP_PROCSEL   → PROC type: select procedural sound `value`
//  ORGAN_OP_REVERB    → stereo reverb: `value` 0=off,1=room,2=hall
enum OrganOp : uint8_t {
    ORGAN_OP_EXIT = 0, ORGAN_OP_ENTER = 1, ORGAN_OP_SET = 2, ORGAN_OP_TYPE = 3,
    ORGAN_OP_VIBCHORUS = 4, ORGAN_OP_LESLIE = 5, ORGAN_OP_DRIVE = 6, ORGAN_OP_PARAM = 7,
    ORGAN_OP_PROCSEL = 8, ORGAN_OP_REVERB = 9
};
struct MsgOrgan {
    uint8_t  id     = MSG_ORGAN;
    uint16_t length = sizeof(MsgOrgan);
    uint8_t  op;      // OrganOp
    uint8_t  index;   // drawbar 0..8 (ORGAN_OP_SET only)
    uint8_t  value;   // 0..8        (ORGAN_OP_SET only)
};

// Map a channel index (0..2) to the WiFi channel number (1, 6, or 11).
// Any other input maps to 1 as a safe fallback.
inline uint8_t magiWifiChannelFromIdx(uint8_t idx) {
    return (idx == 1) ? 6 : (idx == 2) ? 11 : 1;
}

// ── Manual PixelPost control structs ───────────────────────────────────────
// Each maps 1:1 onto the matching pixelpostSetX() setter on the server.
struct MsgPixelpostSetEffect {
    uint8_t  id     = MSG_PIXELPOST_SET_EFFECT;
    uint16_t length = sizeof(MsgPixelpostSetEffect);
    uint8_t  effectIdx;
};

struct MsgPixelpostSetSlider {
    uint8_t  id     = MSG_PIXELPOST_SET_SLIDER;
    uint16_t length = sizeof(MsgPixelpostSetSlider);
    uint8_t  value;     // 0..255
};

struct MsgPixelpostSetTouchpad {
    uint8_t  id     = MSG_PIXELPOST_SET_TOUCHPAD;
    uint16_t length = sizeof(MsgPixelpostSetTouchpad);
    uint8_t  x;         // 0..255
    uint8_t  y;         // 0..255
    uint8_t  touched;   // 0 = lifted, 1 = finger down
};

struct MsgPixelpostPowerOff {
    uint8_t  id     = MSG_PIXELPOST_POWER_OFF;
    uint16_t length = sizeof(MsgPixelpostPowerOff);
    uint8_t  off;       // 0 = clear, 1 = power off
};

struct MsgPixelpostSetPostCount {
    uint8_t  id     = MSG_PIXELPOST_SET_POST_COUNT;
    uint16_t length = sizeof(MsgPixelpostSetPostCount);
    uint8_t  count;     // total posts in show (0 = unknown)
};

// No payload — server holds the OTA SSID / password / URL constants.
struct MsgPixelpostFirmwareUpdate {
    uint8_t  id     = MSG_PIXELPOST_FIRMWARE_UPDATE;
    uint16_t length = sizeof(MsgPixelpostFirmwareUpdate);
};

// Photo-sensitivity / max-brightness control.  Single byte:
//   upper nibble = max brightness (0..15, 15 = full)
//   lower nibble = smoothing      (0..4 useful, higher clamped)
//   0x00         = layer no-op (passthrough on the post side)
struct MsgPixelpostSetFlashCtrl {
    uint8_t  id     = MSG_PIXELPOST_SET_FLASH_CTRL;
    uint16_t length = sizeof(MsgPixelpostSetFlashCtrl);
    uint8_t  flashCtrl;
};

// Live light override from the performance page.  op selects the action; the
// first NEXT/PREV/POW grabs effect control away from the PXL POST track, and
// RELEASE hands it back so the track drives the lights again.
enum PpOverrideOp : uint8_t {
    PPO_RELEASE   = 0,   // return control to the PXL POST track
    PPO_NEXT      = 1,   // grab + cycle to the next effect
    PPO_PREV      = 2,   // grab + cycle to the previous effect
    PPO_WHITE_ON  = 3,   // WHITE button pressed  — grab + ramp to full white
    PPO_WHITE_OFF = 4,   // WHITE button released — fade back to black
};
struct MsgPixelpostOverride {
    uint8_t  id     = MSG_PIXELPOST_OVERRIDE;
    uint16_t length = sizeof(MsgPixelpostOverride);
    uint8_t  op;       // PpOverrideOp
};

// Client → server: patch any bytes in the Song struct
// Sent as 4 header bytes + length data bytes (not the full SONG_PATCH_MAX payload).
#define SONG_PATCH_MAX 64

// Variable-length send: only the bytes actually carrying the patch are
// sent.  Caller MUST set `length = 6 + dataLen` and pass `6 + dataLen`
// to gMagiLink.send so the wire length matches the actual byte count.
// NSDMI's default `length = sizeof(MsgSetSongData)` is wrong for this
// message because trailing `data[]` bytes after `dataLen` are garbage.
struct MsgSetSongData {
    uint8_t  id     = MSG_SET_SONG_DATA;
    uint16_t length = sizeof(MsgSetSongData);   // OVERRIDE before send
    uint16_t offset;                            // byte offset into Song struct
    uint8_t  dataLen;                           // payload bytes (1..SONG_PATCH_MAX)
    uint8_t  data[SONG_PATCH_MAX];
};

// ── Song list ──────────────────────────────────────────────────────────────────
#define SL_NAME_LEN  16   // max song name chars including null
#define SL_PER_PKT    7   // song names per list-response packet

// Client → Server: request page of song list
struct MsgSongListReq {
    uint8_t  id     = MSG_SONG_LIST_REQ;
    uint16_t length = sizeof(MsgSongListReq);
    uint8_t  page;   // 0-based page
};

// Server → Client: one page of the song list
struct MsgSongListResp {
    uint8_t  id     = MSG_SONG_LIST_RESP;
    uint16_t length = sizeof(MsgSongListResp);
    uint8_t  page;
    uint8_t  totalPages;
    uint8_t  count;                            // entries in this packet (0..SL_PER_PKT)
    char     names[SL_PER_PKT][SL_NAME_LEN];   // 7 × 16 = 112 bytes
};

// Client → Server: request a specific song by page + index-within-page
struct MsgSongLoadReq {
    uint8_t  id     = MSG_SONG_LOAD_REQ;
    uint16_t length = sizeof(MsgSongLoadReq);
    uint8_t  page;
    uint8_t  index;
};

// Client → Server: request a specific song by name (no extension).
// Server appends ".mgt" to build the SD path.  Used by the setlist feature
// where the client stores a bare song name rather than a positional ref.
// Migrated to MagiLink — {id, length, name}.
struct MsgSongLoadNameReq {
    uint8_t  id     = MSG_SONG_LOAD_NAME;
    uint16_t length = sizeof(MsgSongLoadNameReq);
    char     name[SL_NAME_LEN];   // null-terminated, no extension
};

// Max destination filename for song save (no extension).
#define SRV_NAME_MAX 24

// Client → Server: delete a file by name
struct MsgSongDelete {
    uint8_t  id     = MSG_SONG_DELETE;
    uint16_t length = sizeof(MsgSongDelete);
    char     name[SRV_NAME_MAX];   // filename to delete (no extension)
};

// Client → Server: raw MIDI bytes to forward directly to MIDI out.
// High-frequency, latency-sensitive — each note-on/off is one of these.
// Payload field `len` (1..3) renamed from the legacy struct's same name
// only because we now also have a wire `length`.
struct MsgMidiData {
    uint8_t  id     = MSG_MIDI_DATA;
    uint16_t length = sizeof(MsgMidiData);
    uint8_t  midiLen;     // number of MIDI bytes in data[] (1..3)
    uint8_t  data[3];
};

// Server → Client: current sequencer position
struct MsgSeqPos {
    MagiMsgType type;    // MSG_SEQ_POS
    uint8_t     pattern; // current pattern index
    uint8_t     row;     // current row within that pattern
};

// Client → Server: scrub to position and play
struct MsgSeek {
    uint8_t  id      = MSG_SEEK;
    uint16_t length  = sizeof(MsgSeek);
    uint8_t  pattern;
    uint8_t  row;
};

// Client → Server: position only (no audible play).  When running, tick
// fires on the next iteration and the existing WAIT/play logic kicks in.
struct MsgGoto {
    uint8_t  id      = MSG_GOTO;
    uint16_t length  = sizeof(MsgGoto);
    uint8_t  pattern;
    uint8_t  row;
};

// Client → Server: queue a block to play after current block ends
struct MsgQueueBlock {
    uint8_t  id     = MSG_QUEUE_BLOCK;
    uint16_t length = sizeof(MsgQueueBlock);
    uint8_t  pattern;
};

// Client → Server: cancel a queued block.
struct MsgCancelQueue {
    uint8_t  id     = MSG_CANCEL_QUEUE;
    uint16_t length = sizeof(MsgCancelQueue);
};

// Client → Server: start column preview
struct MsgPreviewStart {
    uint8_t  id     = MSG_PREVIEW_START;
    uint16_t length = sizeof(MsgPreviewStart);
    uint8_t  pattern;
    uint8_t  col;   // 1..MAX_COLUMNS-1 — col 0 makes no MIDI sound
};

// Client → Server: stop column preview.
struct MsgPreviewStop {
    uint8_t  id     = MSG_PREVIEW_STOP;
    uint16_t length = sizeof(MsgPreviewStop);
};

// Server → Client: preview playhead position (row within the previewed pattern)
struct MsgPreviewRow {
    MagiMsgType type;  // MSG_PREVIEW_ROW
    uint8_t     row;
};

// Client → Server: patch bytes in instruments array.  Same shape +
// variable-length send convention as MsgSetSongData — caller must set
// `length = 6 + dataLen` and pass `6 + dataLen` to gMagiLink.send.
struct MsgInstrumentsPatch {
    uint8_t  id     = MSG_INSTRUMENTS_PATCH;
    uint16_t length = sizeof(MsgInstrumentsPatch);   // OVERRIDE before send
    uint16_t offset;                                 // byte offset into Instrument[MAX_INSTRUMENTS]
    uint8_t  dataLen;                                // payload bytes (1..SONG_PATCH_MAX)
    uint8_t  data[SONG_PATCH_MAX];
};

// Client → Server: set or clear a single note (migrated to MagiLink).
struct MsgNoteSet {
    uint8_t  id      = MSG_NOTE_SET;
    uint16_t length  = sizeof(MsgNoteSet);
    uint8_t  pattern;
    uint8_t  row;
    uint8_t  col;
    Note     note;       // all-zero = clear the cell
};

// Client → Server: audition the note at (pattern, row, col) for ~500ms.
// Server reads the cell from the active song so transpose/velocity/program
// stay in sync without duplicating logic on the client.
struct MsgNoteAudition {
    uint8_t  id      = MSG_NOTE_AUDITION;
    uint16_t length  = sizeof(MsgNoteAudition);
    uint8_t  pattern;
    uint8_t  row;
    uint8_t  col;
};

// Client → Server: fire a raw MIDI note-on on (channel, note, velocity).
// No note-off is sent; intended for short percussion samples whose synth
// envelope finishes them naturally.  channel == SFX_CHANNEL plays column
// `col`'s sample instead, pitched by the raw MIDI note (60 = native + TUNE)
// — the tune-by-ear path while the sequencer is stopped.
struct MsgAuditionRawNote {
    uint8_t  id       = MSG_AUDITION_RAW_NOTE;
    uint16_t length   = sizeof(MsgAuditionRawNote);
    uint8_t  channel;   // 1..16, or SFX_CHANNEL (see col)
    uint8_t  note;      // 0..127
    uint8_t  velocity;  // 0..127
    uint8_t  col;       // SFX only: source column for sample + tune (0xFF = none)
};

// Client → Server: program change for audition (drum-kit select etc.).
// Emitted on the server's MIDI task so it stays coherent with the
// sequencer's running-status cache (unlike a raw MsgMidiData write).
struct MsgAuditionProgram {
    uint8_t  id       = MSG_AUDITION_PROGRAM;
    uint16_t length   = sizeof(MsgAuditionProgram);
    uint8_t  channel;   // 1..16
    uint8_t  program;   // 0..127
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
    uint8_t  id     = MSG_SAMPLE_LIST_REQ;
    uint16_t length = sizeof(MsgSampleListReq);
    uint8_t  page;
};

struct MsgSampleListEntry {
    uint8_t id;                          // 1..127, stable
    char    name[SAMPLE_NAME_LEN];       // filename including .wav
};

struct MsgSampleListResp {
    uint8_t            id     = MSG_SAMPLE_LIST_RESP;
    uint16_t           length = sizeof(MsgSampleListResp);
    uint8_t            page;
    uint8_t            totalPages;
    uint8_t            count;            // entries in this packet
    uint8_t            totalEntries;     // total across all pages
    MsgSampleListEntry entries[SAMPLES_PER_PKT];
};

// ── New struct-driven backup messages (v2 transport) ──────────────────────────
// Every message starts with {id, length} so the receiver always knows how
// many bytes follow, even for types it doesn't recognise.

struct MsgStartBackup {
    uint8_t  id     = MSG_START_BACKUP;
    uint16_t length = sizeof(MsgStartBackup);
};

struct MsgBackupHeader {
    uint8_t  id     = MSG_BACKUP_HEADER;
    uint16_t length = sizeof(MsgBackupHeader);
    char     filename[24];
    uint32_t file_size;
    uint16_t file_index;  // 1-based
    uint16_t file_total;
};
// 35 bytes

struct MsgBackupBody {
    uint8_t  id     = MSG_BACKUP_BODY;
    uint16_t length = sizeof(MsgBackupBody);
    uint16_t data_len;    // 0..1024 — meaningful bytes in `data`
    uint8_t  data[1024];  // trailing bytes after data_len are undefined
};
// 1029 bytes

struct MsgEndOfData {
    uint8_t  id     = MSG_END_OF_DATA;
    uint16_t length = sizeof(MsgEndOfData);
};

// ── Song push / load (server → client) ────────────────────────────────────
// Server takes the MagiLink mutex, sends HEADER once, then loops sending
// BODY chunks (data_len ≤ 1024) until the total declared in HEADER has
// been sent, then releases.  Receiver tracks bytes received and fires
// the "song ready" transition when bytes_received == total_size.

struct MsgSongPushHeader {
    uint8_t  id     = MSG_SONG_PUSH_HEADER;
    uint16_t length = sizeof(MsgSongPushHeader);
    uint32_t total_size;  // total bytes that will follow across BODY messages
};
// 7 bytes

struct MsgSongPushBody {
    uint8_t  id     = MSG_SONG_PUSH_BODY;
    uint16_t length = sizeof(MsgSongPushBody);
    uint16_t data_len;    // 0..1024 — meaningful bytes in `data`
    uint8_t  data[1024];
};
// 1029 bytes (same shape as MsgBackupBody)

// Server → client: "no song loaded".  Sent in lieu of a SONG_PUSH stream
// when the server has nothing active on connect.
struct MsgNoSong {
    uint8_t  id     = MSG_NO_SONG;
    uint16_t length = sizeof(MsgNoSong);
};

// ── Instruments push (server → client) ─────────────────────────────────────
// Triggered by MsgInstrumentsReq from client.  Server streams the full
// Instrument[MAX_INSTRUMENTS] array as HEADER (total_size) + N×BODY.
// Body shape matches MsgBackupBody/MsgSongPushBody so the server reuses
// sStreamBody (id rewritten per use).

struct MsgInstrumentsReq {
    uint8_t  id     = MSG_INSTRUMENTS_REQ;
    uint16_t length = sizeof(MsgInstrumentsReq);
};

struct MsgInstrumentsPushHeader {
    uint8_t  id     = MSG_INSTRUMENTS_PUSH_HEADER;
    uint16_t length = sizeof(MsgInstrumentsPushHeader);
    uint32_t total_size;
};
// 7 bytes

struct MsgInstrumentsPushBody {
    uint8_t  id     = MSG_INSTRUMENTS_PUSH_BODY;
    uint16_t length = sizeof(MsgInstrumentsPushBody);
    uint16_t data_len;
    uint8_t  data[1024];
};
// 1029 bytes — same shape as MsgBackupBody / MsgSongPushBody

// ── Restore (client → server) ──────────────────────────────────────────────
// Client streams an arbitrary file (song or instruments) to be written
// to SD on the server.  HEADER carries destination filename, the
// isInstruments flag, and total_size.  BODY chunks carry raw file bytes.

struct MsgRestoreHeader {
    uint8_t  id     = MSG_RESTORE_HEADER;
    uint16_t length = sizeof(MsgRestoreHeader);
    char     name[SRV_FNAME_MAX];   // 24 — filename (with extension)
    uint8_t  isInstruments;         // 1 = /instruments.mgt, 0 = /songs/<name>
    uint32_t total_size;            // bytes that follow across BODY messages
};
// 1 + 2 + 24 + 1 + 4 = 32 bytes

struct MsgRestoreBody {
    uint8_t  id     = MSG_RESTORE_BODY;
    uint16_t length = sizeof(MsgRestoreBody);
    uint16_t data_len;
    uint8_t  data[1024];
};
// 1029 bytes (shares wire shape with other body messages)

// ── Song save (client → server) ────────────────────────────────────────────
// Client takes the MagiLink mutex, sends HEADER once, then loops sending
// BODY chunks of raw Song bytes (data_len ≤ 1024) until song_bytes have
// been sent.  Receiver assembles in srvActiveBuf + sizeof(SongFileHeader)
// and stamps SongFileHeader at offset 0 from the header itself.  Empty
// name means "push to active without writing to SD".

struct MsgSongSaveHeader {
    uint8_t  id     = MSG_SAVE_SONG_HEADER;
    uint16_t length = sizeof(MsgSongSaveHeader);
    char     name[SRV_NAME_MAX];     // 24 — destination filename (no .mgt), "" = push only
    uint32_t song_bytes;             // bytes of Song that follow across BODY messages
    SongFileHeader song_file_header; // 8 — magic/version, copied into srvActiveBuf[0..8]
};
// 1 + 2 + 24 + 4 + 8 = 39 bytes

struct MsgSongSaveBody {
    uint8_t  id     = MSG_SAVE_SONG_BODY;
    uint16_t length = sizeof(MsgSongSaveBody);
    uint16_t data_len;
    uint8_t  data[1024];
};
// 1029 bytes (same shape as MsgBackupBody / MsgSongPushBody)

// ── Save active (client → server) ─────────────────────────────────────────
// Tells the server to write its current in-memory song (srvActiveBuf) to
// SD under `name`.  Server already has every edit (via patch / note-set),
// so the wire payload is just the filename.

struct MsgSaveActive {
    uint8_t  id     = MSG_SAVE_ACTIVE;
    uint16_t length = sizeof(MsgSaveActive);
    char     name[SRV_NAME_MAX];     // 24 — destination filename (no .mgt)
    uint8_t  is_autosave;            // 1 = write to /autosave/, 0 = write to /songs/
};
// 1 + 2 + 24 + 1 = 28 bytes

// ── New song (client → server) ────────────────────────────────────────────
// Asks the server to call initSong() on srvActiveBuf so its in-memory
// song matches the client's freshly-wiped copy.  No payload needed.

struct MsgNewSong {
    uint8_t  id     = MSG_NEW_SONG;
    uint16_t length = sizeof(MsgNewSong);
};
// 3 bytes

// Client → server: drop to OFF / No Song (mirrors the server's local "-- OFF --"
// selection).  Server clears srvHasActive, stops the sequencer, and replies with
// MsgNoSong so the client clears too.
struct MsgSetNoSong {
    uint8_t  id     = MSG_SET_NO_SONG;
    uint16_t length = sizeof(MsgSetNoSong);
};
// 3 bytes

// ── Generic file server ─────────────────────────────────────────────────────
// Kinds are an enum (not free-form paths) so the server controls the
// directory whitelist.  Extend by adding a new entry on both sides and a
// path row in the server's lookup table.
#define FILE_NAME_LEN     48    // generic file server filename cap (with extension)
#define FILE_LIST_PER_PKT  7    // mirrors SL_PER_PKT

enum FileKind : uint8_t {
    FK_DRUMTRACKS = 0,   // /drumtracks/*.txt   (drum-patterns.com format)
    FK_SETLISTS   = 1,   // /setlists/*.set     (performer setlists, text)
    // Add future kinds here.  Server's `fileKindToPath()` must mirror.
    FK__COUNT             // sentinel for bounds checks
};

// Client → server: ask for one page of the file list for `kind`.
struct MsgFileListReq {
    uint8_t  id     = MSG_FILE_LIST_REQ;
    uint16_t length = sizeof(MsgFileListReq);
    uint8_t  kind;     // FileKind value
    uint8_t  page;     // 0-based
};

// Server → client: one page of filenames (with extension).  `kind` is
// echoed so the receiver can demux against any in-flight requests.
struct MsgFileListResp {
    uint8_t  id     = MSG_FILE_LIST_RESP;
    uint16_t length = sizeof(MsgFileListResp);
    uint8_t  kind;
    uint8_t  page;
    uint8_t  totalPages;
    uint8_t  count;
    char     names[FILE_LIST_PER_PKT][FILE_NAME_LEN];
};

// Client → server: request the contents of a named file in `kind`.
struct MsgFileLoadReq {
    uint8_t  id     = MSG_FILE_LOAD_REQ;
    uint16_t length = sizeof(MsgFileLoadReq);
    uint8_t  kind;
    char     name[FILE_NAME_LEN];   // with extension
};

// Server → client: header preceding the chunked body stream.  `found=0`
// (file missing) means no BODY chunks will follow.  `kind` echoed.
struct MsgFileLoadHeader {
    uint8_t  id     = MSG_FILE_LOAD_HEADER;
    uint16_t length = sizeof(MsgFileLoadHeader);
    uint8_t  kind;
    uint32_t total_size;
    uint8_t  found;
};

// Server → client: one chunk of raw file bytes.  Stream is closed by the
// existing MsgEndOfData (id 0x83).
struct MsgFileLoadBody {
    uint8_t  id     = MSG_FILE_LOAD_BODY;
    uint16_t length = sizeof(MsgFileLoadBody);
    uint16_t data_len;     // 0..1024
    uint8_t  data[1024];
};
// 1029 bytes

// Client → server: begin a chunked file upload.  `kind` selects the dir via
// the server's whitelist; `name` is the destination filename (with extension).
// BODY chunks follow; the server completes on byte count (no terminator — same
// as the restore stream), then replies MsgFileSaveAck.
struct MsgFileSaveHeader {
    uint8_t  id     = MSG_FILE_SAVE_HEADER;
    uint16_t length = sizeof(MsgFileSaveHeader);
    uint8_t  kind;
    char     name[FILE_NAME_LEN];   // with extension
    uint32_t total_size;            // bytes that follow across BODY messages
};

struct MsgFileSaveBody {
    uint8_t  id     = MSG_FILE_SAVE_BODY;
    uint16_t length = sizeof(MsgFileSaveBody);
    uint16_t data_len;     // 0..1024
    uint8_t  data[1024];
};
// 1029 bytes

// Server → client: result of a file save (kind echoed).  ok=0 on any failure.
struct MsgFileSaveAck {
    uint8_t  id     = MSG_FILE_SAVE_ACK;
    uint16_t length = sizeof(MsgFileSaveAck);
    uint8_t  kind;
    uint8_t  ok;
};

#pragma pack(pop)

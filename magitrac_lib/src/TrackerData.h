#pragma once
#include <Arduino.h>

// ── Constants ────────────────────────────────────────────────────────────────

#define MAX_PATTERNS      50
#define MAX_ROWS          64
#define MAX_COLUMNS        9   // 1 input (col 0) + 8 outputs (cols 1..8)
#define MAX_SONG_NOTES  4000   // sparse pool shared across all patterns
#define MAX_INSTRUMENTS  256
#define INSTRUMENT_NAME_LEN 12  // 11 chars + null

// ── Sparse note node ──────────────────────────────────────────────────────────
// Stored in Song::notePool[MAX_SONG_NOTES]. Each pattern holds a circular linked
// list through this pool (last node's next == noteHead). Free nodes form a
// separate singly-linked chain via noteFreeHead (tail sentinel: 0xFFFF).
#define NOTE_NULL 0xFFFF

struct NoteNode {
    uint8_t  row;
    uint8_t  col;
    uint8_t  note;
    uint8_t  velocity;   // 0–127 = explicit, 0x80+ = use default (100)
    uint8_t  effect;
    uint8_t  param;
    uint16_t next;   // next node index in pool (circular within pattern, or free list chain)
};

// note values: 0 = empty, 1 = C-0, 2 = C#0, ..., 12 = B-0, 13 = C-1, ...
// octave = (note-1)/12,  semitone = (note-1)%12
#define NOTE_EMPTY  0
#define NOTE_OFF    0xFF // silence: sends note-off on the column, no note-on
#define NOTE_ANY    0xFE // col-0: WAIT/SYNC triggers on any performed note
#define NOTE_MAX    96   // B-7
#define EFFECT_SYNC 0x0E // column-0 effect: snap to this row when performer plays matching note
#define EFFECT_WAIT 0x0F // column-0 effect: snap AND halt until performer plays matching note

// Block-end navigation — stored in Pattern::blockEndNav as xxyyyyyy
//   xx = mode:  00=loop, 01=forward, 10=backward, 11=absolute
//   yyyyyy = target (0–63), meaning depends on mode
#define NAV_MODE_MASK   0xC0
#define NAV_TARGET_MASK 0x3F
#define NAV_LOOP        0x00  // 00: loop current pattern
#define NAV_FWD         0x40  // 01: jump forward by y patterns
#define NAV_BACK        0x80  // 10: jump backward by y patterns
#define NAV_ABS         0xC0  // 11: absolute jump to pattern y

// ── Structs ──────────────────────────────────────────────────────────────────

// Velocity: 0x80 (high bit set) = use default (100).  0–127 = explicit.
#define VEL_DEFAULT 0x80

struct Note {
    uint8_t note;        // 0 = empty, 1–96
    uint8_t velocity;    // 0–127 = explicit, 0x80+ = use default (100)
    uint8_t effect;      // 0 = none, 0x00–0xFF (2 hex digits)
    uint8_t param;       // effect parameter byte (0x00–0xFF)
};

// ── Instrument (library preset — no MIDI channel) ────────────────────────────

struct Instrument {
    char    name[INSTRUMENT_NAME_LEN]; // display name (11 chars + null)
    uint8_t bankMSB;      // CC0, 0–127
    uint8_t program;      // 0–127
    uint8_t volume;       // CC7, 0–127
    int8_t  transpose;    // semitones, –24 to +24
    uint8_t _pad[2];      // reserved (sizeof stays 18 for SD compat)
};

// ── Performer sync — note matching table ──────────────────────────────────────
//
// Each pattern defines which pitch classes the performer may play at its input
// note rows, and what happens when each is played.
// Indexed by semitone (0=C, 1=C#, ... 11=B) — octave is ignored for matching.

// What happens to block position when this note is played
enum class BlockSwitch : uint8_t {
    STAY      = 0,  // remain in current block, continue from current row
    SAME_POS  = 1,  // jump to switchTarget block, keep current row position
    TOP       = 2,  // jump to switchTarget block, restart from row 0
};

// What happens to the transposition state when this note is played
enum class TransposeAction : uint8_t {
    KEEP   = 0,  // leave current transposition unchanged
    NOTE   = 1,  // derive transpose from performed note's pitch class (C=0,D=2,E=4…)
    CUSTOM = 2,  // apply transposeValue semitones to all output notes
};

// What happens to the row position when the performer plays a different pitch class
enum class KeyChangeMode : uint8_t {
    SAME_POS = 0,  // keep current row position unchanged
    TOP      = 1,  // scan to end of block observing BLK+/BLK-, jump to row 0 of result
};

struct InputNoteEntry {
    BlockSwitch     switchMode;      // what to do with block position (default: STAY)
    uint8_t         switchTarget;    // target pattern index (used when switchMode != STAY)
    TransposeAction transposeAction; // whether to change transposition (default: KEEP)
    int8_t          transposeValue;  // semitone offset to apply (used when action == SET)
};

// ── Performance pad config ──────────────────────────────────────────────────
#define PERF_PAD_COUNT    8
#define PERF_PAD_NAME_LEN 12   // 11 chars + null

struct PerfPadConfig {
    char    name[PERF_PAD_NAME_LEN];  // custom name; empty string = use block number
    uint8_t mode;                      // 0 = IMMEDIATE, 1 = QUEUED
};

// Column 0 is always the input (MIDI-in) track. Columns 1..N-1 are MIDI output.
#define INPUT_COLUMN 0

// ── Per-column MIDI output settings (one per column per pattern) ─────────────

struct ColumnSettings {
    uint8_t midiChannel;               // 1–16 (0 = muted/unset)
    uint8_t bankMSB;                   // CC0, 0–127
    uint8_t program;                   // 0–127
    uint8_t volume;                    // CC7, 0–127
    int8_t  transpose;                 // semitones, –24 to +24
    uint8_t mute;                      // 0 = unmuted, 1 = muted
    uint8_t _pad[2];                   // reserved, write as 0
    char    name[INSTRUMENT_NAME_LEN]; // display name (11 chars + null)
};
// sizeof(ColumnSettings) = 20

struct Pattern {
    uint16_t       noteHead;       // index into Song::notePool of first note (NOTE_NULL = empty)
    uint8_t        length;         // active row count: 16, 24, 32, 48, or 64
    uint8_t        referenceNote;  // semitone (0–11) that maps to zero transposition
    InputNoteEntry inputNotes[12]; // one entry per pitch class
    char           name[16];       // display name shown on tracker main page
    uint8_t        keyChangeMode;  // KeyChangeMode: 0=SAME_POS, 1=TOP
    uint8_t        blockEndNav;    // xxyyyyyy: 00=loop, 01=fwd y, 10=back y, 11=abs y
    uint8_t        _padOld[2];     // reserved, write as 0
};

struct Song {
    NoteNode   notePool[MAX_SONG_NOTES]; // sparse note storage shared across all patterns
    uint16_t   noteFreeHead;             // head of free list (NOTE_NULL = pool exhausted)
    Pattern    patterns[MAX_PATTERNS];
    ColumnSettings columns[MAX_COLUMNS]; // per-song column MIDI output config (shared by all patterns)
    uint8_t    numPatterns;
    uint8_t    startPattern;       // which block the engine begins on (performer navigates from here)
    uint16_t   bpm;                // starting BPM (updated live by performer sync)
    uint16_t   minBPM;             // derived tempo clamped to this floor (fumble protection)
    uint16_t   maxBPM;             // derived tempo clamped to this ceiling
    uint8_t    speed;              // ticks per row (classic: 6)
    char       name[32];
    uint8_t    midiInChannel;      // 0=ANY, 1–16
    uint8_t    midiInNoteMin;      // lowest accepted MIDI note, 0–127
    uint8_t    midiInNoteMax;      // highest accepted MIDI note, 0–127
    uint8_t    performerMask;      // bits 0-3: MIDI channels 1-4 driven by performer keyboard
    PerfPadConfig perfPads[PERF_PAD_COUNT];  // performance mode pad config
    uint8_t    _songPad[3];        // reserved, write as 0
};

// ── Helper functions ──────────────────────────────────────────────────────────

// Writes 3-char note string + null terminator into buf (needs 4 bytes).
// e.g. "C-4", "C#3", "---"
void noteToString(uint8_t note, char* buf);

// Writes 4-char effect string + null terminator (needs 5 bytes).
// e.g. "0D02", "----"
void effectToString(uint8_t effect, uint8_t param, char* buf);

// Writes 2-char velocity string + null terminator (needs 3 bytes).
// 0x80+ → "--", 0–127 → hex e.g. "5F"
void velToString(uint8_t velocity, char* buf);

// Writes 4-char effect string + null terminator (needs 5 bytes).
// e.g. "0D02", "----"
void effectToString5(uint8_t effect, uint8_t param, char* buf);

// Writes 4-char col-0 metadata string + null terminator (needs 5 bytes).
// Shows "WAIT", "SYNC", or "    ".
void col0MetaToString(const Note& n, char* buf);

// Pack semitone (0=C … 11=B) and octave (0–7) into a note value.
inline uint8_t makeNote(uint8_t semitone, uint8_t octave) {
    return (octave * 12) + semitone + 1;
}

// ── Undo buffer ───────────────────────────────────────────────────────────────

// Stores the state of one cell before the last committed edit.
struct UndoEntry {
    bool    valid;
    uint8_t patternIdx;
    uint8_t row;
    uint8_t column;
    Note    before;   // note value that was there before the edit
};

// ── Compact file note (7 bytes) ──────────────────────────────────────────
// Used for on-disk storage only. Notes are serialized in pattern order,
// then (row,col) order within each pattern.

struct SerializedNote {
    uint8_t pattern;
    uint8_t row;
    uint8_t col;
    uint8_t note;
    uint8_t velocity;    // 0x80 = default, 0–127 = explicit
    uint8_t effect;
    uint8_t param;
};

// .mgt file layout (v17 compact):
//   [ SongFileHeader              ]  (8 bytes)
//   [ Pattern x MAX_PATTERNS      ]  (72 x 50 = 3600 bytes; noteHead ignored on load)
//   [ ColumnSettings x MAX_COLUMNS ] (20 x 9 = 180 bytes; per-song column config)
//   [ Song tail                   ]  (numPatterns ... _songPad)
//   [ uint16_t noteCount          ]  (2 bytes)
//   [ SerializedNote x noteCount  ]  (7 bytes each)
//
// Small songs = small files.  MAX_SONG_NOTES can change without a format bump.
// Wire transfer (ESP-NOW) still uses the raw Song struct.
// Bump FILE_VERSION whenever Pattern or Song tail layout changes,
// or when MAX_COLUMNS changes (it sizes Song::columns).

#define SONG_FILE_MAGIC   0x4D414754UL   // "MAGT" as uint32
#define SONG_FILE_VERSION 17

struct SongFileHeader {
    uint32_t magic;    // must equal SONG_FILE_MAGIC
    uint8_t  version;  // must equal SONG_FILE_VERSION
    uint8_t  _pad[3];  // reserved, write as 0
};

// ── Default song factory ──────────────────────────────────────────────────────

// Fills *song with safe defaults and a simple demo pattern.
void initSong(Song* song);

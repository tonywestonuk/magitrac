#pragma once
#include "TrackerData.h"
#include <string.h>
#include <SD.h>

// ── Song file migration ──────────────────────────────────────────────────────
//
// v11 (raw dump): [Header(8)] [notePool(2048×8)+freeHead(2)] [32×PatternV15(232)] [tail(48)]
// v13 (compact):  [Header(8)] [50×PatternV15(232)] [tail(48)] [noteCount(2)] [noteCount×6-byte notes]
// v14 (compact):  same as v13 but SerializedNote gains velocity field (7 bytes each)
// v15 (compact):  same as v14 but Song tail grows (perfPads[8] added)
// v16 (compact):  ColumnSettings moved from Pattern to Song (per-song, not per-block)
//                 [Header(8)] [50×Pattern(72)] [Song::columns(160)] [tail] [noteCount] [notes]
// v17 (compact):  MAX_COLUMNS grew from 8 → 9 (one input + 8 outputs)
//                 [Header(8)] [50×Pattern(72)] [Song::columns(180)] [tail] [noteCount] [notes]
// v18 (compact):  MAX_COLUMNS grew from 9 → 21 (one input + 20 outputs)
//                 [Header(8)] [50×Pattern(72)] [Song::columns(420)] [tail] [noteCount] [notes]
// v19 (compact):  Song tail gains transposeChMask (uint16_t) between performerMask
//                 and perfPads; _songPad shrinks 3→1.  Total tail size unchanged,
//                 but field order shifts — needs explicit byte-level reads on v15-v18.
//
// In v11–v15, ColumnSettings sat inside Pattern (228+pad bytes). In v16 it lives
// in Song. Migration lifts pattern[0]'s columns into Song::columns and discards
// per-block columns from other patterns.

// Historical column counts (do NOT track current MAX_COLUMNS).
static const int V16_MAX_COLUMNS = 8;
static const int V17_MAX_COLUMNS = 9;

static const int V11_MAX_PATTERNS   = 32;
static const int V11_MAX_SONG_NOTES = 2048;

// Old v11 col-0 instrument values (for migration only)
static const uint8_t V11_COL0_NAV_NEXT = 1;
static const uint8_t V11_COL0_NAV_PREV = 2;

// ── Pre-v16 Pattern layout (with embedded ColumnSettings) ───────────────────
// Used by all four migration paths to read on-disk patterns.
struct PatternV15 {
    uint16_t       noteHead;
    uint8_t        length;
    uint8_t        referenceNote;
    InputNoteEntry inputNotes[12];
    char           name[16];
    uint8_t        keyChangeMode;
    uint8_t        blockEndNav;
    uint8_t        _padOld[2];
    ColumnSettings columns[V16_MAX_COLUMNS];   // historical: always 8 in v15
};

// Copy fields from a PatternV15 (file layout) into a new Pattern (memory layout).
static inline void copyPatternV15ToNew(const PatternV15& src, Pattern& dst) {
    dst.noteHead      = src.noteHead;
    dst.length        = src.length;
    dst.referenceNote = src.referenceNote;
    memcpy(dst.inputNotes, src.inputNotes, sizeof(dst.inputNotes));
    memcpy(dst.name, src.name, sizeof(dst.name));
    dst.keyChangeMode = src.keyChangeMode;
    dst.blockEndNav   = src.blockEndNav;
    dst._padOld[0]    = src._padOld[0];
    dst._padOld[1]    = src._padOld[1];
}

// Read N old-layout patterns from f, convert into out->patterns[0..N-1], and
// lift pattern[0]'s columns into out->columns (per-song in v16+).
static inline bool readPatternsV15(File& f, Song* out, int count) {
    PatternV15 tmp;
    for (int i = 0; i < count; i++) {
        if (f.read((uint8_t*)&tmp, sizeof(tmp)) != (int)sizeof(tmp)) return false;
        copyPatternV15ToNew(tmp, out->patterns[i]);
        if (i == 0) {
            // Lift pattern[0]'s 8 columns to per-song columns (cols 8+ stay zeroed)
            for (int c = 0; c < V16_MAX_COLUMNS && c < MAX_COLUMNS; c++) {
                out->columns[c] = tmp.columns[c];
            }
        }
    }
    return true;
}

// Read the Song tail from a v15-v18 file (no transposeChMask).
//   File layout:    numPatterns..performerMask | perfPads | _songPad[3]
//   Struct layout:  numPatterns..performerMask | <1 pad> | transposeChMask[2] | perfPads | _songPad[1]
// IMPORTANT: do NOT use offsetof(Song, transposeChMask) as the head size —
// the compiler inserts a 1-byte alignment padding before that uint16_t, and
// reading that many bytes from the file would consume the first byte of
// perfPads, shifting every subsequent read.  We compute the head size from
// performerMask explicitly so it stays correct regardless of alignment.
// transposeChMask is set to its default (all channels follow transpose except
// ch 10, the standard drum channel).
static inline bool readSongTailV15to18(File& f, Song* out) {
    static const size_t HEAD_SIZE =
        offsetof(Song, performerMask) + sizeof(out->performerMask) - offsetof(Song, numPatterns);
    if (f.read((uint8_t*)&out->numPatterns, HEAD_SIZE) != (int)HEAD_SIZE) return false;
    if (f.read((uint8_t*)out->perfPads, sizeof(out->perfPads)) != (int)sizeof(out->perfPads)) return false;
    uint8_t discard[3];
    if (f.read(discard, sizeof(discard)) != (int)sizeof(discard)) return false;
    out->transposeChMask = 0xFDFF;  // all channels except ch 10 (drums)
    return true;
}

// ── v11 migration (stream from file) ─────────────────────────────────────────
// File must be seeked past the SongFileHeader.

static inline bool songMigrateV11FromFile(File& f, Song* out) {
    memset(out, 0, sizeof(Song));

    // 1. Read old notePool (2048 nodes) + noteFreeHead
    static const int OLD_POOL_BYTES = (int)(V11_MAX_SONG_NOTES * sizeof(NoteNode) + sizeof(uint16_t));
    if (f.read((uint8_t*)out->notePool, OLD_POOL_BYTES) != OLD_POOL_BYTES) return false;

    // 2. Extend free list to cover new nodes (2048..3999)
    uint16_t oldFreeHead = out->noteFreeHead;
    for (int i = V11_MAX_SONG_NOTES; i < MAX_SONG_NOTES - 1; i++)
        out->notePool[i].next = (uint16_t)(i + 1);
    out->notePool[MAX_SONG_NOTES - 1].next = oldFreeHead;
    out->noteFreeHead = (uint16_t)V11_MAX_SONG_NOTES;

    // 3. Read 32 patterns (v11 PatternV15 layout) into first 32 slots
    if (!readPatternsV15(f, out, V11_MAX_PATTERNS)) return false;

    // 4. Initialise empty patterns 32..49
    for (int i = V11_MAX_PATTERNS; i < MAX_PATTERNS; i++)
        out->patterns[i].noteHead = NOTE_NULL;

    // 5. Read song tail
    static const int TAIL_SIZE = (int)(sizeof(Song) - offsetof(Song, numPatterns));
    if (f.read((uint8_t*)&out->numPatterns, TAIL_SIZE) != TAIL_SIZE) return false;

    // 6. Convert col-0 instrument BLK+/BLK- → Pattern::blockEndNav
    for (int p = 0; p < out->numPatterns; p++) {
        uint16_t head = out->patterns[p].noteHead;
        if (head == NOTE_NULL) continue;
        uint16_t idx = head;
        do {
            NoteNode& n = out->notePool[idx];
            if (n.col == 0 && n.velocity != 0) {
                if (n.velocity == V11_COL0_NAV_NEXT)
                    out->patterns[p].blockEndNav = NAV_FWD | 1;
                else if (n.velocity == V11_COL0_NAV_PREV)
                    out->patterns[p].blockEndNav = NAV_BACK | 1;
            }
            n.velocity = VEL_DEFAULT;  // v11 had no velocity — use default
            idx = n.next;
        } while (idx != head);
    }

    return true;
}

// ── v13 → v16 migration (compact notes without velocity) ────────────────────
struct SerializedNoteV13 {
    uint8_t pattern;
    uint8_t row;
    uint8_t col;
    uint8_t note;
    uint8_t effect;
    uint8_t param;
};

static inline bool songMigrateV13FromFile(File& f, Song* out) {
    memset(out, 0, sizeof(Song));

    // 1. Patterns (PatternV15 layout)
    if (!readPatternsV15(f, out, MAX_PATTERNS)) return false;

    // 2. Song tail
    static const size_t TAIL_SIZE = sizeof(Song) - offsetof(Song, numPatterns);
    if (f.read((uint8_t*)&out->numPatterns, TAIL_SIZE) != (int)TAIL_SIZE) return false;

    // 3. Note count
    uint16_t noteCount = 0;
    if (f.read((uint8_t*)&noteCount, sizeof(noteCount)) != (int)sizeof(noteCount)) return false;
    if (noteCount > MAX_SONG_NOTES) return false;

    // 4. Reset noteHeads
    for (int p = 0; p < MAX_PATTERNS; p++)
        out->patterns[p].noteHead = NOTE_NULL;

    // 5. Read v13 notes (6 bytes each) and set velocity to default
    uint16_t tail[MAX_PATTERNS];
    for (int p = 0; p < MAX_PATTERNS; p++) tail[p] = NOTE_NULL;

    for (uint16_t i = 0; i < noteCount; i++) {
        SerializedNoteV13 sn;
        if (f.read((uint8_t*)&sn, sizeof(sn)) != (int)sizeof(sn)) return false;
        if (sn.pattern >= MAX_PATTERNS) return false;

        NoteNode& node = out->notePool[i];
        node.row       = sn.row;
        node.col       = sn.col;
        node.note      = sn.note;
        node.velocity  = VEL_DEFAULT;
        node.effect    = sn.effect;
        node.param     = sn.param;
        node.next      = i;

        uint8_t p = sn.pattern;
        if (out->patterns[p].noteHead == NOTE_NULL) {
            out->patterns[p].noteHead = i;
            tail[p] = i;
        } else {
            out->notePool[tail[p]].next = i;
            tail[p] = i;
        }
    }

    // 6. Close circular lists
    for (int p = 0; p < MAX_PATTERNS; p++) {
        if (out->patterns[p].noteHead != NOTE_NULL)
            out->notePool[tail[p]].next = out->patterns[p].noteHead;
    }

    // 7. Build free list
    if (noteCount < MAX_SONG_NOTES) {
        out->noteFreeHead = noteCount;
        for (uint16_t i = noteCount; i < MAX_SONG_NOTES - 1; i++)
            out->notePool[i].next = i + 1;
        out->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    } else {
        out->noteFreeHead = NOTE_NULL;
    }

    return true;
}

// ── v14 → v16 migration ─────────────────────────────────────────────────────
// v15 added PerfPadConfig perfPads[8] to the Song tail.
// In v14 the tail was 48 bytes (numPatterns..performerMask + _songPad[3]).

static inline bool songMigrateV14FromFile(File& f, Song* out) {
    memset(out, 0, sizeof(Song));

    // 1. Patterns (PatternV15 layout)
    if (!readPatternsV15(f, out, MAX_PATTERNS)) return false;

    // 2. Old song tail — numPatterns through performerMask, then 3 bytes padding.
    // Compute the head size from performerMask explicitly — using
    // offsetof(transposeChMask) would include alignment padding that doesn't
    // exist in the v14 file and would shift every subsequent read.
    static const size_t V14_TAIL_USEFUL =
        offsetof(Song, performerMask) + sizeof(out->performerMask) - offsetof(Song, numPatterns);
    static const size_t V14_TAIL_PAD    = 3;
    if (f.read((uint8_t*)&out->numPatterns, V14_TAIL_USEFUL) != (int)V14_TAIL_USEFUL) return false;
    uint8_t discard[V14_TAIL_PAD];
    if (f.read(discard, V14_TAIL_PAD) != (int)V14_TAIL_PAD) return false;
    // perfPads stays zeroed = defaults; transposeChMask gets default below.
    out->transposeChMask = 0xFDFF;

    // 3. Note count
    uint16_t noteCount = 0;
    if (f.read((uint8_t*)&noteCount, sizeof(noteCount)) != (int)sizeof(noteCount)) return false;
    if (noteCount > MAX_SONG_NOTES) return false;

    // 4. Reset noteHeads
    for (int p = 0; p < MAX_PATTERNS; p++)
        out->patterns[p].noteHead = NOTE_NULL;

    // 5. Read notes (7-byte format)
    uint16_t tail[MAX_PATTERNS];
    for (int p = 0; p < MAX_PATTERNS; p++) tail[p] = NOTE_NULL;

    for (uint16_t i = 0; i < noteCount; i++) {
        SerializedNote sn;
        if (f.read((uint8_t*)&sn, sizeof(sn)) != (int)sizeof(sn)) return false;
        if (sn.pattern >= MAX_PATTERNS) return false;

        NoteNode& node = out->notePool[i];
        node.row       = sn.row;
        node.col       = sn.col;
        node.note      = sn.note;
        node.velocity  = sn.velocity;
        node.effect    = sn.effect;
        node.param     = sn.param;
        node.next      = i;

        uint8_t p = sn.pattern;
        if (out->patterns[p].noteHead == NOTE_NULL) {
            out->patterns[p].noteHead = i;
            tail[p] = i;
        } else {
            out->notePool[tail[p]].next = i;
            tail[p] = i;
        }
    }

    // 6. Close circular lists + build free list
    for (int p = 0; p < MAX_PATTERNS; p++) {
        if (out->patterns[p].noteHead != NOTE_NULL)
            out->notePool[tail[p]].next = out->patterns[p].noteHead;
    }
    if (noteCount < MAX_SONG_NOTES) {
        out->noteFreeHead = noteCount;
        for (uint16_t i = noteCount; i < MAX_SONG_NOTES - 1; i++)
            out->notePool[i].next = i + 1;
        out->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    } else {
        out->noteFreeHead = NOTE_NULL;
    }

    return true;
}

// ── v15 → v16 migration ─────────────────────────────────────────────────────
// Same as v14/15 reader except patterns are v15 layout (with embedded columns)
// and tail is the v15 tail (with perfPads). Lift pattern[0]'s columns into
// out->columns; per-block column settings on other patterns are discarded.

static inline bool songMigrateV15FromFile(File& f, Song* out) {
    memset(out, 0, sizeof(Song));

    // 1. Patterns (PatternV15 layout) — also lifts pattern[0]'s columns
    if (!readPatternsV15(f, out, MAX_PATTERNS)) return false;

    // 2. Song tail — v15-v18 layout (no transposeChMask; transposeChMask defaulted)
    if (!readSongTailV15to18(f, out)) return false;

    // 3. Note count
    uint16_t noteCount = 0;
    if (f.read((uint8_t*)&noteCount, sizeof(noteCount)) != (int)sizeof(noteCount)) return false;
    if (noteCount > MAX_SONG_NOTES) return false;

    // 4. Reset noteHeads (disk values are stale)
    for (int p = 0; p < MAX_PATTERNS; p++)
        out->patterns[p].noteHead = NOTE_NULL;

    // 5. Read notes (7-byte format)
    uint16_t tail[MAX_PATTERNS];
    for (int p = 0; p < MAX_PATTERNS; p++) tail[p] = NOTE_NULL;

    for (uint16_t i = 0; i < noteCount; i++) {
        SerializedNote sn;
        if (f.read((uint8_t*)&sn, sizeof(sn)) != (int)sizeof(sn)) return false;
        if (sn.pattern >= MAX_PATTERNS) return false;

        NoteNode& node = out->notePool[i];
        node.row       = sn.row;
        node.col       = sn.col;
        node.note      = sn.note;
        node.velocity  = sn.velocity;
        node.effect    = sn.effect;
        node.param     = sn.param;
        node.next      = i;

        uint8_t p = sn.pattern;
        if (out->patterns[p].noteHead == NOTE_NULL) {
            out->patterns[p].noteHead = i;
            tail[p] = i;
        } else {
            out->notePool[tail[p]].next = i;
            tail[p] = i;
        }
    }

    // 6. Close circular lists + build free list
    for (int p = 0; p < MAX_PATTERNS; p++) {
        if (out->patterns[p].noteHead != NOTE_NULL)
            out->notePool[tail[p]].next = out->patterns[p].noteHead;
    }
    if (noteCount < MAX_SONG_NOTES) {
        out->noteFreeHead = noteCount;
        for (uint16_t i = noteCount; i < MAX_SONG_NOTES - 1; i++)
            out->notePool[i].next = i + 1;
        out->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    } else {
        out->noteFreeHead = NOTE_NULL;
    }

    return true;
}

// ── v16 migration (stream from file) ────────────────────────────────────────
// File must be seeked past the SongFileHeader.
// v16 differs from current only in Song::columns size (8 cols vs current MAX).
static inline bool songMigrateV16FromFile(File& f, Song* out) {
    memset(out, 0, sizeof(Song));

    // 1. Patterns (current Pattern layout — unchanged since v16)
    if (f.read((uint8_t*)out->patterns, sizeof(out->patterns)) != (int)sizeof(out->patterns)) return false;

    // 2. Per-song column settings — historical 8 entries; remaining cols zeroed
    static const size_t V16_COLS_BYTES = (size_t)V16_MAX_COLUMNS * sizeof(ColumnSettings);
    ColumnSettings v16cols[V16_MAX_COLUMNS];
    if (f.read((uint8_t*)v16cols, V16_COLS_BYTES) != (int)V16_COLS_BYTES) return false;
    for (int c = 0; c < V16_MAX_COLUMNS && c < MAX_COLUMNS; c++) {
        out->columns[c] = v16cols[c];
    }

    // 3. Song tail — v15-v18 layout (no transposeChMask; transposeChMask defaulted)
    if (!readSongTailV15to18(f, out)) return false;

    // 4. Note count
    uint16_t noteCount = 0;
    if (f.read((uint8_t*)&noteCount, sizeof(noteCount)) != (int)sizeof(noteCount)) return false;
    if (noteCount > MAX_SONG_NOTES) return false;

    // 5. Reset noteHeads (disk values are stale)
    for (int p = 0; p < MAX_PATTERNS; p++)
        out->patterns[p].noteHead = NOTE_NULL;

    // 6. Read notes
    uint16_t tail[MAX_PATTERNS];
    for (int p = 0; p < MAX_PATTERNS; p++) tail[p] = NOTE_NULL;
    for (uint16_t i = 0; i < noteCount; i++) {
        SerializedNote sn;
        if (f.read((uint8_t*)&sn, sizeof(sn)) != (int)sizeof(sn)) return false;
        if (sn.pattern >= MAX_PATTERNS) return false;

        NoteNode& node = out->notePool[i];
        node.row = sn.row; node.col = sn.col; node.note = sn.note;
        node.velocity = sn.velocity; node.effect = sn.effect; node.param = sn.param;
        node.next = i;

        uint8_t p = sn.pattern;
        if (out->patterns[p].noteHead == NOTE_NULL) { out->patterns[p].noteHead = i; tail[p] = i; }
        else { out->notePool[tail[p]].next = i; tail[p] = i; }
    }

    // 7. Close circular lists + build free list
    for (int p = 0; p < MAX_PATTERNS; p++) {
        if (out->patterns[p].noteHead != NOTE_NULL)
            out->notePool[tail[p]].next = out->patterns[p].noteHead;
    }
    if (noteCount < MAX_SONG_NOTES) {
        out->noteFreeHead = noteCount;
        for (uint16_t i = noteCount; i < MAX_SONG_NOTES - 1; i++)
            out->notePool[i].next = i + 1;
        out->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    } else {
        out->noteFreeHead = NOTE_NULL;
    }

    return true;
}

// ── v17 migration (stream from file) ────────────────────────────────────────
// File must be seeked past the SongFileHeader.
// v17 differs from current only in Song::columns size (9 cols vs current MAX).
static inline bool songMigrateV17FromFile(File& f, Song* out) {
    memset(out, 0, sizeof(Song));

    // 1. Patterns (current Pattern layout — unchanged since v16)
    if (f.read((uint8_t*)out->patterns, sizeof(out->patterns)) != (int)sizeof(out->patterns)) return false;

    // 2. Per-song column settings — historical 9 entries; remaining cols zeroed
    static const size_t V17_COLS_BYTES = (size_t)V17_MAX_COLUMNS * sizeof(ColumnSettings);
    ColumnSettings v17cols[V17_MAX_COLUMNS];
    if (f.read((uint8_t*)v17cols, V17_COLS_BYTES) != (int)V17_COLS_BYTES) return false;
    for (int c = 0; c < V17_MAX_COLUMNS && c < MAX_COLUMNS; c++) {
        out->columns[c] = v17cols[c];
    }

    // 3. Song tail — v15-v18 layout (no transposeChMask; transposeChMask defaulted)
    if (!readSongTailV15to18(f, out)) return false;

    // 4. Note count
    uint16_t noteCount = 0;
    if (f.read((uint8_t*)&noteCount, sizeof(noteCount)) != (int)sizeof(noteCount)) return false;
    if (noteCount > MAX_SONG_NOTES) return false;

    // 5. Reset noteHeads (disk values are stale)
    for (int p = 0; p < MAX_PATTERNS; p++)
        out->patterns[p].noteHead = NOTE_NULL;

    // 6. Read notes
    uint16_t tail[MAX_PATTERNS];
    for (int p = 0; p < MAX_PATTERNS; p++) tail[p] = NOTE_NULL;
    for (uint16_t i = 0; i < noteCount; i++) {
        SerializedNote sn;
        if (f.read((uint8_t*)&sn, sizeof(sn)) != (int)sizeof(sn)) return false;
        if (sn.pattern >= MAX_PATTERNS) return false;

        NoteNode& node = out->notePool[i];
        node.row = sn.row; node.col = sn.col; node.note = sn.note;
        node.velocity = sn.velocity; node.effect = sn.effect; node.param = sn.param;
        node.next = i;

        uint8_t p = sn.pattern;
        if (out->patterns[p].noteHead == NOTE_NULL) { out->patterns[p].noteHead = i; tail[p] = i; }
        else { out->notePool[tail[p]].next = i; tail[p] = i; }
    }

    // 7. Close circular lists + build free list
    for (int p = 0; p < MAX_PATTERNS; p++) {
        if (out->patterns[p].noteHead != NOTE_NULL)
            out->notePool[tail[p]].next = out->patterns[p].noteHead;
    }
    if (noteCount < MAX_SONG_NOTES) {
        out->noteFreeHead = noteCount;
        for (uint16_t i = noteCount; i < MAX_SONG_NOTES - 1; i++)
            out->notePool[i].next = i + 1;
        out->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    } else {
        out->noteFreeHead = NOTE_NULL;
    }

    return true;
}

// ── v18 migration (stream from file) ────────────────────────────────────────
// v18 differs from current only in the Song tail layout (no transposeChMask).
// Patterns and columns match the current MAX_COLUMNS = 21.
static inline bool songMigrateV18FromFile(File& f, Song* out) {
    memset(out, 0, sizeof(Song));

    // 1. Patterns (current Pattern layout — unchanged since v16)
    if (f.read((uint8_t*)out->patterns, sizeof(out->patterns)) != (int)sizeof(out->patterns)) return false;

    // 2. Per-song column settings — current size
    if (f.read((uint8_t*)out->columns, sizeof(out->columns)) != (int)sizeof(out->columns)) return false;

    // 3. Song tail — v15-v18 layout
    if (!readSongTailV15to18(f, out)) return false;

    // 4. Note count
    uint16_t noteCount = 0;
    if (f.read((uint8_t*)&noteCount, sizeof(noteCount)) != (int)sizeof(noteCount)) return false;
    if (noteCount > MAX_SONG_NOTES) return false;

    for (int p = 0; p < MAX_PATTERNS; p++)
        out->patterns[p].noteHead = NOTE_NULL;

    uint16_t tail[MAX_PATTERNS];
    for (int p = 0; p < MAX_PATTERNS; p++) tail[p] = NOTE_NULL;
    for (uint16_t i = 0; i < noteCount; i++) {
        SerializedNote sn;
        if (f.read((uint8_t*)&sn, sizeof(sn)) != (int)sizeof(sn)) return false;
        if (sn.pattern >= MAX_PATTERNS) return false;

        NoteNode& node = out->notePool[i];
        node.row = sn.row; node.col = sn.col; node.note = sn.note;
        node.velocity = sn.velocity; node.effect = sn.effect; node.param = sn.param;
        node.next = i;

        uint8_t p = sn.pattern;
        if (out->patterns[p].noteHead == NOTE_NULL) { out->patterns[p].noteHead = i; tail[p] = i; }
        else { out->notePool[tail[p]].next = i; tail[p] = i; }
    }

    for (int p = 0; p < MAX_PATTERNS; p++) {
        if (out->patterns[p].noteHead != NOTE_NULL)
            out->notePool[tail[p]].next = out->patterns[p].noteHead;
    }
    if (noteCount < MAX_SONG_NOTES) {
        out->noteFreeHead = noteCount;
        for (uint16_t i = noteCount; i < MAX_SONG_NOTES - 1; i++)
            out->notePool[i].next = i + 1;
        out->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    } else {
        out->noteFreeHead = NOTE_NULL;
    }

    return true;
}

// ── Compact format: write (v19) ─────────────────────────────────────────────
// Layout: header + patterns + Song::columns + tail + noteCount + notes.

static inline bool songWriteCompact(File& f, const Song* song) {
    // 1. Header
    SongFileHeader hdr;
    hdr.magic   = SONG_FILE_MAGIC;
    hdr.version = SONG_FILE_VERSION;
    hdr._pad[0] = hdr._pad[1] = hdr._pad[2] = 0;
    if (f.write((const uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) return false;

    // 2. Patterns (noteHead values written but rebuilt on load)
    if (f.write((const uint8_t*)song->patterns, sizeof(song->patterns)) != sizeof(song->patterns)) return false;

    // 3. Per-song column settings
    if (f.write((const uint8_t*)song->columns, sizeof(song->columns)) != sizeof(song->columns)) return false;

    // 4. Song tail (numPatterns through _songPad)
    static const size_t TAIL_SIZE = sizeof(Song) - offsetof(Song, numPatterns);
    if (f.write((const uint8_t*)&song->numPatterns, TAIL_SIZE) != TAIL_SIZE) return false;

    // 5. Count notes across all patterns
    uint16_t noteCount = 0;
    for (int p = 0; p < MAX_PATTERNS; p++) {
        uint16_t head = song->patterns[p].noteHead;
        if (head == NOTE_NULL) continue;
        uint16_t idx = head;
        do {
            noteCount++;
            idx = song->notePool[idx].next;
        } while (idx != head);
    }
    if (f.write((const uint8_t*)&noteCount, sizeof(noteCount)) != sizeof(noteCount)) return false;

    // 6. Serialize notes in pattern order, (row,col) order within each
    for (int p = 0; p < MAX_PATTERNS; p++) {
        uint16_t head = song->patterns[p].noteHead;
        if (head == NOTE_NULL) continue;
        uint16_t idx = head;
        do {
            const NoteNode& n = song->notePool[idx];
            SerializedNote sn;
            sn.pattern  = (uint8_t)p;
            sn.row      = n.row;
            sn.col      = n.col;
            sn.note     = n.note;
            sn.velocity = n.velocity;
            sn.effect   = n.effect;
            sn.param    = n.param;
            if (f.write((const uint8_t*)&sn, sizeof(sn)) != sizeof(sn)) return false;
            idx = n.next;
        } while (idx != head);
    }

    return true;
}

// ── CRC32 over the compact serialization (save read-back verify) ────────────
// magiCrc32 is a streaming, composable CRC-32/IEEE: seed with 0, feed chunks in
// any order, and the running value equals the CRC of everything fed so far
// (the leading/trailing inversions make magiCrc32(magiCrc32(0,a),b) == CRC of
// a‖b).  songCrc32 walks the song in the EXACT byte order songWriteCompact()
// emits, so its value equals the CRC of the bytes that function writes to disk.
//
// KEEP THE TWO IN LOCKSTEP — any change to songWriteCompact's on-disk layout
// must be mirrored here, or every server save-verify will (loudly) fail.
static inline uint32_t magiCrc32(uint32_t crc, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    crc = ~crc;
    while (len--) {
        crc ^= *d++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline uint32_t songCrc32(const Song* song) {
    uint32_t c = 0;

    SongFileHeader hdr;
    hdr.magic   = SONG_FILE_MAGIC;
    hdr.version = SONG_FILE_VERSION;
    hdr._pad[0] = hdr._pad[1] = hdr._pad[2] = 0;
    c = magiCrc32(c, &hdr, sizeof(hdr));

    c = magiCrc32(c, song->patterns, sizeof(song->patterns));
    c = magiCrc32(c, song->columns,  sizeof(song->columns));

    static const size_t TAIL_SIZE = sizeof(Song) - offsetof(Song, numPatterns);
    c = magiCrc32(c, &song->numPatterns, TAIL_SIZE);

    uint16_t noteCount = 0;
    for (int p = 0; p < MAX_PATTERNS; p++) {
        uint16_t head = song->patterns[p].noteHead;
        if (head == NOTE_NULL) continue;
        uint16_t idx = head;
        do { noteCount++; idx = song->notePool[idx].next; } while (idx != head);
    }
    c = magiCrc32(c, &noteCount, sizeof(noteCount));

    for (int p = 0; p < MAX_PATTERNS; p++) {
        uint16_t head = song->patterns[p].noteHead;
        if (head == NOTE_NULL) continue;
        uint16_t idx = head;
        do {
            const NoteNode& n = song->notePool[idx];
            SerializedNote sn;
            sn.pattern  = (uint8_t)p;
            sn.row      = n.row;
            sn.col      = n.col;
            sn.note     = n.note;
            sn.velocity = n.velocity;
            sn.effect   = n.effect;
            sn.param    = n.param;
            c = magiCrc32(c, &sn, sizeof(sn));
            idx = n.next;
        } while (idx != head);
    }
    return c;
}

// ── Compact format: read (v16) ──────────────────────────────────────────────
// File must be seeked past the SongFileHeader.

static inline bool songReadCompact(File& f, Song* out) {
    memset(out, 0, sizeof(Song));

    // 1. Patterns
    if (f.read((uint8_t*)out->patterns, sizeof(out->patterns)) != (int)sizeof(out->patterns)) return false;

    // 2. Per-song column settings
    if (f.read((uint8_t*)out->columns, sizeof(out->columns)) != (int)sizeof(out->columns)) return false;

    // 3. Song tail
    static const size_t TAIL_SIZE = sizeof(Song) - offsetof(Song, numPatterns);
    if (f.read((uint8_t*)&out->numPatterns, TAIL_SIZE) != (int)TAIL_SIZE) return false;

    // 4. Note count
    uint16_t noteCount = 0;
    if (f.read((uint8_t*)&noteCount, sizeof(noteCount)) != (int)sizeof(noteCount)) return false;
    if (noteCount > MAX_SONG_NOTES) return false;

    // 5. Reset all pattern noteHeads (disk values are stale)
    for (int p = 0; p < MAX_PATTERNS; p++)
        out->patterns[p].noteHead = NOTE_NULL;

    // 6. Read notes and pack sequentially into pool[0..noteCount-1]
    uint16_t tail[MAX_PATTERNS];
    for (int p = 0; p < MAX_PATTERNS; p++) tail[p] = NOTE_NULL;

    for (uint16_t i = 0; i < noteCount; i++) {
        SerializedNote sn;
        if (f.read((uint8_t*)&sn, sizeof(sn)) != (int)sizeof(sn)) return false;
        if (sn.pattern >= MAX_PATTERNS) return false;

        NoteNode& node = out->notePool[i];
        node.row       = sn.row;
        node.col       = sn.col;
        node.note      = sn.note;
        node.velocity  = sn.velocity;
        node.effect    = sn.effect;
        node.param     = sn.param;
        node.next      = i; // temporary self-link

        uint8_t p = sn.pattern;
        if (out->patterns[p].noteHead == NOTE_NULL) {
            out->patterns[p].noteHead = i;
            tail[p] = i;
        } else {
            out->notePool[tail[p]].next = i;
            tail[p] = i;
        }
    }

    // 7. Close circular lists (tail -> head)
    for (int p = 0; p < MAX_PATTERNS; p++) {
        if (out->patterns[p].noteHead != NOTE_NULL) {
            out->notePool[tail[p]].next = out->patterns[p].noteHead;
        }
    }

    // 8. Build free list from noteCount..MAX_SONG_NOTES-1
    if (noteCount < MAX_SONG_NOTES) {
        out->noteFreeHead = noteCount;
        for (uint16_t i = noteCount; i < MAX_SONG_NOTES - 1; i++)
            out->notePool[i].next = i + 1;
        out->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    } else {
        out->noteFreeHead = NOTE_NULL;
    }

    return true;
}

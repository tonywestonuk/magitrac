#pragma once
#include "TrackerData.h"
#include <string.h>
#include <SD.h>

// ── Song file read / write ───────────────────────────────────────────────────
//
// One format only: the compact layout tagged SONG_FILE_VERSION (see the layout
// table in TrackerData.h).
//   [Header(8)] [Pattern × MAX_PATTERNS] [Song::columns] [tail] [noteCount(2)]
//   [noteCount × SerializedNote(7)]
// Written by songWriteCompact, read by songReadCompact, checksummed by
// songCrc32.
//
// The readers for older layouts (v11 … v19) were removed once every song on
// the SD card had been re-saved in the current version.  A file carrying any
// other version byte is now refused by songReadAnyVersion() — the one dispatch
// point at the bottom of this file, which both sketches call.  (The file name
// is historical: there is no migration left in it.)  If the layout changes
// again, bump SONG_FILE_VERSION and ask the owner whether a one-off conversion
// of the existing songs is wanted — do not add a standing migration reader
// unasked.  Old files are converted OFFLINE with magitrac/tools/mgt_upgrade.py (a
// Python port of the removed readers, v14–v19 → v20); extend that tool for the
// new version instead.

// ── Compact format: write (v20) ─────────────────────────────────────────────
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

    // 4. Song tail (numPatterns through flags, plus the trailing alignment byte)
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

// ── Compact format: read (current version) ──────────────────────────────────────────────
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

// ── Version dispatch ────────────────────────────────────────────────────────
// Read the body of a song file whose SongFileHeader (already consumed by the
// caller) carried `version`.  Only SONG_FILE_VERSION is accepted.  Returns
// true if the song loaded into *out.  *known (may be null) tells a false
// return apart: true = the version is current but the read gave up (short
// read / failed sanity check), false = not the current version, no reader
// exists for it.
static inline bool songReadAnyVersion(File& f, uint8_t version, Song* out, bool* known) {
    if (version != SONG_FILE_VERSION) {
        if (known) *known = false;
        return false;
    }
    if (known) *known = true;
    return songReadCompact(f, out);
}

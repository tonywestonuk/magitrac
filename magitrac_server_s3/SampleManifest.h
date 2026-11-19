#pragma once
#include <stdint.h>

// ── /samples/samples.txt manifest ────────────────────────────────────────────
//
// Maintains a stable {id, filename} mapping so the composer's choice of sample
// survives the addition of new .wav files (which would otherwise shift the
// alphabetical order).  Format is ASCII, one entry per line:
//     <id>=<filename.wav>\n
// IDs are 1-based, unique, in [1, 127].  Entries for missing .wav files are
// preserved (re-uploading the same name later re-links them).

#define SM_NAME_LEN     24    // includes .wav extension, null-terminated
#define SM_MAX_ENTRIES  50    // soft cap chosen for BSS; PROG range allows 127

struct SmEntry {
    uint8_t  id;
    char     name[SM_NAME_LEN];
    uint32_t size;    // file size in bytes as of the last sync; 0 = not on the card
};

// Reads existing manifest, scans /samples/ for new .wav files, appends any
// new ones with the next free id, writes manifest back, and publishes the new
// table in memory.  The table is built to one side and swapped in, so lookups
// from other tasks (the MIDI task finding a song's sample) never see it empty
// or half-built; on failure the previous table stays.  trim.txt is loaded by
// the first successful sync only.  Loop task only.  Returns true on success.
bool sampleManifestSync();

// Number of entries currently in memory (call after sampleManifestSync()).
int sampleManifestCount();

// Pointer to entry at index 0..count-1, or nullptr if out of range.
const SmEntry* sampleManifestAt(int idx);

// Filename for stable id 1..127, or nullptr if no entry exists.
const char* sampleManifestNameFor(uint8_t id);

// File size (bytes) of sample `id` as of the last sync; 0 if it has no entry or
// its file was not on the card.
uint32_t sampleManifestSizeFor(uint8_t id);

// ── /samples/trim.txt — non-destructive trim/loop/tune metadata ──────────────
// The WAVs never change; playback (and the PSRAM preloader) applies these.
// Format: <id>=<startFrame>:<endFrame>:<loop>[:<tuneCents>[:<velOff>[:<keepLen>[:<pitchCHz>]]]]\n
//   endFrame 0 = end of file; the fields after loop are optional (older files
//   have fewer) and default to 0.

struct SampleTrim {
    uint8_t  id;           // 0 = free slot
    uint32_t startFrame;
    uint32_t endFrame;     // 0 = end of file
    uint8_t  loop;
    int16_t  tuneCents;    // the sample's own TUNE, ± cents from native.  Added
                           // to an SFX column's TUNE wherever the sample plays.
    uint8_t  velOff;       // 1 = a MIDI key plays it at its own level, however
                           // hard it is struck (0 = louder/softer with velocity)
    uint8_t  keepLen;      // 1 = other pitches keep its length (PSOLA, psola.cpp)
                           // instead of playing faster / slower
    uint32_t pitchCHz;     // its detected pitch in centi-Hz, for KEEP LENGTH's
                           // grains: 0 = not analysed yet, 1 = no clear pitch.
                           // Set by sampleTrimSetPitch, never by sampleTrimSet.
};

// Loaded alongside the manifest by sampleManifestSync().
// nullptr = nothing stored (play the whole file, no loop, native pitch).
const SampleTrim* sampleTrimFor(uint8_t id);

// The sample's own TUNE in cents (0 when nothing is stored).  RAM lookup only.
static inline int16_t sampleTuneCentsFor(uint8_t id) {
    const SampleTrim* t = sampleTrimFor(id);
    return t ? t->tuneCents : 0;
}

// True if KEEP LENGTH is on for the sample.  RAM lookup only.
static inline bool sampleKeepLenFor(uint8_t id) {
    const SampleTrim* t = sampleTrimFor(id);
    return t && t->keepLen;
}

// True if the sample's velocity sensitivity is switched off.  RAM lookup only.
static inline bool sampleVelOffFor(uint8_t id) {
    const SampleTrim* t = sampleTrimFor(id);
    return t && t->velOff;
}

// Update/insert in RAM (everything at its default removes the entry — unless
// its pitch is known, which is kept).  Leaves pitchCHz alone.
void sampleTrimSet(uint8_t id, uint32_t startFrame, uint32_t endFrame, uint8_t loop,
                   int16_t tuneCents = 0, uint8_t velOff = 0, uint8_t keepLen = 0);
// Record the sample's detected pitch (centi-Hz; 1 = no clear pitch).
void sampleTrimSetPitch(uint8_t id, uint32_t pitchCHz);

// Persist the RAM table to /samples/trim.txt (SD write — loop task only).
bool sampleTrimSave();

// Re-read /samples/trim.txt, replacing the table in memory — after a restore
// has put a new trim.txt on the card.  Loop task only.
void sampleTrimReload();

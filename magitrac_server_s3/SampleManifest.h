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
// new ones with the next free id, writes manifest back, and leaves the table
// populated in memory.  Returns true on success.
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
// Format: <id>=<startFrame>:<endFrame>:<loop>[:<tuneCents>]\n
//   endFrame 0 = end of file; tuneCents is optional (older files have three
//   fields) and defaults to 0.

struct SampleTrim {
    uint8_t  id;           // 0 = free slot
    uint32_t startFrame;
    uint32_t endFrame;     // 0 = end of file
    uint8_t  loop;
    int16_t  tuneCents;    // the sample's own TUNE, ± cents from native.  Added
                           // to an SFX column's TUNE wherever the sample plays.
};

// Loaded alongside the manifest by sampleManifestSync().
// nullptr = nothing stored (play the whole file, no loop, native pitch).
const SampleTrim* sampleTrimFor(uint8_t id);

// The sample's own TUNE in cents (0 when nothing is stored).  RAM lookup only.
static inline int16_t sampleTuneCentsFor(uint8_t id) {
    const SampleTrim* t = sampleTrimFor(id);
    return t ? t->tuneCents : 0;
}

// Update/insert in RAM (everything at its default removes the entry).
void sampleTrimSet(uint8_t id, uint32_t startFrame, uint32_t endFrame, uint8_t loop,
                   int16_t tuneCents = 0);

// Persist the RAM table to /samples/trim.txt (SD write — loop task only).
bool sampleTrimSave();

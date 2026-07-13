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
    uint8_t id;
    char    name[SM_NAME_LEN];
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

// ── /samples/trim.txt — non-destructive trim/loop metadata ───────────────────
// The WAVs never change; playback (and the PSRAM preloader) applies these.
// Format: <id>=<startFrame>:<endFrame>:<loop>\n   (endFrame 0 = end of file)

struct SampleTrim {
    uint8_t  id;           // 0 = free slot
    uint32_t startFrame;
    uint32_t endFrame;     // 0 = end of file
    uint8_t  loop;
};

// Loaded alongside the manifest by sampleManifestSync().
// nullptr = no trim stored (play the whole file, no loop).
const SampleTrim* sampleTrimFor(uint8_t id);

// Update/insert in RAM (start==0 && end==0 && !loop removes the entry).
void sampleTrimSet(uint8_t id, uint32_t startFrame, uint32_t endFrame, uint8_t loop);

// Persist the RAM table to /samples/trim.txt (SD write — loop task only).
bool sampleTrimSave();

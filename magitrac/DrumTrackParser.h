#pragma once
#include <stdint.h>
#include "DrumGmMap.h"

// ── Drum-track text-file parser ──────────────────────────────────────────────
// Parses the drum-patterns.com text format:
//   Tempo: NNN
//   Time: N/N
//   Swing: NN
//   Order: a,b,c,...
//   [N] Pattern Name
//   CH X-X-X-X-X-X-X-X-       ← 16 chars, 'X' = hit, '-' = silence
//   OH ----------------
//   ... (13 instrument rows per block)
//
// Multiple blocks per file.  Of the header values only Tempo is kept (it
// sets the audition speed); Time / Swing / Order are recognised and skipped.

#define DRUM_PATTERN_STEPS       16
#define DRUM_PATTERN_MAX_INSTRS  13      // matches DRUM_GM_ENTRIES
#define DRUM_PATTERN_MAX_BLOCKS  32
#define DRUM_BLOCK_NAME_LEN      48

struct DrumPatternInstr {
    char    code[DRUM_GM_CODE_LEN];
    uint8_t hits[DRUM_PATTERN_STEPS];   // 1 = hit, 0 = silence

    bool isActive() const;              // any hits set
};

struct DrumPatternBlock {
    char             name[DRUM_BLOCK_NAME_LEN];
    DrumPatternInstr instrs[DRUM_PATTERN_MAX_INSTRS];
    uint8_t          numInstrs;        // count of populated entries in `instrs`

    // Indices (into instrs[]) of the active instruments (those with at least
    // one hit), written into `out`.  Returns the number written, at most
    // `cap` — size `out` to DRUM_PATTERN_MAX_INSTRS to get them all.
    int   activeIndices(int* out, int cap) const;
};

class DrumPatternFile {
public:
    DrumPatternFile() = default;
    ~DrumPatternFile();
    DrumPatternFile(const DrumPatternFile&)            = delete;   // owns _blocks
    DrumPatternFile& operator=(const DrumPatternFile&) = delete;

    // Parse `text` of `len` bytes.  Returns true on success (at least one
    // block recognised).  Re-parsing replaces any previous content.  Also
    // false if the block storage could not be allocated.
    bool parse(const char* text, uint32_t len);

    int  blockCount() const               { return _blockCount; }
    const DrumPatternBlock* block(int i) const {
        return (_blocks && i >= 0 && i < _blockCount) ? &_blocks[i] : nullptr;
    }

    int  tempo() const                    { return _tempo; }

private:
    // DRUM_PATTERN_MAX_BLOCKS entries (~9.5 KB) in PSRAM, allocated by the
    // first parse() and kept — the object itself sits inside a global page.
    DrumPatternBlock* _blocks = nullptr;
    int  _blockCount = 0;
    int  _tempo      = 0;
};

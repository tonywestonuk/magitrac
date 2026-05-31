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
// Multiple blocks per file.  Header values (tempo / time / swing / order)
// are parsed for display but not used by the import logic.

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

    int   activeCount() const;          // instruments with at least one hit
    // Indices (into instrs[]) of active instruments, written into `out`.
    // Returns number written.  Caller must size `out` to >= activeCount().
    int   activeIndices(int* out, int cap) const;
};

class DrumPatternFile {
public:
    // Parse `text` of `len` bytes.  Returns true on success (at least one
    // block recognised).  Re-parsing replaces any previous content.
    bool parse(const char* text, uint32_t len);

    int  blockCount() const               { return _blockCount; }
    const DrumPatternBlock* block(int i) const {
        return (i >= 0 && i < _blockCount) ? &_blocks[i] : nullptr;
    }

    int  tempo() const                    { return _tempo; }
    const char* time() const              { return _time; }
    int  swing() const                    { return _swing; }

private:
    DrumPatternBlock _blocks[DRUM_PATTERN_MAX_BLOCKS];
    int  _blockCount = 0;
    int  _tempo      = 0;
    char _time[8]    = {};
    int  _swing      = 0;
};

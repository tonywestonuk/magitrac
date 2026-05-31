#include "DrumTrackParser.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

bool DrumPatternInstr::isActive() const {
    for (int i = 0; i < DRUM_PATTERN_STEPS; i++) if (hits[i]) return true;
    return false;
}

int DrumPatternBlock::activeCount() const {
    int n = 0;
    for (int i = 0; i < numInstrs; i++) if (instrs[i].isActive()) n++;
    return n;
}

int DrumPatternBlock::activeIndices(int* out, int cap) const {
    int n = 0;
    for (int i = 0; i < numInstrs && n < cap; i++) {
        if (instrs[i].isActive()) out[n++] = i;
    }
    return n;
}

// Move `p` past the end of the current line, return new position.
static uint32_t skipLine(const char* text, uint32_t len, uint32_t p) {
    while (p < len && text[p] != '\n') p++;
    if (p < len) p++;
    return p;
}

// Length of the current line (excluding any \n / \r).
static uint32_t lineLen(const char* text, uint32_t len, uint32_t p) {
    uint32_t q = p;
    while (q < len && text[q] != '\n' && text[q] != '\r') q++;
    return q - p;
}

// Compare prefix case-insensitively.  Returns true if `text[p..]` starts
// with `prefix`.
static bool startsWith(const char* text, uint32_t len, uint32_t p, const char* prefix) {
    uint32_t k = 0;
    while (prefix[k] && (p + k) < len) {
        if (tolower((unsigned char)text[p + k]) != tolower((unsigned char)prefix[k])) return false;
        k++;
    }
    return prefix[k] == '\0';
}

// Copy up to `cap-1` chars from text[p..eol] into `out` (trimming trailing
// spaces).  `out` is null-terminated.
static void copyLine(const char* text, uint32_t len, uint32_t p, char* out, int cap) {
    uint32_t n = lineLen(text, len, p);
    if ((int)n > cap - 1) n = cap - 1;
    memcpy(out, text + p, n);
    out[n] = '\0';
    while (n > 0 && (out[n-1] == ' ' || out[n-1] == '\t')) out[--n] = '\0';
}

bool DrumPatternFile::parse(const char* text, uint32_t len) {
    _blockCount = 0;
    _tempo      = 0;
    _time[0]    = '\0';
    _swing      = 0;
    if (!text || len == 0) return false;

    uint32_t p = 0;
    DrumPatternBlock* cur = nullptr;

    while (p < len) {
        // Skip blank lines.
        while (p < len && (text[p] == '\n' || text[p] == '\r')) p++;
        if (p >= len) break;

        // Header keys (only outside a block; harmless if inside too).
        if (startsWith(text, len, p, "Tempo:")) {
            char buf[16]; copyLine(text, len, p + 6, buf, sizeof(buf));
            _tempo = atoi(buf);
            p = skipLine(text, len, p);
            continue;
        }
        if (startsWith(text, len, p, "Time:")) {
            copyLine(text, len, p + 5, _time, sizeof(_time));
            // Strip leading spaces in _time.
            char* s = _time;
            while (*s == ' ' || *s == '\t') s++;
            if (s != _time) memmove(_time, s, strlen(s) + 1);
            p = skipLine(text, len, p);
            continue;
        }
        if (startsWith(text, len, p, "Swing:")) {
            char buf[16]; copyLine(text, len, p + 6, buf, sizeof(buf));
            _swing = atoi(buf);
            p = skipLine(text, len, p);
            continue;
        }
        if (startsWith(text, len, p, "Order:")) {
            // Skip — kept for compat with the file format but not used.
            p = skipLine(text, len, p);
            continue;
        }

        // New block header: "[N] Pattern Name"
        if (text[p] == '[') {
            if (_blockCount >= DRUM_PATTERN_MAX_BLOCKS) break;
            cur = &_blocks[_blockCount++];
            cur->numInstrs = 0;
            memset(cur->instrs, 0, sizeof(cur->instrs));
            // Skip "[N] " and copy name.
            uint32_t q = p + 1;
            while (q < len && text[q] != ']') q++;
            if (q < len) q++;
            while (q < len && (text[q] == ' ' || text[q] == '\t')) q++;
            copyLine(text, len, q, cur->name, DRUM_BLOCK_NAME_LEN);
            p = skipLine(text, len, p);
            continue;
        }

        // Instrument row inside a block.  Format: "XX <16 chars>".  First
        // two chars are alpha (the code), then whitespace, then the step
        // string.  Anything else: skip.
        if (cur && p + 3 < len && isalpha((unsigned char)text[p]) && isalpha((unsigned char)text[p+1])) {
            char code[DRUM_GM_CODE_LEN];
            code[0] = (char)toupper((unsigned char)text[p]);
            code[1] = (char)toupper((unsigned char)text[p+1]);
            code[2] = '\0';
            uint32_t q = p + 2;
            while (q < len && (text[q] == ' ' || text[q] == '\t')) q++;
            // Now `q` should point at the first of 16 step chars.
            uint32_t avail = lineLen(text, len, q);
            if (avail >= DRUM_PATTERN_STEPS && cur->numInstrs < DRUM_PATTERN_MAX_INSTRS) {
                DrumPatternInstr& instr = cur->instrs[cur->numInstrs++];
                memcpy(instr.code, code, DRUM_GM_CODE_LEN);
                for (int i = 0; i < DRUM_PATTERN_STEPS; i++) {
                    char c = text[q + i];
                    instr.hits[i] = (c == 'X' || c == 'x') ? 1 : 0;
                }
            }
        }
        p = skipLine(text, len, p);
    }

    return _blockCount > 0;
}

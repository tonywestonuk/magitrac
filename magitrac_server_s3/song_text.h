#pragma once
#include <stddef.h>
#include <stdint.h>
#include "TrackerData.h"

// ── Song ⇄ text, for the AI (claude_ai.cpp) ─────────────────────────────────
//
// A line-per-thing text form of a Song that a language model can read and
// write reliably (the raw struct is a linked note pool).  The grammar is
// documented for the model in claude_ai.cpp's system prompt — keep the two in
// step.  The format is the COMPLETE song: blocks, columns and notes not in
// the text are cleared.  The organ patch is written as ORGAN (+ SYNTH) lines;
// a reply without them keeps the base song's patch.  The song-level settings
// it doesn't carry (perf pads, MIDI-in channel/range, performer mask,
// transpose mask, flags) are taken from `base`.

struct TextBuf {
    char*  p;
    size_t cap;
    size_t len;
    bool   overflow;    // a write didn't fit (the text is then truncated)
};

void textPrintf(TextBuf& out, const char* fmt, ...);

// Appends the song (MAGITRAC SONG 1 ... END).  False if it didn't fit.
bool songToText(const Song* s, TextBuf& out);

struct SongTextResult {
    int  line;          // first bad line (1-based), 0 if none
    int  notes;
    int  blocks;
    int  skipped;       // lines ignored (unknown or malformed)
    char err[80];
    char summary[96];   // the SUMMARY line, if any
};

// Parses the text (anything outside MAGITRAC SONG ... END is ignored) into
// *dst.  dst must not alias base.  On failure dst is garbage and r.err says why.
bool songFromText(const char* text, size_t len, const Song* base, Song* dst,
                  SongTextResult& r);

#pragma once
#include <stdint.h>

// ── KEEP LENGTH — change a sample's pitch without changing its length ───────
// For samples with KEEP LENGTH on (sample editor).  Plain resampling plays a
// higher note faster and shorter.  This stretches the sample in time by the
// pitch ratio WITHOUT changing its pitch (WSOLA), then plays that stretched
// sample `ratio` times faster, like a normal pitched note: the pitch moves by
// `ratio`, the length comes back to the original, and the sound changes
// exactly as normal playback would (brighter going up) — vibrato, glides and
// speech intonation all come through.  No pitch detection needed, so drums,
// chords and noise work too.
//
// Both steps run together, per output frame.  The stretched sample is a chain
// of Hann-windowed grains (32 ms, 50 % overlap, so at most two at once and
// the windows sum to exactly 1).  Each grain starts where the sample's own
// time has got to, nudged (up to ±8 ms) to where its waveform best matches
// the end of the grain before — that matching is what keeps the pitch.  The
// grains are read `ratio` times faster with 4-point cubic interpolation.  The
// first grain plays unwindowed from the very first frame, so the attack is
// the sample's own; a one-shot fades out over its last 8 ms and ends exactly
// on time.
//
// Replaces PSOLA (one global pitch period per sample): that held the output
// at one fixed pitch, flattening any pitch movement into a robotic drone.
//
// Pure code — SamplePlayer's RAM voices call it per output frame; host-tested
// in test_timestretch.cpp.

#define TS_MAX_GRAINS 3

struct TsGrain {
    int32_t start;    // source frame the grain starts on
    float   o;        // read position within the grain (source frames, 0..len)
    bool    first;    // the sample's first grain: no fade-in
};

struct TsState {
    float    base;      // source frames per output frame at normal speed
    float    step;      // grain read speed per output frame: ratio x base
    float    ratio;
    int32_t  len;       // grain length, source frames (~32 ms)
    int32_t  hop;       // grain spacing in the stretched sample (len / 2)
    int32_t  tol;       // search range for the best match, ± source frames
    float    srcHop;    // sample time between grains (hop / ratio)
    float    src;       // sample time of the next grain's start
    float    untilNext; // stretched-sample frames to the next grain
    int32_t  prev;      // where the previous grain started
    float    t;         // sample time now (source frames) — the length clock
    uint8_t  ng;
    TsGrain  g[TS_MAX_GRAINS];
};

// ratio: the pitch change (2 = an octave up, clamped to ±4 octaves).
// outRate: the codec's rate.
void tsStart(TsState& s, uint32_t srcRate, float ratio, uint32_t outRate);

// The next output frame (L, R: int16 range).  False once a one-shot has
// played its full length (a loop runs until the voice is released).
bool tsNext(TsState& s, const int16_t* data, uint32_t frames, int ch, bool loop,
            int32_t& outL, int32_t& outR);

// Correlation multiply-adds done so far by grain matching (cost tracking for
// the host test).
uint32_t tsSearchOps();

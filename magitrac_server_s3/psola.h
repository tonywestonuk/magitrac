#pragma once
#include <stdint.h>

// ── PSOLA — change a sample's pitch without changing its length ─────────────
// For samples with KEEP LENGTH on (sample editor).  Plain resampling plays a
// higher note faster and shorter; this keeps the sample moving through time at
// its normal speed and rebuilds the pitch from short overlapping grains.
//
// Pitch-synchronous overlap-add: grains are cut from the sample on its own
// waveform cycles (every `period` source frames — from the sample's detected
// pitch) without resampling them, Hann-windowed, and laid down one per
// output period (period / ratio).  So the pitch moves by `ratio`, the length
// stays, and a voice keeps its own formants instead of going chipmunk.  A
// sample with no clear pitch gets a fixed ~15 ms "period" (plain granular).
//
// Grains are two periods long, shorter for big rises so no more than
// PSOLA_MAX_GRAINS overlap.  A sample at the codec's rate is read whole frame
// by whole frame (exact); any other rate with 4-point cubic interpolation —
// linear interpolation dulled the top end by 2 dB above 6 kHz, 4 dB above 10.
// Pure code — SamplePlayer's RAM voices call it per output frame; host-tested
// in test_psola.cpp.

#define PSOLA_MAX_GRAINS 8

struct PsolaGrain {
    float pos;        // source frame being read
    float ph;         // window phase 0..1
};

struct PsolaState {
    float    t;           // where in the sample we are (source frames)
    float    base;        // source frames per output frame at normal speed
    float    period;      // source frames between grain marks
    float    spacing;     // output frames between grain starts
    float    glen;        // grain length, source frames
    float    phStep;      // window phase per output frame
    float    norm;        // overlap-add level correction
    float    untilNext;   // output frames to the next grain
    bool     ending;      // past the end: no new grains, finish the old
    bool     whole;       // source frames per output frame is a whole number
                          // (a 32 kHz sample): grains start on whole frames
                          // and are copied exactly, with no interpolation
    uint8_t  ng;
    PsolaGrain g[PSOLA_MAX_GRAINS];
};

// periodSrc: the sample's pitch period in source frames (0 = no clear pitch).
// ratio: the pitch change (2 = an octave up).  outRate: the codec's rate.
void psolaStart(PsolaState& s, uint32_t srcRate, float periodSrc, float ratio, uint32_t outRate);

// The next output frame (L, R: int16 range).  False once the sample has
// finished (not `loop`) and the last grain has played out.
bool psolaNext(PsolaState& s, const int16_t* data, uint32_t frames, int ch, bool loop,
               int32_t& outL, int32_t& outR);

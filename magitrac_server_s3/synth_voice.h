#pragma once
#include <stdint.h>

// The SYNTH PROC sound's voice: two oscillators (saw / square / triangle /
// sine, osc 2 also noise) and a sub-octave square, mixed into a resonant
// two-pole low-pass (a zero-delay-feedback state-variable filter, stable up to
// full resonance), with one ADSR driving both the level and the cutoff.  The
// settings and their units are in OrganSynth.h.  drawbar_organ.cpp renders
// each sounding voice a block at a time; there is no dependence on the
// hardware, so test_synth_voice.cpp exercises it on the Mac.
//
// Speed matters (12 voices inside the organ's 4 ms block): a voice is
// rendered a whole block in one loop with its state in locals, and the
// filter's coefficients are recomputed every SYNTH_CTRL samples from a table
// indexed in octaves — no tan/exp in the audio path.

#define SYNTH_CTRL 32

// Derived once per audio block from the 14 settings — shared by all voices.
struct SynthBlock {
    uint8_t wave1, wave2;
    float   mix1, mix2, sub;
    float   width;              // pulse width of a SQUARE
    float   ratio2;             // osc 2 frequency / osc 1 (pitch + detune)
    float   cutoffOct, envOct, damp;   // cutoff as log2(Hz)
    float   kAtk, kDec, sus, kRel;     // per-sample envelope coefficients
    float   fs;
};

enum : uint8_t { SYN_ST_ATTACK = 0, SYN_ST_DECAY, SYN_ST_RELEASE, SYN_ST_DONE };

// Must fit in ProcVoiceState: drawbar_organ.cpp overlays the two (a voice is
// one or the other) to keep the voice table's internal RAM the same.
struct SynthVoice {
    uint32_t ph1, ph2, phSub;
    uint32_t inc1;
    uint32_t rng;
    float    env;               // 0..1
    float    s1, s2;            // filter state
    float    vel;               // velocity → level
    float    keyOct;            // cutoff key tracking, octaves (constant per note)
    uint8_t  stage;
};

void synthPrepare(SynthBlock& b, const uint8_t* p, float fs);

// Start a note.  `fromEnv` is where the envelope starts (0, or the level the
// voice was already at when it is retriggered or stolen — no click).
void synthNoteOn(SynthVoice& v, float hz, uint8_t vel, float fromEnv, float fs);

// Render n samples (best a multiple of SYNTH_CTRL), ADDING them into out[]:
// roughly ±1 at full level, envelope and velocity applied.  gate = the key
// (or pedal) is still down.  Sets stage SYN_ST_DONE (and stops) when the
// release has died away.
void synthRender(SynthVoice& v, const SynthBlock& b, bool gate, float* out, int n);

#pragma once
#include <stdint.h>

// ── Pitch detection for ADD SAMPLE ──────────────────────────────────────────
// Finds the fundamental of a recorded take, so the new sample can be tuned
// to play a C at MIDI note 60.
//
// Fourier analysis: Hann-windowed 4096-point FFTs over the loudest parts of
// the take (up to PD_MAX_FRAMES of them), their power spectra averaged.  The
// fundamental is the candidate whose harmonic series (1f..6f) best fits that
// spectrum — not simply the strongest peak, which on most real sounds is a
// harmonic and would tune it an octave or a fifth out.  The estimate is then
// refined from the interpolated peaks of up to ten harmonics.
//
// Incremental, so the loop task never stalls: pitchBegin() picks the frames,
// each pitchStep() does one FFT (a few ms) and returns true when the answer
// is ready.  `pcm` must stay valid (and unchanged) until then, or pitchAbort().
// One analysis at a time: a pitchBegin() replaces any running one, so a
// caller that steps over several passes checks pitchJob() is still its own.

#define PD_MAX_FRAMES 48

struct PitchResult {
    bool  found;      // a clear pitch (false: silent, noisy, unpitched)
    float hz;         // the fundamental
    float clarity;    // 0..1: the share of the spectrum's energy in its harmonics
};

// ch: interleaved channels (a stereo sample is analysed on its left).
bool        pitchBegin(const int16_t* pcm, uint32_t frames, uint32_t rate, uint8_t ch = 1);
bool        pitchStep();          // true = finished (also when begin failed)
PitchResult pitchResult();        // valid once pitchStep() returned true
void        pitchAbort();         // stop and free (safe when idle)
bool        pitchBusy();
uint32_t    pitchJob();           // changes with every pitchBegin()

// The cents that put `hz` on middle C (261.63 Hz), clamped to ±4800 (the
// sample editor's TUNE range).
int16_t     pitchTuneToMiddleC(float hz);
// The nearest note to `hz`: MIDI number, and how far off it is in cents.
void        pitchNearestNote(float hz, uint8_t& note, int8_t& cents);

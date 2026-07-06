#pragma once
#include <stdint.h>

// Sample-organ analysis: turn a WAV into a sequence of single-cycle wavetables,
// one per ~0.1 s frame, capturing the sample's evolving timbre (spectral
// envelope) but NOT its pitch.  The drawbar synth then plays these wavetables
// at keyboard pitch (footages = pitch layers), morphing through the frames as a
// note is held — a "spectral resynthesis organ".
//
// Analysis: per frame, forward FFT → magnitude → smoothed envelope → rebuild a
// single cycle additively with a SHARED random phase (so morphing between
// frames only changes amplitudes, staying smooth, and energy is spread so it
// isn't a buzzy impulse).

#define SAMPLE_ORGAN_WT 1024   // wavetable length = analysis FFT length

// Call once at boot (allocates PSRAM frame store + FFT + tables).
void sampleOrganInit();

// Load + analyse a WAV (mono 16-bit PCM, ≤5 s used) into the frame store.
// SLOW (SD read + ~50 FFTs + additive rebuild, ~0.2-0.4 s) and touches SD —
// call from the main loop task only, never the synth task.  Returns false on
// open/format failure (frame count then 0 = silent).
bool sampleOrganLoad(const char* wavPath);

int  sampleOrganFrameCount();            // 0 = nothing loaded / loading
const int16_t* sampleOrganFrame(int i);  // i-th wavetable (SAMPLE_ORGAN_WT int16), or nullptr

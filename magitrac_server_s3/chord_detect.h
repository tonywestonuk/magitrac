// chord_detect.h — chroma-based chord recognition (pure, no audio I/O).
//
// Given a magnitude spectrum (from the mic FFT), fold all energy into a
// 12-bin chroma vector (one bin per pitch class, summed across octaves) and
// match it against a small table of chord templates.  Chroma folding is the
// standard approach for chord ID: harmonics and octave doublings of a note
// all land on the same pitch class, so a note's overtones reinforce rather
// than masquerade as extra chord tones.
//
// This file is deliberately audio-agnostic — mic_spectrum.cpp owns capture
// and the FFT; it hands the magnitude array here.  Keeping the music theory
// separate makes the templates and thresholds easy to tweak live.
#pragma once
#include <stdint.h>

enum ChordQuality {
    CH_NONE = 0,
    CH_MAJ,     // major triad      0 4 7
    CH_MIN,     // minor triad      0 3 7
    CH_DOM7,    // dominant 7th     0 4 7 10
    CH_MIN7,    // minor 7th        0 3 7 10
    CH_MAJ7,    // major 7th        0 4 7 11
    CH_DIM,     // diminished       0 3 6
    CH_AUG,     // augmented        0 4 8
    CH_SUS4,    // suspended 4th    0 5 7
    CH_SUS2,    // suspended 2nd    0 2 7
    CH_5,       // power chord      0 7
};

struct ChordResult {
    bool        valid;        // true when a chord was identified (vs silence)
    int8_t      root;         // pitch class 0..11 (C=0), -1 when !valid
    ChordQuality quality;
    float       confidence;   // 0..1 — share of chroma energy on chord tones
    float       chroma[12];   // normalised to peak=1, for the on-screen bars
    float       level;        // 0..1 input loudness gauge (peak-relative)
};

// Analyse a magnitude spectrum.  mag[k] is the magnitude of FFT bin k;
// magLen is the number of usable bins (typically fftSize/2).  Returns a
// ChordResult; .valid is false when the input is below the silence gate.
ChordResult chordAnalyze(const float* mag, int magLen,
                         float sampleRateHz, int fftSize);

// Display helpers.
const char* chordRootName(int8_t pc);          // "C", "C#", ... "B"
const char* chordQualitySuffix(ChordQuality q); // "", "m", "7", "maj7", ...
// Full name into out (e.g. "Gm7", "C#dim").  out must hold >= 8 chars.
void chordFullName(const ChordResult& r, char* out, int outLen);

// 12-bit mask of the pitch classes that make up the chord (bit pc set).
// 0 when !valid.  Lets the UI highlight chord-tone bars.
uint16_t chordToneMask(const ChordResult& r);

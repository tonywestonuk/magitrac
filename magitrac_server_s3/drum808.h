#pragma once
#include <stdint.h>

// The 808 PROC sound: an analogue-style drum machine played on the General
// MIDI drum map (drumInstForNote).  Each drum is built the way io-808
// (github.com/vincentriemer/io-808, a Web Audio model of the TR-808) builds
// it — the same oscillators, filters, envelope shapes, times and levels:
//   BD  sine swept 98 → 48 Hz + 1 ms click, low-pass (TONE), soft clip
//   SD  476 + 238 Hz sines (50 ms) + high-passed noise (75 ms, SNAPPY)
//   toms / congas  a sine per drum (TUNE between two pitches) + click,
//       the toms also a little low-passed pink noise
//   RS  480 Hz sine and a 1750 Hz triangle through a half-wave rectifier and
//       a 10 ms "swing" VCA, high-passed;  CL  2450 Hz triangle, 40 ms
//   CP  band-passed noise: four 10 ms saw bursts + a 115 ms tail
//   MA  high-passed noise, 30 ms;  CB  540 + 800 Hz squares, band-passed
//   hats / cymbal  six detuned square waves (263 … 845 Hz) split into 5 and
//       10 kHz bands, each band its own envelope; a closed hat chokes an
//       open one
// Velocity is the accent: 100 = io-808's level, 127 about 1.4×.  Notes are
// one-shots: a drum plays out whatever the note-off does.
//
// What is shared by every voice is worked out once per block in
// drumPrepare (the square bank, the noise and the filters that come before a
// drum's VCA), so a voice only runs what is after its VCA.  No dependence on
// the hardware: test_drum808.cpp runs it on the Mac.

#define DRUM_BLOCK_MAX   128
#define DRUM_WORK_FLOATS (7 * DRUM_BLOCK_MAX)   // drumInit's work area

enum : uint8_t {
    DR_BD = 0, DR_SD, DR_LT, DR_MT, DR_HT, DR_LC, DR_MC, DR_HC,
    DR_RS, DR_CL, DR_CP, DR_MA, DR_CB, DR_CY, DR_OH, DR_CH,
    DR_COUNT, DR_DONE = 0xFF
};

// Must fit in ProcVoiceState: drawbar_organ.cpp overlays them.
struct DrumVoice {
    uint8_t  inst;      // DR_*, DR_DONE once it has played out
    uint8_t  choked;    // an open hat cut off by a closed one: fading fast
    uint32_t t;         // samples since the hit
    uint32_t dur;       // samples until it has died away
    float    gain;      // velocity × the drum's level
    float    s[10];     // the drum's own state (oscillators, filters, envelopes)
};

struct DrumSvf { float k, a1, a2, a3; };

struct DrumShared {
    float fs, invFs;
    uint32_t clickN;                 // samples in the 1 ms click pulse
    DrumSvf bp10k, bp5k, hp10k, hp8k, hp5k, bp1k, lp10k, lp5k,
            bp2640, bp480, hp480, hp2450, bp2450, snHp, bdLp;
    float cyK[3], cbK, clK;          // exponential decay factors per sample
    float metalPh[6], metalInc[6], cowPh[2], cowInc[2];
    uint32_t rng;
    float pink[7];
    float zM10[2], zM5[2], zSn[2], zCp[2], zMa[2], zTm[2];
    float *metal10, *metal5, *snare, *clap, *mar, *tom, *cow;   // this block, pre-VCA
    // The sliders, as drumPrepare last saw them (new hits take them).
    float bdDecMs, ohDecMs, snappy, tune;
    int   bdTone, sdTone, cyDec;          // what bdLp / snHp / cyK were last built for
    float cyTone, cyScale, accent;
    float lvl[5];                         // KICK, SNARE, HATS, TOMS, PERC
};

// DR_* for a General MIDI drum note, -1 for a note the 808 doesn't play.
int  drumInstForNote(uint8_t note);

// `work` = DRUM_WORK_FLOATS floats, kept for the life of `d`.
void drumInit(DrumShared& d, float fs, float* work);

// Once per block, before any drumRender: the settings (0..8, OrganSynth.h —
// BD DEC, BD TONE, SNAPPY, OH DEC, TUNE, then SD TONE, CY TONE, CY DEC,
// ACCENT and five levels; any past `count` are 4) and which drums are
// sounding (bit = DR_*).
void drumPrepare(DrumShared& d, const int* settings, uint32_t instMask, int n, int count = 5);

void drumNoteOn(DrumVoice& v, int inst, uint8_t vel, const DrumShared& d);
void drumChoke(DrumVoice& v);

// ADDS n samples (n <= DRUM_BLOCK_MAX) into out[].  false once played out.
bool drumRender(DrumVoice& v, const DrumShared& d, float* out, int n);

// Roughly how loud the voice still is, 0..1 (for voice stealing).
float drumLevel(const DrumVoice& v);

// The mix bus: a soft limiter (io-808 has one on its output), ±1 peak.
void drumBus(float* buf, int n);

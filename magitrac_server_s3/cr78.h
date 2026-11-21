#pragma once
#include <stdint.h>

// The CR78 PROC sound: Roland's CR-78 CompuRhythm, played on the General
// MIDI drum map (cr78InstForNote).
//
// Based on CW-78 by A Thousand Details - https://github.com/athousanddetails/schwung-cw-78
// (GPL-3.0; Magitrac is GPL-3.0 too).  Changed: rewritten for the ESP32 organ
// engine — float, a block per voice, the shared parts worked out once per
// block — and cut down to the voices alone (no sequencer, drive stages, sends
// or compressor), with five sliders and the GM map.  The circuit models, the
// component values and the factory figures are CW-78's, which took them from
// Roland's 1979 service notes (the factory alignment table on page 30).
//
// Three families, as on the VG-11 voice board:
//   kick, congas, bongos   a phase-shift network inside a transistor loop:
//       a struck resonator whose ring is the loop gain (longer = louder),
//       a transistor clip on the output, the strike's click leaking through,
//       a small pitch lift at the start
//   snare                  the same loop at 340 Hz + noise through its own
//       27 ms capacitor (SNAPPY balances the two)
//   rim shot, claves       a 700 mH coil and capacitor, struck and left to
//       ring (1480 Hz 5 ms, 2630 Hz 18 ms)
//   hi-hat, cymbal, maracas   the one shared noise source through a capacitor
//       envelope each (60 / 350 / 20 ms); the cymbal through a 9098 Hz tank
//   cowbell (800 + 555 Hz squares, band-passed), metallic beat (three free-
//       running oscillators near 6 kHz), guiro (a 77–125 Hz multivibrator
//       whose edges ping a 6126 Hz tank — a scrape), tambourine (noise and
//       the strike into a 3.5 kHz tank)
// Every voice is struck by the same front end: a 1 ms trigger pulse through
// 0.027 µF / 270 k and a steering diode.  Velocity is that trigger's
// voltage (a harder hit sounds different, not just louder) and, below 70%,
// a gain.  Decays are Roland's: time to a tenth of the peak.
//
// No dependence on the hardware: test_cr78.cpp runs it on the Mac.

#define CR78_BLOCK_MAX   128
#define CR78_WORK_FLOATS (3 * CR78_BLOCK_MAX)   // cr78Init's work area

enum : uint8_t {
    CR_BD = 0, CR_SD, CR_RS, CR_HH, CR_CY, CR_MA, CR_CL, CR_HB,
    CR_LB, CR_LC, CR_CB, CR_TB, CR_GU, CR_MB,
    CR_COUNT, CR_DONE = 0xFF
};

// Must fit in ProcVoiceState: drawbar_organ.cpp overlays them.
struct Cr78Voice {
    uint8_t  inst;      // CR_*, CR_DONE once it has played out
    uint8_t  pad[3];
    uint32_t t;         // samples since the hit
    uint32_t dur;       // samples until it has died away
    float    gain;      // the drum's mix level × the velocity's gain part
    float    s[10];     // the drum's own state
};

struct Cr78Svf { float k, a1, a2, a3; };

struct Cr78Shared {
    float fs, invFs;
    float trigA;                     // the front end's AC coupling, per sample
    uint32_t pulseN;                 // samples in the 1 ms trigger pulse
    float hpA[CR_COUNT];             // each drum's output high-pass (one-pole)
    Cr78Svf tank[CR_COUNT];          // fixed resonators (RS CL CY GU TB)
    Cr78Svf cowBand;
    float cowPh[2], cowInc[2], mbPh[3], mbInc[3];
    float zCow[2];
    uint32_t rng;
    float *noise, *cow, *mb;         // this block: shared noise, cowbell pair, metal bank
    // The settings, as cr78Prepare last saw them (new hits take them).
    float bdDec, snappy, snapS, hhDec, cyDec, tune;
    float click, bdTune, sdTune, sdRing, percDec;
    int   hatTone;                        // what the hat/cymbal filters were last built for
    float lvl[3];                         // KICK, SNARE, PERC
};

// CR_* for a General MIDI drum note, -1 for a note the CR-78 doesn't play.
int  cr78InstForNote(uint8_t note);

// `work` = CR78_WORK_FLOATS floats, kept for the life of `d`.
void cr78Init(Cr78Shared& d, float fs, float* work);

// Once per block, before any cr78Render: the settings (0..8, OrganSynth.h —
// BD DEC, SNAPPY, HH DEC, CY DEC, TUNE, then BD TONE, BD TUNE, SD TUNE,
// SD RING, HAT TONE, PERC DEC and three levels; any past `count` are 4) and
// which drums are sounding (bit = CR_*).
void cr78Prepare(Cr78Shared& d, const int* settings, uint32_t instMask, int n, int count = 5);

// `note` picks between the guiro's short and long scrapes.
void cr78NoteOn(Cr78Voice& v, int inst, uint8_t note, uint8_t vel, const Cr78Shared& d);

// ADDS n samples (n <= CR78_BLOCK_MAX) into out[].  false once played out.
bool cr78Render(Cr78Voice& v, const Cr78Shared& d, float* out, int n);

// Roughly how loud the voice still is, 0..1 (for voice stealing).
float cr78Level(const Cr78Voice& v);

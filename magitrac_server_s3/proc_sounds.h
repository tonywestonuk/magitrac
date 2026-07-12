#pragma once
#include <stdint.h>

// Procedural sounds for the organ's PROC type: each entry is a hand-written
// generator that synthesises its timbre at keyboard pitch — no drawbars, no
// SD, no samples.  The registry (name + param sliders) is mirrored by the
// client's OrganPage list; keep the two in the same order.
//
// Voice model shared by all generators: two wavetable oscillators (main +
// chorus twin) reading a per-voice "morphed" table that crossfades from an
// ATTACK frame to a SUSTAIN frame over the first ~80 ms of the note, plus an
// optional filtered-noise pick transient.  The frames are built additively at
// boot from harmonic tables measured off a reference recording, so a new
// sound is usually just two rows of numbers + a registry entry.

#define PROC_SOUND_COUNT 2
#define PROC_MAX_PARAMS  5       // PROC mode has no drawbars → room for 5 sliders
#define PROC_WT          1024    // wavetable length (10-bit phase index)

// Slider index semantics are partly fixed by the render loop: [0] scales the
// generator's noise (live), [2] is the global BRIGHT low-pass (live), [4] is
// the formant gain (live).  [1] and [3] are free per sound (consumed by
// procNoteOn / procBlockPrep / the envelope).
struct ProcSoundInfo {
    const char* name;
    int         nparam;
    const char* pname[PROC_MAX_PARAMS];
    uint8_t     pdef[PROC_MAX_PARAMS];   // slider defaults 0..8
    uint8_t     gain;    // output gain: table sample × gain = mix contribution.
                         // The tables are PEAK-normalised, but a dense harmonic
                         // stack has far lower RMS than a sine — match perceived
                         // loudness to the drawbars (8 ≈ one full drawbar PEAK,
                         // ~40 ≈ an 888 registration's LOUDNESS).
    uint8_t     envParam;  // slider index driving the amp-attack time (swell),
                           // 0xFF = organ default (~3 ms).  ms = 5 + v²·12.
    uint16_t    relMs;     // amp release ms, 0 = organ default (~12 ms)
};
extern const ProcSoundInfo PROC_SOUNDS[PROC_SOUND_COUNT];

// Per-voice generator state (embedded in the organ's Voice struct).
struct ProcVoiceState {
    uint32_t ph1, ph2;     // main + chorus-twin oscillator phases (Q32)
    uint32_t inc1, inc2;   // current (modulated) increments — what procSample reads
    uint32_t inc1base, inc2base;   // settled-pitch increments
    int32_t  scoopQ16;     // pitch-scoop glide factor, Q16 (65536 = settled)
    int32_t  morphQ16;     // 0..65536 ATTACK→SUSTAIN frame crossfade
    int32_t  noiseEnv;     // Q15 noise envelope (0 = done; CHOIR refloors it)
    int32_t  noiseLp;      // one-pole state of the filtered noise
    uint32_t rng;
    uint32_t lfoPh1, lfoPh2;    // per-osc vibrato LFO phases (CHOIR ensemble)
    uint32_t lfoInc1, lfoInc2;  // per-BLOCK phase increments, randomised per note
    int32_t  lastBreath;        // BREATH value baked into scratch (CHOIR; -1 = none)
};

// Build the additive frames.  Called from organInit().
void procSoundsInit();

// Start a voice: set oscillator increments from the note frequency and the
// sound's params (CHORUS detune is baked here — applies to notes played next).
void procNoteOn(int sound, ProcVoiceState& ps, float f0, const volatile int* param);

// Once per rendered block (NOT per voice): advance global per-sound modulation.
// CHOIR: the transport wow/flutter — one disc motor, so the whole ensemble
// drifts together (measured off the discs: ~1 Hz ±20 cents + ~14 Hz ±5 cents).
void procBlockTick(int sound);

// Once per block per voice: advance the frame morph and rebuild the voice's
// crossfaded wavetable into `scratch` (PROC_WT samples); per-sound block-rate
// modulation (CHOIR vibrato, breath refloor) reads the live sliders here.
void procBlockPrep(int sound, ProcVoiceState& ps, int16_t* scratch,
                   const volatile int* param);

// Frame tables (built by procSoundsInit; exposed for the inline sampler).
extern int16_t gProcTabA[PROC_SOUND_COUNT][PROC_WT];   // ATTACK frame
extern int16_t gProcTabS[PROC_SOUND_COUNT][PROC_WT];   // SUSTAIN frame

// Per-sound formant stack — up to 3 global peaking biquads on the PROC output
// (pitch-INDEPENDENT, like a vocal tract; a wavetable bump would move with the
// note).  TWO TRIBES: one stage at 900 Hz ("throat"); CHOIR: 520/1050/2800 Hz
// (the "ahh" vowel + singer's formant).  Slider [4] 0..8 scales the stage
// gains via a coefficient table built at init.  Caller must clamp after this —
// the boost can exceed int16.
#define PROC_FORMANT_STAGES 3
struct ProcFormantState { float x1, x2, y1, y2; };
extern ProcFormantState gProcFormant[PROC_FORMANT_STAGES];
extern uint8_t gProcFormantNstage[PROC_SOUND_COUNT];
// b0 b1 b2 a1 a2 (a0-normalised) per sound / stage / slider value
extern float gProcFormantCoef[PROC_SOUND_COUNT][PROC_FORMANT_STAGES][9][5];

static inline int32_t procFormant(int32_t s, int sound, int gainSlider) {
    if (gainSlider <= 0) return s;
    float y = (float)s;
    const int nst = gProcFormantNstage[sound];
    for (int st = 0; st < nst; st++) {
        const float* c = gProcFormantCoef[sound][st][gainSlider];
        ProcFormantState& f = gProcFormant[st];
        float x = y;
        y = c[0]*x + c[1]*f.x1 + c[2]*f.x2 - c[3]*f.y1 - c[4]*f.y2;
        f.x2 = f.x1; f.x1 = x;
        f.y2 = f.y1; f.y1 = y;
    }
    return (int32_t)y;
}

// One output sample, int16-scaled, before the organ envelope/master gain.
// `atk` is the ATTACK slider 0..8 (noise transient amount).  Inline — this
// runs 32 kHz × voices inside the synth loop.
static inline int32_t procSample(int sound, ProcVoiceState& ps,
                                 const int16_t* sc, int atk) {
    (void)sound;   // sound-agnostic: per-sound character lives in noteOn/blockPrep
    ps.ph1 += ps.inc1;
    ps.ph2 += ps.inc2;
    int32_t s = ((int32_t)sc[ps.ph1 >> 22] * 5 +
                 (int32_t)sc[ps.ph2 >> 22] * 3) >> 3;   // 5/8 main + 3/8 twin
    if (ps.noiseEnv > 0) {
        ps.rng = ps.rng * 1664525u + 1013904223u;
        int32_t n = (int16_t)(ps.rng >> 16);
        ps.noiseLp += (n - ps.noiseLp) >> 2;             // ~1.3 kHz pick colour
        s += (((ps.noiseLp * ps.noiseEnv) >> 15) * atk) >> 4;
        ps.noiseEnv -= (ps.noiseEnv >> 10) + 1;          // ~32 ms decay
    }
    return s;
}

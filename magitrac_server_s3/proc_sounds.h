#pragma once
#include <stdint.h>

// Procedural sounds for the organ's PROC type: each entry is a hand-written
// generator that synthesises its timbre at keyboard pitch — no drawbars, no
// SD, no samples.  The registry (name + param sliders) is mirrored by the
// client's OrganPage list; keep the two in the same order.
//
// The order is also what songs store: OrganPatch.voice = 4 + the index, and a
// number past the end plays the last sound.  REED took over index 0 when TWO
// TRIBES and THX were removed (16 Nov 2026), so CHOIR kept its number and the
// old VOCODER number (7) still lands on VOCODER.  Append new sounds at the end.
//
// CHOIR's voice model (REED and VOCODER have their own, in drawbar_organ.cpp):
// two wavetable oscillators (main +
// chorus twin) reading a per-voice "morphed" table that crossfades from an
// ATTACK frame to a SUSTAIN frame over the first ~80 ms of the note, plus an
// optional filtered-noise pick transient.  The frames are built additively at
// boot from harmonic tables measured off a reference recording, so a new
// sound is usually just two rows of numbers + a registry entry.

#define PROC_SOUND_COUNT 4
#define PROC_MAX_PARAMS  5       // PROC mode has no drawbars → room for 5 sliders
#define PROC_WT          1024    // wavetable length (10-bit phase index)

// VOCODER is a special PROC sound: each held note is one oscillator,
// and the summed notes are the CARRIER of a channel vocoder driven by the
// microphone — the chord speaks what is said into the mic.  Sliders:
//   [0] WAVE   0 = sawtooth; 1..8 = pulse, from 50% wide (square) at 1 down
//              to 6% (thin) at 8
//   [1] PWM    how far a slow per-note LFO swings the pulse's width (nothing
//              to do on the sawtooth)
//   [2] SSS    consonant noise
//   [3] GATE   how loud the mic must be before the vocoder opens (stage bleed)
//   [4] SMOOTH how slowly the bands let go: crisp and robotic … smeared and choral
// None of the shared slider meanings below apply to it.  The oscillator and
// the vocoder live in drawbar_organ.cpp.
#define PROC_IDX_VOC 2

// CHOIR — the Orchestron disc choir, the one wavetable sound (proc_sounds.cpp).
#define PROC_IDX_CHOIR 1

// REED — a reed organ (harmonium / accordion), the other special PROC sound.
// A free reed chops the air into puffs, so each rank is a pulse wave (the
// VOCODER's polyBLEP oscillator, so high notes don't alias), several ranks an
// octave or a fifth apart per note.  Sliders:
//   [0] WAVE    pulse width: 0 = near square (hollow) … 8 = thin (nasal)
//   [1] CELESTE a second 8' rank tuned sharp: 0 = off, 1..8 = 3..24 cents
//   [2] BRIGHT  the shared PROC low-pass (as for every sound but VOCODER)
//   [3] STOPS   which ranks sound — see REED_STOPS in drawbar_organ.cpp
//   [4] CHAMBER the reed chamber's nasal resonance (the shared formant slot,
//               one peak at 1.4 kHz, +3 dB a step).  Turned right up it is an
//               oboe: WAVE 8, CELESTE 0, BRIGHT 1, STOPS 0, CHAMBER 8, and the
//               organ's V1 scanner vibrato (~±14 cents at 6.9 Hz — the
//               recording's was ±15-20 cents at 5-6 Hz).
// A fixed soft attack (atkMs) and a short pitch scoop from slightly flat give
// the reed "speaking"; no key click.  Not velocity-sensitive (no organ sound is).
#define PROC_IDX_REED 0

// SYNTH — a subtractive synth (synth_voice.cpp, its own render path in
// drawbar_organ.cpp).  It has no sliders: its 14 settings come from
// organSetSynthParam (OrganSynth.h), edited on the client's SYNTH page.
// Songs store it as voice 8, not 4 + 3 (see ORGAN_VOICE_SYNTH).
#define PROC_IDX_SYNTH 3

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
    uint16_t    atkMs;     // fixed amp attack ms when envParam is 0xFF;
                           // 0 = organ default (~3 ms)
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
// NOTE: nothing calls this at present (renderBlock in drawbar_organ.cpp has no
// call), so the wow/flutter offset stays 0 and is not in the sound.
void procBlockTick(int sound);

// Once per block per voice: advance the frame morph and rebuild the voice's
// crossfaded wavetable into `scratch` (PROC_WT samples); per-sound block-rate
// modulation (CHOIR vibrato, breath refloor) reads the live sliders here.
void procBlockPrep(int sound, ProcVoiceState& ps, int16_t* scratch,
                   const volatile int* param);

// Frame tables (built by procSoundsInit into PSRAM — only procBlockPrep reads
// them, sequentially at block rate; the per-sample hot path reads the voice's
// INTERNAL scratch copy, so PSRAM latency never touches the audio loop).
extern int16_t* gProcTabA[PROC_SOUND_COUNT];   // ATTACK frame
extern int16_t* gProcTabS[PROC_SOUND_COUNT];   // SUSTAIN frame

// Per-sound formant stack — up to 3 global peaking biquads on the PROC output
// (pitch-INDEPENDENT, like a vocal tract; a wavetable bump would move with the
// note).  CHOIR: 590/1050/2800 Hz (the "ahh" vowel + singer's formant); REED:
// one stage at 1.4 kHz (the chamber).  Slider [4] 0..8 scales the stage
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

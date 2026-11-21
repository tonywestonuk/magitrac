#pragma once
#include <math.h>
#include <string.h>
#include "TrackerData.h"

// ── Organ voice numbers (OrganPatch.voice) ───────────────────────────────────
// 0..3 = DRAWBAR, TONEWHEEL, CLAUDE, NEBULA.  4.. = the PROC sounds, in the
// server's PROC_SOUNDS order: REED, CHOIR, VOCODER, SYNTH, 808, CR78.  7 is skipped:
// it was VOCODER's number before 16 Nov 2026 and songs saved then still carry
// it, so it keeps meaning VOCODER; SYNTH is stored as 8, 808 as 9, CR78 as 10.
#define ORGAN_VOICE_PROC   4
#define ORGAN_VOICE_SYNTH  8
#define ORGAN_VOICE_808    9
#define ORGAN_VOICE_CR78   10
#define ORGAN_VOICE_LAST   ORGAN_VOICE_CR78
#define ORGAN_PROC_VOCODER 2
#define ORGAN_PROC_SYNTH   3
#define ORGAN_PROC_808     4
#define ORGAN_PROC_CR78    5

// PROC sound index for a voice number, or -1 for the four organ types.  A
// number past the end plays the last sound.
static inline int organProcFromVoice(uint8_t voice) {
    if (voice < ORGAN_VOICE_PROC) return -1;
    if (voice == 7) return ORGAN_PROC_VOCODER;
    if (voice >= ORGAN_VOICE_LAST) return ORGAN_PROC_CR78;
    if (voice >= ORGAN_VOICE_SYNTH) return voice - ORGAN_VOICE_SYNTH + ORGAN_PROC_SYNTH;
    return voice - ORGAN_VOICE_PROC;
}
static inline uint8_t organVoiceFromProc(int proc) {
    return (uint8_t)(proc >= ORGAN_PROC_SYNTH ? ORGAN_VOICE_SYNTH + (proc - ORGAN_PROC_SYNTH)
                                              : ORGAN_VOICE_PROC + proc);
}
static inline bool organVoiceIsSynth(uint8_t voice) {
    return organProcFromVoice(voice) == ORGAN_PROC_SYNTH;
}
// The drum machines (808, CR78) play the GM drum map: the performer's
// transpose must not move their notes.
static inline bool organVoiceIsDrums(uint8_t voice) {
    int p = organProcFromVoice(voice);
    return p == ORGAN_PROC_808 || p == ORGAN_PROC_CR78;
}

// ── 808 — its five sliders (0..8), for the engine and the organ page ─────────
// io-808's knobs at its defaults sit at slider 4.
static inline float drum808KickDecayMs(int v) { return 50.0f + 62.5f * (float)v; }   // 50 .. 550 ms
static inline float drum808KickToneHz(int v)  { return 200.0f + 250.0f * (float)v; } // low-pass 200 .. 2200 Hz
static inline float drum808Snappy(int v)      { return (float)v / 8.0f; }            // snare noise 0 .. 1
static inline float drum808OpenHatMs(int v)   { return 90.0f + 45.0f * (float)v; }   // 90 .. 450 ms
static inline float drum808Tune(int v)        { return (float)v / 8.0f; }            // toms/congas low .. high pitch

// ── The drum machines' 14 settings ──────────────────────────────────────────
// The five sliders, then nine more kept in the drawbar bytes (a drum machine
// has no drawbars), so a song needs no new room for them.  All 0..8, 4 = the
// machine as built.
#define ORGAN_DRUM_PARAMS 14
static inline uint8_t& organDrumParam(OrganPatch& op, int i) {
    return (i < ORGAN_PATCH_SLIDERS) ? op.sliders[i] : op.bars[i - ORGAN_PATCH_SLIDERS];
}
static inline uint8_t organDrumParam(const OrganPatch& op, int i) {
    return (i < ORGAN_PATCH_SLIDERS) ? op.sliders[i] : op.bars[i - ORGAN_PATCH_SLIDERS];
}
static inline float drumOctaves(int v, float perStep) { return powf(2.0f, (float)(v - 4) * perStep); }
// A level slider: 0 = off, otherwise 3 dB a step (4 = as built, 8 = +12 dB).
static inline float drumLevelGain(int v)       { return (v <= 0) ? 0.0f : powf(10.0f, (float)(v - 4) * 0.15f); }
// 808 [5..13]: SD TONE, CY TONE, CY DEC, ACCENT, then the KICK, SNARE, HATS
// (hats + cymbal), TOMS (toms + congas) and PERC (rim, claves, clap,
// maracas, cowbell) levels.
static inline float drum808SnareToneHz(int v)  { return 5800.0f * drumOctaves(v, 0.25f); }   // snare noise high-pass 2.9 .. 11.6 kHz
static inline float drum808CymbalTone(int v)   { return (float)(v - 4) / 4.0f; }            // -1 dark .. +1 bright
static inline float drum808CymbalDecay(int v)  { return drumOctaves(v, 0.5f); }              // × io-808's, ¼ .. 4
static inline float drum808Accent(int v)       { return (float)v / 4.0f; }                  // velocity's reach: 0 none, 1 io-808, 2 double
// CR78 [5..13]: BD TONE, BD TUNE, SD TUNE, SD RING, HAT TONE, PERC DEC, then
// the KICK, SNARE and PERC (conga, bongos, rim, claves, cowbell, tambourine,
// guiro, metallic beat) levels.
static inline float cr78ClickScale(int v)      { return (v <= 0) ? 0.0f : drumOctaves(v, 0.5f); }  // kick's click, off .. 4×
static inline float cr78SnareRingMs(int v)     { return 60.0f * drumOctaves(v, 0.5f); }    // shell 15 .. 240 ms
static inline float cr78HatTone(int v)         { return drumOctaves(v, 0.25f); }           // hats/cymbal/maracas filters ½ .. 2×
static inline float cr78PercDecay(int v)       { return drumOctaves(v, 0.5f); }            // conga/bongo ring ¼ .. 4×

// ── CR78 — its five sliders (0..8); 4 is Roland's factory alignment ─────────
static inline float cr78Halving(int v)        { return powf(2.0f, (float)(v - 4) * 0.5f); }
static inline float cr78KickDecayMs(int v)    { return 100.0f * cr78Halving(v); }   // 25 .. 400 ms
// SNAPPY, the snare's snap against its shell: 4 = as fitted to a real CR-78
// on record, 0 = CW-78's original voicing (30 dB less, shorter), 8 = +8 dB.
static inline float cr78SnapDb(int v)         { return (v <= 4) ? (float)(v - 4) * 7.5f : (float)(v - 4) * 2.0f; }
static inline float cr78Snappy(int v)         { return powf(10.0f, cr78SnapDb(v) / 20.0f); }     // 1 = the record
static inline float cr78SnapMs(int v)         { return (v >= 4) ? 50.0f : 27.0f + 5.75f * (float)v; }  // decay
static inline float cr78HatDecayMs(int v)     { return 60.0f * cr78Halving(v); }    // 15 .. 240 ms
static inline float cr78CymbalDecayMs(int v)  { return 350.0f * cr78Halving(v); }   // 88 .. 1400 ms
static inline float cr78TuneSemis(int v)      { return (float)(v - 4) * 1.5f; }      // drums ±6 semitones

// ── SYNTH — the organ's subtractive PROC sound ───────────────────────────────
// Two oscillators and a sub into a resonant low-pass, one ADSR shaping both
// the level and the filter.  Its 14 settings live in the song's OrganPatch in
// place of the drawbars and sliders (a PROC sound has no drawbars): setting i
// is bars[i] for i < 9, then sliders[i - 9].  No file-format change.
#define ORGAN_SYNTH_PARAMS 14
enum OrganSynthParam : uint8_t {
    SYN_OSC1 = 0,   // waveform (SYN_WAVE_*, no NOISE)
    SYN_OSC2,       // waveform (SYN_WAVE_*)
    SYN_MIX,        // 0 = osc 1 only, 8 = even, 16 = osc 2 only
    SYN_PITCH,      // osc 2 tuning, 12 = unison (0..24 = -12..+12 semitones)
    SYN_DETUNE,     // osc 2 fine tuning sharp, 0..16 (see synthDetuneCents)
    SYN_PW,         // pulse width of a SQUARE, 0 = 50% .. 16 = 5% (thin)
    SYN_SUB,        // square one octave below osc 1, level 0..16
    SYN_CUTOFF,     // filter cutoff 0..16 (see synthCutoffHz)
    SYN_RESO,       // filter resonance 0..16
    SYN_ENV,        // how far the envelope opens the filter, 0..16
    SYN_ATTACK,     // envelope times / level, 0..16 (see the synth*Ms functions)
    SYN_DECAY,
    SYN_SUSTAIN,
    SYN_RELEASE,
};
enum : uint8_t { SYN_WAVE_SAW = 0, SYN_WAVE_SQUARE, SYN_WAVE_TRI, SYN_WAVE_SINE, SYN_WAVE_NOISE };

static const char* const SYN_WAVE_NAMES[5] = { "SAW", "SQUARE", "TRI", "SINE", "NOISE" };

// Highest value of each setting (all start at 0).
static const uint8_t ORGAN_SYNTH_MAX[ORGAN_SYNTH_PARAMS] =
    { 3, 4, 16, 24, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16 };

// Names used by the AI's song text (song_text.cpp) — keep in step with the
// prompt in claude_ai.cpp.
static const char* const ORGAN_SYNTH_KEYS[ORGAN_SYNTH_PARAMS] =
    { "osc1", "osc2", "mix", "pitch", "detune", "pw", "sub",
      "cutoff", "reso", "env", "attack", "decay", "sustain", "release" };

// A warm two-saw sound: what SYNTH starts as.
static const uint8_t ORGAN_SYNTH_DEFAULT[ORGAN_SYNTH_PARAMS] =
    { SYN_WAVE_SAW, SYN_WAVE_SAW, 8, 12, 5, 0, 0, 10, 3, 4, 2, 8, 12, 5 };

// Starting points on the SYNTH page — keep the AI prompt's examples
// (claude_ai.cpp) in step.  Settings in OrganSynthParam order.
struct OrganSynthPreset { const char* name; uint8_t p[ORGAN_SYNTH_PARAMS]; };
#define ORGAN_SYNTH_PRESET_N 6
static const OrganSynthPreset ORGAN_SYNTH_PRESETS[ORGAN_SYNTH_PRESET_N] = {
    { "PAD",     { SYN_WAVE_SAW,    SYN_WAVE_SAW,    8, 12, 6, 0, 0,  9, 2, 2, 9, 10, 14, 9 } },
    { "STRINGS", { SYN_WAVE_SAW,    SYN_WAVE_SAW,    8, 12, 5, 0, 0, 11, 1, 1, 7,  8, 14, 8 } },
    { "BRASS",   { SYN_WAVE_SAW,    SYN_WAVE_SAW,    8, 12, 3, 0, 0,  7, 3, 7, 4,  7, 11, 5 } },
    { "BASS",    { SYN_WAVE_SAW,    SYN_WAVE_SQUARE, 6,  0, 2, 4, 6,  6, 5, 6, 0,  6,  6, 3 } },
    { "LEAD",    { SYN_WAVE_SQUARE, SYN_WAVE_SAW,    8, 12, 4, 6, 0, 10, 5, 3, 0,  8, 13, 4 } },
    { "PLUCK",   { SYN_WAVE_SAW,    SYN_WAVE_SQUARE, 6, 24, 3, 4, 0,  5, 6, 9, 0,  6,  0, 6 } },
};

// The drawbar organ's starting point (classic 888, the PROC sliders' defaults).
static const uint8_t ORGAN_DEFAULT_BARS[ORGAN_PATCH_BARS]       = { 8, 8, 8, 0, 0, 0, 0, 0, 0 };
static const uint8_t ORGAN_DEFAULT_SLIDERS[ORGAN_PATCH_SLIDERS] = { 4, 4, 8, 4, 4 };

static inline uint8_t& organSynthParam(OrganPatch& op, int i) {
    return (i < ORGAN_PATCH_BARS) ? op.bars[i] : op.sliders[i - ORGAN_PATCH_BARS];
}
static inline uint8_t organSynthParam(const OrganPatch& op, int i) {
    return (i < ORGAN_PATCH_BARS) ? op.bars[i] : op.sliders[i - ORGAN_PATCH_BARS];
}
static inline uint8_t organSynthClamp(int i, int v) {
    if (i < 0 || i >= ORGAN_SYNTH_PARAMS || v < 0) return 0;
    return (uint8_t)(v > ORGAN_SYNTH_MAX[i] ? ORGAN_SYNTH_MAX[i] : v);
}

// A drum patch saved before the nine existed still holds the organ's default
// registration in those bytes (its levels would read as OFF): treat it as 4s.
static inline bool organDrumBarsStale(const OrganPatch& op) {
    return organVoiceIsDrums(op.voice) && !memcmp(op.bars, ORGAN_DEFAULT_BARS, sizeof(op.bars));
}
static inline void organDrumBarsRepair(OrganPatch& op) {
    if (organDrumBarsStale(op)) memset(op.bars, 4, sizeof(op.bars));
}

// Change the patch's voice.  SYNTH and the drum machines give the shared bytes
// meanings of their own, so moving onto one of them — or from one onto the
// organ — resets them to that side's defaults (as when the song had no organ
// patch yet): SYNTH's settings, a drum machine's 14 at 4, the organ's
// registration.  Moving between organ sounds keeps them.
static inline void organPatchSetVoice(OrganPatch& op, uint8_t voice) {
    bool had      = (op.flags & 1) != 0;
    bool wasSynth = had && organVoiceIsSynth(op.voice);
    bool wasDrums = had && organVoiceIsDrums(op.voice);
    bool isSynth  = organVoiceIsSynth(voice);
    bool isDrums  = organVoiceIsDrums(voice);
    bool same     = had && organProcFromVoice(op.voice) == organProcFromVoice(voice);
    op.flags |= 1;
    op.voice  = voice;
    if (isDrums) {
        if (!same) for (int i = 0; i < ORGAN_DRUM_PARAMS; i++) organDrumParam(op, i) = 4;
    } else if (isSynth) {
        if (!wasSynth) for (int i = 0; i < ORGAN_SYNTH_PARAMS; i++) organSynthParam(op, i) = ORGAN_SYNTH_DEFAULT[i];
    } else if (!had || wasSynth || wasDrums) {
        memcpy(op.bars, ORGAN_DEFAULT_BARS, sizeof(op.bars));
        memcpy(op.sliders, ORGAN_DEFAULT_SLIDERS, sizeof(op.sliders));
    }
}

// ── What the settings mean (the engine and the screens both use these) ───────
static inline float synthCutoffHz(int v)     { return 40.0f * powf(2.0f, (float)v * 0.55f); }   // 40 Hz .. 17.8 kHz
static inline float synthResoDamp(int v)     { return 2.0f - (float)v * 0.11875f; }             // 2 (none) .. 0.1 (ringing)
static inline float synthEnvOctaves(int v)   { return (float)v * 0.4f; }                         // 0 .. 6.4 octaves
static inline float synthDetuneCents(int v)  { return (float)(v * v) * 0.2f; }                  // 0 .. 51 cents
static inline float synthPulseWidth(int v)   { return 0.5f - (float)v * 0.028f; }               // 50% .. 5%
static inline float synthAttackMs(int v)     { return 1.0f + (float)(v * v * v); }               // 1 ms .. 4.1 s
static inline float synthDecayMs(int v)      { return 10.0f + 1.5f * (float)(v * v * v); }       // 10 ms .. 6.2 s
static inline float synthReleaseMs(int v)    { return 5.0f + 2.0f * (float)(v * v * v); }        // 5 ms .. 8.2 s

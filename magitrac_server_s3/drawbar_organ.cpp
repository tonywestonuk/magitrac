#include "drawbar_organ.h"
#include "audio_codec.h"
#include "proc_sounds.h"
#include <Arduino.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ── Tunables ──────────────────────────────────────────────────────────────────
#define ORGAN_SINGLE_SINE 0      // DEBUG: 1 = one pure 8' sine per note (level
                                 // from the 8' drawbar); 0 = full additive mix
#define ORGAN_VOICES   12        // max simultaneous notes
#define SINE_BITS      10        // table size = 1 << SINE_BITS
#define SINE_SIZE      (1 << SINE_BITS)
#define BLOCK_FRAMES   128       // stereo frames per codec write (~4 ms @ 32 kHz)

// Tonewheel model tuning.
static const float   TW_TOP_HZ   = 6000.0f;  // highest tonewheel; partials above fold down an octave
static const int     CLICK_GAIN  = 4;        // key-click loudness (tonewheel only)
static const int32_t CLICK_DECAY = 170;      // Q15 per sample → ~6 ms click decay

// ── Effects (global — apply to whichever organ type) ──────────────────────────
// Vibrato/chorus scanner: 0=off, 1..3 = V1/V2/V3 (vibrato, wet only),
// 4..6 = C1/C2/C3 (chorus, dry+wet).  A motor-driven scanner sweeping a tapped
// delay line, modelled as one LFO-swept fractional delay.
#define CHORUS_LEN 128                                // power of 2 (samples)
static const uint32_t CHORUS_LFO_INC  = 926102;       // ~6.9 Hz @ 32 kHz
static const int      CHORUS_BASE_Q8  = 12 << 8;      // base delay (8.8 fixed)
static const int      CHORUS_DEPTH_Q8 = 36 << 8;      // max sweep depth (scaled by level)
static int16_t  s_chorusBuf[CHORUS_LEN];
static int      s_chorusWi  = 0;
static uint32_t s_chorusLfo = 0;

// Leslie rotating speaker: 0=stop/off, 1=slow (chorale), 2=fast (tremolo).
// Amplitude tremolo + a Doppler-swept delay, with the rotor speed ramping
// toward its target (inertia) on a switch.
#define LESLIE_LEN 256
static const float LESLIE_SLOW_HZ     = 0.8f;
static const float LESLIE_FAST_HZ     = 6.6f;
static const int   LESLIE_AM_Q15      = 9830;         // 0.30 tremolo depth
static const int   LESLIE_DOP_BASE_Q8 = 8 << 8;
static const int   LESLIE_DOP_DEPTH_Q8= 24 << 8;      // ~0.75 ms Doppler sweep
static int16_t  s_leslieBuf[LESLIE_LEN];
static int      s_leslieWi    = 0;
static uint32_t s_lesliePhase = 0;
static float    s_leslieRate  = 0.0f;                 // current Hz (ramped)

// Tube drive: 0=off, 1=on — gentle soft-clip warmth.
static const float DRIVE_GAIN = 1.8f;

// Stereo reverb: 0=off, 1=room, 2=hall.  Freeverb-lite — 6 parallel damped
// feedback combs + 3 series allpasses per channel, mono-fed, with the right
// channel's delays offset (+23 samples, freeverb's "stereospread") so the tail
// decorrelates into width.  Lengths are the classic 44.1 kHz tunings rescaled
// to 32 kHz.  ~33 KB of int16 delay lines, ~150 int ops/sample — the wash that
// fills the spectrum valleys ("spacy"), which no dry voice tweak can.
#define RV_COMBS 6
#define RV_APS   3
#define RV_COMB_MAX 1140
#define RV_AP_MAX   428
static const int16_t RV_COMB_LEN[2][RV_COMBS] = {
    {  809,  877,  937, 1007, 1061, 1117 },
    {  832,  900,  960, 1030, 1084, 1140 },
};
static const int16_t RV_AP_LEN[2][RV_APS] = {
    { 405, 321, 247 },
    { 428, 344, 270 },
};
static int16_t s_rvCombBuf[2][RV_COMBS][RV_COMB_MAX];
static int16_t s_rvApBuf[2][RV_APS][RV_AP_MAX];
static int32_t s_rvCombLp[2][RV_COMBS];     // per-comb damping filter state
static int16_t s_rvCombPos[2][RV_COMBS];
static int16_t s_rvApPos[2][RV_APS];
// Per-mode voicing: room = tighter/darker, hall = the long spacy wash.
static const int RV_FB_Q15[3]   = { 0, 26214, 29491 };  // comb feedback .80/.90
static const int RV_DAMP_Q8[3]  = { 0,    90,     64 }; // LP amount .35/.25
static const int RV_WET_Q15[3]  = { 0, 19661, 29491 };  // wet mix   .60/.90
static const int RV_IN_Q15 = 655;                       // input scale ~0.02

static volatile int s_vibChorus = 0;   // 0..6
static volatile int s_leslie    = 0;   // 0..2
static volatile int s_drive     = 0;   // 0..1
static volatile int s_reverb    = 0;   // 0..2
static volatile bool s_sustain  = false;   // damper pedal (MIDI CC64) held

// Per-type knob params (0..8 each).  Meaning depends on the active type:
//  TONEWHEEL: [0]=click.   NEBULA: [0]=detune, [1]=glide, [2]=bright.
//  PROC: the selected sound's sliders (5 fit — no drawbars in that mode).
#define ORGAN_PARAMS PROC_MAX_PARAMS
static volatile int s_param[ORGAN_PARAMS] = { 4, 5, 6, 4, 4 };
static volatile bool s_nebRetune = false;   // NEBULA detune changed → re-tune held voices

// Master output scale.  mix accumulator is shifted right by this and clamped to
// int16.  Plenty of digital headroom (full-scale WAVs play clean through the
// same codec) — this just sets the working level: at shift 8 a single 8'
// partial at full drawbar (8) lands ~ -30 dBFS, the level that matched the Nord
// on the PA; lower drawbar settings scale below it.  Fuller registrations sum
// several partials and sit louder, still well under full scale.
static const int ORGAN_MASTER_SHIFT = 8;

// Envelope rates (per-sample Q15 increments).  Organ = near-instant on/off but
// not a hard step (avoids clicks).  ~3 ms attack, ~12 ms release @ 32 kHz.
static const int32_t ENV_ATTACK_INC  = 32767 / (3  * AUDIO_CODEC_RATE_HZ / 1000);
static const int32_t ENV_RELEASE_DEC = 32767 / (12 * AUDIO_CODEC_RATE_HZ / 1000);

// Voice model.
enum OrganType { ORGAN_DRAWBAR = 0, ORGAN_TONEWHEEL = 1, ORGAN_CLAUDE = 2, ORGAN_NEBULA = 3,
                 ORGAN_PROC = 4 };
static volatile int s_type = ORGAN_DRAWBAR;
static const char* const ORGAN_TYPE_NAMES[ORGAN_TYPE_COUNT] =
    { "DRAWBAR", "TONEWHEEL", "CLAUDE", "NEBULA", "PROC" };

// PROC type: hand-written procedural generators (see proc_sounds.cpp) — no
// drawbars; the client picks a sound from a list and its param sliders map to
// s_param.  Each voice reads a per-voice morphed wavetable rebuilt per block.
static int16_t s_voiceScratch[ORGAN_VOICES][PROC_WT];   // current morphed wavetable per voice
static int32_t s_procLp   = 0;    // BRIGHT one-pole state (global, PROC only)
static volatile int s_procSel = 0;    // selected procedural sound

// NEBULA: a sci-fi, THX-flavoured but playable voice.  Each footage is TWO
// widely-detuned oscillators (thick chorused wall) read from a BRIGHT wheel
// (extra harmonics → sparkle up high), and every note GLIDES up to pitch on
// attack (the cinematic whoosh).  ~18 osc/voice.
static int16_t   nebTab[SINE_SIZE];
static const float NEB_DETUNE     = 16.0f;   // cents — half-spread of the twin pair
static const float NEB_GLIDE_CENTS= -55.0f;  // start pitch offset, bends up to 0

// CLAUDE: a deep, lush, evolving pad.  Pure sines, but each partial is seeded
// slightly off-pitch (static micro-detune) so one note shimmers and chords form
// a wide ensemble that slowly evolves; a warm voicing + slow per-partial
// "breathing" make it bloom while held.  ~9 osc/voice like the others.
static const float CLAUDE_DETUNE      = 8.0f;    // cents — per-partial static spread
static const int   CLAUDE_BREATH_Q8   = 40;      // amplitude breathing depth (/256)
static const int   CLAUDE_VOICE_Q8[ORGAN_DRAWBARS] =   // gentle warm taper (256 = unity)
                       { 256, 240, 256, 230, 200, 210, 168, 168, 184 };
static uint32_t s_breathPhase[ORGAN_DRAWBARS] = { 0 };
static uint32_t s_breathInc[ORGAN_DRAWBARS]   = { 0 };   // set in organInit

// DRAWBAR: ideal additive — pure integer harmonic ratios.
//                              16'   5 1/3' 8'   4'   2 2/3' 2'   1 3/5' 1 1/3' 1'
static const float RATIO_DB[ORGAN_DRAWBARS] =
                       { 0.5f, 1.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f };
// TONEWHEEL: the Hammond takes each drawbar pitch from another tonewheel at an
// EQUAL-TEMPERED interval (semitones {-12,+7,0,+12,+19,+24,+28,+31,+36} from the
// 8' unison), so the partials are slightly inharmonic and beat against each
// other — the heart of the Hammond shimmer.  Values are 2^(semitones/12); note
// the +28 "third" (5.04 vs pure 5.0) is the most detuned, as on a real B3.
static const float RATIO_TW[ORGAN_DRAWBARS] =
                       { 0.5f, 1.498307f, 1.0f, 2.0f, 2.996614f, 4.0f, 5.039684f, 5.993295f, 8.0f };

const char* const ORGAN_FOOTAGE[ORGAN_DRAWBARS] =
    { "16", "5.3", "8", "4", "2.7", "2", "1.6", "1.3", "1" };

// ── State ─────────────────────────────────────────────────────────────────────
static int16_t  sineTab[SINE_SIZE];   // DRAWBAR: pure sine per partial
static int16_t  twTab[SINE_SIZE];     // TONEWHEEL: quasi-sine wheel (sine + a few % harmonics)
static volatile bool s_active = false;

// How non-sinusoidal the tonewheel is (per-wheel harmonic amounts, like the
// real tooth-profile + nonlinear pickup).  Bigger = brighter/edgier wheels.
static const float TW_H2 = 0.15f, TW_H3 = 0.08f, TW_H4 = 0.04f;

// Drawbar registration — written by the UI task, read by the synth task.  A
// torn read of one int is harmless (value just lags a block), so no lock.
static volatile int s_drawbar[ORGAN_DRAWBARS] = { 8, 8, 8, 0, 0, 0, 0, 0, 0 };

enum EnvStage { ENV_IDLE = 0, ENV_ATTACK, ENV_SUSTAIN, ENV_RELEASE };

struct Voice {
    uint8_t  note;
    uint8_t  stage;       // EnvStage
    uint8_t  sustained;   // key released while the damper pedal was down
    int32_t  env;         // Q15, 0..32767
    int32_t  clickEnv;    // Q15 key-click transient (tonewheel only)
    uint32_t rng;         // per-voice noise state (key click)
    uint32_t phase[ORGAN_DRAWBARS];
    uint32_t inc[ORGAN_DRAWBARS];   // 0 = partial above Nyquist, skipped
    uint32_t phase2[ORGAN_DRAWBARS];// NEBULA detuned twin oscillators
    uint32_t inc2[ORGAN_DRAWBARS];
    int32_t  glideQ16;              // NEBULA attack pitch glide, Q16 (65536 = settled)
    ProcVoiceState proc;            // PROC generator state
};
static Voice voices[ORGAN_VOICES];
static volatile int s_voiceCount = 0;   // for the header readout

// MIDI → synth event queue (lock-free across cores).
struct OrganEvent { uint8_t on; uint8_t note; uint8_t vel; };
static QueueHandle_t s_evtQueue = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────
static inline float midiHz(uint8_t note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

static void voiceStart(Voice& v, uint8_t note) {
    v.note      = note;
    v.stage     = ENV_ATTACK;
    v.sustained = 0;
    if (s_type == ORGAN_PROC) {
        if (v.proc.rng == 0) v.proc.rng = 0x1234567u ^ (note * 2654435761u);
        procNoteOn(s_procSel, v.proc, midiHz(note), s_param);
        v.clickEnv = 0;
        v.glideQ16 = 65536;
        return;
    }
    bool tw     = (s_type == ORGAN_TONEWHEEL);
    bool claude = (s_type == ORGAN_CLAUDE);
    bool neb    = (s_type == ORGAN_NEBULA);
    const float* ratio = tw ? RATIO_TW : RATIO_DB;   // CLAUDE/NEBULA use pure ratios + detune
    float f0 = midiHz(note);
    const float nyq = AUDIO_CODEC_RATE_HZ * 0.5f;
    if (v.rng == 0) v.rng = 0x1234567u ^ (note * 2654435761u);
    for (int h = 0; h < ORGAN_DRAWBARS; h++) {
        float fh = f0 * ratio[h];
        // Tonewheel: only 91 wheels exist, so a partial past the top wheel folds
        // down octaves instead of vanishing (Hammond foldback).  Drawbar: gate
        // anything past Nyquist to silence.
        if (tw) { while (fh > TW_TOP_HZ) fh *= 0.5f; }
        if (claude) {
            // Per-partial static micro-detune + random start phase → ensemble shimmer.
            v.rng = v.rng * 1664525u + 1013904223u;
            float cents = (((int)(v.rng >> 16) - 32768) / 32768.0f) * CLAUDE_DETUNE;
            fh *= powf(2.0f, cents / 1200.0f);
            v.phase[h] = v.rng;
        } else {
            v.phase[h] = 0;
        }
        v.inc[h]   = (fh >= nyq) ? 0
                   : (uint32_t)((double)fh * 4294967296.0 / AUDIO_CODEC_RATE_HZ);
        if (neb) {
            // A widely-detuned twin pair around the footage → thick chorused wall.
            // DETUNE knob (s_param[0], 0..8) sets the half-spread, ~2..30 cents.
            float det = 2.0f + s_param[0] * 3.5f;
            float fa = fh * powf(2.0f, -det / 1200.0f);
            float fb = fh * powf(2.0f,  det / 1200.0f);
            v.inc[h]  = (fa >= nyq) ? 0 : (uint32_t)((double)fa * 4294967296.0 / AUDIO_CODEC_RATE_HZ);
            v.inc2[h] = (fb >= nyq) ? 0 : (uint32_t)((double)fb * 4294967296.0 / AUDIO_CODEC_RATE_HZ);
            v.rng = v.rng * 1664525u + 1013904223u; v.phase[h]  = v.rng;
            v.rng = v.rng * 1664525u + 1013904223u; v.phase2[h] = v.rng;
        }
    }
    v.clickEnv = tw ? 32767 : 0;            // key click on attack (tonewheel)
    // NEBULA: start flat and glide up to pitch.  GLIDE knob (s_param[1]) sets the
    // depth: 0 = no glide, 8 = ~-96 cents.
    float glCents = -(float)s_param[1] * 12.0f;
    v.glideQ16 = neb ? (int32_t)(powf(2.0f, glCents / 1200.0f) * 65536.0f) : 65536;
}

static void applyNoteOn(uint8_t note, uint8_t vel) {
    (void)vel;   // organ is touch-insensitive; velocity only gates on/off
    // Retrigger if this note is already sounding.
    for (int i = 0; i < ORGAN_VOICES; i++) {
        if (voices[i].stage != ENV_IDLE && voices[i].note == note) {
            voiceStart(voices[i], note);
            return;
        }
    }
    // Grab an idle voice.
    for (int i = 0; i < ORGAN_VOICES; i++) {
        if (voices[i].stage == ENV_IDLE) { voiceStart(voices[i], note); return; }
    }
    // Steal the quietest voice (lowest envelope).
    int best = 0; int32_t lo = voices[0].env;
    for (int i = 1; i < ORGAN_VOICES; i++) {
        if (voices[i].env < lo) { lo = voices[i].env; best = i; }
    }
    voiceStart(voices[best], note);
}

static void applyNoteOff(uint8_t note) {
    for (int i = 0; i < ORGAN_VOICES; i++) {
        if (voices[i].stage != ENV_IDLE && voices[i].stage != ENV_RELEASE &&
            voices[i].note == note) {
            // Damper pedal down: the key is up but the note rings on until the
            // pedal lifts (voices with keys still held are never `sustained`).
            if (s_sustain) voices[i].sustained = 1;
            else           voices[i].stage = ENV_RELEASE;
        }
    }
}

static void drainEvents() {
    OrganEvent e;
    while (s_evtQueue && xQueueReceive(s_evtQueue, &e, 0) == pdTRUE) {
        if (e.on && e.vel > 0) applyNoteOn(e.note, e.vel);
        else                   applyNoteOff(e.note);
    }
    if (!s_sustain) {
        // Pedal lifted (or was never down): release everything it was holding.
        for (int i = 0; i < ORGAN_VOICES; i++) {
            if (voices[i].sustained) {
                voices[i].sustained = 0;
                if (voices[i].stage != ENV_IDLE) voices[i].stage = ENV_RELEASE;
            }
        }
    }
}

// Recompute a NEBULA voice's detuned twin increments from its note + the
// current DETUNE knob, WITHOUT touching phase/glide — so held notes re-tune
// smoothly (the oscillators just change rate, no click).
static void nebRetuneVoice(Voice& v) {
    float det = 2.0f + s_param[0] * 3.5f;
    float f0  = midiHz(v.note);
    const float nyq = AUDIO_CODEC_RATE_HZ * 0.5f;
    float mlo = powf(2.0f, -det / 1200.0f), mhi = powf(2.0f, det / 1200.0f);
    for (int h = 0; h < ORGAN_DRAWBARS; h++) {
        float fh = f0 * RATIO_DB[h];
        float fa = fh * mlo, fb = fh * mhi;
        v.inc[h]  = (fa >= nyq) ? 0 : (uint32_t)((double)fa * 4294967296.0 / AUDIO_CODEC_RATE_HZ);
        v.inc2[h] = (fb >= nyq) ? 0 : (uint32_t)((double)fb * 4294967296.0 / AUDIO_CODEC_RATE_HZ);
    }
}

// Render BLOCK_FRAMES stereo frames into `out` (interleaved L,R int16).
static void renderBlock(int16_t* out) {
    // DRAWBAR/CLAUDE use pure sines; TONEWHEEL uses the richer quasi-sine wheel.
    bool tw     = (s_type == ORGAN_TONEWHEEL);
    bool claude = (s_type == ORGAN_CLAUDE);
    bool neb  = (s_type == ORGAN_NEBULA);
    bool proc = (s_type == ORGAN_PROC);
    const int16_t* wtab = neb ? nebTab : (tw ? twTab : sineTab);

    // PROC: snapshot the sound selection + noise slider once per block, and
    // let the sound override the organ's near-instant envelope (CHOIR swells
    // over an ATTACK-slider time and releases slowly).
    int procSel  = s_procSel;
    int procAtk  = s_param[0];
    int procGain = PROC_SOUNDS[procSel].gain;
    int32_t envAtkInc = ENV_ATTACK_INC;
    int32_t envRelDec = ENV_RELEASE_DEC;
    if (proc) {
        const ProcSoundInfo& si = PROC_SOUNDS[procSel];
        if (si.envParam != 0xFF) {
            int sv = s_param[si.envParam];
            int ms = 5 + sv * sv * 12;                    // 5..773 ms
            envAtkInc = 32767 / (ms * (int)(AUDIO_CODEC_RATE_HZ / 1000));
            if (envAtkInc < 1) envAtkInc = 1;
        }
        if (si.relMs) {
            envRelDec = 32767 / ((int)si.relMs * (int)(AUDIO_CODEC_RATE_HZ / 1000));
            if (envRelDec < 1) envRelDec = 1;
        }
    }

    // DETUNE knob moved → re-tune every sounding NEBULA voice in place.
    if (neb && s_nebRetune) {
        s_nebRetune = false;
        for (int i = 0; i < ORGAN_VOICES; i++)
            if (voices[i].stage != ENV_IDLE) nebRetuneVoice(voices[i]);
    }

    // Snapshot the registration once per block.  CLAUDE folds its warm voicing
    // and slow per-partial breathing into the per-bar level here, so the
    // sample loop pays nothing extra.
    int db[ORGAN_DRAWBARS];
    for (int h = 0; h < ORGAN_DRAWBARS; h++) {
        int v = s_drawbar[h];
        if (claude && v > 0) {
            int16_t lf = sineTab[s_breathPhase[h] >> (32 - SINE_BITS)];   // -32767..32767
            s_breathPhase[h] += s_breathInc[h];
            int br = 256 + ((CLAUDE_BREATH_Q8 * lf) >> 15);               // 256 ± depth
            v = (v * CLAUDE_VOICE_Q8[h] * br) >> 16;                      // warm + breathing
        }
        if (neb && v > 0 && h >= 4) {
            v = (v * (32 + s_param[2] * 28)) >> 8;   // BRIGHT knob fades the upper footages
        }
        db[h] = v;
    }

    // Snapshot effect settings once per block.
    int  vc        = s_vibChorus;                 // 0..6
    bool vcOn      = (vc != 0);
    bool vcVibOnly = (vc >= 1 && vc <= 3);        // V = wet only; C = dry+wet
    int  vcLevel   = vcVibOnly ? vc : (vc - 3);   // 1..3
    int  vcDepthQ8 = (CHORUS_DEPTH_Q8 * vcLevel) / 3;

    bool driveOn = (s_drive != 0);

    int rvb      = s_reverb;
    int rvFb     = RV_FB_Q15[rvb];
    int rvDamp   = RV_DAMP_Q8[rvb];
    int rvWet    = RV_WET_Q15[rvb];

    // Leslie rotor speed ramps toward its target (inertia on a slow/fast/stop switch).
    float lesTarget = (s_leslie == 2) ? LESLIE_FAST_HZ : (s_leslie == 1) ? LESLIE_SLOW_HZ : 0.0f;
    s_leslieRate += (lesTarget - s_leslieRate) * 0.004f;
    bool     leslieOn  = (s_leslie != 0) || (s_leslieRate > 0.02f);
    uint32_t leslieInc = (uint32_t)((double)s_leslieRate * 4294967296.0 / AUDIO_CODEC_RATE_HZ);

    for (int n = 0; n < BLOCK_FRAMES; n++) {
        int32_t mix = 0;

        for (int i = 0; i < ORGAN_VOICES; i++) {
            Voice& v = voices[i];
            if (v.stage == ENV_IDLE) continue;

            int32_t acc = 0;
#if ORGAN_SINGLE_SINE
            // DEBUG: one pure sine per note — the 8' fundamental only, its
            // level set by the 8' drawbar (index 2).  Flip ORGAN_SINGLE_SINE
            // to 0 to restore the full 9-partial additive mix.
            v.phase[2] += v.inc[2];
            acc = (int32_t)wtab[v.phase[2] >> (32 - SINE_BITS)] * db[2];
#else
            if (neb) {
                // Advance the attack glide once per block (creep up to settled).
                if (n == 0 && v.glideQ16 < 65536) {
                    v.glideQ16 += (65536 - v.glideQ16) >> 5;
                    v.glideQ16 += 1;
                    if (v.glideQ16 > 65536) v.glideQ16 = 65536;
                }
                bool gliding = (v.glideQ16 < 65536);
                for (int h = 0; h < ORGAN_DRAWBARS; h++) {
                    if (db[h] == 0) continue;
                    if (gliding) {
                        v.phase[h]  += (uint32_t)(((uint64_t)v.inc[h]  * (uint32_t)v.glideQ16) >> 16);
                        v.phase2[h] += (uint32_t)(((uint64_t)v.inc2[h] * (uint32_t)v.glideQ16) >> 16);
                    } else {
                        v.phase[h]  += v.inc[h];
                        v.phase2[h] += v.inc2[h];
                    }
                    // Two detuned twins per footage → thick chorused wall (>>1 keeps level).
                    int16_t s = (int16_t)((wtab[v.phase[h]  >> (32 - SINE_BITS)] +
                                           wtab[v.phase2[h] >> (32 - SINE_BITS)]) >> 1);
                    acc += (int32_t)s * db[h];
                }
            } else if (proc) {
                // PROC: rebuild this voice's morphed wavetable once per block,
                // then the generator's oscillators read it.  Gain comes from
                // the sound's registry entry (RMS-matched to the drawbars).
                if (n == 0) procBlockPrep(procSel, v.proc, s_voiceScratch[i], s_param);
                acc = procSample(procSel, v.proc, s_voiceScratch[i], procAtk) * procGain;
            } else {
                for (int h = 0; h < ORGAN_DRAWBARS; h++) {
                    if (v.inc[h] == 0 || db[h] == 0) continue;
                    v.phase[h] += v.inc[h];
                    int16_t s = wtab[v.phase[h] >> (32 - SINE_BITS)];
                    acc += (int32_t)s * db[h];
                }
            }
#endif

            // Per-voice envelope (Q15), advanced once per sample.
            switch (v.stage) {
                case ENV_ATTACK:
                    v.env += envAtkInc;
                    if (v.env >= 32767) { v.env = 32767; v.stage = ENV_SUSTAIN; }
                    break;
                case ENV_RELEASE:
                    v.env -= envRelDec;
                    if (v.env <= 0) { v.env = 0; v.stage = ENV_IDLE; }
                    break;
                default: break;
            }
            mix += (int32_t)(((int64_t)acc * v.env) >> 15);   // 64-bit: acc*env overflows int32

            // Key click — a fast-decaying noise burst on attack, added outside
            // the tonal envelope so it isn't swallowed by the attack ramp.
            if (v.clickEnv > 0) {
                v.rng = v.rng * 1664525u + 1013904223u;       // LCG noise
                int16_t noise = (int16_t)(v.rng >> 16);
                mix += (((int32_t)noise * v.clickEnv) >> 15) * s_param[0];   // CLICK knob
                v.clickEnv -= CLICK_DECAY;
                if (v.clickEnv < 0) v.clickEnv = 0;
            }
        }

        int32_t s = mix >> ORGAN_MASTER_SHIFT;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        // PROC BRIGHT slider — global one-pole low-pass tone control — then the
        // THROAT formant (boost can exceed int16, so re-clamp before the FX
        // chain writes int16 delay buffers).
        if (proc) {
            int alpha = 16 + s_param[2] * 30;          // 16 (dark) .. 256 (open)
            s_procLp += ((s - s_procLp) * alpha) >> 8;
            s = procFormant(s_procLp, procSel, s_param[4]);
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
        }

        // 1. Scanner vibrato/chorus — dry mixed with an LFO-swept fractional delay.
        if (vcOn) {
            s_chorusBuf[s_chorusWi] = (int16_t)s;
            s_chorusLfo += CHORUS_LFO_INC;
            int16_t lfo = sineTab[s_chorusLfo >> (32 - SINE_BITS)];   // -32767..32767
            int dq8 = CHORUS_BASE_Q8 + (int)(((int32_t)vcDepthQ8 * (lfo + 32768)) >> 16);
            int di = dq8 >> 8, fr = dq8 & 255;
            int r0 = (s_chorusWi - di      + CHORUS_LEN) & (CHORUS_LEN - 1);
            int r1 = (s_chorusWi - di - 1  + CHORUS_LEN) & (CHORUS_LEN - 1);
            int wet = (s_chorusBuf[r0] * (256 - fr) + s_chorusBuf[r1] * fr) >> 8;
            s_chorusWi = (s_chorusWi + 1) & (CHORUS_LEN - 1);
            s = vcVibOnly ? wet : ((s + wet) >> 1);   // V = vibrato (wet), C = chorus (dry+wet)
        }

        // 2. Tube drive — gentle soft-clip warmth (FPU is cheap on the S3).
        if (driveOn) {
            float x = (s * (1.0f / 32768.0f)) * DRIVE_GAIN;
            s = (int32_t)lrintf(tanhf(x) * 32767.0f);
        }

        // 3. Leslie rotating speaker — STEREO: the horn swings L<->R, so each
        // channel gets opposite Doppler (two delay taps in anti-phase) and
        // anti-phase tremolo.  Off → both channels carry the (mono) dry signal.
        int32_t l = s, r = s;
        if (leslieOn) {
            s_leslieBuf[s_leslieWi] = (int16_t)s;
            s_lesliePhase += leslieInc;
            int16_t rot = sineTab[s_lesliePhase >> (32 - SINE_BITS)];   // -32767..32767
            int dqL = LESLIE_DOP_BASE_Q8 + (int)(((int32_t)LESLIE_DOP_DEPTH_Q8 * (rot + 32768)) >> 16);
            int dqR = LESLIE_DOP_BASE_Q8 + (int)(((int32_t)LESLIE_DOP_DEPTH_Q8 * (32768 - rot)) >> 16);
            int diL = dqL >> 8, frL = dqL & 255;
            int diR = dqR >> 8, frR = dqR & 255;
            int l0 = (s_leslieWi - diL     + LESLIE_LEN) & (LESLIE_LEN - 1);
            int l1 = (s_leslieWi - diL - 1 + LESLIE_LEN) & (LESLIE_LEN - 1);
            int r0 = (s_leslieWi - diR     + LESLIE_LEN) & (LESLIE_LEN - 1);
            int r1 = (s_leslieWi - diR - 1 + LESLIE_LEN) & (LESLIE_LEN - 1);
            int dopL = (s_leslieBuf[l0] * (256 - frL) + s_leslieBuf[l1] * frL) >> 8;
            int dopR = (s_leslieBuf[r0] * (256 - frR) + s_leslieBuf[r1] * frR) >> 8;
            s_leslieWi = (s_leslieWi + 1) & (LESLIE_LEN - 1);
            int ampL = 32768 + ((LESLIE_AM_Q15 * rot) >> 15);
            int ampR = 32768 - ((LESLIE_AM_Q15 * rot) >> 15);
            l = ((int32_t)dopL * ampL) >> 15;
            r = ((int32_t)dopR * ampR) >> 15;
        }

        // 4. Stereo reverb — mono-fed comb/allpass bank per channel, added on
        // top of the dry signal.  The tail keeps ringing after note-off (the
        // synth task only sleeps when the organ deactivates, so tails play out).
        if (rvb) {
            int32_t inp = ((l + r) * RV_IN_Q15) >> 16;   // (l+r)/2 × 0.02
            for (int c = 0; c < 2; c++) {
                int32_t wet = 0;
                for (int k = 0; k < RV_COMBS; k++) {
                    int16_t* buf = s_rvCombBuf[c][k];
                    int pos = s_rvCombPos[c][k];
                    int32_t y = buf[pos];
                    // one-pole damping in the feedback path (dulls the tail)
                    s_rvCombLp[c][k] += ((y - s_rvCombLp[c][k]) * (256 - rvDamp)) >> 8;
                    int32_t fb = inp + ((s_rvCombLp[c][k] * rvFb) >> 15);
                    if (fb >  32767) fb =  32767;
                    if (fb < -32768) fb = -32768;
                    buf[pos] = (int16_t)fb;
                    if (++pos >= RV_COMB_LEN[c][k]) pos = 0;
                    s_rvCombPos[c][k] = (int16_t)pos;
                    wet += y;
                }
                for (int k = 0; k < RV_APS; k++) {
                    int16_t* buf = s_rvApBuf[c][k];
                    int pos = s_rvApPos[c][k];
                    int32_t y = buf[pos];
                    int32_t in2 = wet + (y >> 1);        // allpass g = 0.5
                    if (in2 >  32767) in2 =  32767;
                    if (in2 < -32768) in2 = -32768;
                    buf[pos] = (int16_t)in2;
                    if (++pos >= RV_AP_LEN[c][k]) pos = 0;
                    s_rvApPos[c][k] = (int16_t)pos;
                    wet = y - (in2 >> 1);
                }
                int32_t& ch = c ? r : l;
                ch += (wet * rvWet) >> 15;
            }
        }

        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        out[2 * n]     = (int16_t)l;
        out[2 * n + 1] = (int16_t)r;
    }

    int live = 0;
    for (int i = 0; i < ORGAN_VOICES; i++) if (voices[i].stage != ENV_IDLE) live++;
    s_voiceCount = live;
}

static void organTaskFn(void*) {
    static int16_t block[BLOCK_FRAMES * 2];
    for (;;) {
        if (!s_active) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        drainEvents();
        renderBlock(block);
        audioCodecPlay((const uint8_t*)block, sizeof(block));  // blocks → paces us
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void organInit() {
    for (int i = 0; i < SINE_SIZE; i++)
        sineTab[i] = (int16_t)lrintf(32767.0f * sinf(2.0f * PI * i / SINE_SIZE));
    // Tonewheel waveform: a quasi-sine wheel.  Find the peak first, then
    // normalise to full scale so it sits at the same level as the pure sine.
    float twPk = 0.0f;
    for (int i = 0; i < SINE_SIZE; i++) {
        float p = 2.0f * PI * i / SINE_SIZE;
        float w = sinf(p) + TW_H2 * sinf(2*p) + TW_H3 * sinf(3*p) + TW_H4 * sinf(4*p);
        if (fabsf(w) > twPk) twPk = fabsf(w);
    }
    float twSc = 32767.0f / twPk;
    for (int i = 0; i < SINE_SIZE; i++) {
        float p = 2.0f * PI * i / SINE_SIZE;
        float w = sinf(p) + TW_H2 * sinf(2*p) + TW_H3 * sinf(3*p) + TW_H4 * sinf(4*p);
        twTab[i] = (int16_t)lrintf(w * twSc);
    }
    // NEBULA bright wheel: sine + a tall harmonic stack → sci-fi sparkle.
    float nbPk = 0.0f;
    for (int i = 0; i < SINE_SIZE; i++) {
        float p = 2.0f * PI * i / SINE_SIZE;
        float w = sinf(p) + 0.18f*sinf(2*p) + 0.12f*sinf(3*p) + 0.08f*sinf(4*p)
                          + 0.05f*sinf(5*p) + 0.035f*sinf(6*p);
        if (fabsf(w) > nbPk) nbPk = fabsf(w);
    }
    float nbSc = 32767.0f / nbPk;
    for (int i = 0; i < SINE_SIZE; i++) {
        float p = 2.0f * PI * i / SINE_SIZE;
        float w = sinf(p) + 0.18f*sinf(2*p) + 0.12f*sinf(3*p) + 0.08f*sinf(4*p)
                          + 0.05f*sinf(5*p) + 0.035f*sinf(6*p);
        nebTab[i] = (int16_t)lrintf(w * nbSc);
    }
    for (int i = 0; i < ORGAN_VOICES; i++) {
        voices[i].stage = ENV_IDLE; voices[i].env = 0;
        voices[i].clickEnv = 0;     voices[i].rng = 0x2545F491u + i * 2654435761u;
        voices[i].glideQ16 = 65536;
    }
    // CLAUDE per-partial breathing LFOs — slow, incommensurate rates (advance
    // once per block).  inc = rate * BLOCK_FRAMES / fs * 2^32.
    for (int h = 0; h < ORGAN_DRAWBARS; h++) {
        float rate = 0.06f + 0.021f * h;   // ~0.06 .. 0.23 Hz
        s_breathInc[h]   = (uint32_t)((double)rate * BLOCK_FRAMES / AUDIO_CODEC_RATE_HZ * 4294967296.0);
        s_breathPhase[h] = h * 0x20000000u;   // spread starting phases
    }
    procSoundsInit();
    s_evtQueue = xQueueCreate(64, sizeof(OrganEvent));
    // 6144: sized when the render path grew floats (formant biquad) + the PROC
    // noteOn path (soft-double powf) — 4096 left thin margin.
    xTaskCreatePinnedToCore(organTaskFn, "ORGAN", 6144, nullptr, 5, nullptr, 0);  // Core 0
}

void organSetActive(bool on) {
    if (on == s_active) return;
    if (on) {
        // Shrink the DMA for low latency BEFORE the synth task starts streaming.
        // (Caller has already stopped the sample player; the reconfig waits out
        // any in-flight write.)
        audioCodecSetLowLatency(true);
        s_active = true;
    } else {
        s_active = false;          // stop the synth task streaming first
        for (int i = 0; i < ORGAN_VOICES; i++) {
            voices[i].stage = ENV_IDLE; voices[i].env = 0; voices[i].sustained = 0;
        }
        s_voiceCount = 0;
        s_sustain    = false;      // don't carry a held pedal into the next session
        if (s_evtQueue) xQueueReset(s_evtQueue);
        audioCodecSetLowLatency(false);   // restore the SD-robust DMA for SamplePlayer
    }
}

bool organActive() { return s_active; }

void organNoteOn(uint8_t note, uint8_t velocity) {
    if (!s_active || !s_evtQueue) return;
    OrganEvent e{ 1, note, velocity };
    xQueueSend(s_evtQueue, &e, 0);
}

void organNoteOff(uint8_t note) {
    if (!s_active || !s_evtQueue) return;
    OrganEvent e{ 0, note, 0 };
    xQueueSend(s_evtQueue, &e, 0);
}

int  organGetDrawbar(int i) {
    return (i >= 0 && i < ORGAN_DRAWBARS) ? s_drawbar[i] : 0;
}

void organSetDrawbar(int i, int value) {
    if (i < 0 || i >= ORGAN_DRAWBARS) return;
    if (value < 0) value = 0;
    if (value > 8) value = 8;
    s_drawbar[i] = value;
}

int organVoiceCount() { return s_voiceCount; }

void organSetType(int t) {
    if (t >= 0 && t < ORGAN_TYPE_COUNT) s_type = t;   // applies to notes played next
}

int organGetType() { return s_type; }

const char* organTypeName(int t) {
    return (t >= 0 && t < ORGAN_TYPE_COUNT) ? ORGAN_TYPE_NAMES[t] : "?";
}

void organSetVibChorus(int v) { if (v >= 0 && v <= 6) s_vibChorus = v; }
void organSetLeslie(int v)    { if (v >= 0 && v <= 2) s_leslie    = v; }
void organSetDrive(int v)     { if (v >= 0 && v <= 1) s_drive     = v; }
void organSetReverb(int v)    { if (v >= 0 && v <= 2) s_reverb    = v; }
void organSetSustain(bool on) { s_sustain = on; }
int  organGetVibChorus()      { return s_vibChorus; }
int  organGetLeslie()         { return s_leslie; }
int  organGetDrive()          { return s_drive; }
int  organGetReverb()         { return s_reverb; }

void organSetParam(int idx, int value) {
    if (idx < 0 || idx >= ORGAN_PARAMS) return;
    if (value < 0) value = 0;
    if (value > 8) value = 8;
    s_param[idx] = value;
    // NEBULA DETUNE (idx 0) is note-onset baked — flag held voices to re-tune.
    if (idx == 0 && s_type == ORGAN_NEBULA) s_nebRetune = true;
}
int organGetParam(int idx) {
    return (idx >= 0 && idx < ORGAN_PARAMS) ? s_param[idx] : 0;
}

void organSetProcSound(int i) {
    if (i >= 0 && i < PROC_SOUND_COUNT) s_procSel = i;   // applies to notes played next
}
int organGetProcSound() { return s_procSel; }

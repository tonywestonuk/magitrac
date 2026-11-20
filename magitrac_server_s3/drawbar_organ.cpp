#include "drawbar_organ.h"
#include "audio_codec.h"
#include "proc_sounds.h"
#include "synth_voice.h"
#include "OrganSynth.h"
#include <Arduino.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

// ── Tunables ──────────────────────────────────────────────────────────────────
#define ORGAN_VOICES   12        // max simultaneous notes
#define SINE_BITS      10        // table size = 1 << SINE_BITS
#define SINE_SIZE      (1 << SINE_BITS)
#define BLOCK_FRAMES   128       // stereo frames per codec write (~4 ms @ 32 kHz)

// Tonewheel model tuning.
static const float   TW_TOP_HZ   = 6000.0f;  // highest tonewheel; partials above fold down an octave
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

// ── Vocoder ────────────────────────────────────────────────────────────────────
// Classic channel vocoder: the mic (modulator) is split into N log-spaced bands;
// each band's amplitude envelope gates the SAME band of the organ (carrier), so
// the held chord "speaks/sings" the mic.  Bandpass biquads are RBJ BPF (0 dB peak
// gain).  Runs in float (the FX chain already does); only when both organ +
// vocoder are on.
//
// Reworked Nov 2026 after measuring the first version on the bench (its actual
// code, run on recorded speech — the mic itself had been proven clean with the
// hold-to-record test):
//   • Its "sibilance" path added white noise in proportion to the whole voice,
//     not just the consonants, and regardless of the organ: it came out 4 dB
//     LOUDER than the voice-shaped organ, and played with no keys held.  That
//     hiss was most of what you heard.  Consonant noise is now fed into the top
//     bands of the CARRIER instead (see vocoderProcess), so the mic's own
//     band envelopes decide when it sounds and no keys = silence.
//   • The output sat 9 dB below the dry organ and depended on how loud / close
//     the voice was.  It now follows the voice's level slowly and scales by it.
//   • The envelope followers were fast enough (1 ms / 10 ms) to carry the
//     voice's own pitch into the organ as a buzz.  Slower, with a smoothing
//     stage: half the wobble.
//   • Analysis filters are a little narrower than the synthesis ones, so each
//     band listens to less of its neighbours.
//   • The organ is a poor thing to shape: an 8-8-8 registration has 0.2% of
//     its energy between 600 Hz and 3.4 kHz, where vowels differ, so the voice
//     could only turn it up and down ("it just changes the volume").  The
//     vocoder was therefore taken off the drawbar organ altogether and made a
//     PROC sound of its own (PROC_IDX_VOC): each held note is one oscillator
//     (vocOsc) — a sawtooth, or a pulse of one of eight widths, by the WAVE
//     slider, the pulse with a slow per-note LFO on its width (PWM) — and
//     those are the carrier.  (No sine: it has no harmonics for a vowel to
//     shape.)
//     The bands above 500 Hz get a fixed treble lift.
// The times, Qs, gain and lift below are bench-derived starting points — tune
// by ear.  SSS, GATE and SMOOTH are on sliders: they depend on the voice, the
// mic, how loud the stage is, and taste.
#define VOC_BANDS 16
static const float VOC_F_LO = 180.0f, VOC_F_HI = 7000.0f;
static const float VOC_Q_MOD = 7.0f;         // analysis (mic) filters
static const float VOC_Q_CAR = 4.0f;         // synthesis (organ) filters
static const float VOC_ATK_MS = 2.0f;        // envelope follower attack
// Its release, and the smoothing after it, come from the SMOOTH slider (0..8):
// release = 5 ms × 2^(slider/2) → 5, 7, 10, 14, 20, 28, 40, 57, 80 ms, with the
// smoothing stage at 0.4 of that (kept within 3 … 25 ms).  3 is the bench
// setting (14 ms / 5.7 ms).  See vocSetSmooth().
#define VOC_NOISE_BAND 12          // bands from here up (3.4 kHz+) also carry noise
static float s_vocGain = 2.0f;     // output level when the loudest band is at the reference — ear-tune knob
static float s_vocSib  = 0.3f;     // consonant noise, relative to the chord's own level — set per block from the SSS slider
static const float VOC_SIB_PER_STEP = 0.075f;   // SSS slider 0..8 → 0 .. 0.6 (4 = 0.3)
#define VOC_CARRIER_SHIFT 1        // extra headroom on the summed carrier (chords would clip)
// Treble lift on the bands above VOC_LIFT_HZ: gain = (f / VOC_LIFT_HZ)^VOC_LIFT_EXP
// (0.5 = +3 dB per octave).  Speech and a sawtooth both fall away with
// frequency; without a lift the upper formants come out too faint to hear.
static const float VOC_LIFT_HZ = 500.0f, VOC_LIFT_EXP = 0.5f, VOC_LIFT_MAX = 8.0f;
// Pulse width: WAVE slider 1..8 → 50% (square) … 6%, in equal steps.  PWM
// slider 0..8 → an LFO swings the width either side of that, by PWM/8 of the
// room there is down to VOC_WIDTH_MIN (so a square at PWM 8 sweeps 4%…96%, and
// a thin pulse only a little — it has nowhere to go).  The LFO runs at about
// VOC_PWM_HZ, each note a little different and starting somewhere different,
// so a chord shimmers rather than pumping.
static const float VOC_WIDTH_WIDE = 0.50f, VOC_WIDTH_THIN = 0.06f;
static const float VOC_PWM_HZ = 0.7f, VOC_WIDTH_MIN = 0.04f;
// A square wave carries about 5 dB more energy than a sawtooth of the same
// height; the pulse is drawn this much shorter so moving WAVE off the sawtooth
// changes the tone and not the loudness.
static const float VOC_PULSE_PEAK = 32767.0f * 0.6f;
static const float VOC_REF_FLOOR = 250.0f;   // the level follower never boosts beyond 1/this
// Band level treated as silence.  GATE slider 0..8 → 12.5 × 2^slider: 12 … 3200
// (each step doubles it = 6 dB; 1 = the bench value, 25).  For scale, normal
// speech close to the mic puts its strong bands at roughly 500–3000.
static float voc_gate = 25.0f;

// Set by the audio task for each pass: the VOCODER sound is selected AND its
// mic feed is in step (see organTaskFn).  renderBlock runs the vocoder only then.
static bool    s_vocBlock = false;
static int16_t s_micBlock[BLOCK_FRAMES * 2];   // modulator block (LEFT = mic), full-duplex

static float voc_mb0[VOC_BANDS], voc_ma1[VOC_BANDS], voc_ma2[VOC_BANDS];   // analysis (mic)
static float voc_cb0[VOC_BANDS], voc_ca1[VOC_BANDS], voc_ca2[VOC_BANDS];   // synthesis (organ)
static float voc_mx1[VOC_BANDS], voc_mx2[VOC_BANDS], voc_my1[VOC_BANDS], voc_my2[VOC_BANDS];
static float voc_cx1[VOC_BANDS], voc_cx2[VOC_BANDS], voc_cy1[VOC_BANDS], voc_cy2[VOC_BANDS];
static float voc_env[VOC_BANDS], voc_sm[VOC_BANDS];
static float voc_lift[VOC_BANDS];
static float voc_ref = 0.0f, voc_carEnv = 0.0f;
static float voc_kAtk, voc_kRel, voc_kSm, voc_kRefUp, voc_kRefDn, voc_kCarUp, voc_kCarDn;
static uint32_t voc_rng = 0x1357BD13u;

static void vocoderReset() {
    for (int b = 0; b < VOC_BANDS; b++) {
        voc_mx1[b]=voc_mx2[b]=voc_my1[b]=voc_my2[b]=0.0f;
        voc_cx1[b]=voc_cx2[b]=voc_cy1[b]=voc_cy2[b]=0.0f;
        voc_env[b]=voc_sm[b]=0.0f;
    }
    voc_ref = VOC_REF_FLOOR; voc_carEnv = 0.0f;
}

static void vocBpf(float f0, float q, float& b0, float& a1, float& a2) {
    float w0 = 2.0f * PI * f0 / AUDIO_CODEC_RATE_HZ;
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = alpha / a0;  a1 = (-2.0f * cosf(w0)) / a0;  a2 = (1.0f - alpha) / a0;   // b1 = 0, b2 = -b0
}
static float vocCoef(float ms) { return 1.0f - expf(-1000.0f / (ms * AUDIO_CODEC_RATE_HZ)); }

// SMOOTH slider (0..8) → the envelope release + smoothing coefficients.
static void vocSetSmooth(int slider) {
    float rel = 5.0f * powf(2.0f, (float)slider * 0.5f);
    float sm  = rel * 0.4f;
    if (sm < 3.0f)  sm = 3.0f;
    if (sm > 25.0f) sm = 25.0f;
    voc_kRel = vocCoef(rel);
    voc_kSm  = vocCoef(sm);
}

static void vocoderInit() {
    for (int b = 0; b < VOC_BANDS; b++) {
        float f0 = VOC_F_LO * powf(VOC_F_HI / VOC_F_LO, (float)b / (VOC_BANDS - 1));
        vocBpf(f0, VOC_Q_MOD, voc_mb0[b], voc_ma1[b], voc_ma2[b]);
        vocBpf(f0, VOC_Q_CAR, voc_cb0[b], voc_ca1[b], voc_ca2[b]);
        float lift = powf(f0 / VOC_LIFT_HZ, VOC_LIFT_EXP);
        voc_lift[b] = (lift < 1.0f) ? 1.0f : (lift > VOC_LIFT_MAX) ? VOC_LIFT_MAX : lift;
    }
    voc_kAtk   = vocCoef(VOC_ATK_MS);
    vocSetSmooth(3);                    // until the slider says otherwise
    voc_kRefUp = vocCoef(10.0f);        voc_kRefDn = vocCoef(1500.0f);
    voc_kCarUp = vocCoef(5.0f);         voc_kCarDn = vocCoef(80.0f);
    vocoderReset();
}

// The correction that rounds off a step in a waveform (polyBLEP): without it a
// sawtooth or pulse played high folds back down as out-of-tune grit.  t = where
// in the cycle (0..1), dt = step per sample.
static inline float vocBlep(float t, float dt) {
    if (t < dt)        { float x = t / dt;           return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { float x = (t - 1.0f) / dt;  return x * x + x + x + 1.0f; }
    return 0.0f;
}

// One VOCODER carrier oscillator sample at the note's pitch: a sawtooth, or
// (pulse = true) a pulse whose high part is `width` of the cycle (0.5 =
// square).  ±32767, like one of the organ's sine partials.
static inline int32_t vocOsc(uint32_t phase, uint32_t inc, bool pulse, float width) {
    float t  = (float)phase * (1.0f / 4294967296.0f);    // 0..1 through the cycle
    float dt = (float)inc   * (1.0f / 4294967296.0f);
    if (!pulse) {                                        // falling ramp, stepping up at 0
        float s = 1.0f - 2.0f * t + vocBlep(t, dt);
        return (int32_t)(s * 32767.0f);
    }
    float t2 = t - width; if (t2 < 0.0f) t2 += 1.0f;     // up at 0, down at `width`
    float s  = ((t < width) ? 1.0f : -1.0f) + vocBlep(t, dt) - vocBlep(t2, dt);
    return (int32_t)(s * VOC_PULSE_PEAK);                // (its DC goes in the band filters)
}

// ── REED (PROC_IDX_REED) ──────────────────────────────────────────────────────
// Each held note sounds up to REED_MAX_RANKS pulse-wave ranks (the STOPS
// slider picks which) plus, with CELESTE up, a second 8' rank tuned sharp.
// The ranks reuse the voice's drawbar oscillator slots — a PROC sound never
// uses them: phase[k] runs the rank, inc[k] is its increment for this block
// (worked out from vocInc, the note's 8' pitch, so STOPS / CELESTE moved with
// a note held apply at once).  glideQ16 is the speaking scoop: a note starts
// REED_SCOOP_CENTS flat and settles over ~15 ms.
#define REED_MAX_RANKS   4
#define REED_SCOOP_CENTS 15.0f
struct ReedStop { uint8_t n; float ratio[REED_MAX_RANKS]; float level[REED_MAX_RANKS]; };
// Ratios to the 8' pitch: 16' = 0.5, 4' = 2, 2 2/3' = 3, 2' = 4.  The 8' is the
// loudest rank, as on a harmonium.
static const ReedStop REED_STOPS[9] = {
    { 1, { 1.0f },                   { 1.0f } },                     // 0: 8'
    { 2, { 1.0f, 2.0f },             { 1.0f, 0.8f } },               // 1: 8' 4'
    { 2, { 0.5f, 1.0f },             { 0.8f, 1.0f } },               // 2: 16' 8'
    { 3, { 0.5f, 1.0f, 2.0f },       { 0.8f, 1.0f, 0.8f } },         // 3: 16' 8' 4'
    { 3, { 1.0f, 2.0f, 4.0f },       { 1.0f, 0.8f, 0.6f } },         // 4: 8' 4' 2'
    { 4, { 0.5f, 1.0f, 2.0f, 4.0f }, { 0.8f, 1.0f, 0.8f, 0.6f } },   // 5: 16' 8' 4' 2'
    { 2, { 1.0f, 3.0f },             { 1.0f, 0.6f } },               // 6: 8' 2 2/3' (nasal)
    { 3, { 0.5f, 1.0f, 3.0f },       { 0.8f, 1.0f, 0.6f } },         // 7: 16' 8' 2 2/3'
    { 4, { 0.5f, 1.0f, 2.0f, 3.0f }, { 0.8f, 1.0f, 0.8f, 0.6f } },   // 8: 16' 8' 4' 2 2/3'
};

// One rank: a pulse `width` of the cycle high, polyBLEP-smoothed, with its DC
// (2·width − 1) taken off so a narrow pulse doesn't push the mix off centre.
// About ±1.
static inline float reedPulse(uint32_t phase, uint32_t inc, float width) {
    float t  = (float)phase * (1.0f / 4294967296.0f);
    float dt = (float)inc   * (1.0f / 4294967296.0f);
    float t2 = t - width; if (t2 < 0.0f) t2 += 1.0f;
    float s  = ((t < width) ? 1.0f : -1.0f) + vocBlep(t, dt) - vocBlep(t2, dt);
    return s - (2.0f * width - 1.0f);
}

// carrier = organ sample, mod = mic sample (both int, int16-ish range).
static int32_t vocoderProcess(int32_t carrier, int32_t mod) {
    float c = (float)carrier, m = (float)mod;

    // Consonant noise: unvoiced sounds (s, t, f, sh) have no pitch, and an
    // organ has almost nothing up where they live, so there is nothing for the
    // top bands to shape.  Give those bands noise to work with — scaled to the
    // chord's own level, so no keys held = no noise, and band-limited by the
    // synthesis filters it is fed through.  The mic's top-band envelopes do the
    // rest: vowels barely open them, sibilants open them wide.
    float ca = fabsf(c);
    voc_carEnv += (ca - voc_carEnv) * ((ca > voc_carEnv) ? voc_kCarUp : voc_kCarDn);
    voc_rng = voc_rng * 1664525u + 1013904223u;
    float nz = (float)((int16_t)(voc_rng >> 16)) * (1.0f / 32768.0f);
    float cHi = c + nz * voc_carEnv * s_vocSib;

    float acc = 0.0f, top = 0.0f;
    for (int b = 0; b < VOC_BANDS; b++) {
        // analysis: mic band → rectify → fast-attack/slow-release → smooth
        float mbp = voc_mb0[b] * (m - voc_mx2[b]) - voc_ma1[b] * voc_my1[b] - voc_ma2[b] * voc_my2[b];
        voc_mx2[b] = voc_mx1[b]; voc_mx1[b] = m; voc_my2[b] = voc_my1[b]; voc_my1[b] = mbp;
        float a = fabsf(mbp);
        float e = voc_env[b];
        e += (a - e) * ((a > e) ? voc_kAtk : voc_kRel);
        voc_env[b] = e;
        float s = voc_sm[b] + (e - voc_sm[b]) * voc_kSm;
        voc_sm[b] = s;
        if (s > top) top = s;

        // synthesis: the same band of the organ (top bands: organ + noise)
        float x = (b >= VOC_NOISE_BAND) ? cHi : c;
        float cbp = voc_cb0[b] * (x - voc_cx2[b]) - voc_ca1[b] * voc_cy1[b] - voc_ca2[b] * voc_cy2[b];
        voc_cx2[b] = voc_cx1[b]; voc_cx1[b] = x; voc_cy2[b] = voc_cy1[b]; voc_cy1[b] = cbp;

        float g = s - voc_gate;                     // below the gate = silence
        if (g > 0.0f) acc += cbp * g * voc_lift[b];
    }

    // Level: follow the voice's loudest band slowly (quick up, 1.5 s down) and
    // scale by it, so speaking quietly or further from the mic still drives the
    // organ to full level.  The floor stops room noise being boosted to it.
    voc_ref += (top - voc_ref) * ((top > voc_ref) ? voc_kRefUp : voc_kRefDn);
    float ref = (voc_ref > VOC_REF_FLOOR) ? voc_ref : VOC_REF_FLOOR;
    if (ref < 2.0f * voc_gate) ref = 2.0f * voc_gate;    // nothing just over a high gate gets full boost
    float out = acc * (s_vocGain / ref);

    if (out >  32767.0f) out =  32767.0f;
    if (out < -32768.0f) out = -32768.0f;
    return (int32_t)out;
}

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

// PROC type: hand-written procedural generators (see proc_sounds.cpp) — no
// drawbars; the client picks a sound from a list and its param sliders map to
// s_param.  A CHOIR voice reads a per-voice morphed wavetable rebuilt per block.
static int16_t s_voiceScratch[ORGAN_VOICES][PROC_WT];   // current morphed wavetable per voice
static int32_t s_procLp   = 0;    // BRIGHT one-pole state (global, PROC only)
static volatile int s_procSel = 0;    // selected procedural sound

// SYNTH's 14 settings (OrganSynth.h), written by the UI / sequencer, read once
// per block into s_synthBlk.
static volatile uint8_t s_synthParam[ORGAN_SYNTH_PARAMS];
static SynthBlock s_synthBlk;
static float      s_synthBuf[BLOCK_FRAMES];   // this block's SYNTH voices, summed
static uint32_t   s_synthUs = 0;              // time spent rendering them (load log)

// NEBULA: a sci-fi, THX-flavoured but playable voice.  Each footage is TWO
// widely-detuned oscillators (thick chorused wall) read from a BRIGHT wheel
// (extra harmonics → sparkle up high), and every note GLIDES up to pitch on
// attack (the cinematic whoosh).  ~18 osc/voice.
static int16_t   nebTab[SINE_SIZE];

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
    uint8_t  isSynth;     // started by SYNTH: `syn` is live, not `proc`
    int32_t  env;         // Q15, 0..32767
    int32_t  clickEnv;    // Q15 key-click transient (tonewheel only)
    uint32_t rng;         // per-voice noise state (key click)
    uint32_t phase[ORGAN_DRAWBARS];
    uint32_t inc[ORGAN_DRAWBARS];   // 0 = partial above Nyquist, skipped
    uint32_t phase2[ORGAN_DRAWBARS];// NEBULA detuned twin oscillators
    uint32_t inc2[ORGAN_DRAWBARS];
    int32_t  glideQ16;              // NEBULA attack pitch glide, Q16 (65536 = settled)
    uint32_t vocPhase, vocInc;      // VOCODER carrier oscillator at the note's pitch
    uint32_t vocLfoPh, vocLfoInc;   // its pulse-width LFO (advanced once per block)
    float    vocWidth;              // this block's pulse width
    union {
        ProcVoiceState proc;        // PROC generator state (CHOIR)
        SynthVoice     syn;         // SYNTH
    };
};
static_assert(sizeof(SynthVoice) <= sizeof(ProcVoiceState), "SYNTH would grow the voice table");
static Voice voices[ORGAN_VOICES];
static volatile int s_voiceCount = 0;   // for the header readout

// MIDI → synth event queue (lock-free across cores).
struct OrganEvent { uint8_t on; uint8_t note; uint8_t vel; };
static QueueHandle_t s_evtQueue = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────
static inline float midiHz(uint8_t note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

static void voiceStart(Voice& v, uint8_t note, uint8_t vel) {
    bool wasLive = (v.stage != ENV_IDLE);
    v.note      = note;
    v.stage     = ENV_ATTACK;
    v.sustained = 0;
    if (s_type == ORGAN_PROC && s_procSel == PROC_IDX_SYNTH) {
        // Retriggered or stolen: carry on from the level it was at (no click).
        float from = (wasLive && v.isSynth) ? v.syn.env : 0.0f;
        v.isSynth  = 1;
        synthNoteOn(v.syn, midiHz(note), vel, from, AUDIO_CODEC_RATE_HZ);
        v.env      = (int32_t)(v.syn.env * 32767.0f);
        v.clickEnv = 0;
        v.glideQ16 = 65536;
        return;
    }
    v.isSynth   = 0;
    // VOCODER carrier oscillator: the note's own pitch, starting at a
    // note-dependent point in its cycle so a chord's notes don't all peak together.
    v.vocInc    = (uint32_t)((double)midiHz(note) * 4294967296.0 / AUDIO_CODEC_RATE_HZ);
    v.vocPhase  = (uint32_t)note * 0x9E3779B9u;
    // Pulse-width LFO: its own starting point and a rate within ±20% of
    // VOC_PWM_HZ per note (per-BLOCK increment).
    if (v.rng == 0) v.rng = 0x1234567u ^ (note * 2654435761u);
    v.rng = v.rng * 1664525u + 1013904223u;
    v.vocLfoPh  = v.rng;
    v.rng = v.rng * 1664525u + 1013904223u;
    float lfoHz = VOC_PWM_HZ * (0.8f + 0.4f * (float)(v.rng >> 16) / 65536.0f);
    v.vocLfoInc = (uint32_t)((double)lfoHz * BLOCK_FRAMES / AUDIO_CODEC_RATE_HZ * 4294967296.0);
    v.vocWidth  = 0.5f;
    if (s_type == ORGAN_PROC) {
        if (s_procSel == PROC_IDX_VOC) {
            // just the carrier oscillator set up above
        } else if (s_procSel == PROC_IDX_REED) {
            // Ranks start at scattered points in their cycles (no phase-locked
            // click as a chord lands); increments are set per block.
            for (int k = 0; k <= REED_MAX_RANKS; k++) {
                v.rng = v.rng * 1664525u + 1013904223u;
                v.phase[k] = v.rng;
                v.inc[k]   = 0;
            }
            v.clickEnv = 0;
            v.glideQ16 = (int32_t)(65536.0f * powf(2.0f, -REED_SCOOP_CENTS / 1200.0f));
            return;
        } else {
            if (v.proc.rng == 0) v.proc.rng = 0x1234567u ^ (note * 2654435761u);
            procNoteOn(s_procSel, v.proc, midiHz(note), s_param);
        }
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
    // The organ sounds ignore velocity (it only gates on/off); SYNTH uses it.
    // Retrigger if this note is already sounding.
    for (int i = 0; i < ORGAN_VOICES; i++) {
        if (voices[i].stage != ENV_IDLE && voices[i].note == note) {
            voiceStart(voices[i], note, vel);
            return;
        }
    }
    // Grab an idle voice.
    for (int i = 0; i < ORGAN_VOICES; i++) {
        if (voices[i].stage == ENV_IDLE) { voiceStart(voices[i], note, vel); return; }
    }
    // Steal the quietest voice (lowest envelope).
    int best = 0; int32_t lo = voices[0].env;
    for (int i = 1; i < ORGAN_VOICES; i++) {
        if (voices[i].env < lo) { lo = voices[i].env; best = i; }
    }
    voiceStart(voices[best], note, vel);
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
        } else if (si.atkMs) {
            envAtkInc = 32767 / ((int)si.atkMs * (int)(AUDIO_CODEC_RATE_HZ / 1000));
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

    // VOCODER sound: its five sliders, once per block.  (vocOn also needs the
    // mic feed primed — see organTaskFn.)
    bool  vocSel   = proc && procSel == PROC_IDX_VOC;
    bool  vocOn    = vocSel && s_vocBlock;
    bool  vocPulse = false;
    float vocBaseW = 0.5f, vocPwmAmp = 0.0f;
    if (vocSel) {
        int wave = s_param[0];                 // 0 = saw, 1..8 = pulse, wide → thin
        vocPulse = (wave > 0);
        if (vocPulse) {
            vocBaseW  = VOC_WIDTH_WIDE - (float)(wave - 1) * ((VOC_WIDTH_WIDE - VOC_WIDTH_THIN) / 7.0f);
            vocPwmAmp = (float)s_param[1] * 0.125f * (vocBaseW - VOC_WIDTH_MIN);
        }
        s_vocSib = (float)s_param[2] * VOC_SIB_PER_STEP;
        voc_gate = 12.5f * (float)(1 << s_param[3]);
        static int lastSmooth = -1;
        int smooth = s_param[4];
        if (smooth != lastSmooth) { lastSmooth = smooth; vocSetSmooth(smooth); }
    }

    // SYNTH sound: its settings, once per block.  Level: ±1 from the voice
    // × 8 drawbar units × the registry gain, as for the other PROC sounds.
    // Each voice is rendered for the whole block here (much cheaper than a
    // sample at a time); the sample loop below just mixes the sum in.
    bool  synthSel   = proc && procSel == PROC_IDX_SYNTH;
    float synthScale = 0.0f;
    if (synthSel) {
        int64_t t0 = esp_timer_get_time();
        uint8_t sp[ORGAN_SYNTH_PARAMS];
        for (int k = 0; k < ORGAN_SYNTH_PARAMS; k++) sp[k] = s_synthParam[k];
        synthPrepare(s_synthBlk, sp, AUDIO_CODEC_RATE_HZ);
        synthScale = 32767.0f * 8.0f * (float)procGain;
        memset(s_synthBuf, 0, sizeof(s_synthBuf));
        for (int i = 0; i < ORGAN_VOICES; i++) {
            Voice& v = voices[i];
            if (v.stage == ENV_IDLE) continue;
            if (!v.isSynth) {                 // left over from the sound before: its state means nothing here
                v.stage = ENV_IDLE; v.env = 0; v.sustained = 0;
                continue;
            }
            synthRender(v.syn, s_synthBlk, v.stage != ENV_RELEASE, s_synthBuf, BLOCK_FRAMES);
            if (v.syn.stage == SYN_ST_DONE) { v.stage = ENV_IDLE; v.env = 0; }
            else v.env = (int32_t)(v.syn.env * 32767.0f);   // so stealing still picks the quietest
        }
        s_synthUs = (uint32_t)(esp_timer_get_time() - t0);
    } else {
        s_synthUs = 0;
    }

    // REED sound: its stops, céleste, width and level, once per block.
    bool  reedSel  = proc && procSel == PROC_IDX_REED;
    const ReedStop* reedStop = &REED_STOPS[0];
    float reedWidth = 0.5f, reedCeleste = 0.0f, reedScale = 0.0f;
    if (reedSel) {
        reedStop  = &REED_STOPS[s_param[3] <= 8 ? s_param[3] : 8];
        reedWidth = 0.45f - (float)s_param[0] * (0.37f / 8.0f);   // 0.45 .. 0.08
        int cel   = s_param[1];
        reedCeleste = cel ? powf(2.0f, (float)(cel * 3) / 1200.0f) : 0.0f;
        // Level: the same RMS whatever the width (a DC-free pulse has RMS
        // 2·sqrt(w(1−w))) and however many ranks sound (they're unrelated in
        // phase, so their powers add).  × 8 × gain = drawbar units, as for
        // the other PROC sounds.
        float pw = 0.0f;
        for (int k = 0; k < reedStop->n; k++) pw += reedStop->level[k] * reedStop->level[k];
        if (cel) pw += 1.0f;
        float pulseRms = 2.0f * sqrtf(reedWidth * (1.0f - reedWidth));
        reedScale = 32767.0f * 8.0f * (float)procGain / (pulseRms * sqrtf(pw));
        // CHAMBER boosts up to +24 dB: take back half of it (in dB) here, so
        // turning it up changes the tone more than the loudness and a
        // four-note chord at CHAMBER 8 still clears the int16 clamp after
        // the formant (simulated: peaks ~14k).
        reedScale *= powf(10.0f, -0.075f * (float)s_param[4]);
    }

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
        if (synthSel) mix = (int32_t)(s_synthBuf[n] * synthScale);

        for (int i = 0; i < ORGAN_VOICES && !synthSel; i++) {
            Voice& v = voices[i];
            if (v.stage == ENV_IDLE) continue;
            if (v.isSynth) {
                // Left over from SYNTH: its state means nothing to this sound.
                v.stage = ENV_IDLE; v.env = 0; v.sustained = 0;
                continue;
            }

            int32_t acc = 0;
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
            } else if (vocSel) {
                // VOCODER: one oscillator per note — the carrier (see vocOsc).
                // The pulse's width moves once per block: the WAVE width ± the
                // note's own slow triangle LFO × the PWM amount.
                if (n == 0) {
                    v.vocLfoPh += v.vocLfoInc;
                    int32_t tri = (int32_t)(v.vocLfoPh >> 15) - 65536;       // -65536..65535 ramp
                    if (tri < 0) tri = -tri;                                 // 0..65536 triangle
                    v.vocWidth = vocBaseW + vocPwmAmp * ((float)tri * (1.0f / 32768.0f) - 1.0f);
                }
                v.vocPhase += v.vocInc;
                // × 8 = one drawbar fully out; × the registry gain (4) = 32 units.
                acc = vocOsc(v.vocPhase, v.vocInc, vocPulse, v.vocWidth) * (8 * procGain);
            } else if (reedSel) {
                // REED: the ranks' increments once per block — the 8' pitch
                // (vocInc) × each rank's ratio × the speaking scoop.  A rank
                // at or past Nyquist is left silent.
                if (n == 0) {
                    if (v.glideQ16 < 32768 || v.glideQ16 > 65536) v.glideQ16 = 65536;
                    if (v.glideQ16 < 65536) {
                        v.glideQ16 += ((65536 - v.glideQ16) >> 2) + 1;
                        if (v.glideQ16 > 65536) v.glideQ16 = 65536;
                    }
                    float base = (float)v.vocInc * ((float)v.glideQ16 * (1.0f / 65536.0f));
                    for (int k = 0; k < reedStop->n; k++) {
                        float f = base * reedStop->ratio[k];
                        v.inc[k] = (f >= 2147483648.0f) ? 0 : (uint32_t)f;
                    }
                    float fc = base * reedCeleste;
                    v.inc[REED_MAX_RANKS] = (fc >= 2147483648.0f) ? 0 : (uint32_t)fc;
                }
                float sum = 0.0f;
                for (int k = 0; k < reedStop->n; k++) {
                    if (v.inc[k] == 0) continue;
                    v.phase[k] += v.inc[k];
                    sum += reedPulse(v.phase[k], v.inc[k], reedWidth) * reedStop->level[k];
                }
                if (v.inc[REED_MAX_RANKS]) {
                    v.phase[REED_MAX_RANKS] += v.inc[REED_MAX_RANKS];
                    sum += reedPulse(v.phase[REED_MAX_RANKS], v.inc[REED_MAX_RANKS], reedWidth);
                }
                acc = (int32_t)(sum * reedScale);
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
            // 64-bit: acc*env overflows int32.
            mix += (int32_t)(((int64_t)acc * v.env) >> 15);

            // Key click — a fast-decaying noise burst on attack, added outside
            // the tonal envelope so it isn't swallowed by the attack ramp.  Mono.
            if (v.clickEnv > 0) {
                v.rng = v.rng * 1664525u + 1013904223u;       // LCG noise
                int16_t noise = (int16_t)(v.rng >> 16);
                int32_t cn = (((int32_t)noise * v.clickEnv) >> 15) * s_param[0];   // CLICK knob
                mix += cn;
                v.clickEnv -= CLICK_DECAY;
                if (v.clickEnv < 0) v.clickEnv = 0;
            }
        }

        // The voices and the FX chain are mono; the Leslie and reverb make the
        // stereo at the output.
        int32_t s = mix >> (vocSel ? ORGAN_MASTER_SHIFT + VOC_CARRIER_SHIFT : ORGAN_MASTER_SHIFT);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        // PROC BRIGHT slider — global one-pole low-pass tone control — then the
        // THROAT formant (boost can exceed int16, so re-clamp before the FX
        // chain writes int16 delay buffers).
        if (proc && !vocSel && !synthSel) {            // (VOCODER's sliders mean other things; SYNTH has its own filter)
            int alpha = 16 + s_param[2] * 30;          // 16 (dark) .. 256 (open)
            s_procLp += ((s - s_procLp) * alpha) >> 8;
            s = procFormant(s_procLp, procSel, s_param[4]);
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
        }

        // VOCODER sound: shape the oscillators (carrier) with the mic (modulator)
        // so the held chord speaks/sings.  Placed before the spatial FX so reverb
        // washes over the voice.  Mic = LEFT channel of the full-duplex record
        // block.  Until the mic feed is in step (the first ~32 ms) it is silent
        // rather than letting the bare oscillators through.
        if (vocSel) s = vocOn ? vocoderProcess(s, s_micBlock[2 * n]) : 0;

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

// ── Vocoder mic feed: keeping the two buffers in step ─────────────────────────
// Mic and speaker run off one clock, 64 frames (2 ms) per DMA buffer, four
// buffers each way.  This task makes 128 frames per pass and is paced by the
// speaker side (the write blocks while its buffers are full).  How much mic
// audio is queued when a pass starts is then fixed by how the two sides
// happened to be filled when the vocoder came on — and both ways it used to
// come on were bad:
//   • vocoder switched on while the organ was already playing: the mic side
//     had been running unread, so it was FULL.  It stayed full — one pass 2 ms
//     late and 2 ms of mic audio was thrown away.
//   • organ started with the vocoder already on (a song's patch): the blocking
//     mic read paced the task instead, the speaker side never filled, and any
//     late pass was a gap in the output.
// So whenever the vocoder comes on (either way) it is primed: for
// VOC_PRIME_BLOCKS passes the mic is not read (the vocoder hears silence)
// while the speaker side fills and takes over the pacing; then the mic side
// is emptied once.  From there each pass finds exactly the two buffers that
// arrived during the previous one — about 4 ms of slack against a late pass on
// the output and 6 ms on the mic.
#define VOC_PRIME_BLOCKS  8       // 32 ms
#define VOC_READ_WAIT_MS  6       // normally the block is already there

// Render load, printed once a second while anything sounds: how long
// renderBlock takes against the block's 4 ms, the most voices at once, and
// "late" = passes where the codec write returned at once (its buffers had run
// dry: the output fell behind and glitched).
#define ORGAN_LOAD_LOG 1
static const uint32_t BLOCK_US = BLOCK_FRAMES * 1000000UL / AUDIO_CODEC_RATE_HZ;

static void organTaskFn(void*) {
    static int16_t block[BLOCK_FRAMES * 2];
#if ORGAN_LOAD_LOG
    uint32_t ldSum = 0, ldMax = 0, ldN = 0, ldLate = 0, ldSynth = 0;
    int      ldVoices = 0;
    uint32_t ldSince = millis();
#endif
    bool vocWas   = false;
    int  vocPrime = 0;
    for (;;) {
        if (!s_active) { vocWas = false; vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        drainEvents();

        // Full-duplex: the mic block is the vocoder's modulator for this pass.
        bool voc = (s_type == ORGAN_PROC && s_procSel == PROC_IDX_VOC);
        if (voc && !vocWas) {
            vocPrime = VOC_PRIME_BLOCKS;
            vocoderReset();              // stale filter state → a click (this task owns it)
        }
        vocWas = voc;
        s_vocBlock = voc && vocPrime == 0;
        if (voc) {
            if (vocPrime > 0) {
                if (--vocPrime == 0) {       // empty the mic side: start from "nothing queued"
                    while (audioCodecRecordSome((uint8_t*)s_micBlock, sizeof(s_micBlock), 0)
                           == sizeof(s_micBlock)) {}
                    s_vocBlock = true;       // in step from this pass on
                }
                memset(s_micBlock, 0, sizeof(s_micBlock));
            } else {
                size_t got = audioCodecRecordSome((uint8_t*)s_micBlock, sizeof(s_micBlock),
                                                  VOC_READ_WAIT_MS);
                if (got < sizeof(s_micBlock)) {
                    memset((uint8_t*)s_micBlock + got, 0, sizeof(s_micBlock) - got);
                }
            }
        }

#if ORGAN_LOAD_LOG
        int64_t t0 = esp_timer_get_time();
#endif
        renderBlock(block);
#if ORGAN_LOAD_LOG
        int64_t t1 = esp_timer_get_time();
#endif
        if (!audioCodecPlay((const uint8_t*)block, sizeof(block)))  // blocks → paces us
            vTaskDelay(1);                  // no codec / reconfiguring — don't spin
#if ORGAN_LOAD_LOG
        uint32_t us   = (uint32_t)(t1 - t0);
        uint32_t wait = (uint32_t)(esp_timer_get_time() - t1);
        ldSum += us; ldN++; ldSynth += s_synthUs;
        if (us > ldMax) ldMax = us;
        if (wait < 100) ldLate++;
        if (s_voiceCount > ldVoices) ldVoices = s_voiceCount;
        if ((uint32_t)(millis() - ldSince) >= 1000) {
            if (ldVoices > 0 || ldLate > 0)
                Serial.printf("[ORGAN] render avg %lu%% (synth %lu%%) max %lu%% of %lu us, voices up to %d, late %lu/%lu, type %d sound %d\n",
                              (unsigned long)(ldSum * 100 / (ldN * BLOCK_US)),
                              (unsigned long)(ldSynth * 100 / (ldN * BLOCK_US)),
                              (unsigned long)(ldMax * 100 / BLOCK_US), (unsigned long)BLOCK_US,
                              ldVoices, (unsigned long)ldLate, (unsigned long)ldN,
                              (int)s_type, (int)s_procSel);
            ldSum = ldMax = ldN = ldLate = ldSynth = 0;
            ldVoices = 0;
            ldSince = millis();
        }
#endif
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
    for (int k = 0; k < ORGAN_SYNTH_PARAMS; k++) s_synthParam[k] = ORGAN_SYNTH_DEFAULT[k];
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
    vocoderInit();
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
    if (i < 0 || i >= PROC_SOUND_COUNT || i == s_procSel) return;
    // Each sound keeps different state in its voices (REED its ranks, VOCODER
    // its carrier, CHOIR a wavetable), so a voice carried across would stop
    // making sense: release whatever is sounding.
    for (int v = 0; v < ORGAN_VOICES; v++)
        if (voices[v].stage != ENV_IDLE) voices[v].stage = ENV_RELEASE;
    s_procSel = i;   // applies to notes played next
}
int organGetProcSound() { return s_procSel; }

void organSetSynthParam(int idx, int value) {
    if (idx < 0 || idx >= ORGAN_SYNTH_PARAMS) return;
    s_synthParam[idx] = organSynthClamp(idx, value);   // read at the next block
}
int organGetSynthParam(int idx) {
    return (idx >= 0 && idx < ORGAN_SYNTH_PARAMS) ? s_synthParam[idx] : 0;
}

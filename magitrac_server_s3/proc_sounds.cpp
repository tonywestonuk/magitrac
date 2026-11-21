#include "proc_sounds.h"
#include "audio_codec.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

// ── Registry ──────────────────────────────────────────────────────────────────
// Order = the number songs store (see proc_sounds.h) — append, don't reorder.
// Slider meanings (CHOIR): BREATH = sustained breath-noise amount (live);
// ENSMBL = twin detune + per-osc vibrato depth (detune baked at note-on,
// vibrato live); BRIGHT = global LP (live); ATTACK = amp swell time 5..773 ms
// (next notes); AHH = vowel formant stack gain (live).
const ProcSoundInfo PROC_SOUNDS[PROC_SOUND_COUNT] = {
    // REED — reed organ (own render path in drawbar_organ.cpp; sliders in
    // proc_sounds.h).  gain 2: one voice at about the RMS of a full drawbar
    // registration — the level is normalised for pulse width and the number
    // of ranks, so WAVE and STOPS change the tone, not the loudness.  Not
    // ear-tuned yet.  18 ms attack (a reed takes a moment to speak), 40 ms
    // release.  CHAMBER 2 = the +6 dB the first version defaulted to.
    { "REED",       5, { "WAVE", "CELESTE", "BRIGHT", "STOPS", "CHAMBER" },
                       { 4, 2, 6, 1, 2 }, 2, 0xFF, 40, 18 },
    { "CHOIR",      5, { "BREATH", "ENSMBL", "BRIGHT", "ATTACK", "AHH" },
                       { 4, 4, 8, 4, 4 }, 25, 3, 250 },
    // VOCODER — mic-driven channel vocoder (own render path in
    // drawbar_organ.cpp; slider meanings in proc_sounds.h).  Defaults: a plain
    // sawtooth, no width LFO, the gate and smoothing where the bench had them.
    // gain 4: the oscillator at full = 32 drawbar-units, as a saw needs to be
    // to fill the vowel bands.  relMs 60: notes let go a little softer than
    // the organ's 12 ms.
    { "VOCODER",    5, { "WAVE", "PWM", "SSS", "GATE", "SMOOTH" },
                       { 0, 0, 4, 1, 3 }, 4, 0xFF, 60 },
    // SYNTH — subtractive synth (synth_voice.cpp); its own envelope, so the
    // envelope fields here are unused.  gain 3: one voice's saw about level
    // with an 888 registration.  Not ear-tuned yet.
    { "SYNTH",      0, { }, { }, 3, 0xFF, 0 },
    // 808 — drum machine (drum808.cpp); its own envelopes too.  gain 16:
    // its limiter's ±1 lands at -6 dBFS (a lone kick ~-10 dBFS, an 888
    // registration sits ~-20).  Not ear-tuned yet.
    { "808",        5, { "BD DEC", "BD TONE", "SNAPPY", "OH DEC", "TUNE" },
                       { 4, 4, 4, 4, 4 }, 16, 0xFF, 0 },
    // CR78 — CR-78 drum machine (cr78.cpp).  gain 16 as for the 808: its
    // kick peaks about where the 808's does.  Not ear-tuned yet.
    { "CR78",       5, { "BD DEC", "SNAPPY", "HH DEC", "CY DEC", "TUNE" },
                       { 4, 4, 4, 4, 4 }, 16, 0xFF, 0 },
};

// ── CHOIR — the Orchestron optical-disc choir ─────────────────────────────────
// Measured from the four Choir*.wav discs (F3/C4/C5).  The vowel lives in
// three pitch-independent formants (F1≈520 Hz, F2≈1050 Hz, singer's formant
// ≈2800 Hz) applied post-mix; the wavetable holds the residual after dividing
// those out of the C4 disc — a smooth ~-13 dB/oct rolloff — so at the design
// AHH value (slider 4) the C4 spectrum reconstructs what was measured.  The
// attack frame is the same voice, dark (fundamental-heavy): the upper
// harmonics bloom in over the ~300 ms morph while the amp swells.  What makes
// it a CHOIR rather than an organ stop is the ensemble smear: each of the two
// oscillators per voice carries its own randomly-rated (5..6.5 Hz) vibrato,
// so harmonics are wobbling bands, not lines — plus a sustained breath-noise
// floor (the discs' harmonic-to-interharmonic ratio is under 1 dB!).
// h1 baked at 0.75 = the BREATH-0 maximum; the live BREATH slider subtracts a
// phase-aligned fundamental from the voice table (0.0625/step, see s_chFund),
// so more breath = less body in one gesture: 0.75 → 0.5 (slider 4, the
// ear-approved sound) → 0.25 (slider 8, airy/hollow).  The measured h1 was
// 1.0 but the discs' PERCEIVED fundamentals are weak — the honest h1 read as
// "too strong".
#define CH_NH 24
#define CH_FUND_MAX  0.75f
#define CH_FUND_STEP 0.0625f
static const float CH_ATTACK[CH_NH] = {
    0.750f, 0.220f, 0.130f, 0.090f, 0.080f, 0.020f, 0.008f, 0.005f,
    0.004f, 0.005f, 0.004f, 0.004f, 0.005f, 0.005f, 0.003f, 0.002f,
    0.002f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f,
};
static const float CH_SUSTAIN[CH_NH] = {
    0.750f, 0.410f, 0.420f, 0.290f, 0.230f, 0.060f, 0.020f, 0.012f,
    0.008f, 0.010f, 0.008f, 0.008f, 0.010f, 0.010f, 0.005f, 0.003f,
    0.003f, 0.002f, 0.002f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f,
};

// One BREATH step of fundamental, in the same units and phase as the h1 baked
// into the CHOIR frames — subtracting br copies is an exact partial reduction.
static int16_t s_chFund[PROC_WT];

// Sustained breath floor: procSample's transient decay is refloored here every
// block, so the note-on puff settles into constant breath instead of dying.
// 16000: doubled from 8000 (ear-tuned — BREATH 4 now gives what 8 used to).
#define CH_BREATH_FLOOR 16000

// Transport wow/flutter — GLOBAL Q16 pitch offset shared by every voice (one
// disc motor).  Two cascaded ~1 Hz one-poles on white noise give the slow
// drift (±20 cents at the 95th percentile, matching the discs), one ~14 Hz
// pole gives the fast jitter (±5 cents).  Advanced once per block by
// procBlockTick; block rate = 32 kHz / 128 = 250 Hz.  NOTE: procBlockTick is
// not called from the render loop at present, so s_chWowQ16 stays 0.
static int32_t  s_chWowLp1 = 0, s_chWowLp2 = 0, s_chFlutLp = 0;
static uint32_t s_chWowRng = 0x9E3779B9u;
static int32_t  s_chWowQ16 = 0;

// CHOIR's ATTACK→SUSTAIN morph: blooms over ~300 ms (voices swelling in).
static const int32_t CH_MORPH_INC =
    (int32_t)(65536.0 * 128 /* BLOCK_FRAMES */ / (0.300 * AUDIO_CODEC_RATE_HZ));

int16_t* gProcTabA[PROC_SOUND_COUNT];   // PSRAM (procSoundsInit allocates)
int16_t* gProcTabS[PROC_SOUND_COUNT];

ProcFormantState gProcFormant[PROC_FORMANT_STAGES] = {};
uint8_t gProcFormantNstage[PROC_SOUND_COUNT];
float gProcFormantCoef[PROC_SOUND_COUNT][PROC_FORMANT_STAGES][9][5];

// RBJ peaking EQ; one coefficient set per slider value (dbStep dB per step)
// so the gain slider is live with zero per-block maths.  `knee`: double the
// slope below the midpoint (ear-tuned — slider 4 = a linear table's 8) and
// halve it above, so the top half extends rather than exploding (+36 dB peaks
// would just be clip fizz).
static void buildFormantStage(int sound, int stage,
                              float f0, float Q, float dbStep,
                              bool knee = false) {
    const float w0 = 2.0f * (float)M_PI * f0 / AUDIO_CODEC_RATE_HZ;
    const float alpha = sinf(w0) / (2.0f * Q);
    const float c = cosf(w0);
    for (int v = 1; v <= 8; v++) {
        float steps = knee ? ((v <= 4) ? 2.0f * v : 8.0f + (v - 4)) : (float)v;
        float A  = powf(10.0f, (dbStep * steps) / 40.0f);
        float a0 = 1.0f + alpha / A;
        float* co = gProcFormantCoef[sound][stage][v];
        co[0] = (1.0f + alpha * A) / a0;
        co[1] = (-2.0f * c) / a0;
        co[2] = (1.0f - alpha * A) / a0;
        co[3] = (-2.0f * c) / a0;
        co[4] = (1.0f - alpha / A) / a0;
    }
}

// Additive frame build with SHARED pseudo-random phases (fixed seed), so the
// morph between frames only moves amplitudes — no phase cancellation sweep —
// and the energy is spread instead of piling into a buzzy impulse.
// `scOut` (optional) reports the normalisation factor so callers can build
// companion tables in the same units (the CHOIR fundamental subtractor).
static void buildFramePair(int sound, const float* hA, const float* hS, int nh,
                           float* scOut = nullptr) {
    // Init-time only — everything transient PSRAM (8 KB of static floats here
    // used to sit in internal RAM forever for two boot-time calls).
    float* a = (float*)heap_caps_malloc(PROC_WT * sizeof(float), MALLOC_CAP_SPIRAM);
    float* s = (float*)heap_caps_malloc(PROC_WT * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!gProcTabA[sound])
        gProcTabA[sound] = (int16_t*)heap_caps_malloc(PROC_WT * sizeof(int16_t),
                                                      MALLOC_CAP_SPIRAM);
    if (!gProcTabS[sound])
        gProcTabS[sound] = (int16_t*)heap_caps_malloc(PROC_WT * sizeof(int16_t),
                                                      MALLOC_CAP_SPIRAM);
    if (!a || !s || !gProcTabA[sound] || !gProcTabS[sound]) {
        free(a); free(s);
        // Leave both tables null (never half-built garbage) — procBlockPrep
        // checks for that and renders the sound silent; everything else lives.
        free(gProcTabA[sound]); gProcTabA[sound] = nullptr;
        free(gProcTabS[sound]); gProcTabS[sound] = nullptr;
        return;
    }
    for (int i = 0; i < PROC_WT; i++) { a[i] = 0.0f; s[i] = 0.0f; }
    uint32_t seed = 0x2F6E2B1u;
    for (int h = 1; h <= nh; h++) {
        seed = seed * 1664525u + 1013904223u;
        float ph = (float)(seed >> 8) * (2.0f * (float)M_PI / 16777216.0f);
        for (int i = 0; i < PROC_WT; i++) {
            float w = sinf(2.0f * (float)M_PI * h * i / PROC_WT + ph);
            a[i] += hA[h - 1] * w;
            s[i] += hS[h - 1] * w;
        }
    }
    // Joint normalisation keeps the attack louder than the sustain, as measured.
    float pk = 0.0f;
    for (int i = 0; i < PROC_WT; i++) {
        if (fabsf(a[i]) > pk) pk = fabsf(a[i]);
        if (fabsf(s[i]) > pk) pk = fabsf(s[i]);
    }
    float sc = 32767.0f / pk;
    for (int i = 0; i < PROC_WT; i++) {
        gProcTabA[sound][i] = (int16_t)lrintf(a[i] * sc);
        gProcTabS[sound][i] = (int16_t)lrintf(s[i] * sc);
    }
    if (scOut) *scOut = sc;
    free(a);
    free(s);
}

void procSoundsInit() {
    float chSc = 0.0f;   // stays 0 if the frame alloc fails → s_chFund all zero
    buildFramePair(PROC_IDX_CHOIR, CH_ATTACK, CH_SUSTAIN, CH_NH, &chSc);
    {   // one BREATH step of h1, with the frames' h1 phase (first LCG draw)
        uint32_t seed = 0x2F6E2B1u * 1664525u + 1013904223u;
        float ph = (float)(seed >> 8) * (2.0f * (float)M_PI / 16777216.0f);
        for (int i = 0; i < PROC_WT; i++)
            s_chFund[i] = (int16_t)lrintf(
                CH_FUND_STEP * sinf(2.0f * (float)M_PI * i / PROC_WT + ph) * chSc);
    }
    gProcFormantNstage[PROC_IDX_CHOIR] = 3;
    buildFormantStage(PROC_IDX_CHOIR, 0,  590.0f, 2.0f, 2.25f, true);   // "ah" F1 (open-mouth)
    buildFormantStage(PROC_IDX_CHOIR, 1, 1050.0f, 3.0f, 2.00f, true);   // "ah" F2
    buildFormantStage(PROC_IDX_CHOIR, 2, 2800.0f, 3.0f, 2.00f, true);   // singer's formant
    gProcFormantNstage[PROC_IDX_VOC] = 0;                  // VOCODER: none (and bypassed)
    gProcFormantNstage[PROC_IDX_SYNTH] = 0;                // SYNTH: none (and bypassed)
    gProcFormantNstage[PROC_IDX_808] = 0;                  // 808: none (and bypassed)
    gProcFormantNstage[PROC_IDX_CR78] = 0;                 // CR78: none (and bypassed)
    gProcFormantNstage[PROC_IDX_REED] = 1;                 // REED: the chamber's nasal peak
    // REED: 1.4 kHz, Q 1.5, 3 dB a step (+24 dB at 8) — fitted to an oboe
    // recording, whose bore resonance sits there; CHAMBER 8 with WAVE 8,
    // BRIGHT 1 and STOPS 0 lands within ~5 dB of its harmonics on B4..A5.
    buildFormantStage(PROC_IDX_REED, 0, 1400.0f, 1.5f, 3.0f);
}

// uint32 LFO phase → triangle -32768..32767.
static inline int32_t triQ15(uint32_t ph) {
    uint32_t p = ph >> 16;                                   // 0..65535
    int32_t t = (p < 32768) ? (int32_t)p : (int32_t)(65536 - p);
    return t * 2 - 32768;
}

// CHOIR is the only sound on this path (REED and VOCODER render their own).
void procNoteOn(int sound, ProcVoiceState& ps, float f0, const volatile int* param) {
    (void)sound;
    const float nyq = AUDIO_CODEC_RATE_HZ * 0.5f;
    float fa, fb;
    {
        // CHOIR: symmetric ENSMBL detune (chord centre stays put) + one
        // random-rate vibrato LFO per oscillator, so no two of the up-to-24
        // oscillators in a chord wobble together.
        float cents = param[1] * 2.0f;
        fa = f0 * powf(2.0f, -cents / 2400.0f);
        fb = f0 * powf(2.0f,  cents / 2400.0f);
        ps.scoopQ16 = 65536;
        ps.rng = ps.rng * 1664525u + 1013904223u;  ps.lfoPh1 = ps.rng;
        ps.rng = ps.rng * 1664525u + 1013904223u;  ps.lfoPh2 = ps.rng;
        ps.rng = ps.rng * 1664525u + 1013904223u;
        float r1 = 5.0f + 1.5f * (float)(ps.rng >> 24) / 255.0f;   // 5..6.5 Hz
        ps.rng = ps.rng * 1664525u + 1013904223u;
        float r2 = 5.0f + 1.5f * (float)(ps.rng >> 24) / 255.0f;
        const float perBlock = 4294967296.0f * 128 /* BLOCK_FRAMES */ / AUDIO_CODEC_RATE_HZ;
        ps.lfoInc1 = (uint32_t)(r1 * perBlock);
        ps.lfoInc2 = (uint32_t)(r2 * perBlock);
        ps.lastBreath = -1;
    }
    ps.inc1base = (fa >= nyq) ? 0 : (uint32_t)((double)fa * 4294967296.0 / AUDIO_CODEC_RATE_HZ);
    ps.inc2base = (fb >= nyq) ? 0 : (uint32_t)((double)fb * 4294967296.0 / AUDIO_CODEC_RATE_HZ);
    ps.inc1 = (uint32_t)(((uint64_t)ps.inc1base * (uint32_t)ps.scoopQ16) >> 16);
    ps.inc2 = (uint32_t)(((uint64_t)ps.inc2base * (uint32_t)ps.scoopQ16) >> 16);
    ps.ph1 = ps.ph2 = 0;
    ps.morphQ16 = 0;
    ps.noiseEnv = 32767;
    ps.noiseLp  = 0;
    if (ps.rng == 0) ps.rng = 0x7E57u ^ (uint32_t)(f0 * 1000.0f);
}

void procBlockTick(int sound) {
    if (sound != PROC_IDX_CHOIR) return;
    s_chWowRng = s_chWowRng * 1664525u + 1013904223u;
    int32_t r = ((int32_t)((s_chWowRng >> 16) & 0xFFFF) - 32768) * 9000 / 32768;
    s_chWowLp1 += ((r - s_chWowLp1) * 6) >> 8;
    s_chWowLp2 += ((s_chWowLp1 - s_chWowLp2) * 6) >> 8;
    s_chWowRng = s_chWowRng * 1664525u + 1013904223u;
    int32_t r2 = ((int32_t)((s_chWowRng >> 16) & 0xFFFF) - 32768) * 350 / 32768;
    s_chFlutLp += ((r2 - s_chFlutLp) * 90) >> 8;
    s_chWowQ16 = s_chWowLp2 + s_chFlutLp;
}

void procBlockPrep(int sound, ProcVoiceState& ps, int16_t* scratch,
                   const volatile int* param) {
    // Frame tables never built (PSRAM alloc failed in buildFramePair): play a
    // silent wavetable rather than dereference null.
    if (!gProcTabA[sound] || !gProcTabS[sound]) {
        memset(scratch, 0, PROC_WT * sizeof(int16_t));
        return;
    }
    {
        // CHOIR ensemble: advance the per-osc vibrato LFOs (block rate) and
        // re-derive the increments; ~2^(c/1200) ≈ 1 + c·0.000578 for small c,
        // so depthQ16 = cents × 37.9.  ENSMBL slider → 0..28 cents peak.
        ps.lfoPh1 += ps.lfoInc1;
        ps.lfoPh2 += ps.lfoInc2;
        int32_t depthQ16 = param[1] * 133;               // 3.5 cents per step
        int32_t m1 = 65536 + s_chWowQ16 + ((triQ15(ps.lfoPh1) * depthQ16) >> 15);
        int32_t m2 = 65536 + s_chWowQ16 + ((triQ15(ps.lfoPh2) * depthQ16) >> 15);
        ps.inc1 = (uint32_t)(((uint64_t)ps.inc1base * (uint32_t)m1) >> 16);
        ps.inc2 = (uint32_t)(((uint64_t)ps.inc2base * (uint32_t)m2) >> 16);
        // Breath: refloor the noise envelope so the note-on puff settles into
        // sustained air instead of the pick transient's die-away.
        if (ps.noiseEnv < CH_BREATH_FLOOR) ps.noiseEnv = CH_BREATH_FLOOR;
        // BREATH also hollows the tone: subtract br fundamental steps from the
        // voice table (phase-aligned, exact), then fade the WHOLE tone on a
        // quadratic (Q8 256/224/128 at br 0/4/8 — barely touches the approved
        // low half, voice→whisper up top; the noise doesn't scale, so it
        // dominates as the tone recedes).  Rebuild while morphing, and again
        // whenever the slider moves after the morph has settled.
        int br = param[0];
        if (br < 0) br = 0;
        if (br > 8) br = 8;
        int32_t q = 256 - br * br * 2;
        const int16_t* A = gProcTabA[PROC_IDX_CHOIR];
        const int16_t* S = gProcTabS[PROC_IDX_CHOIR];
        int32_t m = ps.morphQ16;
        if (m > 65536) {
            if (br == ps.lastBreath) return;
            for (int i = 0; i < PROC_WT; i++)
                scratch[i] = (int16_t)(
                    (((int32_t)S[i] - br * s_chFund[i]) * q) >> 8);
        } else {
            ps.morphQ16 = (m >= 65536) ? 65537
                        : (m + CH_MORPH_INC > 65536) ? 65536 : m + CH_MORPH_INC;
            int32_t inv = 65536 - m;   // m == 65536 → pure S (the final copy)
            for (int i = 0; i < PROC_WT; i++)
                scratch[i] = (int16_t)(
                    (((((int32_t)A[i] * inv + (int32_t)S[i] * m) >> 16)
                      - br * s_chFund[i]) * q) >> 8);
        }
        ps.lastBreath = br;
    }
}

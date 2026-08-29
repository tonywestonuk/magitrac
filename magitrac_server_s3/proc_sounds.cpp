#include "proc_sounds.h"
#include "audio_codec.h"
#include <math.h>
#include <stdlib.h>
#include "esp_heap_caps.h"

// ── Registry ──────────────────────────────────────────────────────────────────
// Slider meanings (TWO TRIBES): ATTACK = pick-noise amount (live); CHORUS =
// twin-osc detune 0..17.6 cents (next notes); BRIGHT = global one-pole LP
// (live); SCOOP = per-note pitch bend-up depth 0..-120 cents (next notes);
// THROAT = fixed 900 Hz formant gain 0..+18 dB (live).
// Slider meanings (CHOIR): BREATH = sustained breath-noise amount (live);
// ENSMBL = twin detune + per-osc vibrato depth (detune baked at note-on,
// vibrato live); BRIGHT = global LP (live); ATTACK = amp swell time 5..773 ms
// (next notes); AHH = vowel formant stack gain (live).
const ProcSoundInfo PROC_SOUNDS[PROC_SOUND_COUNT] = {
    { "TWO TRIBES", 5, { "ATTACK", "CHORUS", "BRIGHT", "SCOOP", "THROAT" },
                       { 4, 4, 8, 4, 4 }, 40, 0xFF, 0 },
    { "CHOIR",      5, { "BREATH", "ENSMBL", "BRIGHT", "ATTACK", "AHH" },
                       { 4, 4, 8, 4, 4 }, 25, 3, 250 },
    // THX DEEP NOTE — the converging cluster (own render path; see thx* below).
    // envParam=0: SWEEP also sets the amp swell (5..773 ms) so a longer sweep
    // fades in more gently.  relMs 2500 = a long ring-out tail.
    // gain 128: the 16 random-phase saws partially cancel, so THX needs a much
    // higher gain than the coherent-wavetable sounds to match their loudness.
    // Peaks stay well clear of clip at 1-3 notes; raise further if still quiet.
    { "THX",        5, { "SWEEP", "SPREAD", "BRIGHT", "STACK", "DETUNE" },
                       { 5, 4, 6, 6, 3 }, 128, 0, 2500 },
};

// ── TWO TRIBES — the FGTH bass stab ───────────────────────────────────────────
// Harmonic frames measured from the record (one stab, f0 ≈ 74.4 Hz / D2):
// a dense bright attack — strong h1-h5, a formant bump around h12-13 (~900 Hz,
// the pick "knock") — decaying over ~80 ms into a dark, round sustain.  The
// chorus-twin oscillator supplies the heavy chorus doubling of the original.
// 24 harmonics ≈ bandlimited to 24·f0; above ~E5 the top of the table aliases,
// which a bass patch never reaches in anger.
#define TT_NH 24
static const float TT_ATTACK[TT_NH] = {
    0.977f, 0.921f, 0.830f, 0.897f, 1.000f, 0.365f, 0.398f, 0.079f,
    0.163f, 0.188f, 0.144f, 0.460f, 0.621f, 0.234f, 0.167f, 0.147f,
    0.085f, 0.278f, 0.128f, 0.096f, 0.251f, 0.154f, 0.179f, 0.095f,
};
static const float TT_SUSTAIN[TT_NH] = {
    1.154f, 0.678f, 0.503f, 0.366f, 0.179f, 0.186f, 0.084f, 0.066f,
    0.098f, 0.116f, 0.089f, 0.100f, 0.090f, 0.038f, 0.025f, 0.037f,
    0.022f, 0.042f, 0.049f, 0.023f, 0.014f, 0.010f, 0.012f, 0.028f,
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
// procBlockTick; block rate = 32 kHz / 128 = 250 Hz.
static int32_t  s_chWowLp1 = 0, s_chWowLp2 = 0, s_chFlutLp = 0;
static uint32_t s_chWowRng = 0x9E3779B9u;
static int32_t  s_chWowQ16 = 0;

// Per-sound morph rate: TWO TRIBES snaps ATTACK→SUSTAIN in ~80 ms (the pick
// decay); CHOIR blooms over ~300 ms (voices swelling in).
static const int32_t MORPH_INC[PROC_SOUND_COUNT] = {
    (int32_t)(65536.0 * 128 /* BLOCK_FRAMES */ / (0.080 * AUDIO_CODEC_RATE_HZ)),
    (int32_t)(65536.0 * 128 /* BLOCK_FRAMES */ / (0.300 * AUDIO_CODEC_RATE_HZ)),
    0,   // THX has its own render path — no wavetable morph
};

int16_t* gProcTabA[PROC_SOUND_COUNT];   // PSRAM (procSoundsInit allocates)
int16_t* gProcTabS[PROC_SOUND_COUNT];

// THX sawtooth: additive to ~24 harmonics (mildly band-limited — tames aliasing
// on the high octaves of the final chord without going dull).  Small internal
// table (2 KB), read by thxSample in the hot loop.
int16_t gThxSaw[PROC_WT];

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
        return;   // PROC sound stays silent; everything else lives
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
    buildFramePair(0, TT_ATTACK, TT_SUSTAIN, TT_NH);
    float chSc;
    buildFramePair(1, CH_ATTACK, CH_SUSTAIN, CH_NH, &chSc);
    {   // one BREATH step of h1, with the frames' h1 phase (first LCG draw)
        uint32_t seed = 0x2F6E2B1u * 1664525u + 1013904223u;
        float ph = (float)(seed >> 8) * (2.0f * (float)M_PI / 16777216.0f);
        for (int i = 0; i < PROC_WT; i++)
            s_chFund[i] = (int16_t)lrintf(
                CH_FUND_STEP * sinf(2.0f * (float)M_PI * i / PROC_WT + ph) * chSc);
    }
    gProcFormantNstage[0] = 1;
    buildFormantStage(0, 0,  900.0f, 2.2f, 2.25f);   // TT throat knock
    gProcFormantNstage[1] = 3;
    buildFormantStage(1, 0,  590.0f, 2.0f, 2.25f, true);   // "ah" F1 (open-mouth)
    buildFormantStage(1, 1, 1050.0f, 3.0f, 2.00f, true);   // "ah" F2
    buildFormantStage(1, 2, 2800.0f, 3.0f, 2.00f, true);   // singer's formant
    gProcFormantNstage[PROC_IDX_THX] = 0;                  // THX: no formant

    // THX sawtooth wavetable (sum of harmonics 1/h, mildly band-limited).
    {
        float peak = 0.0f;
        for (int i = 0; i < PROC_WT; i++) {
            float y = 0.0f;
            for (int h = 1; h <= 24; h++)
                y += sinf(2.0f * (float)M_PI * h * i / PROC_WT) / (float)h;
            gThxSaw[i] = (int16_t)lrintf(y * 1000.0f);   // temp scale
            float a = fabsf(y);
            if (a > peak) peak = a;
        }
        float sc = 30000.0f / (peak * 1000.0f);
        for (int i = 0; i < PROC_WT; i++)
            gThxSaw[i] = (int16_t)lrintf((float)gThxSaw[i] * sc);
    }
}

// ── THX DEEP NOTE ─────────────────────────────────────────────────────────────
static const int   THX_BLK_RATE   = AUDIO_CODEC_RATE_HZ / 128;  // ~250 Hz (blocks/s)
static const float THX_START_CTR  = 280.0f;   // centre of the initial "mumble" band

// Stereo-wander tunables (ear-tune).  SMOOTH = one-pole shift, higher = slower
// drift.  PHASE_MAX = biggest wandering L/R phase offset (Q32; 1<<29 ≈ ⅛ cycle).
static const int      THX_PAN_SMOOTH   = 7;          // pan position drift (~0.5 s)
static const int      THX_PHASE_SMOOTH = 6;          // width/phase drift (~0.25 s)
static const uint32_t THX_PHASE_MAX    = (1u << 30);

void thxNoteOn(ThxVoice& tv, float rootHz, const volatile int* param) {
    int sweep  = param[0];   // 0..8  convergence time
    int spread = param[1];   // 0..8  width/chaos of the initial cluster
    int stack  = param[3];   // 0..8  octave span of the final chord
    int detune = param[4];   // 0..8  per-osc shimmer in the final chord
    if (tv.rng == 0) tv.rng = 0xC0FFEEu ^ (uint32_t)(rootHz * 1000.0f);

    float sec  = 2.0f + sweep * 2.0f;                 // 2..18 s glide
    tv.glidePos = 0;
    tv.glideInc = (uint32_t)(65536.0f / (sec * (float)THX_BLK_RATE));
    if (tv.glideInc == 0) tv.glideInc = 1;

    float octaves = 3.0f + stack * 0.5f;              // 3..7 octaves span
    float halfW   = 60.0f + spread * 42.0f;           // ±60..±396 Hz start spread
    const double kInc = 4294967296.0 / AUDIO_CODEC_RATE_HZ;
    const float  nyq  = AUDIO_CODEC_RATE_HZ * 0.5f;

    for (int j = 0; j < THX_OSC; j++) {
        // Start: random absolute pitch — the iconic low mumble (note-independent).
        tv.rng = tv.rng * 1664525u + 1013904223u;
        float u  = (float)(tv.rng >> 8) / 16777216.0f;      // 0..1
        float fs = THX_START_CTR - halfW + u * (2.0f * halfW);
        if (fs < 30.0f) fs = 30.0f;

        // Target: EXACT stacked octaves on the root — every 3rd voice a perfect
        // fifth above its octave (the D/A "power" voicing), bottom octave kept
        // pure root for a solid bass.  These are the in-tune pitches the cluster
        // must land on.
        int   oct      = (int)((float)j * octaves / THX_OSC);   // 0..octaves-1
        float interval = (oct >= 1 && (j % 3) == 2) ? 1.5f : 1.0f;   // 1.5 = pure fifth
        // Chorus detune only (±DETUNE cents) — small enough that the settled
        // chord stays IN TUNE with a shimmer, not the old ±(4·DETUNE) that read
        // as out of tune.  DETUNE 0 = dead exact.
        tv.rng = tv.rng * 1664525u + 1013904223u;
        float dc = (((float)(tv.rng >> 8) / 16777216.0f) - 0.5f) * (detune * 2.0f);
        float ft = rootHz * powf(2.0f, (float)oct) * interval * powf(2.0f, dc / 1200.0f);
        while (ft >= nyq) ft *= 0.5f;

        tv.incStart[j]  = (uint32_t)((double)fs * kInc);
        tv.incTarget[j] = (uint32_t)((double)ft * kInc);
        tv.incCur[j]    = tv.incStart[j];
        tv.wob[j]       = 0;

        // UNIFORM glide rate (256 = 1.0): every oscillator follows the same eased
        // progress, so they all lock onto the chord at the SAME instant — the
        // satisfying "whoosh into place".  (A spread here smears the arrival; the
        // start pitches + wobble still keep the sweep itself alive.)
        tv.rateQ8[j] = 256;

        // Amplitude weight: bass octaves ~1.5×, top ~0.9×; scaled so the bank
        // sums to roughly one int16 (÷ THX_OSC baked in).  The live pan gains are
        // derived from this per block (thxBlockPrep) as each osc wanders.
        float w   = 1.5f - ((float)oct / octaves) * 0.6f;
        float amp = w * 224.0f / THX_OSC;
        tv.ampBaseQ8[j] = (uint16_t)amp;
        // Pan-wander WIDTH by octave: bass stays near centre (solid low end),
        // higher octaves swing wide → the immersive, swimming wall.
        float spread = (octaves > 1.0f) ? ((float)oct / (octaves - 1.0f)) : 0.0f;  // 0 bass..1 top
        tv.wanderWQ8[j] = (uint8_t)(64.0f + spread * 191.0f);        // 64..255 (Q8)
        tv.panWob[j] = 0; tv.phOffWob[j] = 0; tv.phOff[j] = 0;
        tv.ampLQ8[j] = (uint16_t)(amp * 0.70711f);                   // centred until 1st block
        tv.ampRQ8[j] = tv.ampLQ8[j];

        // Random start phase (no attack click from aligned saws).
        tv.rng = tv.rng * 1664525u + 1013904223u;
        tv.ph[j] = tv.rng;
    }
}

void thxBlockPrep(ThxVoice& tv) {
    if (tv.glidePos < 65536) {
        tv.glidePos += tv.glideInc;
        if (tv.glidePos > 65536) tv.glidePos = 65536;
    }
    uint32_t gp = tv.glidePos;
    for (int j = 0; j < THX_OSC; j++) {
        uint32_t p = (uint32_t)(((uint64_t)gp * tv.rateQ8[j]) >> 8);
        if (p > 65536) p = 65536;
        // Smoothstep 3p²−2p³ (Q16): slow wander at the start, then a graceful
        // deceleration INTO the target so the unified arrival settles rather
        // than snapping (p² alone arrives at full pitch-velocity → a kink).
        uint32_t p2 = (uint32_t)(((uint64_t)p * p) >> 16);
        uint32_t pc = (uint32_t)(((uint64_t)p2 * (uint32_t)(196608 - 2 * p)) >> 16);
        int64_t  d  = (int64_t)tv.incTarget[j] - (int64_t)tv.incStart[j];
        uint32_t inc = (uint32_t)((int64_t)tv.incStart[j] + ((d * (int64_t)pc) >> 16));

        // Smoothed per-osc detune wobble (one-pole random walk), fading out as the
        // voice converges → the "living" mumble that settles into a steady chord.
        tv.rng = tv.rng * 1664525u + 1013904223u;
        int32_t tgt = ((int32_t)((tv.rng >> 16) & 0xFFFF) - 32768) >> 4;   // ±2048
        tv.wob[j] += (tgt - tv.wob[j]) >> 4;                               // ~smoothed
        int32_t fade = 65536 - (int32_t)p;                                 // 0 when settled
        int32_t cQ16 = ((int64_t)tv.wob[j] * fade) >> 16;                  // Q16 mult factor
        tv.incCur[j] = (uint32_t)(((uint64_t)inc * (uint32_t)(65536 + cQ16)) >> 16);

        // ── Stereo wander (continuous — keeps the sustained chord swimming) ──
        // Pan POSITION: smoothed random walk → recompute constant-power gains.
        tv.rng = tv.rng * 1664525u + 1013904223u;
        int32_t pt = (int32_t)((tv.rng >> 16) & 0xFFFF) - 32768;           // ±32768
        tv.panWob[j] += (pt - tv.panWob[j]) >> THX_PAN_SMOOTH;
        float pan = 0.5f + (tv.panWob[j] * (1.0f / 32768.0f)) * 0.5f
                          * (tv.wanderWQ8[j] * (1.0f / 256.0f));
        if (pan < 0.0f) pan = 0.0f; else if (pan > 1.0f) pan = 1.0f;
        float th  = pan * 1.5707963f;
        float amp = (float)tv.ampBaseQ8[j];
        tv.ampLQ8[j] = (uint16_t)(amp * cosf(th));
        tv.ampRQ8[j] = (uint16_t)(amp * sinf(th));

        // WIDTH: wandering inter-channel phase offset (the "wide" — decorrelates
        // L/R per osc).  Signed → negative wraps modulo 2³² (= reading earlier).
        tv.rng = tv.rng * 1664525u + 1013904223u;
        int32_t ot = (int32_t)((tv.rng >> 16) & 0xFFFF) - 32768;
        tv.phOffWob[j] += (ot - tv.phOffWob[j]) >> THX_PHASE_SMOOTH;
        tv.phOff[j] = (uint32_t)(int32_t)(((int64_t)tv.phOffWob[j] * (int64_t)THX_PHASE_MAX) >> 15);
    }
}

// uint32 LFO phase → triangle -32768..32767.
static inline int32_t triQ15(uint32_t ph) {
    uint32_t p = ph >> 16;                                   // 0..65535
    int32_t t = (p < 32768) ? (int32_t)p : (int32_t)(65536 - p);
    return t * 2 - 32768;
}

void procNoteOn(int sound, ProcVoiceState& ps, float f0, const volatile int* param) {
    const float nyq = AUDIO_CODEC_RATE_HZ * 0.5f;
    float fa = f0, fb;
    if (sound == 1) {
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
    } else {
        float cents = param[1] * 2.2f;      // CHORUS 0..8 → 0..17.6 cents detune
        fb = f0 * powf(2.0f, cents / 1200.0f);
        // SCOOP 0..8 → start 0..-120 cents flat, bending up to pitch over ~50 ms
        // (the played-with-a-slide feel of the record).
        float scCents = -15.0f * param[3];
        ps.scoopQ16 = (param[3] > 0)
                    ? (int32_t)(powf(2.0f, scCents / 1200.0f) * 65536.0f) : 65536;
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
    if (sound != 1) return;
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
    if (sound == 1) {
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
        const int16_t* A = gProcTabA[1];
        const int16_t* S = gProcTabS[1];
        int32_t m = ps.morphQ16;
        if (m > 65536) {
            if (br == ps.lastBreath) return;
            for (int i = 0; i < PROC_WT; i++)
                scratch[i] = (int16_t)(
                    (((int32_t)S[i] - br * s_chFund[i]) * q) >> 8);
        } else {
            ps.morphQ16 = (m >= 65536) ? 65537
                        : (m + MORPH_INC[1] > 65536) ? 65536 : m + MORPH_INC[1];
            int32_t inv = 65536 - m;   // m == 65536 → pure S (the final copy)
            for (int i = 0; i < PROC_WT; i++)
                scratch[i] = (int16_t)(
                    (((((int32_t)A[i] * inv + (int32_t)S[i] * m) >> 16)
                      - br * s_chFund[i]) * q) >> 8);
        }
        ps.lastBreath = br;
        return;
    }
    if (ps.scoopQ16 < 65536) {
        // Advance the pitch scoop toward settled (~50 ms; 7/32 per 4 ms block).
        ps.scoopQ16 += ((65536 - ps.scoopQ16) * 7) >> 5;
        ps.scoopQ16 += 1;
        if (ps.scoopQ16 >= 65536) {
            ps.scoopQ16 = 65536;
            ps.inc1 = ps.inc1base;
            ps.inc2 = ps.inc2base;
        } else {
            ps.inc1 = (uint32_t)(((uint64_t)ps.inc1base * (uint32_t)ps.scoopQ16) >> 16);
            ps.inc2 = (uint32_t)(((uint64_t)ps.inc2base * (uint32_t)ps.scoopQ16) >> 16);
        }
    }
    int32_t m = ps.morphQ16;
    if (m > 65536) return;                   // settled — scratch already holds S
    const int16_t* A = gProcTabA[sound];
    const int16_t* S = gProcTabS[sound];
    if (m >= 65536) {                        // final copy, then skip forever
        for (int i = 0; i < PROC_WT; i++) scratch[i] = S[i];
        ps.morphQ16 = 65537;
        return;
    }
    ps.morphQ16 = (m + MORPH_INC[sound] > 65536) ? 65536 : m + MORPH_INC[sound];
    int32_t inv = 65536 - m;
    for (int i = 0; i < PROC_WT; i++)
        scratch[i] = (int16_t)(((int32_t)A[i] * inv + (int32_t)S[i] * m) >> 16);
}

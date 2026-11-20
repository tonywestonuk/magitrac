#include "synth_voice.h"
#include "OrganSynth.h"
#include <math.h>

// The core builds for size (-Os); this file is the organ's hot loop.
#pragma GCC optimize("O2")

static const float PHASE_TO_T = 1.0f / 4294967296.0f;
static const float ENV_DONE   = 0.001f;    // -60 dB: the release has died away (under any mix)
static const float ATK_TARGET = 1.3f;      // attack heads past 1 so it arrives in finite time

// Filter coefficient g = tan(pi fc / fs) by cutoff in octaves (log2 Hz), from
// 20 Hz to 0.45 fs, linearly interpolated: the per-voice control step does no
// tan or exp.  ~0.07 octave a step; rebuilt if fs changes.
#define G_TAB_N 129
static float s_gTab[G_TAB_N];
static float s_gFs = 0.0f, s_gOctMin = 0.0f, s_gOctMax = 0.0f, s_gScale = 0.0f;

static void buildGTable(float fs) {
    s_gFs     = fs;
    s_gOctMin = log2f(20.0f);
    s_gOctMax = log2f(fs * 0.45f);
    s_gScale  = (float)(G_TAB_N - 1) / (s_gOctMax - s_gOctMin);
    for (int i = 0; i < G_TAB_N; i++) {
        float oct = s_gOctMin + (float)i / s_gScale;
        s_gTab[i] = tanf(3.14159265f * exp2f(oct) / fs);
    }
}

static inline float gAt(float oct) {
    if (oct <= s_gOctMin) return s_gTab[0];
    if (oct >= s_gOctMax) return s_gTab[G_TAB_N - 1];
    float x  = (oct - s_gOctMin) * s_gScale;
    int   i  = (int)x;
    float fr = x - (float)i;
    return s_gTab[i] + (s_gTab[i + 1] - s_gTab[i]) * fr;
}

// Per-sample coefficient for an exponential segment with time constant tauMs.
static float segCoef(float tauMs, float fs) {
    float n = tauMs * fs * 0.001f;
    return (n < 1.0f) ? 1.0f : 1.0f - expf(-1.0f / n);
}

void synthPrepare(SynthBlock& b, const uint8_t* p, float fs) {
    if (fs != s_gFs) buildGTable(fs);
    b.fs     = fs;
    b.wave1  = p[SYN_OSC1] <= SYN_WAVE_SINE ? p[SYN_OSC1] : SYN_WAVE_SAW;
    b.wave2  = p[SYN_OSC2] <= SYN_WAVE_NOISE ? p[SYN_OSC2] : SYN_WAVE_SAW;
    b.mix2   = (float)(p[SYN_MIX] > 16 ? 16 : p[SYN_MIX]) / 16.0f;
    b.mix1   = 1.0f - b.mix2;
    b.sub    = (float)(p[SYN_SUB] > 16 ? 16 : p[SYN_SUB]) / 16.0f;
    b.width  = synthPulseWidth(p[SYN_PW] > 16 ? 16 : p[SYN_PW]);
    int semis = (int)(p[SYN_PITCH] > 24 ? 24 : p[SYN_PITCH]) - 12;
    b.ratio2 = powf(2.0f, ((float)semis * 100.0f + synthDetuneCents(p[SYN_DETUNE])) / 1200.0f);
    b.cutoffOct = log2f(synthCutoffHz(p[SYN_CUTOFF] > 16 ? 16 : p[SYN_CUTOFF]));
    b.envOct    = synthEnvOctaves(p[SYN_ENV] > 16 ? 16 : p[SYN_ENV]);
    b.damp      = synthResoDamp(p[SYN_RESO] > 16 ? 16 : p[SYN_RESO]);
    // Attack: time to reach full level (1.47 time constants toward 1.3).
    // Decay and release: time to fall about 40 dB (4.6 time constants).
    b.kAtk = segCoef(synthAttackMs(p[SYN_ATTACK]) / 1.466f, fs);
    b.kDec = segCoef(synthDecayMs(p[SYN_DECAY])   / 4.6f,  fs);
    b.kRel = segCoef(synthReleaseMs(p[SYN_RELEASE]) / 4.6f, fs);
    b.sus  = (float)(p[SYN_SUSTAIN] > 16 ? 16 : p[SYN_SUSTAIN]) / 16.0f;
}

void synthNoteOn(SynthVoice& v, float hz, uint8_t vel, float fromEnv, float fs) {
    double inc = (double)hz * 4294967296.0 / fs;
    v.inc1  = (inc >= 2147483648.0) ? 0 : (uint32_t)inc;
    if (v.rng == 0) v.rng = 0x6D2B79F5u ^ (uint32_t)(hz * 977.0f);
    v.rng   = v.rng * 1664525u + 1013904223u;
    v.ph1   = v.rng;                     // scattered starts: a chord doesn't land as one spike
    v.rng   = v.rng * 1664525u + 1013904223u;
    v.ph2   = v.rng;
    v.phSub = v.ph1 >> 1;
    // Cutoff follows half the keyboard: an octave up the keys = half an
    // octave up the filter, centred on middle C.
    v.keyOct = 0.5f * log2f(hz * (1.0f / 261.63f));
    v.vel   = 0.35f + 0.65f * (float)(vel > 127 ? 127 : vel) / 127.0f;
    if (!(fromEnv > 0.0f && fromEnv <= 1.0f)) {
        fromEnv = 0.0f;
        v.s1 = v.s2 = 0.0f;
    }
    v.env   = fromEnv;
    v.stage = SYN_ST_ATTACK;
}

// The correction that rounds off a step (polyBLEP), so high notes don't alias.
static inline __attribute__((always_inline)) float blep(float t, float dt) {
    if (t < dt)        { float x = t / dt;          return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}

static inline __attribute__((always_inline))
float osc(uint8_t wave, uint32_t ph, float dt, float width, uint32_t& rng) {
    float t = (float)ph * PHASE_TO_T;
    switch (wave) {
        case SYN_WAVE_SQUARE: {
            float t2 = t - width; if (t2 < 0.0f) t2 += 1.0f;
            float s  = ((t < width) ? 1.0f : -1.0f) + blep(t, dt) - blep(t2, dt);
            return s - (2.0f * width - 1.0f);
        }
        case SYN_WAVE_TRI:
            return 4.0f * fabsf(t - 0.5f) - 1.0f;
        case SYN_WAVE_SINE: {
            float x = 2.0f * t - 1.0f;                 // fast sine, ~0.1% error
            float y = 4.0f * x * (1.0f - fabsf(x));
            return 0.225f * (y * fabsf(y) - y) + y;
        }
        case SYN_WAVE_NOISE:
            rng = rng * 1664525u + 1013904223u;
            return (float)(int32_t)rng * (1.0f / 2147483648.0f);
        default:
            return 1.0f - 2.0f * t + blep(t, dt);
    }
}

void synthRender(SynthVoice& v, const SynthBlock& b, bool gate, float* out, int n) {
    if (v.stage == SYN_ST_DONE) return;
    if (!gate && v.stage != SYN_ST_RELEASE) v.stage = SYN_ST_RELEASE;

    // Everything the loop touches, in locals.
    uint32_t ph1 = v.ph1, ph2 = v.ph2, phSub = v.phSub, rng = v.rng;
    uint32_t inc1 = v.inc1;
    float    f2   = (float)inc1 * b.ratio2;
    uint32_t inc2 = (f2 >= 2147483648.0f) ? 0 : (uint32_t)f2;
    uint32_t incS = inc1 >> 1;
    float dt1 = (float)inc1 * PHASE_TO_T, dt2 = (float)inc2 * PHASE_TO_T, dtS = (float)incS * PHASE_TO_T;
    float env = v.env, s1 = v.s1, s2 = v.s2, gain = v.vel;
    uint8_t stage = v.stage;
    const uint8_t w1 = b.wave1, w2 = b.wave2;
    const float mix1 = b.mix1, mix2 = b.mix2, sub = b.sub, width = b.width;
    const bool on1  = inc1 && mix1 > 0.0f;
    const bool onS  = inc1 && sub > 0.0f;
    const bool on2  = mix2 > 0.0f && (inc2 || w2 == SYN_WAVE_NOISE);

    for (int base = 0; base < n; base += SYNTH_CTRL) {
        // Control step: the filter, from the envelope's level now.
        float g  = gAt(b.cutoffOct + v.keyOct + b.envOct * env);
        float a1 = 1.0f / (1.0f + g * (g + b.damp));
        float a2 = g * a1, a3 = g * a2;
        int end = base + SYNTH_CTRL < n ? base + SYNTH_CTRL : n;

        for (int i = base; i < end; i++) {
            if (stage == SYN_ST_ATTACK) {
                env += (ATK_TARGET - env) * b.kAtk;
                if (env >= 1.0f) { env = 1.0f; stage = SYN_ST_DECAY; }
            } else if (stage == SYN_ST_DECAY) {
                env += (b.sus - env) * b.kDec;
            } else {
                env -= env * b.kRel;
                if (env < ENV_DONE) { env = 0.0f; stage = SYN_ST_DONE; break; }
            }

            float x = 0.0f;
            if (on1) { ph1 += inc1; x += mix1 * osc(w1, ph1, dt1, width, rng); }
            if (onS) {
                phSub += incS;
                float t  = (float)phSub * PHASE_TO_T;
                float t2 = t - 0.5f; if (t2 < 0.0f) t2 += 1.0f;
                x += sub * (((t < 0.5f) ? 1.0f : -1.0f) + blep(t, dtS) - blep(t2, dtS));
            }
            if (on2) { ph2 += inc2; x += mix2 * osc(w2, ph2, dt2, width, rng); }

            // Zero-delay-feedback state-variable filter, low-pass output.
            float v3 = x - s2;
            float o1 = a1 * s1 + a2 * v3;
            float o2 = s2 + a2 * s1 + a3 * v3;
            s1 = 2.0f * o1 - s1;
            s2 = 2.0f * o2 - s2;

            out[i] += o2 * env * gain;
        }
        if (stage == SYN_ST_DONE) break;
    }

    v.ph1 = ph1; v.ph2 = ph2; v.phSub = phSub; v.rng = rng;
    v.env = env; v.s1 = s1; v.s2 = s2; v.stage = stage;
}

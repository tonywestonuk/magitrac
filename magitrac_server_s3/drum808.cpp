#include "drum808.h"
#include "OrganSynth.h"
#include <math.h>
#include <string.h>

// The core builds for size (-Os); this file is the organ's hot loop.
#pragma GCC optimize("O2")

// ── GM drum map → 808 drum ────────────────────────────────────────────────────
// 35..82.  Rides and crashes all play the cymbal; bongos the two higher
// congas; tambourine, cabasa and shaker the maracas.
static const int8_t GM_MAP[82 - 35 + 1] = {
    DR_BD, DR_BD, DR_RS, DR_SD, DR_CP, DR_SD, DR_LT, DR_CH,   // 35 kick 2 .. 42 closed hat
    DR_LT, DR_CH, DR_MT, DR_OH, DR_MT, DR_HT, DR_CY, DR_HT,   // 43 .. 50 high tom
    DR_CY, DR_CY, DR_CY, DR_MA, DR_CY, DR_CB, DR_CY, -1,      // 51 ride .. 58 vibraslap
    DR_CY, DR_HC, DR_MC, DR_HC, DR_MC, DR_LC, -1,    -1,      // 59 ride 2 .. 66 low timbale
    -1,    -1,    DR_MA, DR_MA, -1,    -1,    -1,    -1,      // 67 agogo .. 74 long guiro
    DR_CL, -1,    -1,    -1,    -1,    -1,    -1,    -1,      // 75 claves .. 82 shaker
};

int drumInstForNote(uint8_t note) {
    if (note == 82) return DR_MA;
    if (note < 35 || note > 82) return -1;
    return GM_MAP[note - 35];
}

// ── Building blocks ───────────────────────────────────────────────────────────
// io-808's levels: equalPower(75) = 0.92 for most drums; the kick adds 0.4
// after its soft clip; the toms and congas use equalPower(75 / 4).
static const float LVL      = 0.92f;
static const float LVL_KICK = 1.32f;
static const float LVL_TOM  = 0.29f;

// Web Audio's low/high-pass Q is a peak in dB (1 dB here); band-pass Q is a Q.
static const float Q_LPHP = 1.122f;

// Zero-delay-feedback state-variable filter (Cytomic): one structure, three
// outputs, matching a bilinear biquad.  z = 2 floats of state.
static DrumSvf svfCoef(float fc, float q, float fs) {
    if (fc > 0.45f * fs) fc = 0.45f * fs;
    float g = tanf((float)M_PI * fc / fs);
    DrumSvf c;
    c.k  = 1.0f / q;
    c.a1 = 1.0f / (1.0f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}
#define SVF_STEP(c, z, v0)                                   \
    float v3 = (v0) - z[1];                                  \
    float v1 = c.a1 * z[0] + c.a2 * v3;                      \
    float v2 = z[1] + c.a2 * z[0] + c.a3 * v3;               \
    z[0] = 2.0f * v1 - z[0];                                 \
    z[1] = 2.0f * v2 - z[1];
static inline float svfLp(const DrumSvf& c, float* z, float x) { SVF_STEP(c, z, x) return v2; }
static inline float svfBp(const DrumSvf& c, float* z, float x) { SVF_STEP(c, z, x) return c.k * v1; }   // 0 dB peak
static inline float svfHp(const DrumSvf& c, float* z, float x) { SVF_STEP(c, z, x) return x - c.k * v1 - v2; }

// sin(2πx), x in [0, 1): folded to ±π/2, 7th-order series (error < 2e-4).
static inline float sin2pi(float x) {
    float y = (x < 0.25f) ? x : (x < 0.75f) ? 0.5f - x : x - 1.0f;
    float z = 6.2831853f * y, z2 = z * z;
    return z * (1.0f - z2 * (1.0f / 6.0f - z2 * (1.0f / 120.0f - z2 * (1.0f / 5040.0f))));
}
// Web Audio's triangle: 0 at the start of the cycle, rising.
static inline float tri(float x) {
    return (x < 0.25f) ? 4.0f * x : (x < 0.75f) ? 2.0f - 4.0f * x : 4.0f * x - 4.0f;
}
static inline float blep(float t, float dt) {
    if (t < dt)        { float x = t / dt;          return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}
// Band-limited square, ±1.
static inline float square(float t, float dt) {
    float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
    return ((t < 0.5f) ? 1.0f : -1.0f) + blep(t, dt) - blep(t2, dt);
}
static inline float wrap(float p) { return (p >= 1.0f) ? p - 1.0f : p; }
// tanh, close enough for a soft clip (exact at 0, ±1 from |x| = 3).
static inline float softClip(float x) {
    if (x >  3.0f) return  1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// io-808's ADGenerator, LINEAR: 0 → amt over a, back to 0 over d (samples).
// Reciprocals made once per block: the S3's FPU has no divide.
struct Lin { float a, ia, id, amt; };
static inline Lin mkLin(float a, float d, float amt) { return { a, 1.0f / a, 1.0f / d, amt }; }
static inline float lin(uint32_t t, const Lin& L) {
    float ft = (float)t;
    if (ft < L.a) return L.amt * ft * L.ia;
    float r = 1.0f - (ft - L.a) * L.id;
    return (r > 0.0f) ? L.amt * r : 0.0f;
}
// EXPONENTIAL: linear up over a, then × k a sample (to 0.0001 at the end of
// the decay).  e = the voice's state.
static inline float expAD(uint32_t t, float a, float amt, float k, float& e) {
    if ((float)t < a) e = amt * (float)t / a;
    else if ((float)t < a + 1.0f) e = amt;
    else e *= k;
    return e;
}
static inline float msS(const DrumShared& d, float ms) { return ms * d.fs * 0.001f; }
static inline float expK(float amt, float decS) { return powf(0.0001f / amt, 1.0f / decS); }

// The 1 ms pulse io-808 adds to the kick and toms, smoothed by a 5 kHz
// low-pass: the voice runs its own copy of the filter (z = 2 floats).
static inline float click(const DrumShared& d, uint32_t t, float* z) {
    return svfLp(d.lp5k, z, (t < d.clickN) ? 1.0f : 0.0f);
}

// Toms and congas: [high, low] pitch, oscillator and noise decay (ms).
struct TomCfg { float hi, lo, oscMs, noiseMs; bool noise; };
static const TomCfg TOMS[6] = {
    { 100, 80,  200, 200, true  },   // DR_LT
    { 160, 120, 130, 155, true  },   // DR_MT
    { 220, 165, 200, 125, true  },   // DR_HT
    { 220, 165, 180, 200, false },   // DR_LC
    { 310, 250, 100, 155, false },   // DR_MC
    { 455, 370, 180, 125, false },   // DR_HC
};

static const float METAL_HZ[6] = { 263, 400, 421, 474, 587, 845 };
static const float COW_HZ[2]   = { 800, 540 };
static const float CY_DEC_MS[3] = { 1125, 400, 150 };   // low band (decay 50), mid, high
static const float CY_AMT       = 0.333f;               // each band at tone 50

void drumInit(DrumShared& d, float fs, float* work) {
    memset(&d, 0, sizeof(d));
    d.fs = fs;
    d.invFs = 1.0f / fs;
    d.clickN = (uint32_t)(fs * 0.001f);
    d.bp10k  = svfCoef(10000, 1, fs);
    d.bp5k   = svfCoef(5000, 1, fs);
    d.hp10k  = svfCoef(10000, Q_LPHP, fs);
    d.hp8k   = svfCoef(8000, Q_LPHP, fs);
    d.hp5k   = svfCoef(5000, Q_LPHP, fs);
    d.bp1k   = svfCoef(1000, 1, fs);
    d.lp10k  = svfCoef(10000, Q_LPHP, fs);
    d.lp5k   = svfCoef(5000, Q_LPHP, fs);
    d.bp2640 = svfCoef(2640, 1, fs);
    d.bp480  = svfCoef(480, 1, fs);
    d.hp480  = svfCoef(480, Q_LPHP, fs);
    d.hp2450 = svfCoef(2450, Q_LPHP, fs);
    d.bp2450 = svfCoef(2450, 1, fs);
    d.snHp   = svfCoef(5800, Q_LPHP, fs);   // io-808's snare TONE at 50
    for (int i = 0; i < 3; i++) d.cyK[i] = expK(CY_AMT, msS(d, CY_DEC_MS[i]));
    d.cbK = expK(0.125f, msS(d, 400));
    d.clK = expK(0.7f, msS(d, 40));
    for (int i = 0; i < 6; i++) d.metalInc[i] = METAL_HZ[i] / fs;
    for (int i = 0; i < 2; i++) d.cowInc[i]   = COW_HZ[i] / fs;
    d.rng = 0x808u;
    d.metal10 = work;
    d.metal5  = work + DRUM_BLOCK_MAX;
    d.snare   = work + 2 * DRUM_BLOCK_MAX;
    d.clap    = work + 3 * DRUM_BLOCK_MAX;
    d.mar     = work + 4 * DRUM_BLOCK_MAX;
    d.tom     = work + 5 * DRUM_BLOCK_MAX;
    d.cow     = work + 6 * DRUM_BLOCK_MAX;
    memset(work, 0, DRUM_WORK_FLOATS * sizeof(float));
    d.bdTone = d.sdTone = d.cyDec = -1;
    const int def[1] = { 4 };
    drumPrepare(d, def, 0, 0, 0);
}

#define DR_BIT(i) (1u << (i))
static const uint32_t USE_METAL10 = DR_BIT(DR_CY) | DR_BIT(DR_OH) | DR_BIT(DR_CH);
static const uint32_t USE_TOMNZ   = DR_BIT(DR_LT) | DR_BIT(DR_MT) | DR_BIT(DR_HT);

void drumPrepare(DrumShared& d, const int* sl, uint32_t mask, int n, int count) {
    int v[ORGAN_DRUM_PARAMS];
    for (int i = 0; i < ORGAN_DRUM_PARAMS; i++)
        v[i] = (i >= count) ? 4 : (sl[i] < 0) ? 0 : (sl[i] > 8) ? 8 : sl[i];
    d.bdDecMs = drum808KickDecayMs(v[0]);
    if (v[1] != d.bdTone) { d.bdTone = v[1]; d.bdLp = svfCoef(drum808KickToneHz(v[1]), Q_LPHP, d.fs); }
    d.snappy  = drum808Snappy(v[2]);
    d.ohDecMs = drum808OpenHatMs(v[3]);
    d.tune    = drum808Tune(v[4]);
    if (v[5] != d.sdTone) { d.sdTone = v[5]; d.snHp = svfCoef(drum808SnareToneHz(v[5]), Q_LPHP, d.fs); }
    d.cyTone  = drum808CymbalTone(v[6]);
    if (v[7] != d.cyDec) {
        d.cyDec   = v[7];
        d.cyScale = drum808CymbalDecay(v[7]);
        for (int i = 0; i < 3; i++) d.cyK[i] = expK(CY_AMT, msS(d, CY_DEC_MS[i] * d.cyScale));
    }
    d.accent  = drum808Accent(v[8]);
    for (int i = 0; i < 5; i++) d.lvl[i] = drumLevelGain(v[9 + i]);

    // A path nobody is listening to stops, and starts again from rest.
    bool m10 = mask & USE_METAL10, m5 = mask & DR_BIT(DR_CY);
    bool sn = mask & DR_BIT(DR_SD), cp = mask & DR_BIT(DR_CP), ma = mask & DR_BIT(DR_MA);
    bool tm = mask & USE_TOMNZ, cb = mask & DR_BIT(DR_CB);
    if (!m10) memset(d.zM10, 0, sizeof(d.zM10));
    if (!m5)  memset(d.zM5, 0, sizeof(d.zM5));
    if (!sn)  memset(d.zSn, 0, sizeof(d.zSn));
    if (!cp)  memset(d.zCp, 0, sizeof(d.zCp));
    if (!ma)  memset(d.zMa, 0, sizeof(d.zMa));
    if (!tm) { memset(d.zTm, 0, sizeof(d.zTm)); memset(d.pink, 0, sizeof(d.pink)); }
    bool noise = sn || cp || ma || tm;

    for (int i = 0; i < n; i++) {
        if (m10 || m5) {
            float m = 0.0f;
            for (int k = 0; k < 6; k++) {
                m += square(d.metalPh[k], d.metalInc[k]);
                d.metalPh[k] = wrap(d.metalPh[k] + d.metalInc[k]);
            }
            m *= 0.3f;
            if (m10) d.metal10[i] = svfBp(d.bp10k, d.zM10, m);
            if (m5)  d.metal5[i]  = svfBp(d.bp5k, d.zM5, m);
        }
        if (cb) {
            float c = square(d.cowPh[0], d.cowInc[0]) + square(d.cowPh[1], d.cowInc[1]);
            d.cowPh[0] = wrap(d.cowPh[0] + d.cowInc[0]);
            d.cowPh[1] = wrap(d.cowPh[1] + d.cowInc[1]);
            d.cow[i] = c;
        }
        if (noise) {
            d.rng = d.rng * 1664525u + 1013904223u;
            float w = (float)(int32_t)d.rng * (1.0f / 2147483648.0f);
            if (sn) d.snare[i] = svfHp(d.snHp, d.zSn, w);
            if (cp) d.clap[i]  = svfBp(d.bp1k, d.zCp, w);
            if (ma) d.mar[i]   = svfHp(d.hp5k, d.zMa, w);
            if (tm) {   // Paul Kellet's pink filter, as io-808 uses
                float* b = d.pink;
                b[0] = 0.99886f * b[0] + w * 0.0555179f;
                b[1] = 0.99332f * b[1] + w * 0.0750759f;
                b[2] = 0.969f   * b[2] + w * 0.153852f;
                b[3] = 0.8665f  * b[3] + w * 0.3104856f;
                b[4] = 0.55f    * b[4] + w * 0.5329522f;
                b[5] = -0.7616f * b[5] - w * 0.016898f;
                float p = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) * 0.11f;
                b[6] = w * 0.115926f;
                d.tom[i] = svfLp(d.lp10k, d.zTm, p);
            }
        }
    }
}

void drumNoteOn(DrumVoice& v, int inst, uint8_t vel, const DrumShared& d) {
    memset(&v, 0, sizeof(v));
    v.inst = (uint8_t)inst;
    float a = (float)vel / 100.0f;
    v.gain = powf(a, 1.5f * d.accent);   // io-808: a^1.5 at ACCENT 4
    float* s = v.s;
    switch (inst) {
        case DR_BD: {
            float dec = msS(d, d.bdDecMs);
            s[1] = 48.0f;                              // frequency (Hz)
            s[6] = dec;
            s[7] = powf(48.0001f / 98.0f, 1.0f / dec); // the sweep down, a sample at a time
            v.dur  = (uint32_t)(msS(d, 2) + dec);
            v.gain *= LVL_KICK * d.lvl[0];
            break;
        }
        case DR_SD: v.dur = (uint32_t)msS(d, 76);  v.gain *= LVL * d.lvl[1]; s[2] = d.snappy; break;
        case DR_LT: case DR_MT: case DR_HT: case DR_LC: case DR_MC: case DR_HC: {
            const TomCfg& c = TOMS[inst - DR_LT];
            s[1] = (c.lo + d.tune * (c.hi - c.lo)) / d.fs;   // phase increment
            float ms = (c.noise && c.noiseMs > c.oscMs) ? c.noiseMs : c.oscMs;
            v.dur  = (uint32_t)msS(d, ms + 1);
            v.gain *= LVL_TOM * d.lvl[3];
            break;
        }
        case DR_RS: v.dur = (uint32_t)msS(d, 15);   v.gain *= LVL * d.lvl[4]; break;
        case DR_CL: v.dur = (uint32_t)msS(d, 41);   v.gain *= LVL * d.lvl[4]; break;
        case DR_CP: v.dur = (uint32_t)msS(d, 116);  v.gain *= LVL * d.lvl[4]; break;
        case DR_MA: v.dur = (uint32_t)msS(d, 31);   v.gain *= LVL * d.lvl[4]; break;
        case DR_CB: v.dur = (uint32_t)msS(d, 416);  v.gain *= LVL * d.lvl[4]; break;
        case DR_CY: v.dur = (uint32_t)msS(d, CY_DEC_MS[0] * d.cyScale + 1); v.gain *= LVL * d.lvl[2]; break;
        case DR_OH: s[2] = msS(d, d.ohDecMs); v.dur = (uint32_t)(s[2] + msS(d, 1)); v.gain *= LVL * d.lvl[2]; break;
        case DR_CH: s[2] = msS(d, 50);        v.dur = (uint32_t)msS(d, 51);         v.gain *= LVL * d.lvl[2]; break;
        default:    v.inst = DR_DONE; break;
    }
}

void drumChoke(DrumVoice& v) { v.choked = 1; }

float drumLevel(const DrumVoice& v) {
    if (v.inst == DR_DONE || v.dur == 0 || v.t >= v.dur) return 0.0f;
    return v.gain * (1.0f - (float)v.t / (float)v.dur);
}

bool drumRender(DrumVoice& v, const DrumShared& d, float* out, int n) {
    if (v.inst == DR_DONE) return false;
    float* s = v.s;
    float g = v.gain;
    const float chokeK = v.choked ? 0.98f : 1.0f;   // ~5 ms to silence
    uint32_t t = v.t;
    if (n > (int)(v.dur - t)) n = (int)(v.dur - t);

    switch (v.inst) {
    case DR_BD: {
        const Lin amp = mkLin(msS(d, 2), s[6], 1.0f);
        const float fa = msS(d, 0.11f), k = s[7], ifs = d.invFs;
        float ph = s[0], f = s[1];
        for (int i = 0; i < n; i++, t++) {
            float ft = (float)t;
            if (ft < fa)             f = 48.0f + 50.0f * ft / fa;
            else if (ft < fa + 1.0f) f = 98.0f;
            else if (f > 48.0f)      { f *= k; if (f < 48.0f) f = 48.0f; }
            float x = (sin2pi(ph) + 0.8f * click(d, t, s + 4)) * lin(t, amp);
            ph = wrap(ph + f * ifs);
            out[i] += softClip(0.6f * svfLp(d.bdLp, s + 2, x)) * g;
        }
        s[0] = ph; s[1] = f;
        break;
    }
    case DR_SD: {
        const Lin body = mkLin(msS(d, 0.1f), msS(d, 50), 0.25f);
        const Lin nz   = mkLin(msS(d, 0.1f), msS(d, 75), s[2]);
        const float i1 = 476.0f * d.invFs, i2 = 238.0f * d.invFs;
        float p1 = s[0], p2 = s[1];
        for (int i = 0; i < n; i++, t++) {
            float tone = (sin2pi(p1) + sin2pi(p2)) * lin(t, body);
            p1 = wrap(p1 + i1); p2 = wrap(p2 + i2);
            out[i] += (tone + d.snare[i] * lin(t, nz)) * g;
        }
        s[0] = p1; s[1] = p2;
        break;
    }
    case DR_LT: case DR_MT: case DR_HT: case DR_LC: case DR_MC: case DR_HC: {
        const TomCfg& c = TOMS[v.inst - DR_LT];
        const Lin osc = mkLin(msS(d, 0.1f), msS(d, c.oscMs), 1.0f);
        const Lin nz  = mkLin(msS(d, 0.1f), msS(d, c.noiseMs), 0.2f);
        const float inc = s[1];
        float ph = s[0];
        for (int i = 0; i < n; i++, t++) {
            float x = sin2pi(ph) * lin(t, osc) + 0.3f * click(d, t, s + 2);
            if (c.noise) x += d.tom[i] * lin(t, nz);
            ph = wrap(ph + inc);
            out[i] += x * g;
        }
        s[0] = ph;
        break;
    }
    case DR_RS: {
        // A 480 Hz sine (band-passed) and the clave's 1750 Hz triangle
        // (high-passed, 40 ms) are half-wave rectified and clipped, through a
        // 10 ms VCA, then high-passed: a hard, woody knock.
        const float a = msS(d, 0.11f);
        const Lin swing = mkLin(a, msS(d, 10), 1.7f);
        const float i1 = 480.0f * d.invFs, i2 = 1750.0f * d.invFs;
        float p1 = s[0], p2 = s[1], ce = s[8];
        for (int i = 0; i < n; i++, t++) {
            float rim = svfBp(d.bp480, s + 2, sin2pi(p1));
            float clv = svfHp(d.hp2450, s + 4, tri(p2)) * expAD(t, a, 0.7f, d.clK, ce);
            p1 = wrap(p1 + i1); p2 = wrap(p2 + i2);
            float x = rim + clv;
            float sw = softClip(3.0f * (x > 0.0f ? x : 0.0f)) * lin(t, swing);
            out[i] += svfHp(d.hp480, s + 6, sw) * g;
        }
        s[0] = p1; s[1] = p2; s[8] = ce;
        break;
    }
    case DR_CL: {
        const float a = msS(d, 0.11f), inc = 2450.0f * d.invFs;
        float ph = s[0], ce = s[3];
        for (int i = 0; i < n; i++, t++) {
            float x = svfBp(d.bp2450, s + 1, tri(ph));
            ph = wrap(ph + inc);
            out[i] += x * expAD(t, a, 0.7f, d.clK, ce) * g;
        }
        s[0] = ph; s[3] = ce;
        break;
    }
    case DR_CP: {
        // Four 10 ms saw bursts (levels 1, 0.5, 0, -0.5 as io-808 has them)
        // over a 115 ms tail.
        const float iBurst = 1.0f / msS(d, 10);
        const Lin rev = mkLin(msS(d, 0.2f), msS(d, 115), 0.75f);
        for (int i = 0; i < n; i++, t++) {
            float pos = (float)t * iBurst, saw = 0.0f;   // in bursts
            int b = (int)pos;
            if (b < 4) saw = (1.0f - 0.5f * (float)b) * (1.0f - (pos - (float)b));
            out[i] += d.clap[i] * (saw + lin(t, rev)) * g;
        }
        break;
    }
    case DR_MA: {
        const Lin env = mkLin(msS(d, 0.2f), msS(d, 30), 0.5f);
        for (int i = 0; i < n; i++, t++) out[i] += d.mar[i] * lin(t, env) * g;
        break;
    }
    case DR_CB: {
        const float dShort = msS(d, 15);
        const Lin sh = mkLin(msS(d, 0.11f), dShort, 0.375f);
        float le = s[2];
        for (int i = 0; i < n; i++, t++) {
            float env = lin(t, sh) + expAD(t, dShort, 0.125f, d.cbK, le);
            out[i] += svfBp(d.bp2640, s, d.cow[i] * env) * g;
        }
        s[2] = le;
        break;
    }
    case DR_CY: {
        const float a = msS(d, 0.1f);
        const float wLo = 1.0f - 0.75f * d.cyTone, wHi = 1.0f + 0.75f * d.cyTone;   // CY TONE
        float e0 = s[0], e1 = s[1], e2 = s[2];
        for (int i = 0; i < n; i++, t++) {
            float lo  = svfHp(d.hp5k,  s + 3, d.metal5[i]  * expAD(t, a, CY_AMT, d.cyK[0], e0));
            float mid = svfHp(d.hp10k, s + 5, d.metal10[i] * expAD(t, a, CY_AMT, d.cyK[1], e1));
            float hi  = svfHp(d.hp8k,  s + 7, d.metal10[i] * expAD(t, a, CY_AMT, d.cyK[2], e2));
            out[i] += (lo * wLo + mid + hi * wHi) * g;
        }
        s[0] = e0; s[1] = e1; s[2] = e2;
        break;
    }
    case DR_OH: case DR_CH: {
        const Lin env = mkLin(msS(d, 0.1f), s[2], 1.0f);
        for (int i = 0; i < n; i++, t++) {
            g *= chokeK;
            out[i] += svfHp(d.hp8k, s, d.metal10[i] * lin(t, env)) * g;
        }
        break;
    }
    }
    v.t = t;
    v.gain = g;
    if (t >= v.dur || g < 1e-4f) { v.inst = DR_DONE; return false; }
    return true;
}

void drumBus(float* buf, int n) {
    for (int i = 0; i < n; i++) buf[i] = softClip(buf[i]);
}

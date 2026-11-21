// Based on CW-78 by A Thousand Details - https://github.com/athousanddetails/schwung-cw-78
// Portions Copyright (C) 2026 A Thousand Details.  GPL-3.0.  Changed: see cr78.h.
#include "cr78.h"
#include "OrganSynth.h"
#include <math.h>
#include <string.h>

// The core builds for size (-Os); this file is the organ's hot loop.
#pragma GCC optimize("O2")

static const float LN10 = 2.3025851f;
static const float PI_F = 3.14159265f;

// ── GM drum map → CR-78 voice ─────────────────────────────────────────────────
// 35..82.  The CR-78 has no toms, clap or open hat: the toms play the conga
// and bongos (low, mid, high), the open hat its cymbal, and the clap nothing.
// The hi congas play the low bongo; triangles and the ride bell the metallic
// beat; wood blocks the claves.
static const int8_t GM_MAP[82 - 35 + 1] = {
    CR_BD, CR_BD, CR_RS, CR_SD, -1,    CR_SD, CR_LC, CR_HH,   // 35 kick 2 .. 42 closed hat
    CR_LC, CR_HH, CR_LB, CR_CY, CR_LB, CR_HB, CR_CY, CR_HB,   // 43 .. 50 high tom
    CR_CY, CR_CY, CR_MB, CR_TB, CR_CY, CR_CB, CR_CY, -1,      // 51 ride .. 58 vibraslap
    CR_CY, CR_HB, CR_LB, CR_LB, CR_LB, CR_LC, -1,    -1,      // 59 ride 2 .. 66 low timbale
    -1,    -1,    CR_MA, CR_MA, -1,    -1,    CR_GU, CR_GU,   // 67 agogo .. 74 long guiro
    CR_CL, CR_CL, CR_CL, -1,    -1,    CR_MB, CR_MB, CR_MA,   // 75 claves .. 82 shaker
};

int cr78InstForNote(uint8_t note) {
    if (note < 35 || note > 82) return -1;
    return GM_MAP[note - 35];
}

// ── Kit balance ───────────────────────────────────────────────────────────────
// CW-78's mix, dB against the kick by peak level (page 30's Vpp column is an
// alignment spec at each voice's own output, not the mix) — except the conga
// and bongos, 9 dB further down: against the CR-78 on OMD's Enola Gay intro
// (1980) CW-78's mix had them that much hot of the snare and hi-hat.  RAW_PEAK puts
// each voice's raw peak — measured at velocity 127 with the sliders at 4,
// by `test_cr78 --fit` — on that level, the kick peaking at KICK_PEAK.
static const float MIX_DB[CR_COUNT] = {
    0, 0, -4, -5, -4, -9, -6, -12, -12, -11, -4, -5, -4, -4,
};
static const float KICK_PEAK = 0.6f;
static const float RAW_PEAK[CR_COUNT] = {   // test_cr78 --fit
    1.4783f, 11.3317f, 0.2378f, 0.2732f, 0.4957f, 0.2509f, 0.0454f,
    3.3135f, 3.2691f, 2.3012f, 0.5088f, 0.2187f, 0.0478f, 0.5421f,
};

// ── Building blocks ───────────────────────────────────────────────────────────
// A zero-delay-feedback state-variable filter: its band-pass (× k) is
// exactly the RBJ constant-peak band-pass CW-78's Resonator uses.
static Cr78Svf svfCoef(float fc, float q, float fs) {
    if (fc > 0.45f * fs) fc = 0.45f * fs;
    if (q < 0.3f) q = 0.3f;
    float g = tanf(PI_F * fc / fs);
    Cr78Svf c;
    c.k  = 1.0f / q;
    c.a1 = 1.0f / (1.0f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}
static inline float res(const Cr78Svf& c, float* z, float x) {
    float v3 = x - z[1];
    float v1 = c.a1 * z[0] + c.a2 * v3;
    float v2 = z[1] + c.a2 * z[0] + c.a3 * v3;
    z[0] = 2.0f * v1 - z[0];
    z[1] = 2.0f * v2 - z[1];
    return c.k * v1;
}
// One-pole high-pass, z = { last input, output }.
static inline float hp(float a, float* z, float x) {
    z[1] = a * (z[1] + x - z[0]);
    z[0] = x;
    return z[1];
}
static inline float hpCoef(float f, float fs) { return expf(-2.0f * PI_F * f / fs); }
// Q for a ring of `t` seconds to a tenth of the peak, at f0.
static inline float qForRing(float f0, float t) { return PI_F * f0 * t / LN10; }
// Per-sample factor for an RC envelope that falls to a tenth in `t` seconds.
static inline float envA(float t, float fs) { return expf(-LN10 / (t * fs)); }

static inline float softClip(float x) {   // tanh, near enough
    if (x >  3.0f) return  1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
// The 2SC1815 stage on +15 / -5 V: clips harder on the negative half.
static inline float transistorClip(float v) {
    return (v >= 0.0f) ? 12.0f * softClip(v * (1.0f / 12.0f))
                       : 4.5f * softClip(v * (1.0f / 4.5f));
}

// The front end every voice shares: a 1 ms pulse of `gateV` through
// 0.027 µF / 270 k (AC coupling at 21.8 Hz) and a silicon diode (0.55 V,
// soft knee).  One strike per hit.  fy = the coupled voltage, starting at
// gateV.
static inline float strikeStep(uint32_t t, float& fy, const Cr78Shared& d) {
    if (t >= d.pulseN) return 0.0f;
    fy *= d.trigA;
    return (fy > 0.55f) ? fy - 0.55f : fy * fy * (1.0f / 1.1f);
}

static inline float blep(float t, float dt) {
    if (t < dt)        { float x = t / dt;          return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}
static inline float square(float t, float dt) {   // band-limited, ±1
    float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
    return ((t < 0.5f) ? 1.0f : -1.0f) + blep(t, dt) - blep(t2, dt);
}
static inline float wrap(float p) { return (p >= 1.0f) ? p - 1.0f : p; }

// ── The phase-shift drum channels (kick, conga, bongos) ──────────────────────
// Frequencies from page 30 (CW-78 shows the ladder's components land within
// 5% of them).  The network's two-pole Q (0.612) is CW-78's fit to the decay
// column; the strike is 390 k / the channel's input R (the kick is struck
// twice as hard).
struct DrumCh { float f0, decay, strike; };
static const float DRUM_Q = 0.612f, STRIKE_DRIVE = 3.2f;
static const float CLICK_BLEED = 0.55f, CLICK_HPF = 700.0f;
static const float LIFT = 0.035f, LIFT_TAU = 0.030f;
static inline const DrumCh& drumCh(int inst) {
    static const DrumCh BD = { 62.5f, 0.100f, 2.05f }, LC = { 208.0f, 0.150f, 1.0f },
                        LB = { 400.0f, 0.040f, 1.0f }, HB = { 600.0f, 0.040f, 1.0f };
    return inst == CR_BD ? BD : inst == CR_LC ? LC : inst == CR_LB ? LB : HB;
}

// Snare: a broader network (Q 1.06, from R516 / R517) at 340 Hz, 60 ms, plus
// the noise through C514 / R511, high-passed at 2.4 kHz.  CHANGED from CW-78,
// whose snap was voiced by ear: at SNAPPY 4 its level (×30 CW-78's) and decay
// (50 ms, not 27) are fitted to the snare on the Enola Gay intro — 1/3-octave
// bands from 500 Hz to 8 kHz within 2.5 dB, the 340 Hz shell within 2.
// SNAPPY 0 is CW-78's own voicing (OrganSynth.h).
static const float SD_F0 = 340.0f, SD_Q = 1.065f;   // the shell's 60 ms ring is SD RING at 4
static const float SD_SNAP_LEVEL = 16.5f, SD_SHELL_DUCK = 0.2475f;   // at the fitted snap

// The fixed-frequency voices: tank (0 = none), its Q or decay, output high-pass.
static const float RS_F0 = 1480.0f, RS_DECAY = 0.005f, CL_F0 = 2630.0f, CL_DECAY = 0.018f;
static const float CY_TANK = 9098.0f;   // L3 45 mH ∥ C521 .0068
static const float GU_TANK = 6126.0f;   // L4 45 mH ∥ C532 .015
static const float TB_TANK = 3503.0f;   // L5 (CW-78's solved 62.7 mH) ∥ C538 .033
static const float HPF_HZ[CR_COUNT] = {
    0, 2400, 900, 6500, 1200, 3200, 1400, 0, 0, 0, 500, 2000, 1200, 2500,
};
static const float NOISE_DRIVE_HH = 0.82f, NOISE_DRIVE_MA = 0.48f;

void cr78Init(Cr78Shared& d, float fs, float* work) {
    memset(&d, 0, sizeof(d));
    d.fs = fs;
    d.invFs = 1.0f / fs;
    d.trigA = expf(-2.0f * PI_F * (1.0f / (2.0f * PI_F * 270.0e3f * 0.027e-6f)) / fs);
    d.pulseN = (uint32_t)(0.001f * fs);
    for (int i = 0; i < CR_COUNT; i++) d.hpA[i] = HPF_HZ[i] > 0 ? hpCoef(HPF_HZ[i], fs) : 0.0f;
    d.hpA[CR_BD] = hpCoef(CLICK_HPF, fs);   // the drum channels' click bleed
    d.hpA[CR_LC] = d.hpA[CR_LB] = d.hpA[CR_HB] = d.hpA[CR_BD];
    d.tank[CR_RS] = svfCoef(RS_F0, qForRing(RS_F0, RS_DECAY), fs);
    d.tank[CR_CL] = svfCoef(CL_F0, qForRing(CL_F0, CL_DECAY), fs);
    d.tank[CR_CY] = svfCoef(CY_TANK, 1.8f, fs);
    d.tank[CR_GU] = svfCoef(GU_TANK, 9.0f, fs);
    d.tank[CR_TB] = svfCoef(TB_TANK, 2.2f, fs);
    d.cowBand = svfCoef(1900.0f, 1.1f, fs);
    const float cowHz[2] = { 800.0f, 555.0f }, mbHz[3] = { 6170.0f, 5620.0f, 4080.0f };
    for (int i = 0; i < 2; i++) { d.cowInc[i] = cowHz[i] / fs; d.cowPh[i] = 0.5f * i; }
    const float mbSeed[3] = { 0.0f, 0.37f, 0.71f };
    for (int i = 0; i < 3; i++) { d.mbInc[i] = mbHz[i] / fs; d.mbPh[i] = mbSeed[i]; }
    d.rng = 0x1978u;
    d.noise = work;
    d.cow   = work + CR78_BLOCK_MAX;
    d.mb    = work + 2 * CR78_BLOCK_MAX;
    memset(work, 0, CR78_WORK_FLOATS * sizeof(float));
    d.hatTone = 4;                       // the filters above are HAT TONE 4's
    const int def[1] = { 4 };
    cr78Prepare(d, def, 0, 0, 0);
}

#define CR_BIT(i) (1u << (i))
static const uint32_t USE_NOISE = CR_BIT(CR_SD) | CR_BIT(CR_HH) | CR_BIT(CR_CY) |
                                  CR_BIT(CR_MA) | CR_BIT(CR_TB);

void cr78Prepare(Cr78Shared& d, const int* sl, uint32_t mask, int n, int count) {
    int v[ORGAN_DRUM_PARAMS];
    for (int i = 0; i < ORGAN_DRUM_PARAMS; i++)
        v[i] = (i >= count) ? 4 : (sl[i] < 0) ? 0 : (sl[i] > 8) ? 8 : sl[i];
    d.bdDec  = cr78KickDecayMs(v[0]) * 0.001f;
    d.snappy = cr78Snappy(v[1]);
    d.snapS  = cr78SnapMs(v[1]) * 0.001f;
    d.hhDec  = cr78HatDecayMs(v[2]) * 0.001f;
    d.cyDec  = cr78CymbalDecayMs(v[3]) * 0.001f;
    d.tune   = powf(2.0f, cr78TuneSemis(v[4]) / 12.0f);
    d.click   = cr78ClickScale(v[5]);
    d.bdTune  = powf(2.0f, cr78TuneSemis(v[6]) / 12.0f);
    d.sdTune  = powf(2.0f, cr78TuneSemis(v[7]) / 12.0f);
    d.sdRing  = cr78SnareRingMs(v[8]) * 0.001f;
    if (v[9] != d.hatTone) {             // HAT TONE moves the hats', cymbal's and maracas' filters
        d.hatTone = v[9];
        float r = cr78HatTone(v[9]);
        d.hpA[CR_HH] = hpCoef(HPF_HZ[CR_HH] * r, d.fs);
        d.hpA[CR_CY] = hpCoef(HPF_HZ[CR_CY] * r, d.fs);
        d.hpA[CR_MA] = hpCoef(HPF_HZ[CR_MA] * r, d.fs);
        d.tank[CR_CY] = svfCoef(CY_TANK * r, 1.8f, d.fs);
    }
    d.percDec = cr78PercDecay(v[10]);
    for (int i = 0; i < 3; i++) d.lvl[i] = drumLevelGain(v[11 + i]);

    bool nz = mask & USE_NOISE, cb = mask & CR_BIT(CR_CB), mb = mask & CR_BIT(CR_MB);
    if (!cb) memset(d.zCow, 0, sizeof(d.zCow));
    for (int i = 0; i < n; i++) {
        if (nz) {   // Q533, the one noise source: every lane hears the same noise
            d.rng ^= d.rng << 13; d.rng ^= d.rng >> 17; d.rng ^= d.rng << 5;
            d.noise[i] = (float)(int32_t)d.rng * (1.0f / 2147483648.0f);
        }
        if (cb) {
            float p = 0.5f * (square(d.cowPh[0], d.cowInc[0]) + square(d.cowPh[1], d.cowInc[1]));
            d.cowPh[0] = wrap(d.cowPh[0] + d.cowInc[0]);
            d.cowPh[1] = wrap(d.cowPh[1] + d.cowInc[1]);
            d.cow[i] = res(d.cowBand, d.zCow, p);
        }
        if (mb) {   // free-running: no two hits catch the bank at the same phase
            float m = 0.0f;
            for (int k = 0; k < 3; k++) {
                m += square(d.mbPh[k], d.mbInc[k]);
                d.mbPh[k] = wrap(d.mbPh[k] + d.mbInc[k]);
            }
            d.mb[i] = m * (1.0f / 3.0f);
        }
    }
}

void cr78NoteOn(Cr78Voice& v, int inst, uint8_t note, uint8_t vel, const Cr78Shared& d) {
    memset(&v, 0, sizeof(v));
    v.inst = (uint8_t)inst;
    float vn = (float)(vel > 127 ? 127 : vel) / 127.0f;
    float* s = v.s;
    float gateV;
    switch (inst) {
        case CR_BD: case CR_LC: case CR_LB: case CR_HB: {
            const DrumCh& c = drumCh(inst);
            gateV = 0.5f + 0.5f * vn;
            s[1] = LIFT;
            s[6] = c.f0 * d.tune * ((inst == CR_BD) ? d.bdTune : 1.0f);
            s[7] = (inst == CR_BD) ? d.bdDec : c.decay * d.percDec;
            s[8] = CLICK_BLEED * ((inst == CR_BD) ? d.click : 1.0f);
            v.dur = (uint32_t)((3.0f * s[7] + 0.02f) * d.fs);
            break;
        }
        case CR_SD:
            gateV = 0.45f + 0.55f * vn;
            s[4] = envA(d.snapS, d.fs);
            s[7] = SD_F0 * d.tune * d.sdTune;
            s[8] = d.snappy;
            s[9] = d.sdRing;
            v.dur = (uint32_t)((3.0f * (s[9] > d.snapS ? s[9] : d.snapS) + 0.01f) * d.fs);
            break;
        case CR_HH: case CR_CY: case CR_MA: {
            float dec = inst == CR_HH ? d.hhDec : inst == CR_CY ? d.cyDec : 0.020f;
            gateV = 0.35f + 0.65f * vn;
            s[2] = envA(dec, d.fs);
            v.dur = (uint32_t)((3.0f * dec + 0.005f) * d.fs);
            break;
        }
        case CR_RS: case CR_CL:
            gateV = 0.4f + 0.6f * vn;
            v.dur = (uint32_t)((3.0f * (inst == CR_RS ? RS_DECAY : CL_DECAY) + 0.005f) * d.fs);
            break;
        case CR_CB: case CR_MB: {
            float dec = (inst == CR_CB) ? 0.060f : 0.050f;
            gateV = 0.4f + 0.6f * vn;
            s[2] = envA(dec, d.fs);
            v.dur = (uint32_t)((3.0f * dec + 0.005f) * d.fs);
            break;
        }
        case CR_GU: {
            // The two guiro notes: a short, fast scrape and a long, slow one,
            // at the hardware's own two rates.
            bool shortG = (note == 73);
            float dec = shortG ? 0.12f : 0.30f;
            gateV = 0.4f + 0.6f * vn;
            s[2] = envA(dec, d.fs);
            s[4] = (shortG ? 125.0f : 77.0f) * d.invFs;
            s[3] = 0.5f - s[4];   // the first tooth on the hit
            v.dur = (uint32_t)((3.0f * dec + 0.01f) * d.fs);
            break;
        }
        case CR_TB:
            gateV = 0.33f + 0.67f * vn;
            s[2] = envA(0.220f, d.fs);
            v.dur = (uint32_t)((3.0f * 0.220f + 0.01f) * d.fs);
            break;
        default:
            v.inst = CR_DONE;
            return;
    }
    s[0] = gateV;
    // The accent (BA662 VCA) only takes over below 70%: above it the trigger
    // voltage carries the velocity on its own.
    float gainPart = (vn < 0.7f) ? vn * (1.0f / 0.7f) : 1.0f;
    v.gain = powf(10.0f, MIX_DB[inst] / 20.0f) * KICK_PEAK / RAW_PEAK[inst] * gainPart;
    // The KICK / SNARE / PERC level settings (hats, cymbal, maracas have none).
    if (inst == CR_BD) v.gain *= d.lvl[0];
    else if (inst == CR_SD) v.gain *= d.lvl[1];
    else if (inst != CR_HH && inst != CR_CY && inst != CR_MA) v.gain *= d.lvl[2];
    if (v.gain <= 0.0f) v.inst = CR_DONE;   // its level is off
}

float cr78Level(const Cr78Voice& v) {
    if (v.inst == CR_DONE || v.dur == 0 || v.t >= v.dur) return 0.0f;
    return 1.0f - (float)v.t / (float)v.dur;
}

bool cr78Render(Cr78Voice& v, const Cr78Shared& d, float* out, int n) {
    if (v.inst == CR_DONE) return false;
    float* s = v.s;
    const float g = v.gain;
    uint32_t t = v.t;
    if (n > (int)(v.dur - t)) n = (int)(v.dur - t);
    const float hpa = d.hpA[v.inst];

    switch (v.inst) {
    case CR_BD: case CR_LC: case CR_LB: case CR_HB: {
        // The loop folded into the pole: a loop of gain g round a resonator
        // of Q rings like a resonator of Q / (1 - g), and amplifies at f0 by
        // 1 / (1 - g) — so a longer decay rings louder, as on the hardware.
        const DrumCh& c = drumCh(v.inst);
        const float f0 = s[6];
        float q = qForRing(f0, s[7]);
        if (q < DRUM_Q) q = DRUM_Q;
        const float loopGain = q / DRUM_Q;
        const float liftA = expf(-32.0f / (LIFT_TAU * d.fs));   // per 32 samples
        const float drive = c.strike * STRIKE_DRIVE, bleed = s[8];
        float fy = s[0], lift = s[1];
        Cr78Svf co = svfCoef(f0 * (1.0f + lift), q, d.fs);
        for (int i = 0; i < n; i++, t++) {
            if ((i & 31) == 0 && i && lift > 1e-4f) {
                lift *= liftA;
                co = svfCoef(f0 * (1.0f + lift), q, d.fs);
            }
            float st = strikeStep(t, fy, d) * drive;
            float y = res(co, s + 2, st) * loopGain + hp(hpa, s + 4, st) * bleed;
            out[i] += transistorClip(y) * g;
        }
        if (lift > 1e-4f) lift *= liftA;   // the block's last step
        s[0] = fy; s[1] = lift;
        break;
    }
    case CR_SD: {
        float q = qForRing(s[7], s[9]);
        if (q < SD_Q) q = SD_Q;
        const float gl = q / SD_Q, ea = s[4], mix = s[8];
        const Cr78Svf co = svfCoef(s[7], q, d.fs);
        float fy = s[0], env = s[3];
        for (int i = 0; i < n; i++, t++) {
            float st = strikeStep(t, fy, d);
            float shell = transistorClip(res(co, s + 1, st * 4.0f) * gl);
            if (st > 0.0f && st * 1.7f > env) env = st * 1.7f;
            float snap = hp(hpa, s + 5, d.noise[i] * env);
            env *= ea;
            out[i] += (shell * (1.0f - SD_SHELL_DUCK * fminf(mix, 2.0f)) + snap * mix * SD_SNAP_LEVEL) * g;
        }
        s[0] = fy; s[3] = env;
        break;
    }
    case CR_HH: case CR_CY: case CR_MA: {
        const float ea = s[2];
        const float drive = v.inst == CR_HH ? NOISE_DRIVE_HH : v.inst == CR_MA ? NOISE_DRIVE_MA : 1.0f;
        const bool tank = (v.inst == CR_CY);
        float fy = s[0], env = s[1];
        for (int i = 0; i < n; i++, t++) {
            float st = strikeStep(t, fy, d);
            if (st > 0.0f && st * 1.6f > env) env = st * 1.6f;
            float y = d.noise[i] * env * drive;
            env *= ea;
            if (tank) y = res(d.tank[CR_CY], s + 3, y);
            out[i] += hp(hpa, s + 5, y) * g;
        }
        s[0] = fy; s[1] = env;
        break;
    }
    case CR_RS: case CR_CL: {   // a coil and a capacitor, struck: nothing else
        const Cr78Svf& co = d.tank[v.inst];
        float fy = s[0];
        for (int i = 0; i < n; i++, t++) {
            float st = strikeStep(t, fy, d) * 6.0f;
            out[i] += hp(hpa, s + 3, res(co, s + 1, st)) * g;
        }
        s[0] = fy;
        break;
    }
    case CR_CB: case CR_MB: {
        const float* src = (v.inst == CR_CB) ? d.cow : d.mb;
        const float ea = s[2];
        float fy = s[0], env = s[1];
        for (int i = 0; i < n; i++, t++) {
            float st = strikeStep(t, fy, d);
            if (st > 0.0f && st * 1.5f > env) env = st * 1.5f;
            out[i] += hp(hpa, s + 3, src[i] * env) * g;
            env *= ea;
        }
        s[0] = fy; s[1] = env;
        break;
    }
    case CR_GU: {
        // Each edge of the multivibrator (a tooth) pings the 6126 Hz tank.
        const float ea = s[2], inc = s[4];
        float fy = s[0], env = s[1], ph = s[3];
        for (int i = 0; i < n; i++, t++) {
            float st = strikeStep(t, fy, d);
            if (st > 0.0f && st * 1.5f > env) env = st * 1.5f;
            bool was = ph >= 0.5f;
            ph = wrap(ph + inc);
            bool now = ph >= 0.5f;
            float tooth = (now != was) ? (now ? 1.0f : -0.85f) * env : 0.0f;
            env *= ea;
            out[i] += hp(hpa, s + 7, res(d.tank[CR_GU], s + 5, tooth)) * g;
        }
        s[0] = fy; s[1] = env; s[3] = ph;
        break;
    }
    case CR_TB: {   // noise and the strike's own crash into the jingles' tank
        const float ea = s[2];
        float fy = s[0], env = s[1];
        for (int i = 0; i < n; i++, t++) {
            float st = strikeStep(t, fy, d);
            if (st > 0.0f && st * 1.6f > env) env = st * 1.6f;
            float src = (d.noise[i] * 0.62f + st * 1.5f) * env;
            env *= ea;
            out[i] += hp(hpa, s + 5, res(d.tank[CR_TB], s + 3, src)) * g;
        }
        s[0] = fy; s[1] = env;
        break;
    }
    }
    v.t = t;
    if (t >= v.dur) { v.inst = CR_DONE; return false; }
    return true;
}

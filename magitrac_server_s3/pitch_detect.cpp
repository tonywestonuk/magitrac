#include "pitch_detect.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "esp_dsp.h"     // twiddle table sized for 4096 by spectrumInit()
static void* pdAlloc(size_t n) {
    void* p = heap_caps_aligned_alloc(32, n, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_aligned_alloc(32, n, MALLOC_CAP_8BIT);
}
static void pdFree(void* p) { heap_caps_free(p); }
#else
static void* pdAlloc(size_t n) { return malloc(n); }
static void  pdFree(void* p)   { free(p); }
#endif

#define PD_N        4096
#define PD_HALF     (PD_N / 2)
#define PD_HOP      2048
#define PD_FMIN     50.0f       // lowest fundamental looked for (Hz)
#define PD_FMAX     1500.0f     // highest
#define PD_HPS_H    6           // harmonics in the candidate score
#define PD_REF_H    10          // harmonics in the refinement
#define PD_SPEC_MAX 5000.0f     // top of the spectrum used
#define PD_CLARITY  0.25f       // below this: no clear pitch
#define PD_SILENT   33.0f       // a frame RMS below this (about -60 dBFS) is silence

static const int16_t* sPcm    = nullptr;
static uint32_t       sFrames = 0;
static uint32_t       sRate   = 0;
static uint8_t        sCh     = 1;         // interleaved channels (reads the first)
static uint32_t       sJob    = 0;         // bumped by every pitchBegin
static uint32_t       sStarts[PD_MAX_FRAMES];
static int            sCount  = 0;
static int            sNext   = 0;
static bool           sBusy   = false;
static float*         sBuf    = nullptr;   // PD_N complex (interleaved re, im)
static float*         sWin    = nullptr;   // PD_N Hann window
static float*         sAcc    = nullptr;   // PD_HALF summed power → magnitudes
static PitchResult    sRes    = {};

// ── FFT ──────────────────────────────────────────────────────────────────────
#ifndef ARDUINO
// Portable iterative radix-2 (the host tests).  In place, interleaved.
static void fftInPlace(float* d, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = d[2 * i], ti = d[2 * i + 1];
            d[2 * i] = d[2 * j]; d[2 * i + 1] = d[2 * j + 1];
            d[2 * j] = tr;       d[2 * j + 1] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float* a = d + 2 * (i + k);
                float* b = d + 2 * (i + k + len / 2);
                float xr = b[0] * cr - b[1] * ci;
                float xi = b[0] * ci + b[1] * cr;
                b[0] = a[0] - xr; b[1] = a[1] - xi;
                a[0] += xr;       a[1] += xi;
                float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}
#endif

static void freeAll() {
    if (sBuf) { pdFree(sBuf); sBuf = nullptr; }
    if (sWin) { pdFree(sWin); sWin = nullptr; }
    if (sAcc) { pdFree(sAcc); sAcc = nullptr; }
}

void pitchAbort() {
    freeAll();
    sBusy = false;
    sPcm  = nullptr;
}

bool pitchBusy() { return sBusy; }
uint32_t pitchJob() { return sJob; }

// ── Frame choice: the loudest parts ──────────────────────────────────────────
static float frameRms(uint32_t start) {
    uint32_t end = start + PD_N;
    if (end > sFrames) end = sFrames;
    double sum = 0;
    for (uint32_t i = start; i < end; i++) sum += (double)sPcm[i * sCh] * sPcm[i * sCh];
    uint32_t n = end > start ? end - start : 1;
    return (float)sqrt(sum / n);
}

bool pitchBegin(const int16_t* pcm, uint32_t frames, uint32_t rate, uint8_t ch) {
    pitchAbort();
    sJob++;
    sRes = PitchResult{ false, 0.0f, 0.0f };
    sPcm = pcm; sFrames = frames; sRate = rate; sCh = ch ? ch : 1;
    sCount = 0; sNext = 0;
    sBusy = true;                       // pitchStep() reports the outcome
    if (!pcm || frames < PD_N / 4 || !rate) { sCount = -1; return false; }

    uint32_t nPos = frames <= PD_N ? 1 : 1 + (frames - PD_N + PD_HOP - 1) / PD_HOP;
    float* rms = (float*)pdAlloc(nPos * sizeof(float));
    if (!rms) { sCount = -1; return false; }
    float maxR = 0;
    for (uint32_t p = 0; p < nPos; p++) {
        rms[p] = frameRms(p * PD_HOP);
        if (rms[p] > maxR) maxR = rms[p];
    }
    if (maxR < PD_SILENT) { pdFree(rms); sCount = -1; return false; }
    // The loudest frames, down to 0.3 of the loudest, at most PD_MAX_FRAMES.
    float floorR = maxR * 0.3f;
    while (sCount < PD_MAX_FRAMES) {
        int best = -1;
        for (uint32_t p = 0; p < nPos; p++)
            if (rms[p] >= floorR && (best < 0 || rms[p] > rms[best])) best = (int)p;
        if (best < 0) break;
        sStarts[sCount++] = (uint32_t)best * PD_HOP;
        rms[best] = -1.0f;
    }
    pdFree(rms);

    sBuf = (float*)pdAlloc(PD_N * 2 * sizeof(float));
    sWin = (float*)pdAlloc(PD_N * sizeof(float));
    sAcc = (float*)pdAlloc(PD_HALF * sizeof(float));
    if (!sBuf || !sWin || !sAcc) { freeAll(); sCount = -1; return false; }
    for (int i = 0; i < PD_N; i++)
        sWin[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (PD_N - 1)));
    memset(sAcc, 0, PD_HALF * sizeof(float));
    return true;
}

// ── Analysis of the averaged spectrum ────────────────────────────────────────
static float sBinHz = 1.0f;

// The magnitude at a fractional bin position (linear between bins), so a
// candidate a little off a harmonic scores a little less — candidates must
// not tie, or a near-miss can win.
static float magAt(const float* m, float x) {
    int k = (int)x;
    if (k < 1 || k >= PD_HALF - 2) return 0.0f;
    float t = x - k;
    return m[k] * (1.0f - t) + m[k + 1] * t;
}

// One refinement pass: the interpolated peaks of up to PD_REF_H harmonics of
// f, each within ±3 % (and maxCents) of where f puts it, averaged into a new
// fundamental.  Also the energy those harmonics hold.  False if none found.
static bool refine(const float* m, float f, float peak, float eps, int kHi,
                   float maxCents, float& out, double& harmE) {
    double wSum = 0, fSum = 0;
    harmE = 0;
    for (int h = 1; h <= PD_REF_H; h++) {
        float x = h * f / sBinHz;
        if (x + 3 >= kHi) break;
        int span = (int)(x * 0.03f) + 1;                 // ±3 %, at least a bin
        int lo = (int)x - span, hi = (int)x + span + 1;
        if (lo < 2) lo = 2;
        int p = lo;
        for (int k = lo; k <= hi; k++) if (m[k] > m[p]) p = k;
        if (m[p] < peak * 0.02f) continue;
        if (m[p] < m[p - 1] || m[p] < m[p + 1]) continue;   // edge of the window, not a peak
        float a = logf(m[p - 1] + eps), b = logf(m[p] + eps), c = logf(m[p + 1] + eps);
        float den = a - 2 * b + c;
        float delta = den != 0 ? 0.5f * (a - c) / den : 0.0f;
        if (delta > 0.5f) delta = 0.5f;
        if (delta < -0.5f) delta = -0.5f;
        float est = (p + delta) * sBinHz / h;
        if (fabsf(1200.0f * log2f(est / f)) > maxCents) continue;
        double w = (double)m[p] * sqrt((double)h);
        wSum += w;
        fSum += w * est;
        for (int k = p - 2; k <= p + 2; k++) harmE += (double)m[k] * m[k];   // the window's main lobe
    }
    if (wSum <= 0) return false;
    out = (float)(fSum / wSum);
    return true;
}

static void analyse() {
    float* m = sAcc;
    for (int k = 0; k < PD_HALF; k++) m[k] = sqrtf(m[k]);
    sBinHz = (float)sRate / PD_N;
    int kLo = (int)ceilf(40.0f / sBinHz);
    int kHi = (int)(PD_SPEC_MAX / sBinHz);
    if (kHi > PD_HALF - 2) kHi = PD_HALF - 2;

    float peak = 0;
    double total = 0;
    for (int k = kLo; k <= kHi; k++) {
        if (m[k] > peak) peak = m[k];
        total += (double)m[k] * m[k];
    }
    if (peak <= 0 || total <= 0) return;
    float eps = peak * 1e-3f;

    // Candidates every 5 cents from PD_FMIN to PD_FMAX: the sum of the log
    // magnitudes at the first PD_HPS_H harmonics (a harmonic product
    // spectrum, on a continuous grid so low notes' harmonics land right).
    float bestF = 0, bestS = -1e30f;
    for (float f = PD_FMIN; f <= PD_FMAX; f *= 1.0028922f) {     // 2^(5/1200)
        float s = 0;
        for (int h = 1; h <= PD_HPS_H; h++) {
            float x = h * f / sBinHz;
            if (x >= kHi) { s += logf(eps); continue; }
            s += logf(magAt(m, x) + eps);
        }
        if (s > bestS) { bestS = s; bestF = f; }
    }
    if (bestF <= 0) return;

    // The score can't tell a sparse sound (a sine, two partials) from its
    // subharmonics: f/3's series lands on f too.  So take the highest
    // multiple of the winner that still accounts for every significant peak.
    {
        float pk[48];
        int   np = 0;
        for (int k = kLo + 1; k < kHi && np < 48; k++) {
            if (m[k] < peak * 0.1f || m[k] < m[k - 1] || m[k] < m[k + 1]) continue;
            float a = m[k - 1], b = m[k], c = m[k + 1];
            float den = a - 2 * b + c;
            float d = den != 0 ? 0.5f * (a - c) / den : 0.0f;
            pk[np++] = (k + d) * sBinHz;
        }
        for (int mult = 6; mult >= 2; mult--) {
            float f = bestF * mult;
            if (f > PD_FMAX) continue;
            bool all = np > 0;
            for (int i = 0; i < np && all; i++) {
                float r = pk[i] / f;
                long  h = lrintf(r);
                float tol = 1.5f * sBinHz + 0.01f * pk[i];
                if (h < 1 || fabsf(pk[i] - h * f) > tol) all = false;
            }
            if (all) { bestF = f; break; }
        }
    }

    // Refine: a loose pass pulls a rough estimate onto the peaks, a tight
    // one measures it.
    float f1, f2;
    double harmE;
    if (!refine(m, bestF, peak, eps, kHi, 100.0f, f1, harmE)) return;
    if (!refine(m, f1, peak, eps, kHi, 30.0f, f2, harmE)) return;
    sRes.hz      = f2;
    sRes.clarity = (float)(harmE / total);
    if (sRes.clarity > 1.0f) sRes.clarity = 1.0f;
    sRes.found   = sRes.clarity >= PD_CLARITY;
}

bool pitchStep() {
    if (!sBusy) return true;
    if (sCount < 0) { sBusy = false; return true; }      // begin() failed / silent
    if (sNext < sCount) {
        uint32_t start = sStarts[sNext++];
        for (int i = 0; i < PD_N; i++) {
            uint32_t j = start + i;
            sBuf[2 * i]     = j < sFrames ? (float)sPcm[j * sCh] * sWin[i] : 0.0f;
            sBuf[2 * i + 1] = 0.0f;
        }
#ifdef ARDUINO
        dsps_fft2r_fc32(sBuf, PD_N);
        dsps_bit_rev_fc32(sBuf, PD_N);
#else
        fftInPlace(sBuf, PD_N);
#endif
        for (int k = 0; k < PD_HALF; k++)
            sAcc[k] += sBuf[2 * k] * sBuf[2 * k] + sBuf[2 * k + 1] * sBuf[2 * k + 1];
        if (sNext < sCount) return false;
    }
    analyse();
    freeAll();
    sBusy = false;
    sPcm  = nullptr;
    return true;
}

PitchResult pitchResult() { return sRes; }

int16_t pitchTuneToMiddleC(float hz) {
    if (hz <= 0) return 0;
    long c = lrintf(1200.0f * log2f(261.6256f / hz));
    if (c >  4800) c =  4800;
    if (c < -4800) c = -4800;
    return (int16_t)c;
}

void pitchNearestNote(float hz, uint8_t& note, int8_t& cents) {
    if (hz <= 0) { note = 0; cents = 0; return; }
    float midi = 69.0f + 12.0f * log2f(hz / 440.0f);
    long n = lrintf(midi);
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    note  = (uint8_t)n;
    cents = (int8_t)lrintf((midi - n) * 100.0f);
}

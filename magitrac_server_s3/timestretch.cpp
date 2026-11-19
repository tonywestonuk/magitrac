#include "timestretch.h"
#include <math.h>

#define TS_WIN_N      128
#define TS_END_FADE   256          // output frames (8 ms) a one-shot fades out over
static float sTsWin[TS_WIN_N + 1]; // Hann, phase 0..1 (516 bytes)
static bool  sTsWinUp = false;
static uint32_t sTsOps = 0;

uint32_t tsSearchOps() { return sTsOps; }

static void winInit() {
    if (sTsWinUp) return;
    for (int i = 0; i <= TS_WIN_N; i++)
        sTsWin[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / TS_WIN_N);
    sTsWinUp = true;
}

// Periodic Hann by table: w(x) + w(x + 0.5) is exactly 1, so two grains half
// a grain apart always sum to unity — no level correction anywhere.
static inline float win(float ph) {
    float x = ph * TS_WIN_N;
    int   i = (int)x;
    if (i >= TS_WIN_N) return 0.0f;
    if (i < 0) return 0.0f;
    float f = x - i;
    return sTsWin[i] + (sTsWin[i + 1] - sTsWin[i]) * f;
}

void tsStart(TsState& s, uint32_t srcRate, float ratio, uint32_t outRate) {
    winInit();
    if (ratio < 0.0625f) ratio = 0.0625f;      // ±4 octaves (the TUNE range)
    if (ratio > 16.0f)   ratio = 16.0f;
    s.ratio = ratio;
    s.base  = (float)srcRate / (float)outRate;
    s.step  = ratio * s.base;
    // 32 ms grains and ±8 ms of search at any sample rate: ±8 ms spans a full
    // cycle down to 62 Hz, so a matching point is always in reach.
    s.len = ((int32_t)(0.032f * srcRate)) & ~1;
    if (s.len < 64) s.len = 64;
    s.hop = s.len / 2;
    s.tol = s.len / 4;
    s.srcHop = (float)s.hop / ratio;
    s.src = s.srcHop;                        // grain 1's sample time
    s.untilNext = (float)s.hop;
    s.prev = 0;
    s.t = 0.0f;
    s.ng = 1;
    s.g[0].start = 0;
    s.g[0].o     = 0.0f;
    s.g[0].first = true;
}

// Channel c of whole frame i: wrapped for a loop, silence outside a one-shot.
static inline float at(const int16_t* d, int32_t n, int ch, bool loop, int32_t i, int c) {
    if ((uint32_t)i >= (uint32_t)n) {
        if (!loop) return 0.0f;
        i %= n;
        if (i < 0) i += n;
    }
    return (float)d[i * ch + c];
}

// 4-point cubic (Catmull-Rom): far flatter to the top than linear.
static inline float cubic(float y0, float y1, float y2, float y3, float f) {
    float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c = -0.5f * y0 + 0.5f * y2;
    return ((a * f + b) * f + c) * f + y1;
}

static inline void readFrame(const int16_t* d, int32_t n, int ch, bool loop,
                             int32_t i, float f, float& L, float& R) {
    if (i >= 1 && i + 2 < n) {                          // inside: direct
        const int16_t* p = d + (i - 1) * ch;
        L = cubic(p[0], p[ch], p[2 * ch], p[3 * ch], f);
        R = ch == 2 ? cubic(p[1], p[1 + ch], p[1 + 2 * ch], p[1 + 3 * ch], f) : L;
        return;
    }
    L = cubic(at(d, n, ch, loop, i - 1, 0), at(d, n, ch, loop, i, 0),
              at(d, n, ch, loop, i + 1, 0), at(d, n, ch, loop, i + 2, 0), f);
    R = ch == 2 ? cubic(at(d, n, ch, loop, i - 1, 1), at(d, n, ch, loop, i, 1),
                        at(d, n, ch, loop, i + 1, 1), at(d, n, ch, loop, i + 2, 1), f)
                : L;
}

// Sum of a[i]·b[i] over `cnt` points `stride` apart (left channel).
static float corr(const int16_t* d, int32_t n, int ch, bool loop,
                  int32_t a, int32_t b, int32_t cnt, int32_t stride) {
    sTsOps += cnt;
    int32_t span = (cnt - 1) * stride;
    float acc = 0.0f;
    if (a >= 0 && b >= 0 && a + span < n && b + span < n) {
        const int16_t* pa = d + a * ch;
        const int16_t* pb = d + b * ch;
        int32_t st = stride * ch;
        for (int32_t k = 0; k < cnt; k++, pa += st, pb += st)
            acc += (float)*pa * (float)*pb;
        return acc;
    }
    for (int32_t k = 0; k < cnt; k++)
        acc += at(d, n, ch, loop, a + k * stride, 0) * at(d, n, ch, loop, b + k * stride, 0);
    return acc;
}

// Where the next grain should start: near `want` (the sample's own time),
// at the point whose waveform best continues the previous grain — whose
// natural continuation starts at `nat`.  A fixed budget per grain at any
// sample rate: a coarse pass over ~64 lags on 64 points, then every lag
// between the coarse neighbours on 128 points.
static int32_t bestStart(const TsState& s, const int16_t* d, int32_t n, int ch, bool loop,
                         int32_t want, int32_t nat) {
    int32_t lo = want - s.tol, hi = want + s.tol;
    if (!loop && lo < 0) lo = 0;
    int32_t ov = s.hop;                                  // the overlap being matched
    int32_t lagStep = (2 * s.tol + 63) / 64;
    if (lagStep < 1) lagStep = 1;
    int32_t pStep = ov / 64 > 0 ? ov / 64 : 1;
    int32_t best = want;
    float   bc = -3.4e38f;
    for (int32_t c = lo; c <= hi; c += lagStep) {
        float v = corr(d, n, ch, loop, c, nat, ov / pStep, pStep);
        if (v > bc) { bc = v; best = c; }
    }
    int32_t b0 = best;
    int32_t fStep = ov / 128 > 0 ? ov / 128 : 1;
    bc = -3.4e38f;
    for (int32_t c = b0 - lagStep + 1; c < b0 + lagStep; c++) {
        if (!loop && c < 0) continue;
        float v = corr(d, n, ch, loop, c, nat, ov / fStep, fStep);
        if (v > bc) { bc = v; best = c; }
    }
    return best;
}

bool tsNext(TsState& s, const int16_t* data, uint32_t frames, int ch, bool loop,
            int32_t& outL, int32_t& outR) {
    int32_t n = (int32_t)frames;
    if (!loop && s.t >= (float)frames) return false;     // full length played

    // A new grain, every `hop` frames of the stretched sample.
    if (s.untilNext <= 0.0f) {
        int32_t want = (int32_t)floorf(s.src + 0.5f);
        int32_t nat  = s.prev + s.hop;
        int32_t st   = bestStart(s, data, n, ch, loop, want, nat);
        if (s.ng < TS_MAX_GRAINS) {
            TsGrain& g = s.g[s.ng++];
            g.start = st;
            g.o     = -s.untilNext;                      // started that far back
            g.first = false;
        }
        s.prev = st;
        s.src += s.srcHop;
        s.untilNext += (float)s.hop;
        if (loop && s.src >= (float)frames) {            // keep the numbers small
            s.src  -= (float)frames;
            s.prev -= n;
        }
    }

    float accL = 0.0f, accR = 0.0f;
    float invLen = 1.0f / (float)s.len;
    for (int i = 0; i < s.ng; ) {
        TsGrain& g = s.g[i];
        float w = (g.first && g.o < (float)s.hop) ? 1.0f : win(g.o * invLen);
        int32_t ip = (int32_t)g.o;
        float   f  = g.o - (float)ip;
        float L, R;
        readFrame(data, n, ch, loop, g.start + ip, f, L, R);
        accL += L * w;
        accR += R * w;
        g.o += s.step;
        if (g.o >= (float)s.len) {                       // done: swap with the last
            s.g[i] = s.g[--s.ng];
            continue;
        }
        i++;
    }
    s.untilNext -= s.step;

    // The length clock: the sample's own time, one output frame's worth.
    s.t += s.base;
    if (loop) {
        if (s.t >= (float)frames) s.t -= (float)frames;
    } else {
        float left = ((float)frames - s.t) / s.base;     // output frames to go
        if (left < (float)TS_END_FADE) {
            float k = left > 0.0f ? left / (float)TS_END_FADE : 0.0f;
            accL *= k;
            accR *= k;
        }
    }

    if (accL >  32767.0f) accL =  32767.0f;
    if (accL < -32768.0f) accL = -32768.0f;
    if (accR >  32767.0f) accR =  32767.0f;
    if (accR < -32768.0f) accR = -32768.0f;
    outL = (int32_t)accL;
    outR = (int32_t)accR;
    return true;
}

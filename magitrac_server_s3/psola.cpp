#include "psola.h"
#include <math.h>

#define PSOLA_WIN_N 128
static float sPsolaWin[PSOLA_WIN_N + 1];          // Hann, 0..1 over phase 0..1 (516 bytes)
static bool  sPsolaWinUp = false;

static void winInit() {
    if (sPsolaWinUp) return;
    for (int i = 0; i <= PSOLA_WIN_N; i++)
        sPsolaWin[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / PSOLA_WIN_N);
    sPsolaWinUp = true;
}

static inline float win(float ph) {
    float x = ph * PSOLA_WIN_N;
    int   i = (int)x;
    if (i >= PSOLA_WIN_N) return 0.0f;
    float f = x - i;
    return sPsolaWin[i] + (sPsolaWin[i + 1] - sPsolaWin[i]) * f;
}

// Level correction, by the pitch change and the grain length (in periods).
// Set from the measurements in test_psola.cpp — see there.
//   rising, two-period grains:  overlap keeps the level (within 1 dB to +19)
//   lowering: gaps between the grains — power ~ 0.75 x ratio (Hann energy),
//             +0.4 dB measured; blended in dB between half and unity speed
//   big rises: grains cut shorter than two periods lose level with them
static float psolaLevel(float ratio, float grainPeriods) {
    float g = 1.0f;
    if (ratio < 1.0f) {
        float amp;
        if (ratio <= 0.5f) amp = sqrtf(0.75f * ratio) * 1.047f;
        else               amp = powf(10.0f, 3.86f * log2f(ratio) / 20.0f);
        g = 1.0f / amp;
    }
    if (grainPeriods < 2.0f && grainPeriods > 0.1f) g *= 2.0f / grainPeriods;
    return g;
}

void psolaStart(PsolaState& s, uint32_t srcRate, float periodSrc, float ratio, uint32_t outRate) {
    winInit();
    if (ratio < 0.0625f) ratio = 0.0625f;     // ±4 octaves (the TUNE range)
    if (ratio > 16.0f)   ratio = 16.0f;
    s.base   = (float)srcRate / (float)outRate;
    s.whole  = (srcRate % outRate) == 0;
    s.period = periodSrc > 1.0f ? periodSrc : 0.015f * srcRate;   // unpitched: ~15 ms grains
    s.spacing = (s.period / s.base) / ratio;                       // output frames
    // Grains two periods long, cut on the waveform's cycles: laid down closer
    // together raises the pitch, further apart (leaving gaps) lowers it.  No
    // more than PSOLA_MAX_GRAINS - 1 spacings long, so big rises fit.
    float glen = 2.0f * s.period;
    float maxLen = (PSOLA_MAX_GRAINS - 1) * s.spacing * s.base;
    if (glen > maxLen) glen = maxLen;
    s.glen   = glen;
    s.phStep = s.base / glen;
    s.norm   = psolaLevel(ratio, glen / s.period);
    s.t = 0.0f;
    s.untilNext = 0.0f;
    s.ending = false;
    s.ng = 0;
    // The first grain starts at its peak on the very first frame, so the
    // attack isn't faded in over half a grain.
    s.g[0].pos = 0.0f;
    s.g[0].ph  = 0.5f;
    s.ng = 1;
    s.untilNext = s.spacing * 0.5f;
}

// One source sample of channel c at whole frame i: wrapped for a loop,
// silence outside a one-shot.
static inline float at(const int16_t* d, int32_t frames, int ch, bool loop, int32_t i, int c) {
    if (i < 0 || i >= frames) {
        if (!loop) return 0.0f;
        i %= frames;
        if (i < 0) i += frames;
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

static inline void readFrame(const int16_t* d, uint32_t frames, int ch, bool loop, bool whole,
                             float pos, float& L, float& R) {
    int32_t n = (int32_t)frames;
    int32_t i = (int32_t)floorf(pos);
    if (whole) {                                     // pos is a whole frame: exact
        L = at(d, n, ch, loop, i, 0);
        R = ch == 2 ? at(d, n, ch, loop, i, 1) : L;
        return;
    }
    if (!loop && (i < -2 || i > n + 1)) { L = R = 0.0f; return; }
    float f = pos - (float)i;
    L = cubic(at(d, n, ch, loop, i - 1, 0), at(d, n, ch, loop, i, 0),
              at(d, n, ch, loop, i + 1, 0), at(d, n, ch, loop, i + 2, 0), f);
    R = ch == 2 ? cubic(at(d, n, ch, loop, i - 1, 1), at(d, n, ch, loop, i, 1),
                        at(d, n, ch, loop, i + 1, 1), at(d, n, ch, loop, i + 2, 1), f)
                : L;
}

bool psolaNext(PsolaState& s, const int16_t* data, uint32_t frames, int ch, bool loop,
               int32_t& outL, int32_t& outR) {
    // A new grain: on the waveform cycle nearest where we are now.
    if (!s.ending && s.untilNext <= 0.0f) {
        if (s.ng < PSOLA_MAX_GRAINS) {
            float mark = floorf(s.t / s.period + 0.5f) * s.period;
            PsolaGrain& g = s.g[s.ng++];
            g.pos = mark - s.glen * 0.5f;
            if (s.whole) g.pos = floorf(g.pos + 0.5f);
            g.ph  = 0.0f;
        }
        s.untilNext += s.spacing;
    }
    s.untilNext -= 1.0f;

    float accL = 0.0f, accR = 0.0f;
    for (int i = 0; i < s.ng; ) {
        PsolaGrain& g = s.g[i];
        float w = win(g.ph);
        float L, R;
        readFrame(data, frames, ch, loop, s.whole, g.pos, L, R);
        accL += L * w;
        accR += R * w;
        g.pos += s.base;
        g.ph  += s.phStep;
        if (g.ph >= 1.0f) {                      // done: drop it (swap with the last)
            s.g[i] = s.g[--s.ng];
            continue;
        }
        i++;
    }

    s.t += s.base;
    if (s.t >= (float)frames) {
        if (loop) s.t -= (float)frames;
        else      s.ending = true;
    }

    float l = accL * s.norm, r = accR * s.norm;
    if (l >  32767.0f) l =  32767.0f;
    if (l < -32768.0f) l = -32768.0f;
    if (r >  32767.0f) r =  32767.0f;
    if (r < -32768.0f) r = -32768.0f;
    outL = (int32_t)l;
    outR = (int32_t)r;
    return !(s.ending && s.ng == 0);
}

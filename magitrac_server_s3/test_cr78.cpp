// Host test for cr78.cpp (the organ's CR78 sound).
//
// Run from Terminal — NOT through the Arduino IDE (it writes a binary here —
// don't commit it):
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++17 -O2 -Wall -DUNIT_TEST -I test_mocks -I . -I ../magitrac_lib/src \
//       test_cr78.cpp -o test_cr78 && ./test_cr78 [wav-dir]
// With a directory it also writes each drum there as a 32 kHz WAV, and a
// short beat (beat.wav), to listen to.  `./test_cr78 --fit` prints the
// RAW_PEAK table for cr78.cpp.
#ifndef ARDUINO
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <chrono>
#include "test_mocks/Arduino.h"
#include "cr78.cpp"

static const float FS = 32000.0f;
static const int   BLK = 128;
static int sPass = 0, sFail = 0;
static void check(bool ok, const char* what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    ok ? sPass++ : sFail++;
}

static float sWork[CR78_WORK_FLOATS];
static const char* NAMES[CR_COUNT] = {
    "bd", "sd", "rs", "hh", "cy", "ma", "cl", "hb", "lb", "lc", "cb", "tb", "gu", "mb",
};
static const uint8_t NOTE[CR_COUNT] = { 36, 38, 37, 42, 49, 70, 75, 60, 61, 64, 56, 54, 74, 81 };

static void writeWav(const char* dir, const char* name, const float* x, int n) {
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.wav", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) { printf("  (can't write %s)\n", path); return; }
    uint32_t data = (uint32_t)n * 2, rate = (uint32_t)FS, byteRate = rate * 2, riff = 36 + data;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    uint32_t fmtLen = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtLen, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int i = 0; i < n; i++) {
        float v = x[i] * 0.5f * 32767.0f;     // the organ puts the bus's ±1 at -6 dBFS
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        int16_t s = (int16_t)lrintf(v);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

struct Hit { float peak, gain; int len, peakAt; bool finite, finished; };

// One drum on its own, a block at a time as the organ does it.
static Hit renderOne(int inst, uint8_t note, uint8_t vel, const int* sl, float* buf, int maxN,
                     int preroll = 0, int cnt = 5) {
    Cr78Shared d;
    cr78Init(d, FS, sWork);
    float junk[BLK];
    for (int i = 0; i < preroll; i++) cr78Prepare(d, sl, 0xFFFF, BLK, cnt);   // let the free-runners run
    cr78Prepare(d, sl, 0, 0, cnt);
    Cr78Voice v;
    cr78NoteOn(v, inst, note, vel, d);
    Hit h = { 0, v.gain, 0, 0, true, false };
    memset(buf, 0, sizeof(float) * maxN);
    (void)junk;
    for (int pos = 0; pos < maxN; pos += BLK) {
        int n = (maxN - pos < BLK) ? maxN - pos : BLK;
        cr78Prepare(d, sl, 1u << inst, n, cnt);
        bool more = cr78Render(v, d, buf + pos, n);
        if (!more) { h.finished = true; h.len = pos + n; break; }
    }
    if (!h.finished) h.len = maxN;
    for (int i = 0; i < maxN; i++) {
        if (!isfinite(buf[i])) h.finite = false;
        if (fabsf(buf[i]) > h.peak) { h.peak = fabsf(buf[i]); h.peakAt = i; }
    }
    return h;
}

// Seconds from the peak until the 2 ms envelope stays under a tenth of it
// (Roland's decay figure: time to -20 dB).
static float decayTo10(const float* x, int n, const Hit& h) {
    int win = (int)(0.002f * FS), last = h.peakAt;
    for (int i = h.peakAt; i < n; i += win / 2) {
        float m = 0;
        for (int j = i; j < i + win && j < n; j++) m = fmaxf(m, fabsf(x[j]));
        if (m >= h.peak * 0.1f) last = i;
    }
    return (float)(last - h.peakAt) / FS;
}

// Frequency from upward zero crossings between a and b seconds.
static float zcFreq(const float* x, float a, float b) {
    int i0 = (int)(a * FS), i1 = (int)(b * FS), first = -1, lastC = -1, c = 0;
    for (int i = i0 + 1; i < i1; i++)
        if (x[i - 1] < 0 && x[i] >= 0) { if (first < 0) first = i; else c++; lastC = i; }
    return (c > 0) ? c * FS / (float)(lastC - first) : 0.0f;
}

static float buf[(int)(3 * 32000)], buf2[(int)(3 * 32000)];
static const int N = (int)(3 * 32000);

int main(int argc, char** argv) {
    bool fit = argc > 1 && !strcmp(argv[1], "--fit");
    const char* dir = (argc > 1 && !fit) ? argv[1] : nullptr;
    const int s4[5] = { 4, 4, 4, 4, 4 };

    if (fit) {
        printf("static const float RAW_PEAK[CR_COUNT] = {   // test_cr78 --fit\n   ");
        for (int i = 0; i < CR_COUNT; i++) {
            Hit h = renderOne(i, NOTE[i], 127, s4, buf, N);
            // Noisy and free-running voices: the loudest of several hits.
            float pk = h.peak / h.gain;
            for (int k = 1; k < 8; k++) {
                Hit h2 = renderOne(i, NOTE[i], 127, s4, buf, N, k * 7);
                pk = fmaxf(pk, h2.peak / h2.gain);
            }
            printf(" %.4ff,", pk);
        }
        printf("\n};\n");
        return 0;
    }

    printf("GM map\n");
    check(cr78InstForNote(36) == CR_BD && cr78InstForNote(35) == CR_BD &&
          cr78InstForNote(38) == CR_SD && cr78InstForNote(40) == CR_SD &&
          cr78InstForNote(37) == CR_RS && cr78InstForNote(42) == CR_HH &&
          cr78InstForNote(44) == CR_HH && cr78InstForNote(46) == CR_CY &&
          cr78InstForNote(49) == CR_CY && cr78InstForNote(51) == CR_CY,
          "kick, snare, rim, hats, cymbals");
    check(cr78InstForNote(41) == CR_LC && cr78InstForNote(45) == CR_LB && cr78InstForNote(48) == CR_HB &&
          cr78InstForNote(64) == CR_LC && cr78InstForNote(60) == CR_HB && cr78InstForNote(61) == CR_LB,
          "toms and congas play conga / low bongo / high bongo");
    check(cr78InstForNote(56) == CR_CB && cr78InstForNote(54) == CR_TB && cr78InstForNote(70) == CR_MA &&
          cr78InstForNote(75) == CR_CL && cr78InstForNote(73) == CR_GU && cr78InstForNote(81) == CR_MB,
          "cowbell, tambourine, maracas, claves, guiro, metallic beat");
    check(cr78InstForNote(39) == -1 && cr78InstForNote(34) == -1 && cr78InstForNote(83) == -1 &&
          cr78InstForNote(0) == -1 && cr78InstForNote(127) == -1, "no clap; off the map is silent");

    printf("Every drum\n");
    {
        bool fin = true, done = true, sane = true, lvl = true;
        char bad[256] = "";
        for (int i = 0; i < CR_COUNT; i++) {
            Hit h = renderOne(i, NOTE[i], 127, s4, buf, N);
            fin &= h.finite;
            done &= h.finished;
            if (h.peak < 0.02f || h.peak > 1.2f) { sane = false; snprintf(bad + strlen(bad), 64, " %s=%.2f", NAMES[i], h.peak); }
            float want = KICK_PEAK * powf(10.0f, MIX_DB[i] / 20.0f);
            float db = 20.0f * log10f(h.peak / want);
            if (db > 1.5f || db < -6.0f) { lvl = false; snprintf(bad + strlen(bad), 64, " %s %+.1fdB", NAMES[i], db); }
            writeWav(dir, NAMES[i], buf, h.len);
        }
        check(fin, "finite");
        check(done, "every drum plays out and frees its voice");
        check(sane, "every drum sounds, none past the bus");
        check(lvl, "kit balance: each drum near CW-78's mix level");
        if (bad[0]) printf("     %s\n", bad);
    }

    printf("Roland's alignment (page 30)\n");
    {
        Hit h = renderOne(CR_BD, 36, 127, s4, buf, N);
        float f = zcFreq(buf, 0.03f, 0.15f), dec = decayTo10(buf, N, h);
        printf("     kick %.1f Hz, decay %.0f ms\n", f, dec * 1000);
        check(f > 58 && f < 67, "kick rings at 62.5 Hz");
        check(dec > 0.07f && dec < 0.16f, "kick decays to a tenth in ~100 ms");

        h = renderOne(CR_LC, 64, 127, s4, buf, N);
        f = zcFreq(buf, 0.01f, 0.08f); dec = decayTo10(buf, N, h);
        printf("     conga %.1f Hz, decay %.0f ms\n", f, dec * 1000);
        check(f > 198 && f < 220 && dec > 0.10f && dec < 0.21f, "conga 208 Hz, ~150 ms");

        h = renderOne(CR_HB, 60, 127, s4, buf, N);
        f = zcFreq(buf, 0.004f, 0.03f);
        printf("     high bongo %.1f Hz\n", f);
        check(f > 570 && f < 630, "high bongo 600 Hz");

        h = renderOne(CR_RS, 37, 127, s4, buf, N);
        f = zcFreq(buf, 0.0015f, 0.008f); dec = decayTo10(buf, N, h);
        printf("     rim shot %.0f Hz, decay %.1f ms\n", f, dec * 1000);
        check(f > 1400 && f < 1560, "rim shot 1480 Hz");
        check(dec > 0.002f && dec < 0.009f, "rim shot ~5 ms");

        h = renderOne(CR_CL, 75, 127, s4, buf, N);
        f = zcFreq(buf, 0.0015f, 0.015f); dec = decayTo10(buf, N, h);
        printf("     claves %.0f Hz, decay %.1f ms\n", f, dec * 1000);
        check(f > 2500 && f < 2760 && dec > 0.011f && dec < 0.026f, "claves 2630 Hz, ~18 ms");

        h = renderOne(CR_HH, 42, 127, s4, buf, N);
        dec = decayTo10(buf, N, h);
        float hhDec = dec;
        h = renderOne(CR_CY, 49, 127, s4, buf, N);
        dec = decayTo10(buf, N, h);
        printf("     hat %.0f ms, cymbal %.0f ms\n", hhDec * 1000, dec * 1000);
        check(hhDec > 0.035f && hhDec < 0.09f, "hi-hat ~60 ms");
        check(dec > 0.22f && dec < 0.50f, "cymbal ~350 ms");
    }

    printf("Sliders\n");
    {
        int sl[5] = { 8, 4, 4, 4, 4 };
        Hit a = renderOne(CR_BD, 36, 127, s4, buf, N);
        Hit b = renderOne(CR_BD, 36, 127, sl, buf2, N);
        check(decayTo10(buf2, N, b) > 2.5f * decayTo10(buf, N, a), "BD DEC lengthens the kick");
        check(b.peak > a.peak, "...and, as on the hardware, a longer ring is louder");
        int tu[5] = { 4, 4, 4, 4, 8 };
        renderOne(CR_BD, 36, 127, tu, buf2, N);
        float f0 = zcFreq(buf, 0.03f, 0.15f), f1 = zcFreq(buf2, 0.03f, 0.15f);
        check(f1 > f0 * 1.3f && f1 < f0 * 1.55f, "TUNE 8 = 6 semitones up");
        int hh[5] = { 4, 4, 8, 4, 4 }, cy[5] = { 4, 4, 4, 0, 4 };
        Hit c = renderOne(CR_HH, 42, 127, hh, buf2, N);
        a = renderOne(CR_HH, 42, 127, s4, buf, N);
        check(c.len > 3 * a.len, "HH DEC lengthens the hat");
        c = renderOne(CR_CY, 49, 127, cy, buf2, N);
        a = renderOne(CR_CY, 49, 127, s4, buf, N);
        check(c.len * 3 < a.len, "CY DEC shortens the cymbal");

        // SNAPPY: the snare's high end (sample-to-sample difference) against its shell.
        auto hiFrac = [](const float* x, int n) {
            double hi = 0, all = 0;
            for (int i = 1; i < n; i++) { hi += (x[i] - x[i - 1]) * (x[i] - x[i - 1]); all += x[i] * x[i]; }
            return hi / (all + 1e-12);
        };
        int s0[5] = { 4, 0, 4, 4, 4 }, s8[5] = { 4, 8, 4, 4, 4 };
        Hit p = renderOne(CR_SD, 38, 127, s0, buf, N);
        Hit q = renderOne(CR_SD, 38, 127, s8, buf2, N);
        check(hiFrac(buf2, q.len) > 3 * hiFrac(buf, p.len), "SNAPPY brings in the snap");
    }

    printf("Velocity\n");
    {
        Hit a = renderOne(CR_BD, 36, 127, s4, buf, N);
        Hit b = renderOne(CR_BD, 36, 40, s4, buf2, N);
        check(b.peak < a.peak * 0.6f && b.peak > a.peak * 0.05f, "a soft kick is quieter");
        Hit c = renderOne(CR_SD, 38, 100, s4, buf2, N);
        Hit d = renderOne(CR_SD, 38, 127, s4, buf, N);
        check(c.peak < d.peak && c.peak > d.peak * 0.5f, "above 70% the trigger voltage alone moves it");
    }

    printf("The nine extra settings\n");
    {
        auto with = [](int k, int v, int* o) { for (int i = 0; i < ORGAN_DRUM_PARAMS; i++) o[i] = 4; o[k] = v; };
        int x[ORGAN_DRUM_PARAMS], y[ORGAN_DRUM_PARAMS];
        with(0, 4, x);
        Hit a = renderOne(CR_SD, 38, 127, x, buf, N, 0, ORGAN_DRUM_PARAMS);
        Hit b = renderOne(CR_SD, 38, 127, s4, buf2, N);
        check(a.peak == b.peak, "all 14 at 4 = the five-slider CR78");
        with(5, 0, x); with(5, 8, y);
        renderOne(CR_BD, 36, 127, x, buf, N, 0, ORGAN_DRUM_PARAMS);
        renderOne(CR_BD, 36, 127, y, buf2, N, 0, ORGAN_DRUM_PARAMS);
        double d0 = 0, d8 = 0;
        for (int i = 1; i < 320; i++) { d0 += fabs(buf[i] - buf[i-1]); d8 += fabs(buf2[i] - buf2[i-1]); }
        check(d8 > 2 * d0, "BD TONE is the kick's click");
        with(6, 8, x);
        renderOne(CR_BD, 36, 127, x, buf, N, 0, ORGAN_DRUM_PARAMS);
        float fk = zcFreq(buf, 0.03f, 0.15f);
        renderOne(CR_LC, 64, 127, x, buf2, N, 0, ORGAN_DRUM_PARAMS);
        float fc = zcFreq(buf2, 0.01f, 0.08f);
        check(fk > 62.5f * 1.3f && fc > 198 && fc < 220, "BD TUNE moves the kick alone");
        with(8, 2, x);
        Hit r2 = renderOne(CR_SD, 38, 127, x, buf, N, 0, ORGAN_DRUM_PARAMS);
        with(8, 6, y);
        Hit r6 = renderOne(CR_SD, 38, 127, y, buf2, N, 0, ORGAN_DRUM_PARAMS);
        check(r6.len > r2.len, "SD RING lengthens the shell");
        with(10, 8, x);
        Hit p8 = renderOne(CR_LC, 64, 127, x, buf, N, 0, ORGAN_DRUM_PARAMS);
        Hit p4 = renderOne(CR_LC, 64, 127, s4, buf2, N);
        check(decayTo10(buf, N, p8) > 2.5f * decayTo10(buf2, N, p4), "PERC DEC lengthens the conga");
        with(11, 0, x);
        Hit k0 = renderOne(CR_BD, 36, 127, x, buf, N, 0, ORGAN_DRUM_PARAMS);
        check(k0.peak == 0 && k0.finished, "KICK level 0 silences the kick (and frees it)");
        with(13, 8, x);
        Hit c8 = renderOne(CR_CB, 56, 127, x, buf, N, 0, ORGAN_DRUM_PARAMS);
        Hit c4 = renderOne(CR_CB, 56, 127, s4, buf2, N);
        check(fabsf(c8.gain / c4.gain - 3.98f) < 0.1f, "PERC level 8 = +12 dB on the cowbell");
        with(9, 8, x);
        Hit hh = renderOne(CR_HH, 42, 127, x, buf, N, 0, ORGAN_DRUM_PARAMS);
        double h0 = 0, h8 = 0;
        for (int i = 1; i < hh.len; i++) h8 += fabs(buf[i] - buf[i-1]);
        Hit hd = renderOne(CR_HH, 42, 127, s4, buf2, N);
        for (int i = 1; i < hd.len; i++) h0 += fabs(buf2[i] - buf2[i-1]);
        check(h8 / hh.peak != h0 / hd.peak, "HAT TONE changes the hat");
    }

    printf("Character\n");
    {
        renderOne(CR_MB, 81, 127, s4, buf, N, 0);
        renderOne(CR_MB, 81, 127, s4, buf2, N, 3);
        double diff = 0;
        for (int i = 0; i < 1600; i++) diff += fabsf(buf[i] - buf2[i]);
        check(diff > 1.0, "metallic beat free-runs: no two hits alike");
        Hit s = renderOne(CR_GU, 73, 127, s4, buf, N);
        Hit l = renderOne(CR_GU, 74, 127, s4, buf2, N);
        check(s.len < l.len, "short guiro is shorter than the long one");
        // The scrape is ticks at the multivibrator rate, ringing at 6.1 kHz.
        float f = zcFreq(buf, 0.0005f, 0.0025f);
        check(f > 5000 && f < 7200, "guiro teeth ring the 6126 Hz tank");
    }

    printf("A beat\n");
    {
        // Bossa nova-ish, 120 BPM sixteenths, four bars.
        const int bars = 4, steps = 16 * bars, stepN = (int)(FS * 0.125f);
        static float beat[16 * 4 * 4000 + 32000];
        int total = steps * stepN + 32000;
        memset(beat, 0, sizeof(float) * total);
        Cr78Shared d;
        cr78Init(d, FS, sWork);
        Cr78Voice vs[16];
        for (auto& v : vs) v.inst = CR_DONE;
        int nextV = 0;
        const char* pat[] = {
            "x..x..x.x..x..x.",   // bd
            "....x.......x...",   // sd
            "x.x.x.x.x.x.x.x.",   // hh
            "..x..x....x..x..",   // cl
            "xxxxxxxxxxxxxxxx",   // ma
        };
        const int inst[] = { CR_BD, CR_SD, CR_HH, CR_CL, CR_MA };
        const uint8_t vel[] = { 120, 110, 90, 100, 70 };
        auto t0 = std::chrono::steady_clock::now();
        for (int pos = 0; pos < total; pos += BLK) {
            int step = pos / stepN;
            if (pos % stepN < BLK && step < steps)
                for (int k = 0; k < 5; k++)
                    if (pat[k][step % 16] == 'x') {
                        cr78NoteOn(vs[nextV], inst[k], NOTE[inst[k]], vel[k], d);
                        nextV = (nextV + 1) & 15;
                    }
            uint32_t mask = 0;
            for (auto& v : vs) if (v.inst != CR_DONE) mask |= 1u << v.inst;
            int n = (total - pos < BLK) ? total - pos : BLK;
            cr78Prepare(d, s4, mask, n);
            for (auto& v : vs) cr78Render(v, d, beat + pos, n);
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        float pk = 0;
        bool fin = true;
        for (int i = 0; i < total; i++) { pk = fmaxf(pk, fabsf(beat[i])); fin &= isfinite(beat[i]); }
        printf("     %.1f s of beat in %.1f ms on the Mac; peak %.2f\n", total / FS, secs * 1000, pk);
        check(fin && pk < 1.5f, "a busy pattern stays on the bus");
        writeWav(dir, "beat", beat, total);
    }

    printf("\n%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
#endif

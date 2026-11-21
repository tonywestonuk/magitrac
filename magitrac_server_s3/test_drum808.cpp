// Host test for drum808.cpp (the organ's 808 sound).
//
// Run from Terminal — NOT through the Arduino IDE (it writes a binary here —
// don't commit it):
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++17 -O2 -Wall -DUNIT_TEST -I test_mocks -I . -I ../magitrac_lib/src \
//       test_drum808.cpp -o test_drum808 && ./test_drum808 [wav-dir]
// With a directory it also writes each drum there as a 32 kHz WAV, and a
// short beat (beat.wav), to listen to.
#ifndef ARDUINO
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <chrono>
#include "test_mocks/Arduino.h"
#include "drum808.cpp"

static const float FS = 32000.0f;
static const int   BLK = 128;
static int sPass = 0, sFail = 0;
static void check(bool ok, const char* what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    ok ? sPass++ : sFail++;
}

static float sWork[DRUM_WORK_FLOATS];

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

struct Hit { float peak, rms; int len; bool finite, finished; };

// One drum on its own, a block at a time as the organ does it.
static Hit renderOne(int inst, uint8_t vel, const int* sl, float* buf, int maxN, int cnt = 5) {
    DrumShared d;
    drumInit(d, FS, sWork);
    DrumVoice v;
    drumPrepare(d, sl, 0, 0, cnt);
    drumNoteOn(v, inst, vel, d);
    Hit h = { 0, 0, 0, true, false };
    double sum = 0;
    memset(buf, 0, sizeof(float) * maxN);
    for (int at = 0; at < maxN; at += BLK) {
        int n = (maxN - at < BLK) ? maxN - at : BLK;
        bool live = v.inst != DR_DONE;
        drumPrepare(d, sl, live ? (1u << inst) : 0, n, cnt);
        if (live && !drumRender(v, d, buf + at, n) && !h.finished) { h.finished = true; h.len = at + n; }
    }
    for (int i = 0; i < maxN; i++) {
        if (!std::isfinite(buf[i])) h.finite = false;
        float a = fabsf(buf[i]);
        if (a > h.peak) h.peak = a;
        sum += (double)buf[i] * buf[i];
    }
    h.rms = (float)sqrt(sum / maxN);
    return h;
}

// Frequency from upward zero crossings in [a, b).
static float zcHz(const float* x, int a, int b) {
    int first = -1, last = -1, n = 0;
    for (int i = a + 1; i < b; i++)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) { if (first < 0) first = i; last = i; n++; }
    return (n < 2) ? 0.0f : (float)(n - 1) * FS / (float)(last - first);
}

static float rmsOf(const float* x, int a, int b) {
    double s = 0;
    for (int i = a; i < b; i++) s += (double)x[i] * x[i];
    return (float)sqrt(s / (b - a));
}

// Level of the sample-to-sample change: how much high end there is.
static float diffRms(const float* x, int n) {
    double s = 0;
    for (int i = 1; i < n; i++) s += (double)(x[i] - x[i - 1]) * (x[i] - x[i - 1]);
    return (float)sqrt(s / (n - 1));
}

static const char* NAMES[DR_COUNT] = {
    "bd", "sd", "lt", "mt", "ht", "lc", "mc", "hc", "rs", "cl", "cp", "ma", "cb", "cy", "oh", "ch" };

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : nullptr;
    const int def[5] = { 4, 4, 4, 4, 4 };
    static float buf[(int)(2.0f * 32000)];
    const int N = (int)(2.0f * FS);

    printf("DrumVoice %zu bytes (must fit ProcVoiceState, 64)\n", sizeof(DrumVoice));
    check(sizeof(DrumVoice) <= 64, "DrumVoice fits the voice table");

    printf("Every drum at velocity 100:\n");
    for (int i = 0; i < DR_COUNT; i++) {
        Hit h = renderOne(i, 100, def, buf, N);
        printf("    %-3s peak %.3f  rms %.4f  %5.0f ms%s\n", NAMES[i], h.peak, h.rms,
               h.len * 1000.0f / FS, h.finite ? "" : "  NOT FINITE");
        char what[64];
        snprintf(what, sizeof(what), "%s: finite, audible, under 2, plays out", NAMES[i]);
        check(h.finite && h.peak > 0.05f && h.peak < 2.0f && h.finished, what);
        writeWav(dir, NAMES[i], buf, h.finished ? h.len + 1600 : N);
    }

    printf("Kick:\n");
    {
        renderOne(DR_BD, 100, def, buf, N);
        float early = zcHz(buf, (int)(0.004f * FS), (int)(0.040f * FS));
        float late  = zcHz(buf, (int)(0.200f * FS), (int)(0.290f * FS));
        printf("    pitch %.0f Hz early, %.0f Hz late\n", early, late);
        check(early > 65 && early < 100, "starts high (98 Hz sweep)");
        // io-808's sweep is exponential over the whole decay (300 ms here):
        // 98 × (48/98)^(245/300) = 55 Hz in the middle of that window.
        check(late > 51 && late < 59, "sweeps down toward 48 Hz over the decay");
        int sl[5] = { 0, 4, 4, 4, 4 };
        Hit s = renderOne(DR_BD, 100, sl, buf, N);
        sl[0] = 8;
        Hit l = renderOne(DR_BD, 100, sl, buf, N);
        printf("    BD DEC 0: %.0f ms, 8: %.0f ms\n", s.len * 1000 / FS, l.len * 1000 / FS);
        check(s.len < l.len && l.len * 1000 / FS > 540, "BD DEC sets the length (50 .. 550 ms)");
        sl[0] = 4; sl[1] = 0;
        renderOne(DR_BD, 100, sl, buf, N);
        float dark = diffRms(buf, (int)(0.01f * FS));
        sl[1] = 8;
        renderOne(DR_BD, 100, sl, buf, N);
        float bright = diffRms(buf, (int)(0.01f * FS));
        check(bright > dark, "BD TONE opens the attack up");
        writeWav(dir, "bd_long", buf, N / 2);
    }

    printf("Snare SNAPPY:\n");
    {
        int sl[5] = { 4, 4, 0, 4, 4 };
        renderOne(DR_SD, 100, sl, buf, N);
        float tail0 = rmsOf(buf, (int)(0.055f * FS), (int)(0.070f * FS));
        sl[2] = 8;
        renderOne(DR_SD, 100, sl, buf, N);
        float tail8 = rmsOf(buf, (int)(0.055f * FS), (int)(0.070f * FS));
        printf("    noise tail rms %.4f at 0, %.4f at 8\n", tail0, tail8);
        check(tail0 < 1e-6f && tail8 > 0.01f, "SNAPPY is the noise");
    }

    printf("Velocity:\n");
    {
        Hit a = renderOne(DR_SD, 127, def, buf, N);
        Hit b = renderOne(DR_SD, 100, def, buf, N);
        Hit c = renderOne(DR_SD, 40, def, buf, N);
        printf("    rms 127: %.4f  100: %.4f  40: %.4f\n", a.rms, b.rms, c.rms);
        check(a.rms > b.rms * 1.2f && c.rms < b.rms * 0.4f, "velocity is the accent");
    }

    printf("The nine extra settings:\n");
    {
        auto with = [](int k, int v, int* out) { for (int i = 0; i < ORGAN_DRUM_PARAMS; i++) out[i] = 4; out[k] = v; };
        int x[ORGAN_DRUM_PARAMS], y[ORGAN_DRUM_PARAMS];
        static float b2[(int)(2.0f * 32000)];
        with(0, 4, x);
        Hit h4 = renderOne(DR_SD, 100, x, buf, N, ORGAN_DRUM_PARAMS);
        Hit h5 = renderOne(DR_SD, 100, def, b2, N);
        check(fabsf(h4.rms - h5.rms) < 1e-6f, "all 14 at 4 = the five-slider 808");
        with(5, 0, x); with(5, 8, y);
        Hit t0 = renderOne(DR_SD, 100, x, buf, N, ORGAN_DRUM_PARAMS);
        Hit t8 = renderOne(DR_SD, 100, y, b2, N, ORGAN_DRUM_PARAMS);
        // In the noise's tail, past the 50 ms tone: how fast it moves for its size.
        double d0 = 0, d8 = 0, r0 = 0, r8 = 0;
        for (int i = (int)(0.052f * FS); i < (int)(0.072f * FS); i++) {
            d0 += fabs(buf[i] - buf[i-1]); d8 += fabs(b2[i] - b2[i-1]);
            r0 += fabs(buf[i]);           r8 += fabs(b2[i]);
        }
        (void)t0; (void)t8;
        check(d8 / r8 > 1.15 * d0 / r0, "SD TONE brightens the snare");
        with(7, 0, x); with(7, 8, y);
        renderOne(DR_CY, 100, x, buf, N, ORGAN_DRUM_PARAMS);
        renderOne(DR_CY, 100, y, b2, N, ORGAN_DRUM_PARAMS);
        float cy0 = rmsOf(buf, (int)(0.4f * FS), (int)(0.8f * FS)), cy8 = rmsOf(b2, (int)(0.4f * FS), (int)(0.8f * FS));
        check(cy8 > 10 * cy0, "CY DEC lengthens the cymbal");
        with(8, 0, x);
        Hit q = renderOne(DR_SD, 40, x, buf, N, ORGAN_DRUM_PARAMS);
        Hit l = renderOne(DR_SD, 127, x, b2, N, ORGAN_DRUM_PARAMS);
        check(fabsf(q.rms - l.rms) < 1e-4f, "ACCENT 0: every hit the same");
        with(9, 0, x);
        Hit k0 = renderOne(DR_BD, 100, x, buf, N, ORGAN_DRUM_PARAMS);
        check(k0.rms == 0 && k0.finished, "KICK level 0 silences the kick (and frees it)");
        with(10, 8, x);
        Hit s8 = renderOne(DR_SD, 100, x, buf, N, ORGAN_DRUM_PARAMS);
        check(fabsf(s8.rms / h4.rms - 3.98f) < 0.1f, "SNARE level 8 = +12 dB");
    }

    printf("TUNE:\n");
    {
        int sl[5] = { 4, 4, 4, 4, 0 };
        renderOne(DR_HC, 100, sl, buf, N);
        float lo = zcHz(buf, (int)(0.005f * FS), (int)(0.08f * FS));
        sl[4] = 8;
        renderOne(DR_HC, 100, sl, buf, N);
        float hi = zcHz(buf, (int)(0.005f * FS), (int)(0.08f * FS));
        printf("    high conga %.0f Hz at 0, %.0f Hz at 8\n", lo, hi);
        check(fabsf(lo - 370) < 12 && fabsf(hi - 455) < 12, "TUNE spans the drum's two pitches");
    }

    printf("Open hat choked by a closed hat:\n");
    {
        DrumShared d;
        drumInit(d, FS, sWork);
        DrumVoice oh;
        drumNoteOn(oh, DR_OH, 100, d);
        memset(buf, 0, sizeof(float) * N);
        int at = 0;
        for (; at < (int)(0.05f * FS); at += BLK) {
            drumPrepare(d, def, 1u << DR_OH, BLK);
            drumRender(oh, d, buf + at, BLK);
        }
        drumChoke(oh);
        int k = 0;
        bool alive = true;
        for (; alive && k < 40; k++, at += BLK) {
            drumPrepare(d, def, 1u << DR_OH, BLK);
            alive = drumRender(oh, d, buf + at, BLK);
        }
        printf("    gone %.1f ms after the choke\n", k * BLK * 1000.0f / FS);
        check(!alive && k * BLK * 1000.0f / FS < 20.0f, "a choked open hat stops within 20 ms");
    }

    printf("GM map:\n");
    check(drumInstForNote(36) == DR_BD && drumInstForNote(38) == DR_SD &&
          drumInstForNote(42) == DR_CH && drumInstForNote(46) == DR_OH &&
          drumInstForNote(49) == DR_CY && drumInstForNote(39) == DR_CP &&
          drumInstForNote(56) == DR_CB && drumInstForNote(75) == DR_CL,
          "kick, snare, hats, cymbal, clap, cowbell, claves where GM has them");
    check(drumInstForNote(34) < 0 && drumInstForNote(60 + 24) < 0 && drumInstForNote(58) < 0,
          "notes off the map are silent");

    // A beat with every voice going: stays finite, and how long it takes.
    printf("Beat:\n");
    {
        DrumShared d;
        drumInit(d, FS, sWork);
        const int V = 12;
        DrumVoice vs[V];
        for (int i = 0; i < V; i++) vs[i].inst = DR_DONE;
        const int steps = 32;
        const int stepN = (int)(FS * 60.0f / 120.0f / 4.0f);
        const int total = steps * stepN;
        static float beat[32 * 4000 + BLK];
        memset(beat, 0, sizeof(beat));
        //                         1 . . . 2 . . . 3 . . . 4 . . .
        const char* rows[] = {
            "bd", "x.........x.......x.......x..x..",
            "sd", "....x.......x.......x.......x.xx",
            "ch", "x.x.x.x.x.x.x...x.x.x.x.x.x.x...",
            "oh", "..............x...............x.",
            "cp", "............x...............x...",
            "lt", "........................x.......",
            "mt", "..........................x.....",
            "ht", "............................x...",
            "cb", "......x...............x.........",
            "cy", "x...............................",
            "rs", "...x.......x.......x.......x....",
            "ma", ".x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x",
        };
        double us = 0;
        int maxLive = 0;
        for (int at = 0; at < total; at += BLK) {
            for (int r = 0; r < (int)(sizeof(rows) / sizeof(rows[0])); r += 2) {
                int inst = -1;
                for (int k = 0; k < DR_COUNT; k++) if (!strcmp(rows[r], NAMES[k])) inst = k;
                for (int st = 0; st < steps; st++) {
                    int when = st * stepN;
                    if (rows[r + 1][st] != 'x' || when < at || when >= at + BLK) continue;
                    if (inst == DR_CH) for (int i = 0; i < V; i++) if (vs[i].inst == DR_OH) drumChoke(vs[i]);
                    int slot = 0; float lo = 1e9f;
                    for (int i = 0; i < V; i++) {
                        float l = (vs[i].inst == DR_DONE) ? -1.0f : drumLevel(vs[i]);
                        if (l < lo) { lo = l; slot = i; }
                    }
                    drumNoteOn(vs[slot], inst, (st % 4 == 0) ? 127 : 100, d);
                }
            }
            auto t0 = std::chrono::steady_clock::now();
            uint32_t mask = 0;
            int live = 0;
            for (int i = 0; i < V; i++) if (vs[i].inst != DR_DONE) { mask |= 1u << vs[i].inst; live++; }
            if (live > maxLive) maxLive = live;
            drumPrepare(d, def, mask, BLK);
            for (int i = 0; i < V; i++) drumRender(vs[i], d, beat + at, BLK);
            drumBus(beat + at, BLK);
            us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        }
        bool fin = true; float pk = 0;
        for (int i = 0; i < total; i++) { if (!std::isfinite(beat[i])) fin = false; pk = fmaxf(pk, fabsf(beat[i])); }
        printf("    %d voices at most, peak %.2f, %.1f us per block on this Mac\n",
               maxLive, pk, us / (total / BLK));
        check(fin && pk <= 1.0f, "two bars of everything: finite, limited to ±1");
        writeWav(dir, "beat", beat, total);
    }

    printf("\n%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
#endif

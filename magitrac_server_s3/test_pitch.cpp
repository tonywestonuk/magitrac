// Host tests for pitch_detect.cpp (ADD SAMPLE's auto-tune).
//
// Run from Terminal — NOT through the Arduino IDE:
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++11 -O2 test_pitch.cpp -o test_pitch && ./test_pitch
#ifndef ARDUINO
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include "pitch_detect.cpp"

static int sPass = 0, sFail = 0;
static const uint32_t RATE = 32000;

struct Partial { float mult, amp; };

// A tone: partials of f0, a slow fade in/out, plus white noise at noiseAmp.
static std::vector<int16_t> tone(float f0, std::vector<Partial> parts, float secs,
                                 float noiseAmp = 0, float level = 12000) {
    uint32_t n = (uint32_t)(secs * RATE);
    std::vector<int16_t> v(n);
    srand(1234);
    for (uint32_t i = 0; i < n; i++) {
        double t = (double)i / RATE, s = 0;
        for (auto& p : parts) s += p.amp * sin(2 * M_PI * f0 * p.mult * t + p.mult);
        double env = fmin(1.0, fmin(t / 0.02, (secs - t) / 0.05));
        s = s * env * level + noiseAmp * ((rand() / (double)RAND_MAX) * 2 - 1);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        v[i] = (int16_t)s;
    }
    return v;
}

static PitchResult run(const std::vector<int16_t>& v) {
    pitchBegin(v.data(), (uint32_t)v.size(), RATE);
    int steps = 0;
    while (!pitchStep()) steps++;
    return pitchResult();
}

static void expectPitch(const char* name, const std::vector<int16_t>& v, float hz) {
    PitchResult r = run(v);
    float cents = r.hz > 0 ? 1200.0f * log2f(r.hz / hz) : 9999;
    bool ok = r.found && fabsf(cents) <= 3.0f;
    printf("  %s  %-44s %8.2f Hz (want %.2f, %+.1f c) clarity %.2f\n",
           ok ? "PASS" : "FAIL", name, r.hz, hz, cents, r.clarity);
    ok ? sPass++ : sFail++;
}

static void expectNone(const char* name, const std::vector<int16_t>& v) {
    PitchResult r = run(v);
    bool ok = !r.found;
    printf("  %s  %-44s found=%d %.2f Hz clarity %.2f\n",
           ok ? "PASS" : "FAIL", name, r.found, r.hz, r.clarity);
    ok ? sPass++ : sFail++;
}

int main() {
    printf("pitch detection\n");
    expectPitch("sine A4 440",            tone(440.0f,  {{1, 1}}, 1.0f), 440.0f);
    expectPitch("sine middle C",          tone(261.63f, {{1, 1}}, 0.6f), 261.63f);
    expectPitch("sine high E6",           tone(1318.5f, {{1, 1}}, 0.5f), 1318.5f);
    expectPitch("sine low A1 55",         tone(55.0f,   {{1, 1}}, 2.0f), 55.0f);
    expectPitch("detuned sine A3 +23c",   tone(220.0f * powf(2, 23 / 1200.0f), {{1, 1}}, 1.0f),
                                          220.0f * powf(2, 23 / 1200.0f));
    expectPitch("sawtooth E2 82.4 (10 partials)",
                tone(82.41f, {{1,1},{2,.5f},{3,.33f},{4,.25f},{5,.2f},{6,.17f},{7,.14f},{8,.12f},{9,.11f},{10,.1f}}, 1.5f),
                82.41f);
    expectPitch("3rd harmonic loudest, G3 196",
                tone(196.0f, {{1,.3f},{2,.5f},{3,1},{4,.4f},{5,.2f}}, 1.0f), 196.0f);
    expectPitch("missing fundamental, 110",
                tone(110.0f, {{2,1},{3,.8f},{4,.6f},{5,.4f},{6,.3f}}, 1.0f), 110.0f);
    expectPitch("voice-like D3 146.8 (formant peak at 5th)",
                tone(146.83f, {{1,.4f},{2,.6f},{3,.7f},{4,.8f},{5,1},{6,.6f},{7,.3f},{8,.2f}}, 1.2f),
                146.83f);
    expectPitch("A3 220 with noise (about 20 dB S/N)",
                tone(220.0f, {{1,1},{2,.5f},{3,.3f}}, 1.0f, 1200.0f), 220.0f);
    expectPitch("quiet sine 330 (-40 dBFS)", tone(330.0f, {{1,1}}, 1.0f, 0, 330), 330.0f);
    expectPitch("long take 20 s, C3",     tone(130.81f, {{1,1},{2,.5f}}, 20.0f), 130.81f);
    expectPitch("short take 0.25 s, A4",  tone(440.0f, {{1,1},{2,.3f}}, 0.25f), 440.0f);

    printf("no clear pitch\n");
    expectNone("white noise", tone(100.0f, {}, 1.0f, 8000.0f));
    {   // a click: one sample spike then silence
        std::vector<int16_t> v(16000, 0);
        v[100] = 30000; v[101] = -20000;
        expectNone("a click", v);
    }
    expectNone("silence", std::vector<int16_t>(32000, 0));

    printf("tuning\n");
    {
        int16_t c = pitchTuneToMiddleC(261.6256f);
        bool ok = c == 0; ok ? sPass++ : sFail++;
        printf("  %s  middle C needs 0 c (got %d)\n", ok ? "PASS" : "FAIL", c);
        c = pitchTuneToMiddleC(220.0f);
        ok = c == 300; ok ? sPass++ : sFail++;
        printf("  %s  A3 needs +300 c (got %d)\n", ok ? "PASS" : "FAIL", c);
        c = pitchTuneToMiddleC(10.0f);
        ok = c == 4800; ok ? sPass++ : sFail++;
        printf("  %s  clamps at +4800 c (got %d)\n", ok ? "PASS" : "FAIL", c);
        uint8_t n; int8_t ct;
        pitchNearestNote(220.0f * powf(2, 23 / 1200.0f), n, ct);
        ok = n == 57 && ct == 23; ok ? sPass++ : sFail++;
        printf("  %s  nearest note A3 +23 c (got %u %+d)\n", ok ? "PASS" : "FAIL", n, ct);
    }

    printf("\n%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
#endif

// Host tests for psola.cpp (KEEP LENGTH pitch shifting), measured with
// pitch_detect.cpp.
//
// Run from Terminal — NOT through the Arduino IDE:
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++11 -O2 test_psola.cpp -o test_psola && ./test_psola
#ifndef ARDUINO
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include "psola.cpp"
#include "pitch_detect.cpp"

static int sPass = 0, sFail = 0;
static const uint32_t OUT_RATE = 32000;

static void check(bool ok, const char* fmt, double a, double b, double c = 0) {
    printf("  %s  ", ok ? "PASS" : "FAIL");
    printf(fmt, a, b, c);
    printf("\n");
    ok ? sPass++ : sFail++;
}

// A harmonic tone (mono): f0 with 6 partials falling 1/n, steady, short fades.
static std::vector<int16_t> tone(float f0, float secs, uint32_t rate, float level = 9000) {
    uint32_t n = (uint32_t)(secs * rate);
    std::vector<int16_t> v(n);
    for (uint32_t i = 0; i < n; i++) {
        double t = (double)i / rate, s = 0;
        for (int h = 1; h <= 6; h++) s += sin(2 * M_PI * f0 * h * t + h) / h;
        double env = fmin(1.0, fmin(t / 0.01, (secs - t) / 0.01));
        v[i] = (int16_t)(s * env * level);
    }
    return v;
}

static std::vector<int16_t> noise(float secs, uint32_t rate) {
    std::vector<int16_t> v((size_t)(secs * rate));
    srand(99);
    for (auto& x : v) x = (int16_t)((rand() % 16001) - 8000);
    return v;
}

static double rms(const int16_t* d, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)d[i] * d[i];
    return sqrt(s / (n ? n : 1));
}

// Render through PSOLA (mono out = L).
static std::vector<int16_t> render(const std::vector<int16_t>& src, uint32_t rate,
                                   float srcHz, float semis) {
    PsolaState s;
    float period = srcHz > 0 ? rate / srcHz : 0;
    psolaStart(s, rate, period, powf(2.0f, semis / 12.0f), OUT_RATE);
    std::vector<int16_t> out;
    int32_t L, R;
    while (psolaNext(s, src.data(), (uint32_t)src.size(), 1, false, L, R)) {
        out.push_back((int16_t)L);
        if (out.size() > 10 * OUT_RATE) break;
    }
    return out;
}

static void shiftCase(const char* name, float f0, uint32_t rate, float semis) {
    auto src = tone(f0, 1.0f, rate);
    auto out = render(src, rate, f0, semis);
    double wantSecs = 1.0, gotSecs = (double)out.size() / OUT_RATE;
    double want = f0 * pow(2.0, semis / 12.0);
    pitchBegin(out.data(), (uint32_t)out.size(), OUT_RATE);
    while (!pitchStep()) {}
    PitchResult r = pitchResult();
    double cents = r.hz > 0 ? 1200.0 * log2(r.hz / want) : 9999;
    // The level: the middle of the output against the middle of the source
    // (resampled to the output rate only in length terms — RMS is rate-free).
    double inR  = rms(src.data() + src.size() / 4, src.size() / 2);
    double outR = rms(out.data() + out.size() / 4, out.size() / 2);
    double dB   = 20 * log10(outR / inR);
    // Steadiness: the RMS of 40 ms blocks across the middle stays within 1.5 dB
    // (long enough to hold several periods even of a gappy low note).
    double lo = 1e9, hi = 0;
    size_t blk = OUT_RATE / 25;
    for (size_t i = out.size() / 5; i + blk < out.size() * 4 / 5; i += blk) {
        double b = rms(out.data() + i, blk);
        if (b < lo) lo = b;
        if (b > hi) hi = b;
    }
    double wobble = 20 * log10(hi / (lo > 0 ? lo : 1));
    printf("%s (%+.0f semitones)\n", name, semis);
    check(fabs(gotSecs - wantSecs) < 0.03, "length %.3f s (want %.3f)", gotSecs, wantSecs);
    check(r.found && fabs(cents) < 5, "pitch %.2f Hz (want %.2f)", r.hz, want);
    check(fabs(dB) < 1.5, "level %+.2f dB vs the source%.0s", dB, 0);
    check(wobble < 1.5, "steady: 40 ms blocks within %.2f dB%.0s", wobble, 0);
}

// Brightness: the share of energy above `cut` Hz (Hann-windowed 4096 FFTs
// across the middle).
static double hfShare(const std::vector<int16_t>& v, uint32_t rate, double cut) {
    const int N = 4096;
    std::vector<float> buf(2 * N);
    double lo = 0, hi = 0;
    for (size_t st = v.size() / 5; st + N < v.size() * 4 / 5; st += N / 2) {
        for (int i = 0; i < N; i++) {
            float w = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / N);
            buf[2 * i] = v[st + i] * w; buf[2 * i + 1] = 0;
        }
        fftInPlace(buf.data(), N);
        for (int k = 1; k < N / 2; k++) {
            double p = buf[2 * k] * buf[2 * k] + buf[2 * k + 1] * buf[2 * k + 1];
            (k * (double)rate / N > cut ? hi : lo) += p;
        }
    }
    return hi / (hi + lo);
}

// A bright tone (partials to 14 kHz falling 1/h^0.8) with a little natural
// pitch wobble: KEEP LENGTH must not dull it.  Linear interpolation inside
// the grains cost 2 dB above 6 kHz and 4 dB above 10 kHz.
static void brightCase(float semis) {
    std::vector<int16_t> src(48000);
    double ph = 0;
    for (size_t i = 0; i < src.size(); i++) {
        double t = (double)i / 32000;
        double f = 220.0 * (1 + 0.004 * sin(2 * M_PI * 5.3 * t));
        ph += 2 * M_PI * f / 32000;
        double v = 0;
        for (int h = 1; h * 220 < 14000; h++) v += sin(h * ph + h * h * 0.3) / pow(h, 0.8);
        src[i] = (int16_t)(v * fmin(1.0, fmin(t / 0.01, (1.5 - t) / 0.01)) * 2500);
    }
    auto out = render(src, 32000, 220.0f, semis);
    for (double cut : { 6000.0, 10000.0 }) {
        double dB = 10 * log10(hfShare(out, OUT_RATE, cut) / hfShare(src, 32000, cut));
        check(dB > -1.0, "bright tone %+.0f semitones: energy above %.0f Hz %+.2f dB", semis, cut, dB);
    }
}

int main() {
    shiftCase("A3 220 Hz, 32 kHz",       220.0f,  32000, +7);
    shiftCase("A3 220 Hz, 32 kHz",       220.0f,  32000, +12);
    shiftCase("A3 220 Hz, 32 kHz",       220.0f,  32000, -12);
    shiftCase("A3 220 Hz, 32 kHz",       220.0f,  32000, -5);
    shiftCase("A3 220 Hz, 32 kHz",       220.0f,  32000, 0);
    shiftCase("E2 82.4 Hz, 44.1 kHz",    82.41f,  44100, +19);
    shiftCase("C5 523 Hz, 48 kHz",       523.25f, 48000, -24);
    shiftCase("C4 261.6 Hz, 22.05 kHz",  261.63f, 22050, +24);

    printf("brightness (KEEP LENGTH mustn't dull the top end)\n");
    for (float x : { 0.0f, 3.0f, 7.0f, -5.0f, -12.0f }) brightCase(x);

    printf("unpitched (noise), +12\n");
    {
        auto src = noise(1.0f, 32000);
        auto out = render(src, 32000, 0, 12);
        double secs = (double)out.size() / OUT_RATE;
        check(fabs(secs - 1.0) < 0.03, "length %.3f s (want %.3f)", secs, 1.0);
    }
    printf("loop: keeps going\n");
    {
        auto src = tone(220.0f, 0.3f, 32000);
        PsolaState s;
        psolaStart(s, 32000, 32000 / 220.0f, 1.5f, OUT_RATE);
        int32_t L, R;
        uint32_t n = 0;
        while (n < 3 * OUT_RATE && psolaNext(s, src.data(), (uint32_t)src.size(), 1, true, L, R)) n++;
        check(n == 3 * OUT_RATE, "rendered %.0f frames of a 0.3 s loop (want %.0f)", n, 3.0 * OUT_RATE);
    }

    printf("\n%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
#endif

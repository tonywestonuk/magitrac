// Host tests for timestretch.cpp (KEEP LENGTH: stretch, then play faster),
// measured with pitch_detect.cpp and a local pitch tracker.
//
// Run from Terminal — NOT through the Arduino IDE:
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++11 -O2 test_timestretch.cpp -o test_timestretch && ./test_timestretch
#ifndef ARDUINO
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <chrono>
#include <vector>
#include <algorithm>
#include "timestretch.cpp"
#include "pitch_detect.cpp"

static int sPass = 0, sFail = 0;
static const uint32_t OUT_RATE = 32000;

static void check(bool ok, const char* fmt, double a = 0, double b = 0, double c = 0) {
    printf("  %s  ", ok ? "PASS" : "FAIL");
    printf(fmt, a, b, c);
    printf("\n");
    ok ? sPass++ : sFail++;
}

// A harmonic tone (mono): 6 partials falling 1/n, short fades.  `hzAt` gives
// the pitch at time t, so it can glide or wobble.
template <class F>
static std::vector<int16_t> toneF(F hzAt, float secs, uint32_t rate, float level = 9000) {
    uint32_t n = (uint32_t)(secs * rate);
    std::vector<int16_t> v(n);
    double ph = 0;
    for (uint32_t i = 0; i < n; i++) {
        double t = (double)i / rate, s = 0;
        ph += 2 * M_PI * hzAt(t) / rate;
        for (int h = 1; h <= 6; h++) s += sin(h * ph + h) / h;
        double env = fmin(1.0, fmin(t / 0.01, (secs - t) / 0.01));
        v[i] = (int16_t)(s * env * level);
    }
    return v;
}
static std::vector<int16_t> tone(float f0, float secs, uint32_t rate) {
    return toneF([f0](double) { return (double)f0; }, secs, rate);
}

static double rms(const int16_t* d, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)d[i] * d[i];
    return sqrt(s / (n ? n : 1));
}

// Render through KEEP LENGTH (mono out = L, or both channels).
static std::vector<int16_t> render(const std::vector<int16_t>& src, uint32_t rate, float semis,
                                   int ch = 1, std::vector<int16_t>* right = nullptr) {
    TsState s;
    tsStart(s, rate, powf(2.0f, semis / 12.0f), OUT_RATE);
    std::vector<int16_t> out;
    int32_t L, R;
    while (tsNext(s, src.data(), (uint32_t)(src.size() / ch), ch, false, L, R)) {
        out.push_back((int16_t)L);
        if (right) right->push_back((int16_t)R);
        if (out.size() > 10 * OUT_RATE) break;
    }
    return out;
}

// Normal pitched playback, as SamplePlayer would with cubic interpolation.
static std::vector<int16_t> resampled(const std::vector<int16_t>& src, uint32_t rate, float semis) {
    double step = pow(2.0, semis / 12.0) * rate / OUT_RATE;
    std::vector<int16_t> o;
    for (double p = 0; p + 2 < src.size(); p += step) {
        long i = (long)p; float f = (float)(p - i);
        float y0 = i ? src[i - 1] : 0;
        o.push_back((int16_t)cubic(y0, src[i], src[i + 1], src[i + 2], f));
    }
    return o;
}

// Local pitch (YIN) of `n` frames at `at`: 0 if none.
static double localHz(const int16_t* x, size_t n, uint32_t rate) {
    int tmin = rate / 1200, tmax = rate / 60;
    int w = (int)n - tmax;
    if (w < 64) return 0;
    std::vector<double> d(tmax + 2, 0.0);
    for (int tau = 1; tau <= tmax + 1; tau++) {
        double s = 0;
        for (int i = 0; i < w; i++) { double e = (double)x[i] - x[i + tau]; s += e * e; }
        d[tau] = s;
    }
    double run = 0;
    std::vector<double> c(tmax + 2, 1.0);
    for (int tau = 1; tau <= tmax + 1; tau++) { run += d[tau]; c[tau] = d[tau] * tau / (run ? run : 1); }
    for (int tau = tmin; tau <= tmax; tau++) {
        if (c[tau] < 0.15 && c[tau] <= c[tau + 1]) {
            double a = c[tau - 1], b = c[tau], e = c[tau + 1];
            double den = a - 2 * b + e;
            double off = fabs(den) > 1e-12 ? 0.5 * (a - e) / den : 0;
            return rate / (tau + off);
        }
    }
    return 0;
}

static void shiftCase(const char* name, float f0, uint32_t rate, float semis) {
    auto src = tone(f0, 1.0f, rate);
    auto out = render(src, rate, semis);
    double gotSecs = (double)out.size() / OUT_RATE;
    double want = f0 * pow(2.0, semis / 12.0);
    pitchBegin(out.data(), (uint32_t)out.size(), OUT_RATE);
    while (!pitchStep()) {}
    PitchResult r = pitchResult();
    double inR  = rms(src.data() + src.size() / 4, src.size() / 2);
    double outR = rms(out.data() + out.size() / 4, out.size() / 2);
    double dB   = 20 * log10(outR / inR);
    double lo = 1e9, hi = 0;
    size_t blk = OUT_RATE / 25;
    for (size_t i = out.size() / 5; i + blk < out.size() * 4 / 5; i += blk) {
        double b = rms(out.data() + i, blk);
        lo = fmin(lo, b); hi = fmax(hi, b);
    }
    double wobble = 20 * log10(hi / (lo > 0 ? lo : 1));
    printf("%s (%+.0f semitones)\n", name, semis);
    check(fabs(gotSecs - 1.0) < 0.002, "length %.4f s (want 1.0000)", gotSecs);
    check(r.found && fabs(1200.0 * log2(r.hz / want)) < 5, "pitch %.2f Hz (want %.2f)", r.hz, want);
    check(fabs(dB) < 1.0, "level %+.2f dB vs the source", dB);
    check(wobble < 1.0, "steady: 40 ms blocks within %.2f dB", wobble);
}

// The pitch MOVEMENT survives: vibrato and a glide come out shifted by the
// same interval all the way through.  (PSOLA with one period per sample held
// the output at a single pitch — a monotone drone — and passed every other
// test.)
static void contourCase(const char* name, float semis, uint32_t rate) {
    auto hz = [](double t) { return (180.0 + 90.0 * t) * (1 + 0.03 * sin(2 * M_PI * 5.5 * t)); };
    auto src = toneF(hz, 1.5f, rate);
    auto out = render(src, rate, semis);
    double r = pow(2.0, semis / 12.0);
    const size_t blk = OUT_RATE / 16;               // ~60 ms analysis, 10 ms steps
    std::vector<double> err, outC, srcC;
    for (size_t i = OUT_RATE / 10; i + blk < out.size() - OUT_RATE / 10; i += OUT_RATE / 100) {
        double ho = localHz(out.data() + i, blk, OUT_RATE);
        size_t si = (size_t)((i + blk / 2) * (double)rate / OUT_RATE);     // same moment in the source
        size_t sb = (size_t)(blk * (double)rate / OUT_RATE);
        double hs = localHz(src.data() + si - sb / 2, sb, rate);
        if (ho <= 0 || hs <= 0) { err.push_back(999); continue; }
        err.push_back(fabs(1200.0 * log2(ho / (hs * r))));
        outC.push_back(1200.0 * log2(ho)); srcC.push_back(1200.0 * log2(hs));
    }
    std::vector<double> e = err;
    std::sort(e.begin(), e.end());
    double med = e[e.size() / 2], p90 = e[e.size() * 9 / 10];
    // How much of the source's pitch movement the output follows.
    double ms = 0, mo = 0;
    for (size_t i = 0; i < srcC.size(); i++) { ms += srcC[i]; mo += outC[i]; }
    ms /= srcC.size(); mo /= outC.size();
    double sxy = 0, sxx = 0;
    for (size_t i = 0; i < srcC.size(); i++) { sxy += (srcC[i] - ms) * (outC[i] - mo); sxx += (srcC[i] - ms) * (srcC[i] - ms); }
    double follow = sxy / sxx;
    printf("%s (%+.0f semitones)\n", name, semis);
    check(med < 10, "pitch follows the source: median error %.1f cents", med);
    check(p90 < 25, "...90%% of 10 ms steps within %.1f cents", p90);
    check(follow > 0.95 && follow < 1.05, "vibrato + glide carried through (slope %.3f, want 1)", follow);
}

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

// Timbre: the same top end as a normal pitched note (that's the point —
// it should sound like normal playback, only the length kept).
static void brightCase(float semis) {
    auto src = toneF([](double t) { return 220.0 * (1 + 0.02 * sin(2 * M_PI * 5.3 * t)); }, 1.5f, 32000);
    // brighter source: add partials up to 12 kHz
    for (size_t i = 0; i < src.size(); i++) {
        double t = i / 32000.0, v = 0;
        for (int h = 7; h * 220 < 12000; h++) v += sin(2 * M_PI * 220 * h * t + h * h * 0.3) / pow(h, 0.8);
        src[i] = (int16_t)fmax(-32768, fmin(32767, src[i] * 0.3 + v * 1500 * fmin(1.0, fmin(t / 0.01, (1.5 - t) / 0.01))));
    }
    auto out = render(src, 32000, semis);
    auto ref = resampled(src, 32000, semis);
    for (double cut : { 3000.0, 6000.0, 10000.0 }) {
        double dB = 10 * log10(hfShare(out, OUT_RATE, cut) / hfShare(ref, OUT_RATE, cut));
        check(fabs(dB) < 1.0, "%+.0f semitones: energy above %.0f Hz %+.2f dB vs normal playback", semis, cut, dB);
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

    printf("pitch movement (vibrato 3%% + a glide)\n");
    contourCase("glide 180->315 Hz, 32 kHz", +7, 32000);
    contourCase("glide 180->315 Hz, 32 kHz", +12, 32000);
    contourCase("glide 180->315 Hz, 32 kHz", -5, 32000);
    contourCase("glide 180->315 Hz, 44.1 kHz", +3, 44100);

    printf("brightness (sounds like a normal pitched note)\n");
    for (float x : { 0.0f, 3.0f, 7.0f, -5.0f, -12.0f }) brightCase(x);

    printf("attack: the start is the sample's own\n");
    {
        auto src = tone(220.0f, 1.0f, 32000);
        for (int i = 0; i < 200; i++) src[i] = (int16_t)(src[i] + (i < 40 ? 20000 * (1 - i / 40.0) : 0));
        auto out = render(src, 32000, 7);
        auto ref = resampled(src, 32000, 7);
        int same = (int)(512 / pow(2.0, 7 / 12.0)) - 2;      // until the second grain starts
        int worst = 0;
        for (int i = 0; i < same; i++) worst = std::max(worst, abs(out[i] - ref[i]));
        check(worst <= 32, "first %.0f frames match normal playback (worst %.0f of 32767)", same, worst);
    }
    printf("end: no click\n");
    {
        auto src = tone(220.0f, 0.5f, 32000);
        for (size_t i = src.size() - 400; i < src.size(); i++) src[i] = 9000;   // cut off loud
        auto out = render(src, 32000, 5);
        double body = rms(out.data() + out.size() / 4, out.size() / 2);
        double last = 0;
        for (size_t i = out.size() - 8; i < out.size(); i++) last = fmax(last, fabs((double)out[i]));
        check(last < body * 0.05, "last frames %.0f vs body RMS %.0f", last, body);
    }
    printf("stereo: channels stay apart\n");
    {
        auto m = tone(330.0f, 0.6f, 44100);
        std::vector<int16_t> st(m.size() * 2, 0);
        for (size_t i = 0; i < m.size(); i++) st[2 * i] = m[i];        // left only
        std::vector<int16_t> right;
        auto left = render(st, 44100, 4, 2, &right);
        check(rms(left.data(), left.size()) > 3000 && rms(right.data(), right.size()) == 0,
              "left %.0f RMS, right %.0f RMS", rms(left.data(), left.size()), rms(right.data(), right.size()));
    }
    printf("loop: keeps going, level steady\n");
    {
        auto src = tone(220.0f, 0.3f, 32000);
        for (size_t i = 0; i < src.size(); i++)                            // seamless loop body
            src[i] = (int16_t)(9000 * sin(2 * M_PI * 220 * (double)i / 32000 * (66.0 / 66.0)));
        TsState s;
        tsStart(s, 32000, 1.5f, OUT_RATE);
        std::vector<int16_t> out;
        int32_t L, R;
        while (out.size() < 3 * OUT_RATE && tsNext(s, src.data(), (uint32_t)src.size(), 1, true, L, R))
            out.push_back((int16_t)L);
        check(out.size() == 3 * OUT_RATE, "rendered %.0f frames of a 0.3 s loop", out.size());
        double a = rms(out.data() + OUT_RATE / 2, OUT_RATE / 2), b = rms(out.data() + 2 * OUT_RATE, OUT_RATE / 2);
        check(fabs(20 * log10(b / a)) < 0.5, "level at 2 s vs 0.5 s: %+.2f dB", 20 * log10(b / a));
    }
    printf("cost (one voice)\n");
    {
        auto src = tone(220.0f, 4.0f, 44100);
        for (float semis : { 12.0f, 24.0f }) {
            uint32_t ops0 = tsSearchOps();
            auto t0 = std::chrono::steady_clock::now();
            auto out = render(src, 44100, semis);
            double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
            double secs = out.size() / (double)OUT_RATE;
            double macs = (tsSearchOps() - ops0) / secs;
            printf("  info  %+.0f: %.0f ns per output frame on this Mac, %.2f M matching MACs/s\n",
                   semis, ns / out.size(), macs / 1e6);
            check(macs < (semis > 12 ? 2e6 : 1e6), "matching %.2f M MACs per second of sound", macs / 1e6);
        }
    }

    printf("\n%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
#endif

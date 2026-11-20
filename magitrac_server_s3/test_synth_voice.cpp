// Host test for synth_voice.cpp (the organ's SYNTH sound).
//
// Run from Terminal — NOT through the Arduino IDE (it writes a binary here —
// don't commit it):
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++17 -Wno-format -DUNIT_TEST -I test_mocks -I . -I ../magitrac_lib/src \
//       test_synth_voice.cpp -o test_synth_voice && ./test_synth_voice
#ifndef ARDUINO
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "test_mocks/Arduino.h"
#include "synth_voice.cpp"

static const float FS = 32000.0f;
static int sPass = 0, sFail = 0;
static void check(bool ok, const char* what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    ok ? sPass++ : sFail++;
}

struct Run { float peak, rms; bool finite; int crossings; };

// Render `n` samples a control step at a time (gate held for the first
// `gateN`), stats over [from, n).  trace[] gets the envelope per sample (it
// moves within a step, but is only read back at the step's end).
static Run render(SynthVoice& v, const SynthBlock& b, int n, int gateN, int from = 0,
                  float* trace = nullptr) {
    Run r = { 0, 0, true, 0 };
    double sum = 0; float prev = 0;
    float buf[SYNTH_CTRL];
    for (int base = 0; base < n; base += SYNTH_CTRL) {
        for (int i = 0; i < SYNTH_CTRL; i++) buf[i] = 0.0f;
        synthRender(v, b, base < gateN, buf, SYNTH_CTRL);
        for (int i = 0; i < SYNTH_CTRL && base + i < n; i++) {
            int k = base + i;
            float y = buf[i];
            if (trace) trace[k] = v.env;
            if (!isfinite(y)) r.finite = false;
            if (k >= from) {
                if (fabsf(y) > r.peak) r.peak = fabsf(y);
                sum += (double)y * y;
                if ((prev < 0) != (y < 0) && k > from) r.crossings++;
            }
            prev = y;
        }
    }
    r.rms = (float)sqrt(sum / (n - from > 0 ? n - from : 1));
    return r;
}

static float hz(int midi) { return 440.0f * powf(2.0f, (midi - 69) / 12.0f); }

static void setup(SynthBlock& b, SynthVoice& v, const uint8_t* p, int midi, uint8_t vel = 100) {
    memset(&v, 0, sizeof(v));
    synthPrepare(b, p, FS);
    synthNoteOn(v, hz(midi), vel, 0.0f, FS);
}

int main() {
    uint8_t p[ORGAN_SYNTH_PARAMS];
    SynthBlock b;
    SynthVoice v;
    static float env[32000 * 6];

    printf("Default sound\n");
    memcpy(p, ORGAN_SYNTH_DEFAULT, sizeof(p));
    setup(b, v, p, 60);
    Run r = render(v, b, 32000, 32000, 16000);
    check(r.finite, "no NaN or infinity");
    check(r.rms > 0.05f && r.peak < 2.0f, "a sensible level while held");

    printf("Envelope times\n");
    memcpy(p, ORGAN_SYNTH_DEFAULT, sizeof(p));
    p[SYN_ATTACK] = 8; p[SYN_SUSTAIN] = 16; p[SYN_RELEASE] = 6;   // 513 ms, 437 ms
    setup(b, v, p, 60);
    render(v, b, 32000 * 3, 32000 * 2, 0, env);
    int atk = 0; while (atk < 32000 * 2 && env[atk] < 0.999f) atk++;
    float atkMs = atk * 1000.0f / FS;
    printf("     attack reached full level at %.0f ms (setting 513)\n", atkMs);
    check(fabsf(atkMs - synthAttackMs(8)) < 0.1f * synthAttackMs(8), "attack takes the time it says");
    int rel = 32000 * 2; while (rel < 32000 * 3 && env[rel] > 0.01f) rel++;
    float relMs = (rel - 32000 * 2) * 1000.0f / FS;
    printf("     release fell 40 dB in %.0f ms (setting 437)\n", relMs);
    check(fabsf(relMs - synthReleaseMs(6)) < 0.15f * synthReleaseMs(6), "release takes the time it says");
    render(v, b, 32000, 0);
    check(v.stage == SYN_ST_DONE, "the voice finishes after its release");

    printf("Decay to sustain\n");
    p[SYN_ATTACK] = 0; p[SYN_DECAY] = 6; p[SYN_SUSTAIN] = 4;
    setup(b, v, p, 60);
    render(v, b, 32000, 32000);
    check(fabsf(v.env - 0.25f) < 0.01f, "settles at the sustain level");

    printf("Pitch\n");
    memcpy(p, ORGAN_SYNTH_DEFAULT, sizeof(p));
    p[SYN_OSC1] = SYN_WAVE_SINE; p[SYN_MIX] = 0; p[SYN_CUTOFF] = 16; p[SYN_ATTACK] = 0;
    p[SYN_SUSTAIN] = 16;
    setup(b, v, p, 69);
    r = render(v, b, 32000 + 3200, 32000 + 3200, 3200);
    printf("     A4 sine crossed zero %d times in 1 s\n", r.crossings);
    check(abs(r.crossings - 880) <= 4, "A4 plays at 440 Hz");
    p[SYN_MIX] = 16; p[SYN_OSC2] = SYN_WAVE_SINE; p[SYN_PITCH] = 24; p[SYN_DETUNE] = 0;
    setup(b, v, p, 69);
    r = render(v, b, 32000 + 3200, 32000 + 3200, 3200);
    check(abs(r.crossings - 1760) <= 6, "osc 2 at +12 plays an octave up");

    printf("Filter\n");
    memcpy(p, ORGAN_SYNTH_DEFAULT, sizeof(p));
    p[SYN_ENV] = 0; p[SYN_ATTACK] = 0; p[SYN_SUSTAIN] = 16; p[SYN_RESO] = 0;
    p[SYN_CUTOFF] = 16;
    setup(b, v, p, 60);
    float open = render(v, b, 16000, 16000, 3200).rms;
    p[SYN_CUTOFF] = 2;
    setup(b, v, p, 60);
    float shut = render(v, b, 16000, 16000, 3200).rms;
    printf("     saw C4: rms %.3f open, %.3f with the filter down\n", open, shut);
    check(shut < open * 0.5f, "turning the cutoff down darkens and quietens a saw");

    printf("Worst case: full resonance, wide open, every pitch\n");
    bool fine = true; float worst = 0;
    for (int w = 0; w <= 3; w++) {
        for (int midi = 24; midi <= 108; midi += 12) {
            memcpy(p, ORGAN_SYNTH_DEFAULT, sizeof(p));
            p[SYN_OSC1] = (uint8_t)w; p[SYN_OSC2] = SYN_WAVE_NOISE; p[SYN_SUB] = 16;
            p[SYN_RESO] = 16; p[SYN_ENV] = 16; p[SYN_CUTOFF] = 8; p[SYN_ATTACK] = 0;
            p[SYN_DECAY] = 4; p[SYN_SUSTAIN] = 8; p[SYN_PW] = 16;
            setup(b, v, p, midi, 127);
            Run x = render(v, b, 32000, 24000);
            if (!x.finite) fine = false;
            if (x.peak > worst) worst = x.peak;
        }
    }
    printf("     loudest sample %.2f\n", worst);
    check(fine, "stays finite");
    check(worst < 12.0f, "stays bounded (the organ clamps the mix)");

    printf("Retrigger\n");
    memcpy(p, ORGAN_SYNTH_DEFAULT, sizeof(p));
    synthPrepare(b, p, FS);
    synthNoteOn(v, hz(60), 100, 0.7f, FS);
    check(fabsf(v.env - 0.7f) < 1e-6f, "a retriggered voice starts where it was");
    v.s1 = NAN;
    synthNoteOn(v, hz(60), 100, NAN, FS);
    check(v.env == 0.0f && v.s1 == 0.0f, "a voice carrying junk starts clean");

    printf("Velocity\n");
    memcpy(p, ORGAN_SYNTH_DEFAULT, sizeof(p));
    setup(b, v, p, 60, 127);
    float loud = render(v, b, 16000, 16000, 8000).rms;
    setup(b, v, p, 60, 20);
    float soft = render(v, b, 16000, 16000, 8000).rms;
    check(soft < loud * 0.6f && soft > loud * 0.3f, "soft notes are quieter, not silent");

    printf("Release ends\n");
    memcpy(p, ORGAN_SYNTH_DEFAULT, sizeof(p));
    p[SYN_RELEASE] = 9;                                    // 1.5 s
    setup(b, v, p, 60);
    render(v, b, 3200 + 80000, 3200);                      // 2.5 s of release
    float tail = 80000 / FS;
    printf("     voice stage %d after %.1f s of a %.1f s release\n", v.stage, tail, synthReleaseMs(9) / 1000.0f);
    check(v.stage == SYN_ST_DONE, "a release frees its voice by 1.6x its time (-60 dB)");

    printf("\n%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
#endif

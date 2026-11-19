// Host test for take_fade.h (ADD SAMPLE's fades at the ends of a take).
//
// Run from Terminal — NOT through the Arduino IDE:
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++11 -O2 test_take_fade.cpp -o test_take_fade && ./test_take_fade
#ifndef ARDUINO
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include "take_fade.h"

static int sPass = 0, sFail = 0;
static void check(bool ok, const char* what, double v) {
    printf("  %s  %s (%.0f)\n", ok ? "PASS" : "FAIL", what, v);
    ok ? sPass++ : sFail++;
}

int main() {
    const uint32_t RATE = 32000;          // the codec's rate (unsigned, like AUDIO_CODEC_RATE_HZ)
    std::vector<int16_t> x(RATE / 2);
    for (size_t i = 0; i < x.size(); i++) x[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * i / RATE));
    std::vector<int16_t> y = x;
    fadeTakeEnds(y.data(), (uint32_t)y.size(), RATE);

    // Every sample is the original scaled by a gain between 0 and 1 — never
    // bigger, never flipped (the bug: negatives became ~-26000 / ~-5200).
    double worst = 0;
    for (size_t i = 0; i < y.size(); i++) {
        if (abs(y[i]) > abs(x[i])) worst = fmax(worst, abs(y[i]) - abs(x[i]));
        if ((long)y[i] * x[i] < 0) worst = fmax(worst, abs(y[i]));
    }
    check(worst == 0, "faded samples only ever get quieter, same sign", worst);
    check(y[0] == 0 && y[y.size() - 1] == 0, "starts and ends on zero", abs(y[0]) + abs(y[y.size() - 1]));
    double jump = 0;
    for (size_t i = 1; i < y.size(); i++) jump = fmax(jump, abs(y[i] - y[i - 1]));
    double srcJump = 0;
    for (size_t i = 1; i < x.size(); i++) srcJump = fmax(srcJump, abs(x[i] - x[i - 1]));
    check(jump <= srcJump, "no step bigger than the sound's own", jump);
    check(y[RATE / 4] == x[RATE / 4], "the middle is untouched", y[RATE / 4]);

    printf("\n%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
#endif

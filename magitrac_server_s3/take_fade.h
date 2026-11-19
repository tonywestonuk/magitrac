#pragma once
#include <stdint.h>

// ── ADD SAMPLE: fade a take's ends ───────────────────────────────────────────
// 5 ms in, 25 ms out (the take is cut mid-sound; 5 ms still ticked), so it
// starts and stops without a click.  Host-tested in test_take_fade.cpp.
//
// All signed on purpose: dividing a negative sample by an unsigned length
// (AUDIO_CODEC_RATE_HZ is unsigned) turned every negative sample in the fades
// into a huge number — a burst of buzz at both ends of every take.
static inline void fadeTakeEnds(int16_t* buf, uint32_t frames, uint32_t rate) {
    int32_t in  = (int32_t)(rate / 200);
    int32_t out = (int32_t)(rate * 25 / 1000);
    for (int32_t i = 0; i < in && (uint32_t)i < frames / 2; i++)
        buf[i] = (int16_t)((int32_t)buf[i] * i / in);
    for (int32_t i = 0; i < out && (uint32_t)i < frames / 2; i++)
        buf[frames - 1 - i] = (int16_t)((int32_t)buf[frames - 1 - i] * i / out);
}

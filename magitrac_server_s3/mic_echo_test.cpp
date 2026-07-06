#include "mic_echo_test.h"
#include "audio_codec.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

// 0.5 s of 16-bit stereo at 32 kHz = 32000 bytes — small enough to live in
// internal SRAM (no PSRAM / staging nonsense).
static constexpr float  REC_SECONDS  = 0.5f;
static constexpr size_t REC_FRAMES   = (size_t)(AUDIO_CODEC_RATE_HZ * REC_SECONDS);  // stereo frames
static constexpr size_t REC_BYTES    = REC_FRAMES * 4;   // 2 ch * 2 bytes

// How many samples to dump per record.  Each line of CSV is ~14 chars, so
// 2048 lines ≈ 28 KB serial output — pasteable in one go.  At 32 kHz that's
// 64 ms = enough to see many cycles of any common test tone.
static constexpr size_t DUMP_FRAMES  = 2048;

void micEchoLoop() {
    Serial.println("[ECHO] half-second mic capture starting");

    if (!audioCodecReady()) {
        Serial.println("[ECHO] codec not initialised; calling audioCodecInit()");
        if (!audioCodecInit()) {
            Serial.println("[ECHO] codec init FAILED — halting");
            while (true) delay(1000);
        }
    }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(
                       REC_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        Serial.printf("[ECHO] internal-RAM alloc of %u B FAILED — halting\n",
                      (unsigned)REC_BYTES);
        while (true) delay(1000);
    }
    Serial.printf("[ECHO] buffer ready: %u bytes (%.2f s)\n",
                  (unsigned)REC_BYTES, REC_SECONDS);

    for (uint32_t iter = 0; ; iter++) {
        Serial.printf("\n[ECHO] === iteration %u ===\n", (unsigned)iter);
        Serial.println("[ECHO] play sine NOW (recording for 0.5 s)…");

        uint32_t t0 = millis();
        if (!audioCodecRecord(buf, REC_BYTES)) {
            Serial.println("[ECHO] record FAILED");
            delay(2000);
            continue;
        }
        uint32_t tRec = millis() - t0;

        int16_t* s = (int16_t*)buf;
        size_t   nFrames = REC_FRAMES;

        int32_t peakL = 0, peakR = 0;
        int64_t sumSqL = 0, sumSqR = 0;
        for (size_t i = 0; i < nFrames; i++) {
            int32_t l = s[i * 2 + 0];
            int32_t r = s[i * 2 + 1];
            int32_t al = l < 0 ? -l : l;
            int32_t ar = r < 0 ? -r : r;
            if (al > peakL) peakL = al;
            if (ar > peakR) peakR = ar;
            sumSqL += (int64_t)l * (int64_t)l;
            sumSqR += (int64_t)r * (int64_t)r;
        }
        float rmsL = sqrtf((float)((double)sumSqL / (double)nFrames));
        float rmsR = sqrtf((float)((double)sumSqR / (double)nFrames));

        Serial.printf("[ECHO] recorded %u frames in %u ms\n",
                      (unsigned)nFrames, (unsigned)tRec);
        Serial.printf("[ECHO] L  peak=%ld (%.1f%%)  rms=%.0f\n",
                      (long)peakL, (float)peakL * 100.0f / 32768.0f, rmsL);
        Serial.printf("[ECHO] R  peak=%ld (%.1f%%)  rms=%.0f\n",
                      (long)peakR, (float)peakR * 100.0f / 32768.0f, rmsR);

        size_t toDump = nFrames < DUMP_FRAMES ? nFrames : DUMP_FRAMES;
        Serial.printf("[ECHO] dumping first %u frames as CSV (L,R)\n", (unsigned)toDump);
        Serial.println("---BEGIN-CSV---");
        Serial.println("L,R");
        for (size_t i = 0; i < toDump; i++) {
            Serial.printf("%d,%d\n", (int)s[i * 2 + 0], (int)s[i * 2 + 1]);
        }
        Serial.println("---END-CSV---");

        Serial.println("[ECHO] sleeping 5 s before next capture");
        delay(5000);
    }
}

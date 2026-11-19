#include "freq_resp_test.h"
#include <SD.h>            // must precede M5Unified (M5GFX rebinds File I/O otherwise)
#include <M5Unified.h>
#include "audio_codec.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include <WiFi.h>
#include <esp_now.h>

static const int      FR_BLOCK  = 256;                 // frames per codec write (8 ms)
static const float    FR_AMP    = 32767.0f * 0.2512f;  // -12 dBFS
static const int      FR_TONES  = 58;
static const uint32_t FR_FADE   = AUDIO_CODEC_RATE_HZ / 200;   // 5 ms
static const uint16_t FR_BG     = 0x0011;

static int16_t* sIo = nullptr;   // FR_BLOCK stereo frames

static void frBanner(const char* line1, const char* line2) {
    auto& lcd = M5.Display;
    lcd.fillScreen(FR_BG);
    lcd.setTextColor(TFT_WHITE, FR_BG);
    lcd.setTextSize(3);
    lcd.setCursor(12, 24);  lcd.print("FREQ TEST");
    lcd.setTextSize(2);
    lcd.setCursor(12, 80);  lcd.print(line1);
    lcd.setCursor(12, 106); lcd.print(line2);
    lcd.setTextSize(1);
    lcd.setCursor(12, 300); lcd.print("Flash the server firmware to exit");
}

static void frWrite(uint32_t frames) {
    // (frames is always a multiple of FR_BLOCK here)
    for (uint32_t done = 0; done < frames; done += FR_BLOCK)
        if (!audioCodecPlay((const uint8_t*)sIo, FR_BLOCK * 4)) delay(2);
}

static void frSilence(float seconds) {
    memset(sIo, 0, FR_BLOCK * 4);
    uint32_t frames = (uint32_t)(seconds * AUDIO_CODEC_RATE_HZ);
    frames -= frames % FR_BLOCK;
    frWrite(frames);
}

// A sine of `hz` for `seconds` (rounded to whole blocks), faded in and out.
// Phase is kept in double so a long tone stays exact.
static void frTone(float hz, float seconds) {
    uint32_t frames = (uint32_t)(seconds * AUDIO_CODEC_RATE_HZ);
    frames -= frames % FR_BLOCK;
    double w = 2.0 * M_PI * hz / AUDIO_CODEC_RATE_HZ;
    for (uint32_t base = 0; base < frames; base += FR_BLOCK) {
        for (int i = 0; i < FR_BLOCK; i++) {
            uint32_t n = base + i;
            float g = 1.0f;
            if (n < FR_FADE)               g = 0.5f - 0.5f * cosf((float)M_PI * n / FR_FADE);
            else if (n >= frames - FR_FADE) g = 0.5f - 0.5f * cosf((float)M_PI * (frames - 1 - n) / FR_FADE);
            int16_t s = (int16_t)lrintf(FR_AMP * g * (float)sin(w * n));
            sIo[i * 2] = s; sIo[i * 2 + 1] = s;
        }
        if (!audioCodecPlay((const uint8_t*)sIo, FR_BLOCK * 4)) delay(2);
    }
}

void freqRespLoop() {
    Serial.println("[FREQTEST] stepped-sine frequency response test, looping");
#ifdef FREQ_RESP_NO_WIFI
    // A/B for RF pickup: radio fully off (ESP-NOW, softAP, STA).
    esp_now_deinit();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[FREQTEST] WiFi OFF");
    const char* wifiNote = "WiFi OFF";
#else
    const char* wifiNote = "WiFi on";
#endif
    sIo = (int16_t*)heap_caps_malloc(FR_BLOCK * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!audioCodecReady() || !sIo) {
        frBanner(audioCodecReady() ? "No memory" : "Audio module", "not found");
        while (true) delay(1000);
    }
    for (uint32_t cycle = 1;; cycle++) {
        char l2[32];
        snprintf(l2, sizeof(l2), "cycle %lu, %s", (unsigned long)cycle, wifiNote);
        frBanner("Playing tones", l2);
        Serial.printf("[FREQTEST] cycle %lu\n", (unsigned long)cycle);
        frSilence(1.0f);
        frTone(1000.0f, 0.5f);
        frSilence(0.5f);
        for (int k = 0; k < FR_TONES; k++) {
            float hz = 20.0f * powf(2.0f, (float)k / 6.0f);
            if (k % 6 == 0) {                         // the LCD costs a little time:
                auto& lcd = M5.Display;               // only once an octave, inside
                lcd.fillRect(12, 106, 220, 20, FR_BG);   // the tone's own audio
                lcd.setTextSize(2);                   // buffering
                lcd.setCursor(12, 106);
                lcd.printf("%.0f Hz", hz);
            }
            frTone(hz, 0.3f);
            frSilence(0.1f);
        }
        frSilence(1.0f);
    }
}

#include "SamplePlayer.h"
#include <Arduino.h>
#include <SD.h>
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ── I2S config — same as MidiMagic (22 kHz, built-in DAC, GPIO 25) ────────────
static const i2s_config_t s_i2s_cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate          = 22050,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_RIGHT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = 0,
    .dma_buf_count        = 2,
    .dma_buf_len          = 1024,
    .use_apll             = 0,
};

static volatile bool      s_stop    = false;
static volatile bool      s_playing = false;
static SemaphoreHandle_t  s_trigger = nullptr;
static char               s_path[64];

static void wavTaskFn(void*) {
    for (;;) {
        xSemaphoreTake(s_trigger, portMAX_DELAY);

        s_stop    = false;
        s_playing = true;

        i2s_driver_install(I2S_NUM_0, &s_i2s_cfg, 0, NULL);
        i2s_set_pin(I2S_NUM_0, NULL);
        i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);

        File f = SD.open(s_path);
        if (f) {
            int16_t  fbuf[256];
            uint32_t obuf[256];
            size_t   bw;

            // Read 44-byte WAV header and extract sample rate (bytes 24-27, little-endian)
            uint8_t hdr[44];
            f.read(hdr, 44);
            uint32_t sampleRate = (uint32_t)hdr[24]
                                | ((uint32_t)hdr[25] << 8)
                                | ((uint32_t)hdr[26] << 16)
                                | ((uint32_t)hdr[27] << 24);
            if (sampleRate < 8000 || sampleRate > 48000) sampleRate = 22050;
            i2s_set_sample_rates(I2S_NUM_0, sampleRate);

            int bytesIn = f.readBytes(reinterpret_cast<char*>(fbuf), sizeof(fbuf));
            while (bytesIn > 0 && !s_stop) {
                for (int i = 0; i < bytesIn / 2; i++)
                    obuf[i] = (uint32_t)((int32_t)fbuf[i] + 32768) << 16;
                // Bounded timeout so stop signal is seen promptly
                i2s_write(I2S_NUM_0, obuf, (size_t)(bytesIn * 2), &bw, pdMS_TO_TICKS(200));
                bytesIn = f.readBytes(reinterpret_cast<char*>(fbuf), sizeof(fbuf));
            }
            f.close();
        }

        i2s_driver_uninstall(I2S_NUM_0);
        s_playing = false;
    }
}

void samplePlayerInit() {
    s_trigger = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(wavTaskFn, "WAV", 4096, nullptr, 5, nullptr, 0);  // Core 0
}

void samplePlayerPlay(const char* path) {
    samplePlayerStop();  // wait for any current playback to finish
    strncpy(s_path, path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';
    xSemaphoreGive(s_trigger);
}

void samplePlayerStop() {
    if (!s_playing) return;
    s_stop = true;
    uint32_t deadline = millis() + 400;
    while (s_playing && (int32_t)(millis() - deadline) < 0)
        vTaskDelay(pdMS_TO_TICKS(10));
}

bool samplePlayerIsPlaying() {
    return s_playing;
}

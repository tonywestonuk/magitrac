#include "SamplePlayer.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
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

static volatile bool      s_stop       = false;
static volatile bool      s_playing    = false;
static SemaphoreHandle_t  s_trigger    = nullptr;
static portMUX_TYPE       s_pathMux    = portMUX_INITIALIZER_UNLOCKED;
static char               s_pendingPath[64];
static volatile bool      s_havePending = false;

static void wavTaskFn(void*) {
    char activePath[64];
    for (;;) {
        xSemaphoreTake(s_trigger, portMAX_DELAY);

        bool havePath;
        portENTER_CRITICAL(&s_pathMux);
        havePath = s_havePending;
        if (havePath) {
            memcpy(activePath, s_pendingPath, sizeof(activePath));
            s_havePending = false;
        }
        portEXIT_CRITICAL(&s_pathMux);
        if (!havePath) continue;

        s_stop    = false;
        s_playing = true;

        // All slow SD work happens BEFORE the DAC turns on, so the moment
        // we enable it the first audio sample is queued and ready.  This
        // eliminates the "garbage / zero output" window between DAC enable
        // and first audio write that produced the previous start click.
        File f = SD.open(activePath);
        if (f) {
            int16_t  fbuf[256];
            uint32_t obuf[256];
            size_t   bw;

            uint8_t hdr[44];
            f.read(hdr, 44);
            uint32_t sampleRate = (uint32_t)hdr[24]
                                | ((uint32_t)hdr[25] << 8)
                                | ((uint32_t)hdr[26] << 16)
                                | ((uint32_t)hdr[27] << 24);
            if (sampleRate < 8000 || sampleRate > 48000) sampleRate = 22050;

            // Pre-read and convert the first audio chunk while the DAC is
            // still off.  The expensive SD read happens here, off the
            // critical audio-start path.
            int bytesIn = f.readBytes(reinterpret_cast<char*>(fbuf), sizeof(fbuf));
            for (int i = 0; i < bytesIn / 2; i++)
                obuf[i] = (uint32_t)((int32_t)fbuf[i] + 32768) << 16;

            // Bring the I2S + DAC up now — first audio chunk goes into the
            // DMA on the very next instruction, so the DAC never sees stale
            // / zero data.
            i2s_driver_install(I2S_NUM_0, &s_i2s_cfg, 0, NULL);
            i2s_set_pin(I2S_NUM_0, NULL);
            i2s_set_sample_rates(I2S_NUM_0, sampleRate);
            i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);

            if (bytesIn > 0) {
                i2s_write(I2S_NUM_0, obuf, (size_t)(bytesIn * 2), &bw, pdMS_TO_TICKS(200));
                bytesIn = f.readBytes(reinterpret_cast<char*>(fbuf), sizeof(fbuf));
                while (bytesIn > 0 && !s_stop) {
                    for (int i = 0; i < bytesIn / 2; i++)
                        obuf[i] = (uint32_t)((int32_t)fbuf[i] + 32768) << 16;
                    i2s_write(I2S_NUM_0, obuf, (size_t)(bytesIn * 2), &bw, pdMS_TO_TICKS(200));
                    bytesIn = f.readBytes(reinterpret_cast<char*>(fbuf), sizeof(fbuf));
                }
            }
            f.close();

            i2s_driver_uninstall(I2S_NUM_0);
        }

        s_playing = false;

        portENTER_CRITICAL(&s_pathMux);
        bool more = s_havePending;
        portEXIT_CRITICAL(&s_pathMux);
        if (more) xSemaphoreGive(s_trigger);
    }
}

void samplePlayerInit() {
    s_trigger = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(wavTaskFn, "WAV", 4096, nullptr, 5, nullptr, 0);  // Core 0
}

// Non-blocking: queues `path` for playback.  If a sample is already playing
// it is interrupted; the new one starts as soon as the task picks up the
// queued path.
void samplePlayerPlay(const char* path) {
    portENTER_CRITICAL(&s_pathMux);
    strncpy(s_pendingPath, path, sizeof(s_pendingPath) - 1);
    s_pendingPath[sizeof(s_pendingPath) - 1] = '\0';
    s_havePending = true;
    portEXIT_CRITICAL(&s_pathMux);
    s_stop = true;                 // interrupt any current play
    xSemaphoreGive(s_trigger);     // wake task (no-op if already pending)
}

// Non-blocking: signals the task to abort the current play.  Audio stops
// within ~200 ms (the i2s_write timeout); s_playing flips false soon after.
void samplePlayerStop() {
    s_stop = true;
}

bool samplePlayerIsPlaying() {
    return s_playing;
}

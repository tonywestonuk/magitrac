#include "SamplePlayer.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include "driver/dac_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// 8-bit DAC version of SamplePlayer — for A/B comparison against the
// PDM TX version.  Uses driver/dac_continuous.h, output on GPIO 26
// (built-in DAC2, channel mask CH1 in the new API).  Samples are
// converted from int16 signed → uint8 unsigned (top byte after centering)
// before being handed to the DAC.
//
// Trade-offs vs PDM TX:
//   - 8-bit dynamic range (~48 dB SNR) vs 16-bit PDM (~80+ dB) —
//     audibly noisier on quiet passages.
//   - Has audible start/end pops on each play (the decoy-pin trick
//     that eliminates pops in PDM doesn't apply here — DAC drives the
//     pin analog, no GPIO matrix manipulation possible).
//   - dac_continuous is a separate driver from new-style I²S, so no
//     link-time conflict with mic_spectrum's ESP_I2S PDM RX.  Both still
//     use I2S0 hardware internally, mutually exclusive at runtime.
//
// To switch back to PDM, restore the previous version of this file
// (driver/i2s_pdm.h + decoy-pin trick).

#define AMP_PIN   26    // GPIO 26 = DAC2 = DAC_CHANNEL_MASK_CH1

static volatile bool      s_stop       = false;
static volatile bool      s_playing    = false;
static SemaphoreHandle_t  s_trigger    = nullptr;
static portMUX_TYPE       s_pathMux    = portMUX_INITIALIZER_UNLOCKED;
static char               s_pendingPath[64];
static volatile bool      s_havePending = false;

// Ping-pong read buffers + an 8-bit DAC buffer.  Larger chunks = fewer
// SD reads per second = lower risk of DMA starvation.
static int16_t            s_bufA[1024];
static int16_t            s_bufB[1024];
static uint8_t            s_dbuf[1024];

static const int          FADE_IN_SAMPLES  = 1024;
static const int          FADE_OUT_SAMPLES = 1024;
static int                s_fadeInRemaining = 0;

static void applyFadeIn(int16_t* buf, int samples) {
    if (s_fadeInRemaining <= 0) return;
    int n = samples < s_fadeInRemaining ? samples : s_fadeInRemaining;
    int alreadyFaded = FADE_IN_SAMPLES - s_fadeInRemaining;
    for (int i = 0; i < n; i++) {
        buf[i] = (int16_t)((int32_t)buf[i] * (alreadyFaded + i) / FADE_IN_SAMPLES);
    }
    s_fadeInRemaining -= n;
}

static void applyFadeOut(int16_t* buf, int samples) {
    int n = samples < FADE_OUT_SAMPLES ? samples : FADE_OUT_SAMPLES;
    int start = samples - n;
    for (int i = 0; i < n; i++) {
        buf[start + i] = (int16_t)((int32_t)buf[start + i] * (n - i) / n);
    }
}

// Convert int16 signed → uint8 unsigned (top byte after centering).
static void int16ToUint8(const int16_t* in, uint8_t* out, int samples) {
    for (int i = 0; i < samples; i++) {
        out[i] = (uint8_t)(((int32_t)in[i] + 32768) >> 8);
    }
}

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

        File f = SD.open(activePath);
        if (f) {
            uint8_t hdr[44];
            f.read(hdr, 44);
            uint32_t sampleRate = (uint32_t)hdr[24]
                                | ((uint32_t)hdr[25] << 8)
                                | ((uint32_t)hdr[26] << 16)
                                | ((uint32_t)hdr[27] << 24);
            if (sampleRate < 8000 || sampleRate > 48000) sampleRate = 22050;

            // Pre-read and fade-in the first audio chunk.
            int16_t* current = s_bufA;
            int16_t* next    = s_bufB;
            int curBytes = f.readBytes(reinterpret_cast<char*>(current), sizeof(s_bufA));
            s_fadeInRemaining = FADE_IN_SAMPLES;
            applyFadeIn(current, curBytes / 2);

            // ── DAC continuous setup ──────────────────────────────────
            // Large DMA buffer (8 × 2048 = 16 KB ≈ 740 ms at 22 kHz)
            // gives the SD reader plenty of headroom for a slow block.
            dac_continuous_handle_t dac = nullptr;
            dac_continuous_config_t cfg = {};
            cfg.chan_mask = DAC_CHANNEL_MASK_CH1;     // GPIO 26 (DAC2)
            cfg.desc_num  = 8;
            cfg.buf_size  = 2048;
            cfg.freq_hz   = sampleRate;
            cfg.offset    = 0;
            cfg.clk_src   = DAC_DIGI_CLK_SRC_DEFAULT;
            cfg.chan_mode = DAC_CHANNEL_MODE_SIMUL;

            esp_err_t r = dac_continuous_new_channels(&cfg, &dac);
            if (r == ESP_OK) r = dac_continuous_enable(dac);
            if (r != ESP_OK) {
                Serial.printf("[SP] DAC init err=%d\n", (int)r);
                if (dac) dac_continuous_del_channels(dac);
                f.close();
                s_playing = false;
                continue;
            }

            // Ping-pong with one-chunk lookahead — apply fade-out to
            // the chunk just before the EOF chunk so audio fades into
            // silence.
            size_t bw;
            int samplesIn = curBytes / 2;
            int16ToUint8(current, s_dbuf, samplesIn);
            dac_continuous_write(dac, s_dbuf, samplesIn, &bw, 1000);

            int curBytes2 = f.readBytes(reinterpret_cast<char*>(next), sizeof(s_bufB));
            int16_t* cur2 = next;
            int16_t* nxt2 = current;

            while (curBytes2 > 0 && !s_stop) {
                int nextBytes = f.readBytes(reinterpret_cast<char*>(nxt2), sizeof(s_bufA));
                if (nextBytes > 0 && !s_stop) {
                    applyFadeIn(cur2, curBytes2 / 2);
                }
                if (nextBytes == 0 || s_stop) {
                    applyFadeOut(cur2, curBytes2 / 2);
                }
                int n = curBytes2 / 2;
                int16ToUint8(cur2, s_dbuf, n);
                dac_continuous_write(dac, s_dbuf, n, &bw, 1000);
                int16_t* tmp = cur2; cur2 = nxt2; nxt2 = tmp;
                curBytes2 = nextBytes;
            }
            f.close();

            dac_continuous_disable(dac);
            dac_continuous_del_channels(dac);
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

void samplePlayerPlay(const char* path) {
    portENTER_CRITICAL(&s_pathMux);
    strncpy(s_pendingPath, path, sizeof(s_pendingPath) - 1);
    s_pendingPath[sizeof(s_pendingPath) - 1] = '\0';
    s_havePending = true;
    portEXIT_CRITICAL(&s_pathMux);
    s_stop = true;
    xSemaphoreGive(s_trigger);
}

void samplePlayerStop() {
    s_stop = true;
}

bool samplePlayerIsPlaying() {
    return s_playing;
}

#include "SamplePlayer.h"
#include "sd_mutex.h"
#include "mic_spectrum.h"      // I²S0 mutex with PDM mic
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include "driver/dac_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

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

static volatile bool             s_stop       = false;
static volatile bool             s_playing    = false;
static SemaphoreHandle_t         s_trigger    = nullptr;
static portMUX_TYPE              s_pathMux    = portMUX_INITIALIZER_UNLOCKED;
static char                      s_pendingPath[64];
static volatile bool             s_havePending = false;

// DAC handle: allocated on first play, kept across back-to-back plays at
// the same rate (no realloc), released at end of play.
static dac_continuous_handle_t   s_dac        = nullptr;
static uint32_t                  s_dacRate    = 0;

// Ping-pong read buffers + 8-bit DAC buffer.  ~5 KB total, allocated on
// each play and freed on completion so nothing sits in BSS when idle.
static const int                 BUF_SAMPLES  = 1024;
static const int                 BUF_BYTES    = BUF_SAMPLES * 2;

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

// Ensure s_dac is allocated and configured for `rate`.  Returns true on
// success.  Strategy:
//   1. If already configured at the right rate, no-op.
//   2. Otherwise tear down (if any), then try the big (16 KB) buffer.
//   3. If that fails (DMA RAM fragmented post-WiFi), fall back to 4 KB.
//   4. Cache the rate so subsequent matching plays skip all alloc work.
static bool ensureDacForRate(uint32_t rate) {
    if (s_dac && s_dacRate == rate) return true;

    if (s_dac) {
        dac_continuous_disable(s_dac);
        dac_continuous_del_channels(s_dac);
        s_dac = nullptr;
        s_dacRate = 0;
    }

    dac_continuous_config_t cfg = {};
    cfg.chan_mask = DAC_CHANNEL_MASK_CH1;     // GPIO 26 (DAC2)
    cfg.freq_hz   = rate;
    cfg.offset    = 0;
    cfg.clk_src   = DAC_DIGI_CLK_SRC_DEFAULT;
    cfg.chan_mode = DAC_CHANNEL_MODE_SIMUL;

    // 4 × 1024 = 4 KB DMA — ~46 ms at 44 kHz, comfortably above the
    // ~10 ms SD-read latency.  Bigger buffers (8 × 2048 = 16 KB) starve
    // WiFi/TCP of DMA-capable RAM, so 4 KB is the cap.
    cfg.desc_num = 4;
    cfg.buf_size = 1024;
    esp_err_t r = dac_continuous_new_channels(&cfg, &s_dac);
    if (r != ESP_OK) {
        Serial.printf("[SP] DAC alloc failed err=%d dma-free=%u\n",
            (int)r,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
        s_dac = nullptr;
        return false;
    }
    s_dacRate = rate;
    Serial.printf("[SP] DAC configured for %u Hz (%u × %u byte DMA)\n",
        (unsigned)rate, (unsigned)cfg.desc_num, (unsigned)cfg.buf_size);
    return true;
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

        // Mic / DAC mutual exclusion on I²S0.  If the mic is active for
        // a needsMic effect, stop it for the duration of the sample then
        // restart afterwards so the user's effect doesn't get permanently
        // muted by a one-off SFX trigger.
        bool micWasActive = spectrumIsActive();
        if (micWasActive) {
            spectrumSetActive(false);
            spectrumWaitStopped(200);   // give I²S0 time to free
        }

        File f;
        { SdLock _; f = SD.open(activePath); }
        if (f) {
            uint8_t hdr[44];
            { SdLock _; f.read(hdr, 44); }
            uint32_t sampleRate = (uint32_t)hdr[24]
                                | ((uint32_t)hdr[25] << 8)
                                | ((uint32_t)hdr[26] << 16)
                                | ((uint32_t)hdr[27] << 24);
            if (sampleRate < 8000 || sampleRate > 48000) sampleRate = 22050;

            uint8_t* playBufs = (uint8_t*)malloc(BUF_BYTES * 2 + BUF_SAMPLES);
            if (!playBufs) {
                Serial.println("[SP] buffer alloc failed");
                { SdLock _; f.close(); }
                s_playing = false;
                if (micWasActive) spectrumSetActive(true);
                continue;
            }
            int16_t* s_bufA = (int16_t*)playBufs;
            int16_t* s_bufB = (int16_t*)(playBufs + BUF_BYTES);
            uint8_t* s_dbuf = playBufs + BUF_BYTES * 2;

            int16_t* current = s_bufA;
            int16_t* next    = s_bufB;
            int curBytes;
            { SdLock _; curBytes = f.readBytes(reinterpret_cast<char*>(current), BUF_BYTES); }
            s_fadeInRemaining = FADE_IN_SAMPLES;
            applyFadeIn(current, curBytes / 2);

            if (!ensureDacForRate(sampleRate)) {
                { SdLock _; f.close(); }
                free(playBufs);
                s_playing = false;
                if (micWasActive) spectrumSetActive(true);
                continue;
            }
            esp_err_t r = dac_continuous_enable(s_dac);
            if (r != ESP_OK) {
                Serial.printf("[SP] dac_continuous_enable err=%d\n", (int)r);
                { SdLock _; f.close(); }
                free(playBufs);
                s_playing = false;
                if (micWasActive) spectrumSetActive(true);
                continue;
            }
            dac_continuous_handle_t dac = s_dac;

            // Ping-pong with one-chunk lookahead — apply fade-out to
            // the chunk just before the EOF chunk so audio fades into
            // silence.
            size_t bw;
            int samplesIn = curBytes / 2;
            int16ToUint8(current, s_dbuf, samplesIn);
            dac_continuous_write(dac, s_dbuf, samplesIn, &bw, 1000);

            int curBytes2;
            { SdLock _; curBytes2 = f.readBytes(reinterpret_cast<char*>(next), BUF_BYTES); }
            int16_t* cur2 = next;
            int16_t* nxt2 = current;

            while (curBytes2 > 0 && !s_stop) {
                int nextBytes;
                { SdLock _; nextBytes = f.readBytes(reinterpret_cast<char*>(nxt2), BUF_BYTES); }
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
            { SdLock _; f.close(); }

            dac_continuous_disable(dac);
            free(playBufs);
        }

        s_playing = false;

        portENTER_CRITICAL(&s_pathMux);
        bool more = s_havePending;
        portEXIT_CRITICAL(&s_pathMux);
        // Hold the DAC across back-to-back plays to skip the realloc;
        // release on the last one so I²S0 is free for the mic again.
        if (!more) {
            samplePlayerReleaseDac();
            if (micWasActive) spectrumSetActive(true);
        } else {
            xSemaphoreGive(s_trigger);
        }
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

void samplePlayerReleaseDac() {
    if (s_dac) {
        Serial.println("[SP] releasing DAC for mic activation");
        dac_continuous_disable(s_dac);
        dac_continuous_del_channels(s_dac);
        s_dac = nullptr;
        s_dacRate = 0;
    }
}

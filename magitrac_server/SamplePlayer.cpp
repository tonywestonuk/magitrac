#include "SamplePlayer.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include "driver/dac_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Built-in DAC on GPIO 25 = DAC1 = channel 0.  Using dac_continuous (new IDF5
// driver) instead of the legacy I²S DAC path so mic_spectrum can use the new
// ESP_I2S PDM driver — legacy and new I²S drivers can't coexist in the same
// firmware (IDF v5 enforces this at boot).  dac_continuous still uses I2S0
// internally on ESP32 classic, but it's a separate driver from the I²S ones
// and only acquires the hardware between dac_continuous_new_channels and
// dac_continuous_del_channels — so mic and sample playback are mutually
// exclusive at runtime, not link-time.

static volatile bool      s_stop       = false;
static volatile bool      s_playing    = false;
static SemaphoreHandle_t  s_trigger    = nullptr;
static portMUX_TYPE       s_pathMux    = portMUX_INITIALIZER_UNLOCKED;
static char               s_pendingPath[64];
static volatile bool      s_havePending = false;

// Read/convert buffers live in BSS so the WAV task stack stays small.
// 2 KB int16 in, 2 KB uint8 out per iteration → larger chunks = fewer SD
// reads per second = lower risk of DMA starvation on a slow SD block.
static int16_t            s_fbuf[1024];
static uint8_t            s_dbuf[1024];

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
            size_t bw;

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
            int bytesIn = f.readBytes(reinterpret_cast<char*>(s_fbuf), sizeof(s_fbuf));
            int samplesIn = bytesIn / 2;
            for (int i = 0; i < samplesIn; i++) {
                // int16 signed → uint8 unsigned (top byte after centering).
                s_dbuf[i] = (uint8_t)(((int32_t)s_fbuf[i] + 32768) >> 8);
            }

            // Bring the DAC up now — first chunk goes out on the very next
            // instruction so the DAC never sees stale / zero data.  Large
            // DMA buffer (8 × 2048 = 16 KB ≈ 740 ms at 22 kHz) gives the
            // SD reader plenty of headroom for a slow block fetch.
            dac_continuous_handle_t dac = nullptr;
            dac_continuous_config_t cfg = {};
            cfg.chan_mask = DAC_CHANNEL_MASK_CH1;     // GPIO 25 (M5Stack speaker)
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

            if (samplesIn > 0) {
                dac_continuous_write(dac, s_dbuf, samplesIn, &bw, 1000);
                bytesIn = f.readBytes(reinterpret_cast<char*>(s_fbuf), sizeof(s_fbuf));
                while (bytesIn > 0 && !s_stop) {
                    samplesIn = bytesIn / 2;
                    for (int i = 0; i < samplesIn; i++) {
                        s_dbuf[i] = (uint8_t)(((int32_t)s_fbuf[i] + 32768) >> 8);
                    }
                    dac_continuous_write(dac, s_dbuf, samplesIn, &bw, 1000);
                    bytesIn = f.readBytes(reinterpret_cast<char*>(s_fbuf), sizeof(s_fbuf));
                }
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
// within ~200 ms (the dac_continuous_write timeout); s_playing flips false
// soon after.
void samplePlayerStop() {
    s_stop = true;
}

bool samplePlayerIsPlaying() {
    return s_playing;
}

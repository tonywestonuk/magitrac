#include "SamplePlayer.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include "driver/i2s_pdm.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// PDM TX out of GPIO 26 (M5Stack Core Basic speaker line, DAC2 pin).
//
// Click-free start / stop using a "decoy pin" technique:
//   1. I²S is initialised pointing at DUMMY_PIN (GPIO 13, not wired).
//      The enable transient — the modulator's startup glitch that we
//      can't suppress any other way — therefore lands on pin 13, where
//      no amp can hear it.
//   2. ~100 ms settle, modulator stabilises on preloaded silence.
//   3. The live I²S signal is *additionally* routed to GPIO 26 via a
//      raw GPIO matrix call (esp_rom_gpio_connect_out_signal).  Pin 26
//      smoothly takes the running PDM-silence (50% duty ≈ mid-rail)
//      with no step — matches the bias the amp's input cap already
//      held the pin at.
//   4. Audio plays.
//   5. Pin 26 is detached from the matrix and goes back to high-Z
//      INPUT.  Amp's input cap stays at mid-rail bias.
//   6. I²S disable lands its tear-down transient on pin 13 again,
//      inaudibly.
//
// Uses IDF I²S API directly (driver/i2s_pdm.h) so we can preload silence
// into DMA before enable and so we can manipulate the GPIO matrix
// ourselves.  ESP_I2S wrapper doesn't expose either.
//
// Per-play install/uninstall: I²S0 is freed for mic_spectrum's PDM RX
// between sample plays.

#define AMP_PIN     26     // speaker / amp input
#define DUMMY_PIN   13     // see note below
#define I2S_PDM_TX_SIG  I2S0O_DATA_OUT23_IDX

// DUMMY_PIN choice: GPIO 13 on the M5Stack Core Basic is
//   - not referenced anywhere else in this codebase (greppable),
//   - not an ESP32 strapping pin (those are 0, 2, 5, 12, 15) — safe to
//     drive at boot,
//   - labelled "I2S_WS" on M5Stack's expansion-header silk-screen, but
//     that's a hardware convention, not an electrical constraint.  As
//     long as nothing is physically wired to GPIO 13 (e.g. an M5Stack
//     I²S HAT or other accessory using the expansion header), the PDM
//     bitstream on pin 13 is silent / harmless.
// If you ever plug in an accessory that uses GPIO 13, pick another free
// non-strapping pin and update DUMMY_PIN.

static volatile bool      s_stop       = false;
static volatile bool      s_playing    = false;
static SemaphoreHandle_t  s_trigger    = nullptr;
static portMUX_TYPE       s_pathMux    = portMUX_INITIALIZER_UNLOCKED;
static char               s_pendingPath[64];
static volatile bool      s_havePending = false;

// Ping-pong read buffers — one chunk of lookahead so we can detect the
// last chunk and apply fade-out before writing it.
static int16_t            s_bufA[1024];
static int16_t            s_bufB[1024];

// Silence buffer — also used for DMA preload.
static int16_t            s_silence[512];

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

// Route the running I²S PDM TX signal to the amp pin.  Modulator must
// already be enabled and outputting silence — this only adds GPIO 26
// as an additional output destination of the same signal that's already
// going to GPIO 13.
static void connectAmpPin() {
    gpio_set_direction((gpio_num_t)AMP_PIN, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(AMP_PIN, I2S_PDM_TX_SIG, false, false);
}

// Stop driving the amp pin.  Matrix says "no peripheral", pad goes to
// high-Z INPUT — amp's input cap holds at its bias point (mid-rail),
// which matches PDM-silence, so the disconnect itself is silent.
static void disconnectAmpPin() {
    esp_rom_gpio_connect_out_signal(AMP_PIN, SIG_GPIO_OUT_IDX, false, false);
    pinMode(AMP_PIN, INPUT);
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

            // ── IDF I²S PDM TX setup — init pointing at DUMMY_PIN ────
            // so the enable transient is inaudible.
            i2s_chan_handle_t tx_chan = nullptr;
            i2s_chan_config_t chan_cfg = {};
            chan_cfg.id            = I2S_NUM_0;
            chan_cfg.role          = I2S_ROLE_MASTER;
            chan_cfg.dma_desc_num  = 6;
            chan_cfg.dma_frame_num = 240;
            chan_cfg.auto_clear    = true;

            esp_err_t r = i2s_new_channel(&chan_cfg, &tx_chan, nullptr);
            if (r != ESP_OK) {
                Serial.printf("[SP] i2s_new_channel err=%d\n", (int)r);
                f.close();
                s_playing = false;
                continue;
            }

            i2s_pdm_tx_config_t pdm_cfg = {
                .clk_cfg  = I2S_PDM_TX_CLK_DEFAULT_CONFIG(sampleRate),
                .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(
                                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
                .gpio_cfg = {
                    .clk   = GPIO_NUM_NC,
                    .dout  = (gpio_num_t)DUMMY_PIN,
                    .invert_flags = { .clk_inv = false },
                },
            };
            r = i2s_channel_init_pdm_tx_mode(tx_chan, &pdm_cfg);
            if (r != ESP_OK) {
                Serial.printf("[SP] init_pdm_tx_mode err=%d\n", (int)r);
                i2s_del_channel(tx_chan);
                f.close();
                s_playing = false;
                continue;
            }

            // Preload DMA with silence so the modulator starts on
            // known-zero data, not heap garbage.
            size_t totalToLoad = 6 * 240 * sizeof(int16_t);
            size_t loadedTotal = 0;
            while (loadedTotal < totalToLoad) {
                size_t thisLoad = 0;
                size_t chunk = totalToLoad - loadedTotal;
                if (chunk > sizeof(s_silence)) chunk = sizeof(s_silence);
                if (i2s_channel_preload_data(tx_chan, s_silence, chunk, &thisLoad) != ESP_OK
                    || thisLoad == 0) break;
                loadedTotal += thisLoad;
            }

            r = i2s_channel_enable(tx_chan);
            if (r != ESP_OK) {
                Serial.printf("[SP] i2s_channel_enable err=%d\n", (int)r);
                i2s_del_channel(tx_chan);
                f.close();
                s_playing = false;
                continue;
            }

            // Let the modulator settle on the DUMMY_PIN before we route
            // the live signal to the amp pin.
            delay(100);
            connectAmpPin();

            // Ping-pong with one-chunk lookahead — apply fade-out to
            // the chunk just before the EOF chunk so audio fades into
            // silence.
            size_t bw;
            while (curBytes > 0 && !s_stop) {
                int nextBytes = f.readBytes(reinterpret_cast<char*>(next), sizeof(s_bufB));
                if (nextBytes > 0 && !s_stop) {
                    applyFadeIn(next, nextBytes / 2);
                }
                if (nextBytes == 0 || s_stop) {
                    applyFadeOut(current, curBytes / 2);
                }
                i2s_channel_write(tx_chan, current, curBytes, &bw, 1000);
                int16_t* tmp = current; current = next; next = tmp;
                curBytes = nextBytes;
            }
            f.close();

            // Tail silence — modulator at mid-rail before we drop
            // the amp pin.
            i2s_channel_write(tx_chan, s_silence, sizeof(s_silence), &bw, 1000);

            // Disconnect amp pin BEFORE disable — disable transient
            // lands on DUMMY_PIN, inaudible.
            disconnectAmpPin();
            delay(20);

            i2s_channel_disable(tx_chan);
            i2s_del_channel(tx_chan);
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
    pinMode(AMP_PIN, INPUT);   // amp pin starts high-Z; amp's bias holds it
    xTaskCreatePinnedToCore(wavTaskFn, "WAV", 4096, nullptr, 5, nullptr, 0);  // Core 0
}

// Non-blocking: queues `path` for playback.  If a sample is already
// playing it is interrupted; the new one starts as soon as the task
// picks up the queued path.
void samplePlayerPlay(const char* path) {
    portENTER_CRITICAL(&s_pathMux);
    strncpy(s_pendingPath, path, sizeof(s_pendingPath) - 1);
    s_pendingPath[sizeof(s_pendingPath) - 1] = '\0';
    s_havePending = true;
    portEXIT_CRITICAL(&s_pathMux);
    s_stop = true;
    xSemaphoreGive(s_trigger);
}

// Non-blocking: signals the task to abort the current play.
void samplePlayerStop() {
    s_stop = true;
}

bool samplePlayerIsPlaying() {
    return s_playing;
}

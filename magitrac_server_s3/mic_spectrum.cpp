// mic_spectrum.cpp — ES8388 mic capture + FFT + beat detect for CoreS3.
//
// Reads 16-bit stereo from the shared audio codec (32 kHz), down-mixes to
// mono, runs an FFT, broadcasts per-band magnitude and beat events to
// pixel_post.  Codec is full-duplex with SamplePlayer; no mutex needed.

#include "mic_spectrum.h"
#include "audio_codec.h"
#include <Arduino.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <arduinoFFT.h>
#include <pixelpost_proto.h>

#define SAMPLE_RATE      AUDIO_CODEC_RATE_HZ
#define SAMPLES_PER_READ 1024     // mono samples; reads 2048 stereo bytes from codec

extern void pixelpostEnqueue(const uint8_t* payload, size_t len);

static float*             vReal     = nullptr;
static float*             vImag     = nullptr;
static int16_t*           stereoBuf = nullptr;
static ArduinoFFT<float>* FFT       = nullptr;

static SemaphoreHandle_t sActiveSem = nullptr;
static volatile bool     sActive    = false;
static bool              sBuffersUp = false;

static float gain[5] = { 60.0f, 60.0f, 60.0f, 60.0f, 60.0f };

static int freqToBin(float f) {
    float binHz = (float)SAMPLE_RATE / (float)SAMPLES_PER_READ;
    int bin = (int)lroundf(f / binHz);
    if (bin < 0) bin = 0;
    if (bin > SAMPLES_PER_READ / 2) bin = SAMPLES_PER_READ / 2;
    return bin;
}

static float bandMagnitude(float fLow, float fHigh) {
    int k0 = freqToBin(fLow);
    int k1 = freqToBin(fHigh);
    if (k1 < k0) { int t = k0; k0 = k1; k1 = t; }
    float sum = 0.0f;
    for (int k = k0; k <= k1; ++k) sum += vReal[k];
    return sum / (float)(k1 - k0 + 1);
}

static void freeBuffers() {
    delete FFT;        FFT       = nullptr;
    free(vReal);       vReal     = nullptr;
    free(vImag);       vImag     = nullptr;
    free(stereoBuf);   stereoBuf = nullptr;
    sBuffersUp = false;
}

static bool allocBuffers() {
    if (sBuffersUp) return true;
    vReal     = (float*)  malloc(SAMPLES_PER_READ * sizeof(float));
    vImag     = (float*)  malloc(SAMPLES_PER_READ * sizeof(float));
    stereoBuf = (int16_t*)malloc(SAMPLES_PER_READ * 2 * sizeof(int16_t));
    if (!vReal || !vImag || !stereoBuf) {
        Serial.println("[MIC] buffer alloc FAILED");
        freeBuffers();
        return false;
    }
    FFT = new ArduinoFFT<float>(vReal, vImag, SAMPLES_PER_READ, (float)SAMPLE_RATE);
    sBuffersUp = true;
    Serial.println("[MIC] buffers ready");
    return true;
}

static void spectrumTask(void*) {
    for (;;) {
        if (!sActive) {
            freeBuffers();
            xSemaphoreTake(sActiveSem, portMAX_DELAY);
            if (!sActive) continue;
            if (!allocBuffers()) { sActive = false; continue; }
        }

        if (!audioCodecRecord((uint8_t*)stereoBuf, SAMPLES_PER_READ * 2 * sizeof(int16_t))) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Downmix L+R → mono, scaled into roughly [-1, +1] for the FFT.
        for (int i = 0; i < SAMPLES_PER_READ; i++) {
            int32_t mix = (int32_t)stereoBuf[i * 2] + (int32_t)stereoBuf[i * 2 + 1];
            vReal[i] = (float)mix / 65536.0f;
            vImag[i] = 0.0f;
        }

        FFT->windowing(FFTWindow::Hamming, FFTDirection::Forward);
        FFT->compute(FFTDirection::Forward);
        FFT->complexToMagnitude();

        // ── Beat detection (Patin-style energy threshold on bass band) ───
        static float    sBeatHistory[32] = {0};
        static int      sBeatHistIdx  = 0;
        static bool     sBeatHistFull = false;
        static uint32_t sLastBeatMs   = 0;

        float bassMag = bandMagnitude(50.0f, 200.0f);

        int histN = sBeatHistFull ? 32 : sBeatHistIdx;
        float histSum = 0.0f;
        for (int i = 0; i < histN; i++) histSum += sBeatHistory[i];
        float histMean = histN > 0 ? histSum / (float)histN : 0.0f;

        uint32_t nowMs = millis();
        const float    BEAT_THRESHOLD = 1.4f;
        const uint32_t BEAT_REFRACTORY_MS = 200;
        const float    BEAT_SQUELCH = 0.0008f;

        if (histMean > BEAT_SQUELCH &&
            bassMag > histMean * BEAT_THRESHOLD &&
            nowMs - sLastBeatMs > BEAT_REFRACTORY_MS) {
            sLastBeatMs = nowMs;
            int strength = (int)((bassMag / histMean - 1.0f) * 128.0f);
            if (strength < 32)  strength = 32;
            if (strength > 255) strength = 255;
            uint8_t beatMsg[2] = { PP_MSG_BEAT, (uint8_t)strength };
            pixelpostEnqueue(beatMsg, sizeof(beatMsg));
        }

        sBeatHistory[sBeatHistIdx] = bassMag;
        sBeatHistIdx = (sBeatHistIdx + 1) % 32;
        if (sBeatHistIdx == 0) sBeatHistFull = true;

        uint8_t msg[6];
        msg[0] = PP_MSG_SPECTRUM;
        const float startFreq = 130.0f;
        for (int i = 0; i < 5; i++) {
            float lowFreq  = startFreq * powf(2.0f, (float)i);
            float highFreq = startFreq * powf(2.0f, (float)(i + 1));
            int itm = (int)(bandMagnitude(lowFreq, highFreq) * gain[i]);
            if (itm > 255) itm = 255;
            gain[i] *= (itm < 22) ? 1.02f : 0.94f;
            if (gain[i] < 1.0f)     gain[i] = 1.0f;
            if (gain[i] > 50000.0f) gain[i] = 50000.0f;
            msg[i + 1] = (uint8_t)itm;
        }
        pixelpostEnqueue(msg, sizeof(msg));
    }
}

void spectrumInit() {
    if (sActiveSem) return;
    sActiveSem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(spectrumTask, "mic", 8192, NULL, 1, NULL, 0);
}

void spectrumSetActive(bool active) {
    Serial.printf("[MIC] setActive(%d) was=%d\n", (int)active, (int)sActive);
    if (active == sActive) return;
    sActive = active;
    if (active) xSemaphoreGive(sActiveSem);
}

bool spectrumIsActive() { return sActive; }

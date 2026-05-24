// mic_spectrum.cpp — see header.
//
// FFT params mirror the M5Paper controller (32 kHz / 2048 samples, 5 octave
// bands from 130 Hz, per-band autogain), so pixel_post's SoundSpectrum
// effect renders identically whether the bands come from the controller or
// from here.
//
// Uses the new ESP_I2S class API (the same one the controller uses
// successfully).  SamplePlayer was converted from legacy I²S to
// dac_continuous specifically to make this possible — IDF v5 won't link
// the legacy and new I²S drivers in the same firmware.

#include "mic_spectrum.h"
#include <Arduino.h>
#include <math.h>
#include <ESP_I2S.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <arduinoFFT.h>
#include <pixelpost_proto.h>

#define PDM_CLK_PIN     22     // Grove Port A — G22 → PDM clock (out, ~2 MHz)
#define PDM_DATA_PIN    21     // Grove Port A — G21 → PDM data  (in from mic)
#define SAMPLE_RATE     32000
#define SAMPLES_PER_READ 2048

extern void pixelpostEnqueue(const uint8_t* payload, size_t len);

// Static so they live in BSS, not on the task stack — 16 KB for the two
// float arrays plus 4 KB for the int16 sample buffer.
static float   vReal[SAMPLES_PER_READ];
static float   vImag[SAMPLES_PER_READ];
static int16_t samples[SAMPLES_PER_READ];

static ArduinoFFT<float> FFT(vReal, vImag, SAMPLES_PER_READ, (float)SAMPLE_RATE);

static I2SClass          I2S;
static SemaphoreHandle_t sActiveSem = nullptr;
static volatile bool     sActive    = false;
static bool              sI2sUp     = false;

// Per-band autogain — multiplicative AGC.  The controller uses linear
// +1/-6 which converges in seconds for loud music but takes minutes for
// quiet ambient sound (high bands need gains in the 10000s).  Multiplicative
// converges in log-time regardless of magnitude.  Ratio 1.05 : 0.85 sits
// in equilibrium around itm ≈ 40 (~80% frames quiet, ~20% loud).
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

static bool startMic() {
    if (sI2sUp) return true;
    I2S.setPinsPdmRx(PDM_CLK_PIN, PDM_DATA_PIN);
    if (!I2S.begin(I2S_MODE_PDM_RX, SAMPLE_RATE,
                   I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        Serial.println("[MIC] I2S.begin FAILED");
        return false;
    }
    sI2sUp = true;
    Serial.println("[MIC] started");
    return true;
}

static void stopMic() {
    if (!sI2sUp) return;
    I2S.end();
    sI2sUp = false;
    Serial.println("[MIC] stopped");
}

static void spectrumTask(void*) {
    for (;;) {
        if (!sActive) {
            stopMic();
            xSemaphoreTake(sActiveSem, portMAX_DELAY);
            if (!sActive) continue;
            if (!startMic()) {
                sActive = false;
                continue;
            }
        }

        I2S.readBytes(reinterpret_cast<char*>(samples), SAMPLES_PER_READ * 2);

        for (int i = 0; i < SAMPLES_PER_READ; i++) {
            // +1610 = empirical DC offset of the controller's mic; same chip
            // family so re-use until measured otherwise.
            vReal[i] = ((float)samples[i] + 1610.0f) / 32768.0f;
            vImag[i] = 0.0f;
        }

        FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
        FFT.compute(FFTDirection::Forward);
        FFT.complexToMagnitude();

        // ── Beat detection (Patin-style energy threshold on bass band) ───
        // Raw 50–200 Hz magnitude — captures kick fundamental + 1st harmonic
        // without hi-hat noise.  Computed pre-AGC so peaks aren't compressed
        // away.  Rolling 32-frame history (~2 s at the 15.6 Hz frame rate).
        // Threshold = 1.4× rolling mean, refractory = 200 ms (300 BPM ceiling).
        // Counting beats mod 4 happens on the pixel_post side.
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
        const float    BEAT_SQUELCH = 0.0008f;  // silence shouldn't trigger

        if (histMean > BEAT_SQUELCH &&
            bassMag > histMean * BEAT_THRESHOLD &&
            nowMs - sLastBeatMs > BEAT_REFRACTORY_MS) {
            sLastBeatMs = nowMs;
            // Strength = how many × over the mean, scaled to 0–255.
            // 1.4× → 51, 3× → 256 (clamped).
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
            // Threshold sets the steady-state value the AGC parks at — on
            // a 44-LED post, 22 puts the average bar around 50%, leaving
            // headroom for transient peaks to push higher.
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
    // Core 0 = WiFi/system core, same as pixel_post worker.  MIDI lives on
    // core 1 and is never touched.  Low priority (1) — WiFi outranks us.
    xTaskCreatePinnedToCore(spectrumTask, "mic", 8192, NULL, 1, NULL, 0);
}

void spectrumSetActive(bool active) {
    Serial.printf("[MIC] setActive(%d) was=%d\n", (int)active, (int)sActive);
    if (active == sActive) return;
    sActive = active;
    if (active) xSemaphoreGive(sActiveSem);
}

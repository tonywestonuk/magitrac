// mic_spectrum.cpp — ES8388 mic capture + FFT + beat detect for CoreS3.
//
// Reads 16-bit stereo from the shared audio codec (32 kHz), down-mixes to
// mono, runs an FFT, broadcasts the 5 per-band magnitudes + beat info on
// each FFT cycle via PPOSB (pixelpostSendAudio).  No heartbeat — when the
// mic isn't active no packets go out.  Posts not running a mic-reactive
// effect drop PPOSB silently.

#include "mic_spectrum.h"
#include "audio_codec.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp32s3_dsp.h"           // hardware-accelerated FFT (~2 ms / 1024 pts)
#include <pixelpost_proto.h>

extern void pixelpostSendAudio(const uint8_t* bands,
                               uint32_t beatSeq, uint8_t beatStrength);

#define SAMPLE_RATE      AUDIO_CODEC_RATE_HZ
#define SAMPLES_PER_READ 1024     // mono samples; reads 2048 stereo bytes from codec

// vReal stays as the public-facing "FFT magnitudes" buffer for
// bandMagnitude(); vImag is gone (esp_dsp returns magnitudes directly).
// monoBuf is the time-domain float input the FFT consumes.
static float*       vReal     = nullptr;
static float*       monoBuf   = nullptr;
static int16_t*     stereoBuf = nullptr;
static ESP32S3_FFT  fft;
static bool         fftInited = false;

static SemaphoreHandle_t sActiveSem   = nullptr;
static volatile bool     sBandsActive = false;   // pixelpost SoundSpectrum gate
static volatile bool     sChordActive = false;   // chord recogniser screen gate
static volatile bool     sScopeActive = false;   // oscilloscope screen gate
static bool              sBuffersUp   = false;
static inline bool wantActive() { return sBandsActive || sChordActive || sScopeActive; }

static float gain[5] = { 60.0f, 60.0f, 60.0f, 60.0f, 60.0f };

// ── Chord path ────────────────────────────────────────────────────────────
// A second, larger FFT (4096-pt ≈ 7.8 Hz/bin at 32 kHz) over a sliding window
// of the last four mic reads.  The 1024-pt band FFT is too coarse to separate
// semitones in the low/mid range; this one resolves them well enough to fold
// into a chroma vector.  Allocated only while the chord screen is up.
#define CHORD_FFT_SIZE 4096
static float*       chordWin   = nullptr;     // last CHORD_FFT_SIZE mono samples
static float*       chordMag   = nullptr;     // magnitudes out of chordFft
static int          chordFill  = 0;           // real samples held in chordWin
static ESP32S3_FFT  chordFft;
static bool         chordFftInited = false;
static bool         chordBuffersUp = false;

static portMUX_TYPE      chordMux  = portMUX_INITIALIZER_UNLOCKED;
static ChordResult       chordLatest;
static volatile uint32_t chordSeq  = 0;

// ── Oscilloscope path ─────────────────────────────────────────────────────
// One DC-blocked, column-averaged sample per display column.  No malloc'd
// buffers — the trace lives in this static array, copied to the UI under a
// spinlock.  (sScopeActive is declared with the other activity flags above.)
// Reads the LEFT channel (LINPUT1, the new mic) — the same source as
// chord + bands.
static int16_t           scopeTrace[SCOPE_COLS];
static portMUX_TYPE      scopeMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t scopeSeq = 0;

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
    // ESP32S3_FFT owns its PSRAM buffers via end()/destructor; we just
    // surrender the caller-side I/O buffers.  Don't call fft.end() here
    // because the next allocBuffers() can reuse the same FFT instance —
    // init() frees and reallocates the internals if the size changes.
    free(vReal);       vReal     = nullptr;
    free(monoBuf);     monoBuf   = nullptr;
    free(stereoBuf);   stereoBuf = nullptr;
    sBuffersUp = false;
}

static bool allocBuffers() {
    if (sBuffersUp) return true;
    vReal     = (float*)  malloc(SAMPLES_PER_READ * sizeof(float));
    monoBuf   = (float*)  malloc(SAMPLES_PER_READ * sizeof(float));
    stereoBuf = (int16_t*)malloc(SAMPLES_PER_READ * 2 * sizeof(int16_t));
    if (!vReal || !monoBuf || !stereoBuf) {
        Serial.println("[MIC] buffer alloc FAILED");
        freeBuffers();
        return false;
    }
    if (!fftInited) {
        if (!fft.init(SAMPLES_PER_READ, SAMPLES_PER_READ, SPECTRAL_AVERAGE)) {
            Serial.println("[MIC] fft.init FAILED — PSRAM exhausted?");
            freeBuffers();
            return false;
        }
        fftInited = true;
    }
    sBuffersUp = true;
    Serial.println("[MIC] buffers ready (hw FFT)");
    return true;
}

static void freeChord() {
    // Surrender the I/O buffers; keep chordFft's PSRAM internals once inited
    // (same policy as the band FFT — cheap to hold, avoids re-init churn).
    free(chordWin);  chordWin = nullptr;
    free(chordMag);  chordMag = nullptr;
    chordFill = 0;
    chordBuffersUp = false;
}

static bool allocChord() {
    if (chordBuffersUp) return true;
    // PSRAM — these are 16 KB each and only sequentially accessed; keep them
    // out of internal SRAM (shared with WiFi/pixelpost).  The chordFft owns
    // its own PSRAM working buffers (see esp32s3_dsp init).
    chordWin = (float*)ps_malloc(CHORD_FFT_SIZE * sizeof(float));
    chordMag = (float*)ps_malloc(CHORD_FFT_SIZE * sizeof(float));
    if (!chordWin || !chordMag) {
        Serial.println("[MIC] chord buffer alloc FAILED");
        freeChord();
        return false;
    }
    if (!chordFftInited) {
        if (!chordFft.init(CHORD_FFT_SIZE, CHORD_FFT_SIZE, SPECTRAL_AVERAGE)) {
            Serial.println("[MIC] chord fft.init FAILED — PSRAM exhausted?");
            freeChord();
            return false;
        }
        chordFftInited = true;
    }
    memset(chordWin, 0, CHORD_FFT_SIZE * sizeof(float));
    chordFill = 0;
    chordBuffersUp = true;
    Serial.println("[MIC] chord buffers ready (4096-pt FFT)");
    return true;
}

// Pixelpost band magnitudes + beat detect, broadcast each FFT cycle.  Reads
// the freshly captured monoBuf and runs the 1024-pt band FFT.
static void processBands() {
    // Hardware FFT — ~2 ms vs ~15 ms for arduinoFFT.  Output is real
    // magnitudes (first SAMPLES_PER_READ/2 bins are unique; we only
    // read the bass + octave bands below).  Hann window is applied
    // inside compute() — bandMagnitude downstream is unchanged.
    fft.compute(monoBuf, vReal, /*use_hann_window=*/true);

    // ── Beat detection (Patin-style energy threshold on bass band) ───
    static float    sBeatHistory[32] = {0};
    static int      sBeatHistIdx  = 0;
    static bool     sBeatHistFull = false;
    static uint32_t sLastBeatMs   = 0;
    static uint32_t sBeatSeq      = 0;
    static uint8_t  sBeatStrength = 0;

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
        sBeatStrength = (uint8_t)strength;
        sBeatSeq++;
    }

    sBeatHistory[sBeatHistIdx] = bassMag;
    sBeatHistIdx = (sBeatHistIdx + 1) % 32;
    if (sBeatHistIdx == 0) sBeatHistFull = true;

    uint8_t bands[PP_AUDIO_BANDS] = {0};
    // Bands 0-3 walk an octave each from 65 Hz; band 4 absorbs everything
    // above 1040 Hz up to ~4160 Hz so cymbals/hats still register.  This
    // isn't a metering analyser — it's lighting that *follows* the music.
    const float startFreq = 65.0f;
    const float topFreq   = 4160.0f;
    for (int i = 0; i < (int)PP_AUDIO_BANDS; i++) {
        float lowFreq  = startFreq * powf(2.0f, (float)i);
        float highFreq = (i == (int)PP_AUDIO_BANDS - 1)
                            ? topFreq
                            : startFreq * powf(2.0f, (float)(i + 1));
        int itm = (int)(bandMagnitude(lowFreq, highFreq) * gain[i]);
        if (itm > 255) itm = 255;
        gain[i] *= (itm < 22) ? 1.01f : 0.97f;
        if (gain[i] < 1.0f)    gain[i] = 1.0f;
        if (gain[i] > 500.0f)  gain[i] = 500.0f;
        bands[i] = (uint8_t)itm;
    }

    pixelpostSendAudio(bands, sBeatSeq, sBeatStrength);
}

// Chroma + chord match over the sliding 4096-sample window.  Called per FFT
// cycle while the chord screen is up; analyses once the window has filled.
static void processChord() {
    // Slide the window: drop the oldest read, append the newest.  Reads the
    // LEFT channel (LINPUT1, the new mic) — the same source the pixelpost
    // bands and the scope use.  This is monoBuf, already downmixed.
    memmove(chordWin, chordWin + SAMPLES_PER_READ,
            (CHORD_FFT_SIZE - SAMPLES_PER_READ) * sizeof(float));
    float* dst = chordWin + (CHORD_FFT_SIZE - SAMPLES_PER_READ);
    for (int i = 0; i < SAMPLES_PER_READ; ++i)
        dst[i] = monoBuf[i];                               // LEFT channel
    if (chordFill < CHORD_FFT_SIZE) chordFill += SAMPLES_PER_READ;
    if (chordFill < CHORD_FFT_SIZE) return;        // not full yet — skip

    chordFft.compute(chordWin, chordMag, /*use_hann_window=*/true);
    ChordResult r = chordAnalyze(chordMag, CHORD_FFT_SIZE / 2,
                                 (float)SAMPLE_RATE, CHORD_FFT_SIZE);

    // Temporal debounce: the displayed chord only *changes* once a new reading
    // has repeated CHORD_STABLE_N times in a row.  Random noise yields a
    // different chord (or none) every frame and never stabilises, so it can't
    // promote itself onto the screen; a held chord agrees with itself and does.
    // The live chroma still updates every frame (bars move) — only the
    // name/root/quality decision is debounced.
    static const int    CHORD_STABLE_N = 3;
    static int8_t       candRoot  = -1;
    static ChordQuality candQual  = CH_NONE;
    static bool         candValid = false;
    static int          candAgree = 0;
    static int8_t       pubRoot   = -1;
    static ChordQuality pubQual   = CH_NONE;
    static bool         pubValid  = false;

    bool same = (r.valid == candValid && r.root == candRoot && r.quality == candQual);
    if (same) { if (candAgree < CHORD_STABLE_N) candAgree++; }
    else      { candValid = r.valid; candRoot = r.root; candQual = r.quality; candAgree = 1; }
    if (candAgree >= CHORD_STABLE_N) { pubValid = candValid; pubRoot = candRoot; pubQual = candQual; }

    ChordResult out = r;                 // keep the live chroma + level
    out.valid   = pubValid;              // but show the debounced decision
    out.root    = pubValid ? pubRoot : -1;
    out.quality = pubQual;

    portENTER_CRITICAL(&chordMux);
    chordLatest = out;
    chordSeq++;
    portEXIT_CRITICAL(&chordMux);
}

// Reduce the freshly captured frame to one sample per display column for the
// oscilloscope: the mean of each column's raw mic (R) samples (a mild
// low-pass that tames per-sample noise and dilutes single-sample glitches),
// with a slow running DC estimate subtracted so the trace stays centred on the
// zero line.  Computed into a local, then published under the spinlock.
static void processScope() {
    const int ch = 0;   // LEFT input (LINPUT1) — new mic
    // Frame DC → slow running estimate (avoids per-frame vertical wobble when
    // the signal isn't an integer number of cycles per frame).
    long sum = 0;
    for (int s = 0; s < SAMPLES_PER_READ; ++s) sum += stereoBuf[s * 2 + ch];
    float dcFrame = (float)sum / (float)SAMPLES_PER_READ;
    static float dcEst = 0.0f;
    dcEst += (dcFrame - dcEst) * 0.05f;

    int16_t tr[SCOPE_COLS];
    for (int c = 0; c < SCOPE_COLS; ++c) {
        int s0 = (int)((long)c       * SAMPLES_PER_READ / SCOPE_COLS);
        int s1 = (int)((long)(c + 1) * SAMPLES_PER_READ / SCOPE_COLS);
        if (s1 <= s0) s1 = s0 + 1;
        long csum = 0;
        for (int s = s0; s < s1; ++s) csum += stereoBuf[s * 2 + ch];
        float m = (float)csum / (float)(s1 - s0) - dcEst;
        if (m >  32767.0f) m =  32767.0f;
        if (m < -32768.0f) m = -32768.0f;
        tr[c] = (int16_t)m;
    }
    portENTER_CRITICAL(&scopeMux);
    memcpy(scopeTrace, tr, sizeof(scopeTrace));
    scopeSeq++;
    portEXIT_CRITICAL(&scopeMux);
}

static void spectrumTask(void*) {
    for (;;) {
        if (!wantActive()) {
            freeBuffers();
            freeChord();
            xSemaphoreTake(sActiveSem, portMAX_DELAY);
            if (!wantActive()) continue;
            if (!allocBuffers()) { sBandsActive = false; sChordActive = false; continue; }
        }

        // Chord buffers come and go with the chord screen, independent of the
        // pixelpost bands.
        if (sChordActive && !chordBuffersUp) {
            if (!allocChord()) sChordActive = false;
        } else if (!sChordActive && chordBuffersUp) {
            freeChord();
        }

        if (!audioCodecRecord((uint8_t*)stereoBuf, SAMPLES_PER_READ * 2 * sizeof(int16_t))) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // L-only into mono, scaled into [-1, +1] for the FFT.  Switched from R
        // to L for the new mic (2026-06-25) — the capsule now drives the LEFT
        // input pair (LINPUT1).  Mixing in the unused R only adds its ADC noise
        // floor, so we take L alone.
        for (int i = 0; i < SAMPLES_PER_READ; i++) {
            monoBuf[i] = (float)stereoBuf[i * 2 + 0] / 32768.0f;
        }

        if (sBandsActive)                    processBands();
        if (sChordActive && chordBuffersUp)  processChord();
        if (sScopeActive)                    processScope();
    }
}

void spectrumInit() {
    if (sActiveSem) return;
    // The ESP-DSP FFT twiddle table is GLOBAL and shared by every ESP32S3_FFT
    // instance; dsps_fft2r_init_fc32() sizes it on the first call and ignores
    // later ones.  We now run two sizes (1024 band + 4096 chord), so size the
    // table for the largest here, before either instance's init() runs —
    // otherwise a band-first init would leave the table too small for the
    // 4096-pt chord FFT (garbage output / OOB reads).
    dsps_fft2r_init_fc32(NULL, CHORD_FFT_SIZE);
    sActiveSem = xSemaphoreCreateBinary();
    // Priority 10 (was 1).  The old prio-1 task sat one notch above idle and
    // was preempted by everything on core 0; 10 keeps the capture loop
    // responsive without outranking WiFi (prio 23).  Note: the ~10 Hz monitor
    // click turned out to be WiFi RF coupling, NOT scheduling — priority alone
    // doesn't fix it (see the 11g/n + beacon-interval change in setup()).
    xTaskCreatePinnedToCore(spectrumTask, "mic", 8192, NULL, 10, NULL, 0);
}

void spectrumSetActive(bool active) {
    Serial.printf("[MIC] setActive(%d) was=%d\n", (int)active, (int)sBandsActive);
    if (active == sBandsActive) return;
    sBandsActive = active;
    if (active) xSemaphoreGive(sActiveSem);
}

bool spectrumIsActive() { return sBandsActive; }

void chordSetActive(bool active) {
    Serial.printf("[MIC] chordSetActive(%d) was=%d\n", (int)active, (int)sChordActive);
    if (active == sChordActive) return;
    sChordActive = active;
    if (active) xSemaphoreGive(sActiveSem);
}

bool chordIsActive() { return sChordActive; }

uint32_t chordResultSeq() { return chordSeq; }

ChordResult chordGetResult() {
    ChordResult r;
    portENTER_CRITICAL(&chordMux);
    r = chordLatest;
    portEXIT_CRITICAL(&chordMux);
    return r;
}

void scopeSetActive(bool active) {
    Serial.printf("[MIC] scopeSetActive(%d) was=%d\n", (int)active, (int)sScopeActive);
    if (active == sScopeActive) return;
    sScopeActive = active;
    if (active) xSemaphoreGive(sActiveSem);
}

bool scopeIsActive() { return sScopeActive; }

uint32_t scopeResultSeq() { return scopeSeq; }

void scopeGetTrace(int16_t* out, int cols) {
    if (cols > SCOPE_COLS) cols = SCOPE_COLS;
    portENTER_CRITICAL(&scopeMux);
    memcpy(out, scopeTrace, cols * sizeof(int16_t));
    portEXIT_CRITICAL(&scopeMux);
}

#include "sample_organ.h"
#include "sd_mutex.h"
#include "esp32s3_dsp.h"
#include "audio_codec.h"
#include <Arduino.h>
#include <SD.h>
#include <math.h>
#include "esp_heap_caps.h"

#define WT          SAMPLE_ORGAN_WT      // 1024
#define FRAMES_MAX  64
#define KMAX        256                  // harmonics rebuilt per wavetable (≤ ~8 kHz @ 31 Hz fund.)
#define FRAME_SEC   0.10f
#define MAX_SECONDS 5

static int16_t*     s_frames   = nullptr;   // FRAMES_MAX × WT (PSRAM)
static volatile int s_frameCount = 0;
static float        s_sinf[WT];             // sine table (float, for the additive rebuild)
static uint16_t     s_phaseIdx[KMAX + 1];   // shared random phase per harmonic
static ESP32S3_FFT  s_fft;
static bool         s_ready = false;

void sampleOrganInit() {
    if (s_ready) return;
    s_frames = (int16_t*)heap_caps_malloc((size_t)FRAMES_MAX * WT * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
    for (int i = 0; i < WT; i++) s_sinf[i] = sinf(2.0f * PI * i / WT);
    uint32_t r = 0x1234567u;
    for (int k = 0; k <= KMAX; k++) { r = r * 1664525u + 1013904223u; s_phaseIdx[k] = (r >> 16) & (WT - 1); }
    dsps_fft2r_init_fc32(NULL, WT);             // global twiddle (idempotent)
    s_fft.init(WT, WT, SPECTRAL_AVERAGE);
    s_ready = (s_frames != nullptr);
}

int sampleOrganFrameCount() { return s_frameCount; }

const int16_t* sampleOrganFrame(int i) {
    return (i >= 0 && i < s_frameCount) ? &s_frames[(size_t)i * WT] : nullptr;
}

bool sampleOrganLoad(const char* wavPath) {
    s_frameCount = 0;
    if (!s_ready) return false;

    File f;
    { SdLock _; f = SD.open(wavPath, FILE_READ); }
    if (!f) return false;

    uint8_t hdr[44];
    { SdLock _; f.read(hdr, 44); }
    uint32_t sampleRate = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) |
                          ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    uint16_t numCh = (uint16_t)hdr[22] | ((uint16_t)hdr[23] << 8);
    uint16_t bits  = (uint16_t)hdr[34] | ((uint16_t)hdr[35] << 8);
    if (numCh != 1 || bits != 16 || sampleRate < 8000 || sampleRate > 48000) {
        Serial.printf("[SAMPORG] '%s' not mono/16-bit (%uch %ubit) — skipped\n",
                      wavPath, (unsigned)numCh, (unsigned)bits);
        { SdLock _; f.close(); }
        return false;
    }

    int   maxSamples = (int)(sampleRate * (float)MAX_SECONDS);
    int16_t* raw = (int16_t*)heap_caps_malloc((size_t)maxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!raw) { { SdLock _; f.close(); } return false; }
    int got;
    { SdLock _; got = (int)(f.read((uint8_t*)raw, (size_t)maxSamples * 2) / 2); f.close(); }

    const int hop = (int)(sampleRate * FRAME_SEC);
    static float frameBuf[WT];
    static float mags[WT];
    static float wtf[WT];
    static float env[WT / 2 + 1];

    int nf = 0;
    for (int start = 0; start + WT <= got && nf < FRAMES_MAX; start += hop, nf++) {
        for (int i = 0; i < WT; i++) frameBuf[i] = raw[start + i] * (1.0f / 32768.0f);
        s_fft.compute(frameBuf, mags, true);          // mags[k] = |FFT| (Hann-windowed)

        // Smooth the magnitude into a pitch-neutral envelope; track the peak so
        // we can skip near-silent harmonics (most of the rebuild cost).
        float peak = 1e-9f;
        for (int k = 1; k <= KMAX; k++) {
            float m = mags[k];
            if (k > 1)         m += mags[k - 1];
            if (k < KMAX)      m += mags[k + 1];
            env[k] = m * (1.0f / 3.0f);
            if (env[k] > peak) peak = env[k];
        }
        const float thr = peak * 0.004f;

        // Rebuild one cycle additively with the shared phase.
        for (int n = 0; n < WT; n++) wtf[n] = 0.0f;
        for (int k = 1; k <= KMAX; k++) {
            float a = env[k];
            if (a < thr) continue;
            uint32_t idx = s_phaseIdx[k];
            for (int n = 0; n < WT; n++) { wtf[n] += a * s_sinf[idx & (WT - 1)]; idx += (uint32_t)k; }
        }

        // Normalise per frame (steady timbre; the note envelope handles loudness).
        float wpk = 1e-9f;
        for (int n = 0; n < WT; n++) { float av = fabsf(wtf[n]); if (av > wpk) wpk = av; }
        float sc = 28000.0f / wpk;
        int16_t* dst = &s_frames[(size_t)nf * WT];
        for (int n = 0; n < WT; n++) dst[n] = (int16_t)lrintf(wtf[n] * sc);
    }

    heap_caps_free(raw);
    s_frameCount = nf;
    // Signature of the first wavetable so different samples are distinguishable
    // in the log (if this is identical across samples, analysis is the problem).
    uint32_t sig = 0;
    if (nf > 0) for (int n = 0; n < WT; n++) sig += (uint32_t)(uint16_t)s_frames[n] * (n + 1);
    Serial.printf("[SAMPORG] '%s' → %d frames (rate %u) sig=%08x\n",
                  wavPath, nf, (unsigned)sampleRate, sig);
    return nf > 0;
}

#include "SamplePlayer.h"
#include "sd_mutex.h"
#include "audio_codec.h"
#include "crash_log.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ES8388 version of SamplePlayer — for CoreS3 + Module Audio.  The codec
// is full-duplex, so we no longer need any I²S0 mutex with mic_spectrum
// — both directions run concurrently on the same I²S peripheral with
// independent DMA channels.
//
// WAVs are assumed mono 16-bit PCM at arbitrary sample rates.  We linear-
// interp resample to the codec's fixed 32 kHz, expand mono → stereo
// (same value on both L+R) and stream to the codec via audioCodecPlay().

static volatile bool             s_stop       = false;
static volatile bool             s_playing    = false;
static SemaphoreHandle_t         s_trigger    = nullptr;
static portMUX_TYPE              s_pathMux    = portMUX_INITIALIZER_UNLOCKED;
static char                      s_pendingPath[64];
static volatile int              s_pendingPitch = 0;   // semitones for the next play
static volatile bool             s_havePending = false;

static const int                 IN_SAMPLES   = 1024;
static const int                 IN_BYTES     = IN_SAMPLES * 2;

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

// Linear-interp resample mono int16 @ inRate → stereo int16 @ codec rate.
// Fixed-point 16.16 phase accumulator; carries `lastIn` across chunks for
// boundary interpolation.
struct Resampler {
    uint32_t step_q16  = 0;
    uint32_t phase_q16 = 0;
    int16_t  lastIn    = 0;

    void configure(uint32_t inRateHz, int pitchSemitones = 0) {
        if (inRateHz == 0) inRateHz = AUDIO_CODEC_RATE_HZ;
        // Base step = file-rate → codec-rate (native pitch).  Pitch tracking
        // scales it by 2^(semitones/12); clamp to ±4 octaves so the step can't
        // overflow or stall.  Runs once per play, so double pow() is cheap.
        if (pitchSemitones < -48) pitchSemitones = -48;
        if (pitchSemitones >  48) pitchSemitones =  48;
        double base  = ((double)inRateHz * 65536.0) / AUDIO_CODEC_RATE_HZ;
        double ratio = exp2((double)pitchSemitones / 12.0);
        uint32_t s   = (uint32_t)(base * ratio + 0.5);
        step_q16  = s ? s : 1;
        phase_q16 = 0;
        lastIn    = 0;
    }

    // `in` mono samples, `out` stereo frames (L,R,L,R…).  Returns frames
    // produced; sets `consumed` to input samples used.
    int process(const int16_t* in, int inSamples,
                int16_t* out, int outCap, int& consumed) {
        int outFrames = 0;
        while (outFrames < outCap) {
            uint32_t idx  = phase_q16 >> 16;
            if ((int)idx >= inSamples) break;
            uint32_t frac = phase_q16 & 0xFFFF;
            int16_t a = (idx == 0) ? lastIn : in[idx - 1];
            int16_t b = in[idx];
            int32_t v = (int32_t)a + (((int32_t)(b - a) * (int32_t)frac) >> 16);
            int16_t s = (int16_t)v;
            out[outFrames * 2 + 0] = s;
            out[outFrames * 2 + 1] = s;
            outFrames++;
            phase_q16 += step_q16;
        }
        consumed = (int)(phase_q16 >> 16);
        if (consumed > inSamples) consumed = inSamples;
        phase_q16 -= ((uint32_t)consumed) << 16;
        if (consumed > 0) lastIn = in[consumed - 1];
        return outFrames;
    }
};

static void wavTaskFn(void*) {
    char activePath[64];
    int  activePitch = 0;
    for (;;) {
        crashSetAudioPhase(CP_IDLE);   // breadcrumb: WAV task idle/blocked
        xSemaphoreTake(s_trigger, portMAX_DELAY);

        bool havePath;
        portENTER_CRITICAL(&s_pathMux);
        havePath = s_havePending;
        if (havePath) {
            memcpy(activePath, s_pendingPath, sizeof(activePath));
            activePitch = s_pendingPitch;
            s_havePending = false;
        }
        portEXIT_CRITICAL(&s_pathMux);
        if (!havePath) continue;

        s_stop    = false;
        s_playing = true;
        crashSetAudioPhase(CP_SAMPLE_READ);   // breadcrumb: audio active

        File f;
        { SdLock _; f = SD.open(activePath); }
        if (!f) {
            s_playing = false;
            continue;
        }

        uint8_t hdr[44];
        { SdLock _; f.read(hdr, 44); }
        uint32_t sampleRate = (uint32_t)hdr[24]
                            | ((uint32_t)hdr[25] << 8)
                            | ((uint32_t)hdr[26] << 16)
                            | ((uint32_t)hdr[27] << 24);
        if (sampleRate < 8000 || sampleRate > 48000) sampleRate = 22050;

        // We only handle canonical mono 16-bit PCM (numChannels @ offset 22,
        // bitsPerSample @ offset 34).  Anything else would be read as noise
        // or wrong-speed, so skip it rather than play garbage.
        uint16_t numCh   = (uint16_t)hdr[22] | ((uint16_t)hdr[23] << 8);
        uint16_t bitsPer = (uint16_t)hdr[34] | ((uint16_t)hdr[35] << 8);
        if (numCh != 1 || bitsPer != 16) {
            Serial.printf("[SP] unsupported WAV (ch=%u bits=%u) — need mono 16-bit\n",
                          (unsigned)numCh, (unsigned)bitsPer);
            { SdLock _; f.close(); }
            s_playing = false;
            continue;
        }

        // Output buffer sized for worst-case upsample (codec / inRate).
        const int OUT_FRAMES_MAX = (IN_SAMPLES * AUDIO_CODEC_RATE_HZ + sampleRate - 1) / sampleRate + 4;
        const int OUT_BYTES_MAX  = OUT_FRAMES_MAX * 4;   // stereo int16

        int16_t* inBuf   = (int16_t*)malloc(IN_BYTES);
        int16_t* nextBuf = (int16_t*)malloc(IN_BYTES);
        int16_t* outBuf  = (int16_t*)malloc(OUT_BYTES_MAX);
        if (!inBuf || !nextBuf || !outBuf) {
            Serial.println("[SP] buffer alloc failed");
            free(inBuf); free(nextBuf); free(outBuf);
            { SdLock _; f.close(); }
            s_playing = false;
            continue;
        }

        Resampler rs;
        rs.configure(sampleRate, activePitch);
        s_fadeInRemaining = FADE_IN_SAMPLES;

        int curBytes;
        { SdLock _; curBytes = f.readBytes(reinterpret_cast<char*>(inBuf), IN_BYTES); }

        while (curBytes > 0 && !s_stop) {
            int nextBytes;
            { SdLock _; nextBytes = f.readBytes(reinterpret_cast<char*>(nextBuf), IN_BYTES); }

            int samples = curBytes / 2;
            applyFadeIn(inBuf, samples);
            if (nextBytes == 0 || s_stop) {
                applyFadeOut(inBuf, samples);
            }

            // Drain the whole input chunk.  With downward pitch one process()
            // call can want to emit more than OUT_FRAMES_MAX frames, so loop
            // until the chunk is consumed instead of dropping the remainder.
            // Phase + lastIn carry correctly across calls via the advanced
            // pointer, so this keeps outBuf small (memory stays flat).
            int done = 0;
            while (done < samples && !s_stop) {
                int consumed = 0;
                int outFrames = rs.process(inBuf + done, samples - done,
                                           outBuf, OUT_FRAMES_MAX, consumed);
                if (outFrames > 0) {
                    audioCodecPlay((const uint8_t*)outBuf, outFrames * 4);
                }
                if (consumed <= 0) break;   // safety: never spin
                done += consumed;
            }

            // Swap buffers
            int16_t* tmp = inBuf; inBuf = nextBuf; nextBuf = tmp;
            curBytes = nextBytes;
        }

        free(inBuf);
        free(nextBuf);
        free(outBuf);
        { SdLock _; f.close(); }

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

void samplePlayerPlay(const char* path, int pitchSemitones) {
    portENTER_CRITICAL(&s_pathMux);
    strncpy(s_pendingPath, path, sizeof(s_pendingPath) - 1);
    s_pendingPath[sizeof(s_pendingPath) - 1] = '\0';
    s_pendingPitch = pitchSemitones;
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

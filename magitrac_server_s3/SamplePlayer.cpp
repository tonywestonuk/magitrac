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
// WAVs are 16-bit PCM, mono or stereo, at arbitrary sample rates.  We linear-
// interp resample to the codec's fixed 32 kHz; mono is duplicated to L+R,
// stereo is resampled per-channel, then streamed via audioCodecPlay().

static volatile bool             s_stop       = false;
static volatile bool             s_playing    = false;
static SemaphoreHandle_t         s_trigger    = nullptr;
static portMUX_TYPE              s_pathMux    = portMUX_INITIALIZER_UNLOCKED;
static char                      s_pendingPath[64];
static volatile float            s_pendingPitch = 0;   // semitones for the next play (fractional OK)
static volatile uint8_t          s_pendingVol   = 127; // 0..127 for the next play
static volatile bool             s_havePending = false;

static const int                 IN_SAMPLES   = 1024;
static const int                 IN_BYTES     = IN_SAMPLES * 2;

static const int          FADE_IN_FRAMES  = 1024;
static const int          FADE_OUT_FRAMES = 1024;
static int                s_fadeInRemaining = 0;   // frames left to fade

// Fades work in frames, not raw samples, so mono and stereo get the same
// fade duration and both channels of a stereo frame get the same gain.
static void applyFadeIn(int16_t* buf, int frames, int ch) {
    if (s_fadeInRemaining <= 0) return;
    int n = frames < s_fadeInRemaining ? frames : s_fadeInRemaining;
    int alreadyFaded = FADE_IN_FRAMES - s_fadeInRemaining;
    for (int i = 0; i < n; i++) {
        int32_t g = alreadyFaded + i;
        for (int c = 0; c < ch; c++) {
            int k = i * ch + c;
            buf[k] = (int16_t)((int32_t)buf[k] * g / FADE_IN_FRAMES);
        }
    }
    s_fadeInRemaining -= n;
}

static void applyFadeOut(int16_t* buf, int frames, int ch) {
    int n = frames < FADE_OUT_FRAMES ? frames : FADE_OUT_FRAMES;
    int start = frames - n;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < ch; c++) {
            int k = (start + i) * ch + c;
            buf[k] = (int16_t)((int32_t)buf[k] * (n - i) / n);
        }
    }
}

// Linear-interp resample int16 @ inRate → stereo int16 @ codec rate.
// Handles mono (1 ch, duplicated to L+R) and stereo (2 ch, L/R resampled
// independently with a shared phase).  Fixed-point 16.16 phase accumulator;
// carries the boundary sample per channel (`lastL`/`lastR`) across chunks.
struct Resampler {
    uint32_t step_q16  = 0;
    uint32_t phase_q16 = 0;
    int      channels  = 1;
    int16_t  lastL     = 0;
    int16_t  lastR     = 0;

    void configure(uint32_t inRateHz, int numChannels, float pitchSemitones = 0) {
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
        channels  = (numChannels == 2) ? 2 : 1;
        lastL     = 0;
        lastR     = 0;
    }

    // `in` interleaved input frames (1 or 2 samples each), `out` stereo frames
    // (L,R,L,R…).  Returns output frames produced; sets `consumed` to input
    // frames used.
    int process(const int16_t* in, int inFrames,
                int16_t* out, int outCap, int& consumed) {
        int outFrames = 0;
        const int ch = channels;
        while (outFrames < outCap) {
            uint32_t idx  = phase_q16 >> 16;
            if ((int)idx >= inFrames) break;
            uint32_t frac = phase_q16 & 0xFFFF;
            int16_t aL = (idx == 0) ? lastL : in[(idx - 1) * ch + 0];
            int16_t bL = in[idx * ch + 0];
            int32_t vL = (int32_t)aL + (((int32_t)(bL - aL) * (int32_t)frac) >> 16);
            if (ch == 2) {
                int16_t aR = (idx == 0) ? lastR : in[(idx - 1) * ch + 1];
                int16_t bR = in[idx * ch + 1];
                int32_t vR = (int32_t)aR + (((int32_t)(bR - aR) * (int32_t)frac) >> 16);
                out[outFrames * 2 + 0] = (int16_t)vL;
                out[outFrames * 2 + 1] = (int16_t)vR;
            } else {
                out[outFrames * 2 + 0] = (int16_t)vL;
                out[outFrames * 2 + 1] = (int16_t)vL;
            }
            outFrames++;
            phase_q16 += step_q16;
        }
        consumed = (int)(phase_q16 >> 16);
        if (consumed > inFrames) consumed = inFrames;
        phase_q16 -= ((uint32_t)consumed) << 16;
        if (consumed > 0) {
            lastL = in[(consumed - 1) * ch + 0];
            lastR = in[(consumed - 1) * ch + (ch == 2 ? 1 : 0)];
        }
        return outFrames;
    }
};

static void wavTaskFn(void*) {
    char  activePath[64];
    float activePitch = 0;
    int32_t activeGainQ15 = 32767;
    for (;;) {
        crashSetAudioPhase(CP_IDLE);   // breadcrumb: WAV task idle/blocked
        xSemaphoreTake(s_trigger, portMAX_DELAY);

        bool havePath;
        portENTER_CRITICAL(&s_pathMux);
        havePath = s_havePending;
        if (havePath) {
            memcpy(activePath, s_pendingPath, sizeof(activePath));
            activePitch = s_pendingPitch;
            // Squared taper (GM CC7-ish): perceived loudness tracks the value
            // better than linear, and 127 stays exactly unity.
            int v = s_pendingVol;
            activeGainQ15 = (int32_t)(v * v * 32767L) / (127 * 127);
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
        size_t hdrRead;
        { SdLock _; hdrRead = f.read(hdr, sizeof(hdr)); }
        if (hdrRead != sizeof(hdr)) {
            Serial.println("[SP] WAV too short — no header");
            { SdLock _; f.close(); }
            s_playing = false;
            continue;
        }
        uint32_t sampleRate = (uint32_t)hdr[24]
                            | ((uint32_t)hdr[25] << 8)
                            | ((uint32_t)hdr[26] << 16)
                            | ((uint32_t)hdr[27] << 24);
        if (sampleRate < 8000 || sampleRate > 48000) sampleRate = 22050;

        // We handle canonical 16-bit PCM, mono or stereo (numChannels @ offset
        // 22, bitsPerSample @ offset 34).  Mono is duplicated to L+R; stereo is
        // resampled per-channel.  Anything else (8/24-bit, >2 ch) would be read
        // as noise or wrong-speed, so skip it rather than play garbage.
        uint16_t numCh   = (uint16_t)hdr[22] | ((uint16_t)hdr[23] << 8);
        uint16_t bitsPer = (uint16_t)hdr[34] | ((uint16_t)hdr[35] << 8);
        if ((numCh != 1 && numCh != 2) || bitsPer != 16) {
            Serial.printf("[SP] unsupported WAV (ch=%u bits=%u) — need 16-bit mono or stereo\n",
                          (unsigned)numCh, (unsigned)bitsPer);
            { SdLock _; f.close(); }
            s_playing = false;
            continue;
        }

        // Output buffer sized for worst-case upsample (codec / inRate) of one
        // input chunk — a chunk holds IN_SAMPLES raw int16s, i.e. fewer frames
        // when stereo.
        const int IN_FRAMES     = IN_SAMPLES / numCh;
        const int OUT_FRAMES_MAX = (IN_FRAMES * AUDIO_CODEC_RATE_HZ + sampleRate - 1) / sampleRate + 4;
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
        rs.configure(sampleRate, numCh, activePitch);
        s_fadeInRemaining = FADE_IN_FRAMES;

        int curBytes;
        { SdLock _; curBytes = f.readBytes(reinterpret_cast<char*>(inBuf), IN_BYTES); }

        while (curBytes > 0 && !s_stop) {
            int nextBytes;
            { SdLock _; nextBytes = f.readBytes(reinterpret_cast<char*>(nextBuf), IN_BYTES); }

            int frames = (curBytes / 2) / numCh; // resampler input frames
            applyFadeIn(inBuf, frames, numCh);
            if (nextBytes == 0 || s_stop) {
                applyFadeOut(inBuf, frames, numCh);
            }

            // Drain the whole input chunk.  With downward pitch one process()
            // call can want to emit more than OUT_FRAMES_MAX frames, so loop
            // until the chunk is consumed instead of dropping the remainder.
            // Phase + boundary samples carry correctly across calls via the
            // advanced pointer, so this keeps outBuf small (memory stays flat).
            int done = 0;                        // input frames consumed
            while (done < frames && !s_stop) {
                int consumed = 0;
                int outFrames = rs.process(inBuf + done * numCh, frames - done,
                                           outBuf, OUT_FRAMES_MAX, consumed);
                if (outFrames > 0) {
                    if (activeGainQ15 < 32767) {
                        for (int k = 0; k < outFrames * 2; k++)
                            outBuf[k] = (int16_t)(((int32_t)outBuf[k] * activeGainQ15) >> 15);
                    }
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

void samplePlayerPlay(const char* path, float pitchSemitones, uint8_t volume) {
    portENTER_CRITICAL(&s_pathMux);
    strncpy(s_pendingPath, path, sizeof(s_pendingPath) - 1);
    s_pendingPath[sizeof(s_pendingPath) - 1] = '\0';
    s_pendingPitch = pitchSemitones;
    s_pendingVol   = volume;
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

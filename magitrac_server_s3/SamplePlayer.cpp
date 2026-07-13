#include "SamplePlayer.h"
#include "SampleManifest.h"
#include "sd_mutex.h"
#include "audio_codec.h"
#include "crash_log.h"
#include "TrackerData.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

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
static volatile uint32_t         s_pendingStart = 0;   // trim, frames
static volatile uint32_t         s_pendingEnd   = 0;   // 0 = end of file
static volatile bool             s_pendingLoop  = false;
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

// ── PSRAM sample cache ────────────────────────────────────────────────────────
// Whole decoded WAVs (16-bit PCM, mono/stereo) held in PSRAM, keyed by the
// manifest id the columns reference.  Loaded/evicted by samplePreloadSync on
// the loop task; read by the RAM voices on the WAV task.  s_cacheLocked makes
// samplePlayRam refuse (→ stream fallback) while the sync mutates the cache,
// so a voice can never start on a buffer that's about to be freed.
#define RAM_CACHE_MAX       16
#define PRELOAD_MAX_BYTES   (200 * 1024)
#define PRELOAD_TOTAL_CAP   (4 * 1024 * 1024)

struct RamSample {
    uint8_t   id;       // manifest id, 0 = free slot
    int16_t*  data;     // PSRAM, interleaved frames (the TRIMMED region only)
    uint32_t  frames;
    uint32_t  rate;
    uint8_t   ch;       // 1 or 2
    uint8_t   loop;     // from trim metadata: held voices wrap at the end
    uint32_t  bytes;
    uint32_t  trimStart, trimEnd;   // what's baked in — re-preload if it moves
    uint8_t   trimLoop;
};
static RamSample     s_ram[RAM_CACHE_MAX] = {};
static size_t        s_ramBytes    = 0;
static volatile bool s_cacheLocked = false;

// ── Polyphonic RAM voices ─────────────────────────────────────────────────────
// Fixed-point steppers over the cached buffers, mixed by the WAV task — into
// the SD stream's chunks when one is playing, or into standalone blocks when
// not.  Short fade-in kills trigger clicks; stop = short fade-out.
#define RAM_VOICES      4
#define RAM_FADE_FRAMES 256

struct RamVoice {
    volatile bool active;
    volatile bool releasing;
    const int16_t* data;
    uint32_t frames;
    uint8_t  ch;
    uint8_t  loop;      // wrap at the end until released
    uint32_t pos;       // integer frame index
    uint32_t frac;      // Q16 fractional position
    uint32_t stepQ16;
    int32_t  gainQ15;
    uint32_t age;       // frames rendered (drives the fade-in)
    int32_t  fadeRem;   // release fade countdown
    uint32_t seq;       // allocation order (steal the oldest)
};
static RamVoice     s_voice[RAM_VOICES] = {};
static uint32_t     s_voiceSeq = 0;
static portMUX_TYPE s_voiceMux = portMUX_INITIALIZER_UNLOCKED;

bool sampleRamVoicesActive() {
    for (int i = 0; i < RAM_VOICES; i++)
        if (s_voice[i].active) return true;
    return false;
}

// Sum every active RAM voice into `out` (stereo int16 frames, already holding
// the SD stream or silence).  Per-voice linear interp + gain + fades; int32
// accumulate with clamp.  Runs on the WAV task only.
static void mixRamInto(int16_t* out, int frames) {
    for (int vi = 0; vi < RAM_VOICES; vi++) {
        RamVoice& v = s_voice[vi];
        if (!v.active) continue;
        const int16_t* d  = v.data;
        const int      ch = v.ch;
        for (int i = 0; i < frames; i++) {
            if (v.pos + 1 >= v.frames) {
                if (!v.loop) { v.active = false; break; }
                v.pos  = 0;                 // held key: wrap and keep singing
                v.frac = 0;
            }
            int32_t g = v.gainQ15;
            if (v.age < RAM_FADE_FRAMES)                  // fade-in
                g = (int32_t)((int64_t)g * v.age / RAM_FADE_FRAMES);
            if (v.releasing) {                            // fade-out then die
                if (v.fadeRem <= 0) { v.active = false; break; }
                g = (int32_t)((int64_t)g * v.fadeRem / RAM_FADE_FRAMES);
                v.fadeRem--;
            }
            const int16_t* fr0 = d + v.pos * ch;
            int32_t aL = fr0[0], bL = fr0[ch];
            int32_t sL = aL + (((bL - aL) * (int32_t)v.frac) >> 16);
            int32_t sR = sL;
            if (ch == 2) {
                int32_t aR = fr0[1], bR = fr0[3];
                sR = aR + (((bR - aR) * (int32_t)v.frac) >> 16);
            }
            int32_t oL = out[i * 2 + 0] + ((sL * g) >> 15);
            int32_t oR = out[i * 2 + 1] + ((sR * g) >> 15);
            if (oL >  32767) oL =  32767;
            if (oL < -32768) oL = -32768;
            if (oR >  32767) oR =  32767;
            if (oR < -32768) oR = -32768;
            out[i * 2 + 0] = (int16_t)oL;
            out[i * 2 + 1] = (int16_t)oR;
            v.frac += v.stepQ16;
            v.pos  += v.frac >> 16;
            v.frac &= 0xFFFF;
            v.age++;
        }
    }
}

int samplePlayRam(uint8_t sampleId, float pitchSemitones, uint8_t volume) {
    if (s_cacheLocked || sampleId == 0) return -1;
    const RamSample* rs = nullptr;
    for (int i = 0; i < RAM_CACHE_MAX; i++)
        if (s_ram[i].id == sampleId) { rs = &s_ram[i]; break; }
    if (!rs) return -1;

    if (pitchSemitones < -48) pitchSemitones = -48;
    if (pitchSemitones >  48) pitchSemitones =  48;
    double step = ((double)rs->rate * 65536.0 / AUDIO_CODEC_RATE_HZ)
                * exp2((double)pitchSemitones / 12.0);
    uint32_t stepQ16 = (uint32_t)(step + 0.5);
    if (stepQ16 == 0) stepQ16 = 1;
    int32_t gain = (int32_t)(volume * volume * 32767L) / (127 * 127);

    // Free voice, else steal the oldest.
    portENTER_CRITICAL(&s_voiceMux);
    int pick = -1;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < RAM_VOICES; i++) {
        if (!s_voice[i].active) { pick = i; break; }
        if (s_voice[i].seq < oldest) { oldest = s_voice[i].seq; pick = i; }
    }
    RamVoice& v = s_voice[pick];
    v.active    = false;          // renderer skips it while we rewrite fields
    v.releasing = false;
    v.data      = rs->data;
    v.frames    = rs->frames;
    v.ch        = rs->ch;
    v.loop      = rs->loop;
    v.pos       = 0;
    v.frac      = 0;
    v.stepQ16   = stepQ16;
    v.gainQ15   = gain;
    v.age       = 0;
    v.fadeRem   = RAM_FADE_FRAMES;
    v.seq       = ++s_voiceSeq;
    v.active    = true;
    portEXIT_CRITICAL(&s_voiceMux);

    xSemaphoreGive(s_trigger);    // wake the WAV task if it's idle
    // Handle = generation | slot: a stale handle (voice ended, slot reused)
    // can never stop the slot's NEW occupant.
    return (int)((v.seq << 3) | (uint32_t)pick);
}

void sampleStopVoice(int voice) {
    if (voice < 0) return;
    int      slot = voice & 7;
    uint32_t gen  = (uint32_t)voice >> 3;
    if (slot >= RAM_VOICES) return;
    if (s_voice[slot].seq == gen) s_voice[slot].releasing = true;
}

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
        uint32_t activeStart = 0, activeEnd = 0;
        bool     activeLoop  = false;
        if (havePath) {
            memcpy(activePath, s_pendingPath, sizeof(activePath));
            activePitch = s_pendingPitch;
            // Squared taper (GM CC7-ish): perceived loudness tracks the value
            // better than linear, and 127 stays exactly unity.
            int v = s_pendingVol;
            activeGainQ15 = (int32_t)(v * v * 32767L) / (127 * 127);
            activeStart = s_pendingStart;
            activeEnd   = s_pendingEnd;
            activeLoop  = s_pendingLoop;
            s_havePending = false;
        }
        portEXIT_CRITICAL(&s_pathMux);
        if (!havePath) {
            // Woken for RAM voices: render standalone blocks until they all
            // finish (or a stream request arrives and takes over the loop).
            if (sampleRamVoicesActive()) {
                s_playing = true;
                crashSetAudioPhase(CP_SAMPLE_READ);
                static int16_t mixBuf[256 * 2];
                while (sampleRamVoicesActive() && !s_havePending) {
                    memset(mixBuf, 0, sizeof(mixBuf));
                    mixRamInto(mixBuf, 256);
                    if (!audioCodecPlay((const uint8_t*)mixBuf, sizeof(mixBuf)))
                        vTaskDelay(1);          // codec reconfiguring — don't spin
                }
                s_playing = false;
            }
            continue;
        }

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

        // PSRAM: sequential, double-buffered — internal heap stays free for
        // the radios exactly when playback + WiFi are busiest (live set).
        int16_t* inBuf   = (int16_t*)heap_caps_malloc(IN_BYTES, MALLOC_CAP_SPIRAM);
        int16_t* nextBuf = (int16_t*)heap_caps_malloc(IN_BYTES, MALLOC_CAP_SPIRAM);
        int16_t* outBuf  = (int16_t*)heap_caps_malloc(OUT_BYTES_MAX, MALLOC_CAP_SPIRAM);
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

        // Trim: start at startFrame, read no further than endFrame; loop =
        // seek back to the region start and keep refilling until stopped.
        const uint32_t frameBytes = (uint32_t)numCh * 2;
        const uint32_t dataStart  = sizeof(hdr) + activeStart * frameBytes;
        const uint32_t loopBytes  = (activeEnd > activeStart)
                                  ? (activeEnd - activeStart) * frameBytes
                                  : 0xFFFFFFFF;             // 0 endFrame = to EOF
        uint32_t budget = loopBytes;
        if (activeStart) { SdLock _; f.seek(dataStart); }

        auto readChunk = [&](int16_t* dst) -> int {
            SdLock _;
            bool wrapped = false;
            for (;;) {
                uint32_t wantB = IN_BYTES;
                if (budget < wantB) wantB = budget;
                int got = 0;
                if (wantB > 0)
                    got = (int)f.readBytes(reinterpret_cast<char*>(dst), wantB);
                if (got > 0) {
                    if (budget != 0xFFFFFFFF) budget -= (uint32_t)got;
                    return got;
                }
                if (!activeLoop || s_stop || wrapped) return 0;
                f.seek(dataStart);                    // wrap the loop region
                budget  = loopBytes;
                wrapped = true;   // a barren read after a wrap ends the voice
            }
        };

        int curBytes = readChunk(inBuf);

        while (curBytes > 0 && !s_stop) {
            int nextBytes = readChunk(nextBuf);

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
                    mixRamInto(outBuf, outFrames);   // RAM polyphony rides the stream
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
        // Stream ended but RAM voices may still be ringing — self-wake so the
        // idle branch keeps rendering them instead of blocking on the trigger.
        if (more || sampleRamVoicesActive()) xSemaphoreGive(s_trigger);
    }
}

void samplePlayerInit() {
    s_trigger = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(wavTaskFn, "WAV", 4096, nullptr, 5, nullptr, 0);  // Core 0
}

void samplePlayerPlay(const char* path, float pitchSemitones, uint8_t volume,
                      uint32_t startFrame, uint32_t endFrame, bool loop) {
    portENTER_CRITICAL(&s_pathMux);
    strncpy(s_pendingPath, path, sizeof(s_pendingPath) - 1);
    s_pendingPath[sizeof(s_pendingPath) - 1] = '\0';
    s_pendingPitch = pitchSemitones;
    s_pendingVol   = volume;
    s_pendingStart = startFrame;
    s_pendingEnd   = endFrame;
    s_pendingLoop  = loop;
    s_havePending = true;
    portEXIT_CRITICAL(&s_pathMux);
    s_stop = true;
    xSemaphoreGive(s_trigger);
}

void samplePlayerStop() {
    s_stop = true;                                        // stream
    for (int i = 0; i < RAM_VOICES; i++)                  // + every RAM voice
        if (s_voice[i].active) s_voice[i].releasing = true;
}

void samplePlayerStopStream() {
    s_stop = true;
}

bool samplePlayerIsPlaying() {
    return s_playing;
}

// ── Preload sync (loop task only — does SD reads) ─────────────────────────────
bool samplePreloadSync(const Song* song) {
    // Never mutate buffers a voice might be reading, and don't fight a
    // feeding stream for the SD bus.  Caller retries next tick.
    if (s_playing || sampleRamVoicesActive()) return false;

    // Wanted set: every SFX column's sample id, deduped.
    uint8_t want[MAX_COLUMNS];
    int nwant = 0;
    if (song) {
        for (int c = 1; c < MAX_COLUMNS; c++) {
            const ColumnSettings& cs = song->columns[c];
            if (cs.midiChannel != SFX_CHANNEL || cs.program == 0) continue;
            bool dup = false;
            for (int i = 0; i < nwant; i++) if (want[i] == cs.program) dup = true;
            if (!dup) want[nwant++] = cs.program;
        }
    }

    s_cacheLocked = true;   // samplePlayRam streams instead while we mutate

    // Evict entries the song no longer references — or whose trim metadata
    // moved since they were baked (the reload below picks up the new region).
    for (int i = 0; i < RAM_CACHE_MAX; i++) {
        if (s_ram[i].id == 0) continue;
        bool keep = false;
        for (int w = 0; w < nwant; w++) if (want[w] == s_ram[i].id) keep = true;
        if (keep) {
            const SampleTrim* tr = sampleTrimFor(s_ram[i].id);
            uint32_t stF = tr ? tr->startFrame : 0;
            uint32_t enF = tr ? tr->endFrame   : 0;
            uint8_t  lp  = tr ? tr->loop       : 0;
            if (s_ram[i].trimStart != stF || s_ram[i].trimEnd != enF ||
                s_ram[i].trimLoop != lp) keep = false;
        }
        if (!keep) {
            free(s_ram[i].data);
            s_ramBytes  -= s_ram[i].bytes;
            s_ram[i]     = RamSample{};
        }
    }

    // Load what's wanted and missing — size-gated, PSRAM only.
    for (int w = 0; w < nwant; w++) {
        bool have = false;
        for (int i = 0; i < RAM_CACHE_MAX; i++)
            if (s_ram[i].id == want[w]) { have = true; break; }
        if (have) continue;
        int slot = -1;
        for (int i = 0; i < RAM_CACHE_MAX; i++)
            if (s_ram[i].id == 0) { slot = i; break; }
        if (slot < 0) break;

        const char* fname = sampleManifestNameFor(want[w]);
        if (!fname) continue;
        char path[64];
        snprintf(path, sizeof(path), "/samples/%s", fname);

        SdLock _;
        File f = SD.open(path);
        if (!f) continue;
        uint32_t fsize = (uint32_t)f.size();
        uint8_t hdr[44];
        if (fsize >= PRELOAD_MAX_BYTES || fsize <= sizeof(hdr) + 8 ||
            f.read(hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); continue; }
        // Same canonical-WAV assumptions as the streamer.
        uint32_t rate = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8)
                      | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
        uint16_t numCh   = (uint16_t)hdr[22] | ((uint16_t)hdr[23] << 8);
        uint16_t bitsPer = (uint16_t)hdr[34] | ((uint16_t)hdr[35] << 8);
        if ((numCh != 1 && numCh != 2) || bitsPer != 16 ||
            rate < 8000 || rate > 48000) { f.close(); continue; }

        uint32_t frameBytes  = (uint32_t)numCh * 2;
        uint32_t totalFrames = (fsize - sizeof(hdr)) / frameBytes;

        // Non-destructive trim: cache only the region playback will use —
        // even a huge file becomes preloadable if its trimmed slice is small.
        const SampleTrim* tr = sampleTrimFor(want[w]);
        uint32_t stF = tr ? tr->startFrame : 0;
        uint32_t enF = (tr && tr->endFrame) ? tr->endFrame : totalFrames;
        if (stF >= totalFrames) stF = 0;
        if (enF > totalFrames || enF <= stF) enF = totalFrames;
        uint32_t dataBytes = (enF - stF) * frameBytes;
        if (dataBytes < frameBytes * 4 || dataBytes >= PRELOAD_MAX_BYTES ||
            s_ramBytes + dataBytes > PRELOAD_TOTAL_CAP) { f.close(); continue; }
        if (stF && !f.seek(sizeof(hdr) + stF * frameBytes)) { f.close(); continue; }

        int16_t* buf = (int16_t*)heap_caps_malloc(dataBytes, MALLOC_CAP_SPIRAM);
        if (!buf) { f.close(); continue; }   // no/full PSRAM → streaming fallback
        if (f.read((uint8_t*)buf, dataBytes) != dataBytes) {
            free(buf); f.close(); continue;
        }
        f.close();

        s_ram[slot].id        = want[w];
        s_ram[slot].data      = buf;
        s_ram[slot].frames    = dataBytes / frameBytes;
        s_ram[slot].rate      = rate;
        s_ram[slot].ch        = (uint8_t)numCh;
        s_ram[slot].loop      = tr ? tr->loop : 0;
        s_ram[slot].bytes     = dataBytes;
        s_ram[slot].trimStart = stF;
        s_ram[slot].trimEnd   = tr ? tr->endFrame : 0;
        s_ram[slot].trimLoop  = tr ? tr->loop : 0;
        s_ramBytes           += dataBytes;
        Serial.printf("[SP] preloaded id=%u %s (%u KB @ %u Hz ch=%u)\n",
                      want[w], fname, (unsigned)(dataBytes / 1024),
                      (unsigned)rate, (unsigned)numCh);
    }

    s_cacheLocked = false;
    return true;
}

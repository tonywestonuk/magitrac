#include "SamplePlayer.h"
#include "SampleManifest.h"
#include "sd_mutex.h"
#include "audio_codec.h"
#include "crash_log.h"
#include "TrackerData.h"
#include "SampleDebug.h"   // [SMPDBG] timing lines (off unless SAMPLE_DEBUG is 1)
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
// Decoded WAVs (16-bit PCM, mono/stereo) held in PSRAM, keyed by manifest id,
// so they can play as RAM voices — polyphonic, and with no SD card involved at
// trigger time.  Filled by samplePreloadSync on the loop task, a piece per
// call; read by samplePlayRam (MIDI task) and the voices (WAV task).
//
// What gets a place, in this order: the loaded song's SFX samples, the sample
// open in the client's editor, then — while the client's samples page is open
// — every sample on the card, shortest first.  A sample longer than
// PRELOAD_MAX_BYTES, or one that would take the cache past PRELOAD_TOTAL_CAP,
// is left out and streams from SD instead (one at a time).  Entries nobody
// wants any more are NOT thrown out until their room is needed, so going back
// to a page or a song finds its samples still loaded.
//
// Two rules keep the three tasks out of each other's way:
//   • an entry appears (and a voice starts on it) under s_voiceMux, so a
//     lookup sees either nothing or a finished entry;
//   • an entry is only removed while no RAM voice is sounding — checked under
//     that same lock, so a voice can't start on it in between — which means
//     nothing can be reading the buffer that is about to be freed.
#define RAM_CACHE_MAX       64
#define PRELOAD_MAX_BYTES   (2 * 1024 * 1024)
#define PRELOAD_TOTAL_CAP   (6 * 1024 * 1024)
#define PRELOAD_STEP_BYTES  (32 * 1024)        // SD read per samplePreloadSync call (~20 ms)
#define PRELOAD_STEP_GENTLE (8 * 1024)         // …while a song is playing (~5 ms on the card)

struct RamSample {
    uint8_t   id;       // manifest id, 0 = free slot
    int16_t*  data;     // PSRAM, interleaved frames (the TRIMMED region only)
    uint32_t  frames;
    uint32_t  rate;
    uint8_t   ch;       // 1 or 2
    uint8_t   loop;     // from trim metadata: held voices wrap at the end
    uint32_t  bytes;
    uint32_t  trimStart, trimEnd;   // the trim metadata AS STORED (unclamped) when
    uint8_t   trimLoop;             //   this was baked — re-preload if it moves
};
static RamSample*    s_ram       = nullptr;   // RAM_CACHE_MAX entries, PSRAM (samplePlayerInit)
static size_t        s_ramBytes  = 0;
static volatile bool s_streaming = false;     // the WAV task has an SD stream running

// ── Polyphonic RAM voices ─────────────────────────────────────────────────────
// Fixed-point steppers over the cached buffers, mixed by the WAV task — into
// the SD stream's chunks when one is playing, or into standalone blocks when
// not.  Short fade-in kills trigger clicks; stop = short fade-out.
#define RAM_VOICES      8      // (voice handles keep the slot in 3 bits — 8 is the most)
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
    if (!s_ram || sampleId == 0) return -1;

    if (pitchSemitones < -48) pitchSemitones = -48;
    if (pitchSemitones >  48) pitchSemitones =  48;
    double  ratio = exp2((double)pitchSemitones / 12.0) * 65536.0 / AUDIO_CODEC_RATE_HZ;
    int32_t gain  = (int32_t)(volume * volume * 32767L) / (127 * 127);

    // Lookup and voice start in one critical section: the entry can't be half
    // written, and it can't be removed in between (removal needs an idle WAV
    // task, and from here on this voice makes it busy).
    int handle = -1;
    portENTER_CRITICAL(&s_voiceMux);
    const RamSample* rs = nullptr;
    for (int i = 0; i < RAM_CACHE_MAX; i++)
        if (s_ram[i].id == sampleId) { rs = &s_ram[i]; break; }
    if (rs) {
        uint32_t stepQ16 = (uint32_t)((double)rs->rate * ratio + 0.5);
        if (stepQ16 == 0) stepQ16 = 1;
        // Free voice, else steal the oldest.
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
        // Handle = generation | slot: a stale handle (voice ended, slot reused)
        // can never stop the slot's NEW occupant.
        handle = (int)((v.seq << 3) | (uint32_t)pick);
    }
    portEXIT_CRITICAL(&s_voiceMux);

    if (handle >= 0) xSemaphoreGive(s_trigger);    // wake the WAV task if it's idle
    return handle;
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

        uint32_t dbgT0 = millis();          // [SMPDBG] stream start-up timing
        bool     dbgFirst = true;
        s_streaming = true;              // the preloader keeps off the card meanwhile
        File f;
        { SdLock _; f = SD.open(activePath); }
        if (!f) {
            SMPDBG("stream %s: open FAILED", activePath);
            s_streaming = false;
            s_playing = false;
            continue;
        }
        uint32_t dbgOpen = millis();

        uint8_t hdr[44];
        size_t hdrRead;
        { SdLock _; hdrRead = f.read(hdr, sizeof(hdr)); }
        if (hdrRead != sizeof(hdr)) {
            Serial.println("[SP] WAV too short — no header");
            { SdLock _; f.close(); }
            s_streaming = false;
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
            s_streaming = false;
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
            s_streaming = false;
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
                    if (activeGainQ15 != 32767) {          // (above unity it can clip — on purpose)
                        for (int k = 0; k < outFrames * 2; k++) {
                            int32_t s = ((int32_t)outBuf[k] * activeGainQ15) >> 15;
                            if (s >  32767) s =  32767;
                            if (s < -32768) s = -32768;
                            outBuf[k] = (int16_t)s;
                        }
                    }
                    mixRamInto(outBuf, outFrames);   // RAM polyphony rides the stream
                    if (dbgFirst) {
                        dbgFirst = false;
                        SMPDBG("stream %s: open %lu ms, first audio %lu ms after the trigger "
                               "(start frame %lu)", activePath,
                               (unsigned long)(dbgOpen - dbgT0),
                               (unsigned long)(millis() - dbgT0),
                               (unsigned long)activeStart);
                    }
                    if (!audioCodecPlay((const uint8_t*)outBuf, outFrames * 4))
                        vTaskDelay(1);          // no codec / reconfiguring — don't spin
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
        SMPDBG("stream %s: ended %lu ms after the trigger (%s)", activePath,
               (unsigned long)(millis() - dbgT0), s_stop ? "stopped" : "ran out");

        s_streaming = false;
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
    // The cache table itself lives in PSRAM too (2 KB of control-rate data).
    s_ram = (RamSample*)heap_caps_calloc(RAM_CACHE_MAX, sizeof(RamSample), MALLOC_CAP_SPIRAM);
    if (!s_ram) s_ram = (RamSample*)calloc(RAM_CACHE_MAX, sizeof(RamSample));
    if (!s_ram) Serial.println("[SP] no memory for the sample cache table — everything streams");
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
static volatile uint8_t s_extraId  = 0;       // see samplePreloadSetExtra
static volatile bool    s_wantPads = false;   // see samplePreloadSetPads

void samplePreloadSetExtra(uint8_t sampleId) { s_extraId = sampleId; }
void samplePreloadSetPads(bool on)           { s_wantPads = on; }

// One load in progress, fed a piece per samplePreloadSync call.
static struct PreloadJob {
    bool     active;
    File     f;
    uint8_t  id;
    int16_t* buf;          // PSRAM, not yet in the cache table
    uint32_t bytes, done;
    uint32_t rate;
    uint8_t  ch;
    uint32_t trimStart, trimEnd;   // raw metadata this is being baked for
    uint8_t  trimLoop;
} s_job;

// Ids that could not be loaded this round (unreadable, unsupported, no room),
// so a round ends instead of trying them on every call.  Cleared when a round
// completes: the next change of plans tries them afresh.
static uint8_t s_failed[16];
static bool failed(uint8_t id)     { return s_failed[id >> 3] & (1u << (id & 7)); }
static void markFailed(uint8_t id) { s_failed[id >> 3] |= (uint8_t)(1u << (id & 7)); }

static int cacheFind(uint8_t id) {
    for (int i = 0; i < RAM_CACHE_MAX; i++) if (s_ram[i].id == id) return i;
    return -1;
}

static void rawTrim(uint8_t id, uint32_t& st, uint32_t& en, uint8_t& lp) {
    const SampleTrim* tr = sampleTrimFor(id);
    st = tr ? tr->startFrame : 0;
    en = tr ? tr->endFrame   : 0;
    lp = tr ? tr->loop       : 0;
}

// Remove entry i — unless a RAM voice is sounding (see the rules above), in
// which case nothing happens and it returns false: try again later.
static bool cacheEvict(int i) {
    int16_t* d = nullptr;
    bool busy  = false;
    portENTER_CRITICAL(&s_voiceMux);
    for (int v = 0; v < RAM_VOICES; v++) if (s_voice[v].active) { busy = true; break; }
    if (!busy) {
        d           = s_ram[i].data;
        s_ramBytes -= s_ram[i].bytes;
        s_ram[i]    = RamSample{};
    }
    portEXIT_CRITICAL(&s_voiceMux);
    if (busy) return false;
    free(d);
    return true;
}

// The samples wanted in the cache, most important first.  Returns the count.
#define PRELOAD_WANT_MAX (MAX_COLUMNS + 1 + SM_MAX_ENTRIES)
static int buildWanted(const Song* song, uint8_t* want) {
    uint8_t seen[16] = {};
    int n = 0;
    auto add = [&](uint8_t id) {
        if (id == 0 || id > 127 || (seen[id >> 3] & (1u << (id & 7)))) return;
        if (n >= PRELOAD_WANT_MAX) return;
        seen[id >> 3] |= (uint8_t)(1u << (id & 7));
        want[n++] = id;
    };
    if (song)                                      // 1. the song's SFX columns
        for (int c = 1; c < MAX_COLUMNS; c++)
            if (song->columns[c].midiChannel == SFX_CHANNEL) add(song->columns[c].program);
    add(s_extraId);                                // 2. the sample in the editor
    if (s_wantPads) {                              // 3. the samples page: all, shortest first
        int total = sampleManifestCount();
        for (;;) {
            const SmEntry* best = nullptr;
            for (int i = 0; i < total; i++) {
                const SmEntry* e = sampleManifestAt(i);
                if (!e || (seen[e->id >> 3] & (1u << (e->id & 7)))) continue;
                if (!best || e->size < best->size) best = e;
            }
            if (!best) break;
            add(best->id);
        }
    }
    return n;
}

static void jobAbort(bool fail) {
    { SdLock _; s_job.f.close(); }
    free(s_job.buf);
    if (fail) markFailed(s_job.id);
    s_job.active = false;
}

// Read the next piece of the load in progress; when it is all in, add it to
// the cache.
static void preloadStep(uint32_t stepBytes) {
    PreloadJob& j = s_job;
    uint32_t want = j.bytes - j.done;
    if (want > stepBytes) want = stepBytes;
    size_t got;
    { SdLock _; got = j.f.read((uint8_t*)j.buf + j.done, want); }
    if (got != want) { jobAbort(true); return; }
    j.done += want;
    if (j.done < j.bytes) return;

    { SdLock _; j.f.close(); }
    j.active = false;

    // Edited while it loaded?  Drop it; the next pass bakes the new region.
    uint32_t st, en; uint8_t lp;
    rawTrim(j.id, st, en, lp);
    int slot = -1;
    for (int i = 0; i < RAM_CACHE_MAX; i++) if (s_ram[i].id == 0) { slot = i; break; }
    if (st != j.trimStart || en != j.trimEnd || lp != j.trimLoop || slot < 0) {
        free(j.buf);
        return;
    }

    RamSample rs = {};
    rs.id        = j.id;
    rs.data      = j.buf;
    rs.frames    = j.bytes / ((uint32_t)j.ch * 2);
    rs.rate      = j.rate;
    rs.ch        = j.ch;
    rs.loop      = j.trimLoop;
    rs.bytes     = j.bytes;
    // Raw metadata, not the clamped region: the "has the trim moved?" test
    // compares against the raw table, and a clamped copy would never match an
    // out-of-range trim (evict + reload for ever).
    rs.trimStart = j.trimStart;
    rs.trimEnd   = j.trimEnd;
    rs.trimLoop  = j.trimLoop;
    portENTER_CRITICAL(&s_voiceMux);
    s_ram[slot]  = rs;                 // appears complete, or not at all
    s_ramBytes  += rs.bytes;
    portEXIT_CRITICAL(&s_voiceMux);
    const char* fname = sampleManifestNameFor(j.id);
    Serial.printf("[SP] preloaded id=%u %s (%u KB @ %u Hz ch=%u; cache %u KB)\n",
                  (unsigned)j.id, fname ? fname : "?", (unsigned)(j.bytes / 1024),
                  (unsigned)j.rate, (unsigned)j.ch, (unsigned)(s_ramBytes / 1024));
    SMPDBG("preloaded id=%u into RAM (%lu KB)", (unsigned)j.id, (unsigned long)(j.bytes / 1024));
}

// One step towards "everything wanted that fits is in the cache".  Returns
// true when there is nothing left to do; false = call again (there is more, or
// it has to wait — for a stream to finish with the card, or for the voices to
// fall quiet before it can remove something).  Never blocks for long: a load
// is read PRELOAD_STEP_BYTES at a time — PRELOAD_STEP_GENTLE with `gentle`,
// for use while a song is playing, so a sample the song streams is never kept
// waiting for the card by more than a few milliseconds.
bool samplePreloadSync(const Song* song, bool gentle) {
    if (!s_ram) return true;
    if (s_streaming) return false;                 // the stream has the card
    if (s_job.active) {
        preloadStep(gentle ? PRELOAD_STEP_GENTLE : PRELOAD_STEP_BYTES);
        return false;
    }

    uint8_t want[PRELOAD_WANT_MAX];
    int  nwant = buildWanted(song, want);
    bool idle  = !sampleRamVoicesActive();         // (a hint; cacheEvict decides for real)

    // Entries whose trim has moved since they were baked go (wanted ones are
    // loaded again below with the new region).
    for (int i = 0; i < RAM_CACHE_MAX; i++) {
        if (s_ram[i].id == 0) continue;
        uint32_t st, en; uint8_t lp;
        rawTrim(s_ram[i].id, st, en, lp);
        if (s_ram[i].trimStart == st && s_ram[i].trimEnd == en && s_ram[i].trimLoop == lp) continue;
        if (!idle || !cacheEvict(i)) return false;
    }

    // The most important wanted sample that isn't in yet: start loading it.
    for (int w = 0; w < nwant; w++) {
        uint8_t id = want[w];
        if (cacheFind(id) >= 0 || failed(id)) continue;

        const char* fname = sampleManifestNameFor(id);
        uint32_t fsize    = sampleManifestSizeFor(id);
        uint32_t st, en; uint8_t lp;
        rawTrim(id, st, en, lp);
        if (!fname || fsize <= 44 + 8) { markFailed(id); continue; }
        // Too long, and not cut down by a trim: it streams.  (Decided from
        // the size the manifest scan noted — no need to open the file.)
        if (st == 0 && en == 0 && fsize - 44 >= PRELOAD_MAX_BYTES) { markFailed(id); continue; }

        // Making room means removing something nobody wants — which has to
        // wait for the player to be idle.  Judge by the file's size (an upper
        // bound) before going near the card.
        bool noSlot = true;
        for (int i = 0; i < RAM_CACHE_MAX; i++) if (s_ram[i].id == 0) { noSlot = false; break; }
        uint32_t upper = fsize - 44;
        if (upper > PRELOAD_MAX_BYTES) upper = PRELOAD_MAX_BYTES;
        if ((noSlot || s_ramBytes + upper > PRELOAD_TOTAL_CAP) && !idle) return false;

        char path[64];
        snprintf(path, sizeof(path), "/samples/%s", fname);
        uint8_t hdr[44];
        bool ok;
        {
            SdLock _;
            s_job.f = SD.open(path);
            ok = s_job.f && s_job.f.read(hdr, sizeof(hdr)) == sizeof(hdr);
        }
        // Same canonical-WAV assumptions as the streamer.
        uint32_t rate    = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8)
                         | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
        uint16_t numCh   = (uint16_t)hdr[22] | ((uint16_t)hdr[23] << 8);
        uint16_t bitsPer = (uint16_t)hdr[34] | ((uint16_t)hdr[35] << 8);
        if (ok) ok = (numCh == 1 || numCh == 2) && bitsPer == 16 && rate >= 8000 && rate <= 48000;

        uint32_t frameBytes = (uint32_t)(numCh ? numCh : 1) * 2, dataBytes = 0, stF = 0;
        if (ok) {
            // Non-destructive trim: cache only the region playback will use.
            uint32_t totalFrames = (fsize - 44) / frameBytes;
            uint32_t enF = en ? en : totalFrames;
            stF = st;
            if (stF >= totalFrames) stF = 0;
            if (enF > totalFrames || enF <= stF) enF = totalFrames;
            dataBytes = (enF - stF) * frameBytes;
            ok = dataBytes >= frameBytes * 4 && dataBytes < PRELOAD_MAX_BYTES;
        }

        // Room: a free slot and the bytes, removing unwanted entries (biggest
        // first) until there is.  Nothing unwanted left = it doesn't fit.
        while (ok) {
            int freeSlot = -1;
            for (int i = 0; i < RAM_CACHE_MAX; i++) if (s_ram[i].id == 0) { freeSlot = i; break; }
            if (freeSlot >= 0 && s_ramBytes + dataBytes <= PRELOAD_TOTAL_CAP) break;
            int victim = -1;
            for (int i = 0; i < RAM_CACHE_MAX; i++) {
                if (s_ram[i].id == 0) continue;
                bool wanted = false;
                for (int k = 0; k < nwant; k++) if (want[k] == s_ram[i].id) { wanted = true; break; }
                if (!wanted && (victim < 0 || s_ram[i].bytes > s_ram[victim].bytes)) victim = i;
            }
            if (victim < 0) { ok = false; break; }
            if (!idle || !cacheEvict(victim)) {    // (the pre-check makes this rare)
                SdLock _; s_job.f.close();
                return false;
            }
        }

        int16_t* buf = ok ? (int16_t*)heap_caps_malloc(dataBytes, MALLOC_CAP_SPIRAM) : nullptr;
        if (ok && buf) {
            SdLock _;
            ok = !stF || s_job.f.seek(44 + stF * frameBytes);
        }
        if (!ok || !buf) {                         // unreadable / unsupported / no room / no PSRAM
            { SdLock _; if (s_job.f) s_job.f.close(); }
            free(buf);
            markFailed(id);
            continue;
        }

        s_job.active    = true;
        s_job.id        = id;
        s_job.buf       = buf;
        s_job.bytes     = dataBytes;
        s_job.done      = 0;
        s_job.rate      = rate;
        s_job.ch        = (uint8_t)numCh;
        s_job.trimStart = st;
        s_job.trimEnd   = en;
        s_job.trimLoop  = lp;
        return false;                              // the next calls read it in
    }

    memset(s_failed, 0, sizeof(s_failed));         // round complete
    return true;
}

#include "sample_record.h"
#include <math.h>
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "MagiMsg.h"
#include "MagiLink.h"
#include "sd_mutex.h"
#include "mic_spectrum.h"
#include "audio_codec.h"
#include "SamplePlayer.h"
#include "SampleManifest.h"
#include "midi_player.h"
#include "drawbar_organ.h"
#include "pitch_detect.h"
#include "take_fade.h"

// The take waits here between STOP and SAVE.  Not in /samples, so a rescan
// for the sample list doesn't pick it up.
#define REC_TMP_PATH   "/rec_take.wav"
#define REC_MIN_FRAMES (AUDIO_CODEC_RATE_HZ / 5)          // 0.2 s
#define REC_MAX_FRAMES ((uint32_t)AUDIO_CODEC_RATE_HZ * SAMPLE_REC_MAX_SECONDS)
#define REC_TAIL_DROP  (AUDIO_CODEC_RATE_HZ * 40 / 1000)  // the finger leaving the
                                                          // glass, heard by the mic
#define REC_STATUS_MS  250

// ── Requests from the link worker ────────────────────────────────────────────
struct RecReq { uint8_t op; char name[SAMPLE_NAME_LEN]; };
static RecReq        sQ[4];
static volatile int  sQHead = 0, sQTail = 0;
static portMUX_TYPE  sQMux = portMUX_INITIALIZER_UNLOCKED;

void sampleRecRequest(uint8_t op, const char* name) {
    portENTER_CRITICAL(&sQMux);
    int next = (sQHead + 1) & 3;
    if (next != sQTail) {                // full: drop (the client re-asks)
        sQ[sQHead].op = op;
        memset(sQ[sQHead].name, 0, sizeof(sQ[sQHead].name));
        if (name) strncpy(sQ[sQHead].name, name, sizeof(sQ[sQHead].name) - 1);
        sQHead = next;
    }
    portEXIT_CRITICAL(&sQMux);
}

static bool popReq(RecReq& r) {
    bool got = false;
    portENTER_CRITICAL(&sQMux);
    if (sQTail != sQHead) { r = sQ[sQTail]; sQTail = (sQTail + 1) & 3; got = true; }
    portEXIT_CRITICAL(&sQMux);
    return got;
}

// ── State ────────────────────────────────────────────────────────────────────
static uint8_t  sState      = SAMPLE_REC_IDLE;
static int16_t* sBuf        = nullptr;      // PSRAM, only while recording
static uint32_t sTakeFrames = 0;
static int32_t  sTakePeak   = 0;
static bool     sTakeClip   = false;
static uint32_t sLastStatusMs = 0;
static uint32_t sLastLevelMs  = 0;
static int32_t  sSendPeak     = 0;          // loudest since the last status
static uint8_t  sLevel        = 0;          // 0..100, for the server's screen
static uint8_t  sLastErr      = SAMPLE_REC_OK;
static bool     sCacheDropped = false;      // tell the caller to refill the cache
static bool     sHaveTake     = false;      // REC_TMP_PATH holds the latest take

// The take's pitch, found while it first plays (pitch_detect.cpp), and the
// TUNE that puts it on middle C.  sTune also pitches the keys and replays.
static uint8_t           sPitchState = SAMPLE_REC_PITCH_NONE;
static uint8_t           sPitchNote  = 0;
static int8_t            sPitchCents = 0;
static volatile int16_t  sTune       = 0;

// REVIEW: the take, still in sBuf, played from the MIDI keys (the MIDI task
// reads these; the loop task publishes / withdraws them).
static int16_t* volatile sKeysBuf    = nullptr;
static volatile uint32_t sKeysFrames = 0;
static int32_t           sKeyVoice[128];      // MIDI task: the voice each held key started
static bool              sKeyVoiceInit = false;

// Withdraw the take from the keys and silence what it is playing, so sBuf can
// be recorded over or freed.  Loop task.
static void keysOff() {
    if (!sKeysBuf) return;
    sKeysBuf = nullptr;
    delay(2);                            // a note-on mid-start on the MIDI task
    samplePlayerStop();
    for (int i = 0; i < 40 && sampleRamVoicesActive(); i++) delay(5);
}

static uint8_t pct(int32_t peak) {
    int32_t p = (peak * 100 + 16383) / 32767;
    return (uint8_t)(p > 100 ? 100 : p);
}

static void sendStatus(uint8_t state, uint8_t err, int32_t peak, bool clip,
                       uint32_t frames, uint8_t sampleId = 0) {
    sLastErr = err;
    MsgSampleRecStatus st;
    st.pitchState = sPitchState;
    st.pitchNote  = sPitchNote;
    st.pitchCents = sPitchCents;
    st.tuneCents  = sTune;
    st.state    = state;
    st.err      = err;
    st.peak     = pct(peak);
    st.clipped  = clip ? 1 : 0;
    st.sampleId = sampleId;
    st.frames   = frames;
    st.rate     = AUDIO_CODEC_RATE_HZ;
    gMagiLink.acquireMutex();
    gMagiLink.send(&st, sizeof(st));
    gMagiLink.releaseMutex();
}

static uint32_t sPitchCHz = 0;          // for KEEP LENGTH (trim.txt): 1 = unclear

static void pitchReset() {
    pitchAbort();
    sPitchCHz   = 0;
    sPitchState = SAMPLE_REC_PITCH_NONE;
    sPitchNote  = 0;
    sPitchCents = 0;
    sTune       = 0;
}

static void freeBuf() {
    pitchAbort();                        // it reads sBuf
    keysOff();
    if (sBuf) { heap_caps_free(sBuf); sBuf = nullptr; }
}

static void removeTmp() {
    SdLock _;
    if (SD.exists(REC_TMP_PATH)) SD.remove(REC_TMP_PATH);
}

static void putLE16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void putLE32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// 16-bit mono PCM at the codec rate.  Written a piece at a time so the card
// lock is never held for long.
static bool writeWav(const int16_t* pcm, uint32_t frames) {
    uint8_t h[44];
    uint32_t dataBytes = frames * 2;
    memcpy(h, "RIFF", 4);       putLE32(h + 4, 36 + dataBytes);
    memcpy(h + 8, "WAVEfmt ", 8); putLE32(h + 16, 16);
    putLE16(h + 20, 1);         putLE16(h + 22, 1);                 // PCM, mono
    putLE32(h + 24, AUDIO_CODEC_RATE_HZ);
    putLE32(h + 28, AUDIO_CODEC_RATE_HZ * 2);                       // byte rate
    putLE16(h + 32, 2);         putLE16(h + 34, 16);                // block align, bits
    memcpy(h + 36, "data", 4);  putLE32(h + 40, dataBytes);

    File f;
    {
        SdLock _;
        if (SD.exists(REC_TMP_PATH)) SD.remove(REC_TMP_PATH);
        f = SD.open(REC_TMP_PATH, FILE_WRITE);
    }
    if (!f) return false;
    bool ok;
    { SdLock _; ok = f.write(h, sizeof(h)) == sizeof(h); }
    const uint8_t* p = (const uint8_t*)pcm;
    uint32_t left = dataBytes;
    while (ok && left) {
        uint32_t n = left > 16384 ? 16384 : left;
        { SdLock _; ok = f.write(p, n) == n; }
        p += n; left -= n;
    }
    { SdLock _; f.close(); }
    if (!ok) removeTmp();
    return ok;
}

// The client's name → "<name>.wav", keeping only characters safe on the card.
static bool makeFileName(const char* in, char* out, size_t outLen) {
    char base[SAMPLE_NAME_LEN];
    size_t n = 0;
    for (const char* c = in; *c && n < sizeof(base) - 1; c++) {
        char ch = *c;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == ' ' || ch == '.';
        base[n++] = ok ? ch : '_';
    }
    base[n] = '\0';
    if (n >= 4 && strcasecmp(base + n - 4, ".wav") == 0) base[n -= 4] = '\0';
    while (n && (base[n - 1] == ' ' || base[n - 1] == '.')) base[--n] = '\0';
    char* b = base;
    while (*b == ' ' || *b == '.') b++;
    if (!*b) return false;
    size_t maxBase = SAMPLE_NAME_LEN - 5;            // room for ".wav" + NUL
    if (strlen(b) > maxBase) b[maxBase] = '\0';
    snprintf(out, outLen, "%s.wav", b);
    return true;
}

// Refused: say so, keeping whatever we had (an AGAIN refused leaves the take).
static void refuse(uint8_t err) {
    bool take = (sState == SAMPLE_REC_TAKE);
    sendStatus(sState, err, take ? sTakePeak : 0, take && sTakeClip, take ? sTakeFrames : 0);
}

static void sendTake(uint8_t state, uint8_t err = SAMPLE_REC_OK) {
    sendStatus(state, err, sTakePeak, sTakeClip, sTakeFrames);
}

// ARM: check we may, find the room, and start the mic so the hold screen has
// a level and the take begins the moment the glass is pressed.
static void armTake() {
    if (sState == SAMPLE_REC_RECORDING) return;
    if (sequencerIsRunning()) { refuse(SAMPLE_REC_ERR_SONG);  return; }
    if (organActive())        { refuse(SAMPLE_REC_ERR_ORGAN); return; }
    if (sampleManifestCount() >= SM_MAX_ENTRIES) { refuse(SAMPLE_REC_ERR_FULL); return; }
    samplePlayerStop();
    removeTmp();                                     // a new take replaces the old
    sHaveTake = false;
    pitchReset();
    sState = SAMPLE_REC_IDLE;
    if (!sBuf) sBuf = (int16_t*)heap_caps_malloc(REC_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!sBuf) {
        // The sample cache (the squares' samples) may be holding the room.
        // Empty it — what's still wanted reloads around the take.
        uint32_t t0 = millis();
        bool dropped;
        while (!(dropped = samplePreloadDropAllNow()) && millis() - t0 < 300) delay(10);
        if (dropped) {
            sCacheDropped = true;
            sBuf = (int16_t*)heap_caps_malloc(REC_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        }
        Serial.printf("[REC] buffer: cache %s, alloc %s\n",
                      dropped ? "emptied" : "BUSY", sBuf ? "ok" : "FAILED");
    }
    if (!sBuf) {
        sendStatus(SAMPLE_REC_IDLE, SAMPLE_REC_ERR_MEMORY, 0, false, 0);
        return;
    }
    micMonitor(true);
    micMonitorPeak();
    sState = SAMPLE_REC_ARMED;
    sLevel = 0;
    Serial.println("[REC] armed - hold the screen");
    sendStatus(sState, SAMPLE_REC_OK, 0, false, 0);
}

// Back to nothing: the mic off, the buffer and any take gone.
static void disarm(uint8_t err) {
    if (sState == SAMPLE_REC_RECORDING) micRecStop();
    samplePlayerStop();
    micMonitor(false);
    freeBuf();
    removeTmp();
    sHaveTake = false;
    pitchReset();
    sState = SAMPLE_REC_IDLE;
    sLevel = 0;
    sendStatus(sState, err, 0, false, 0);
}

static void playTake() {
    if (!sHaveTake || organActive()) return;
    samplePlayerStop();
    samplePlayerPlay(REC_TMP_PATH, sTune * 0.01f);
}

// The analysis is done: tune it, and tell the client.
static void pitchFinish() {
    PitchResult r = pitchResult();
    sPitchCHz = r.found ? (uint32_t)lrintf(r.hz * 100.0f) : 1;
    if (sPitchCHz < 1) sPitchCHz = 1;
    if (r.found) {
        sPitchState = SAMPLE_REC_PITCH_FOUND;
        pitchNearestNote(r.hz, sPitchNote, sPitchCents);
        sTune = pitchTuneToMiddleC(r.hz);
    } else {
        sPitchState = SAMPLE_REC_PITCH_UNCLEAR;
        sTune = 0;
    }
    Serial.printf("[REC] pitch: %s %.2f Hz (clarity %.2f) - tune %+d c\n",
                  r.found ? "found" : "unclear", (double)r.hz, (double)r.clarity, (int)sTune);
}

// The glass let go (or the 30 s limit): write the take and play it back on
// the server's REVIEW screen.  The buffer and the mic stay up there, so a
// RESAMPLE starts as fast as the first take did.
static void stopTake() {
    micRecStop();
    uint32_t frames = micRecFrames();
    bool clip = micRecClipped();
    int32_t peak = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t a = sBuf[i] < 0 ? -(int32_t)sBuf[i] : sBuf[i];
        if (a > peak) peak = a;
    }
    if (frames < REC_MIN_FRAMES) {
        // A tap, not a hold: back where we were — the previous take, if any.
        sState = sHaveTake ? SAMPLE_REC_REVIEW : SAMPLE_REC_ARMED;
        Serial.printf("[REC] too short (%u frames)\n", (unsigned)frames);
        if (sHaveTake) sendTake(sState, SAMPLE_REC_ERR_SHORT);
        else           sendStatus(sState, SAMPLE_REC_ERR_SHORT, peak, clip, frames);
        return;
    }
    // Lose the last 40 ms (lifting the finger off the glass is right by the
    // mic), then fade the ends so it starts and stops without a click.
    if (frames > REC_MIN_FRAMES + REC_TAIL_DROP) frames -= REC_TAIL_DROP;
    fadeTakeEnds(sBuf, frames, AUDIO_CODEC_RATE_HZ);
    uint32_t t0 = millis();
    bool ok = writeWav(sBuf, frames);
    Serial.printf("[REC] take %.2f s, peak %u%%%s - %s in %lu ms\n",
                  (double)frames / AUDIO_CODEC_RATE_HZ, (unsigned)pct(peak),
                  clip ? " CLIPPED" : "", ok ? "written" : "WRITE FAILED",
                  (unsigned long)(millis() - t0));
    sLevel = 0;
    if (!ok) {
        sState = SAMPLE_REC_RECORDING;     // so disarm() leaves nothing behind
        disarm(SAMPLE_REC_ERR_SD);
        return;
    }
    sHaveTake   = true;
    pitchReset();
    sPitchState = SAMPLE_REC_PITCH_ANALYSING;
    pitchBegin(sBuf, frames, AUDIO_CODEC_RATE_HZ);   // stepped in sampleRecTick
    sKeysFrames = frames;
    sKeysBuf    = sBuf;                  // the MIDI keys play it now
    sState      = SAMPLE_REC_REVIEW;
    sTakeFrames = frames;
    sTakePeak   = peak;
    sTakeClip   = clip;
    sendTake(sState);
    playTake();
}

// ── The server's screen (loop task) ──────────────────────────────────────────
void sampleRecPress() {
    if ((sState != SAMPLE_REC_ARMED && sState != SAMPLE_REC_REVIEW) || !sBuf) return;
    keysOff();                           // a RESAMPLE records over what the keys play
    pitchAbort();                        // ...and what the analysis reads
    samplePlayerStop();                  // ...and mustn't record the playback
    if (!micRecStart(sBuf, REC_MAX_FRAMES)) { disarm(SAMPLE_REC_ERR_MEMORY); return; }
    sState        = SAMPLE_REC_RECORDING;
    sLastStatusMs = millis();
    sSendPeak     = 0;
    Serial.println("[REC] recording");
    sendStatus(sState, SAMPLE_REC_OK, 0, false, 0);
}

void sampleRecRelease() {
    if (sState == SAMPLE_REC_RECORDING) stopTake();
}

void sampleRecReplay() {
    if (sState == SAMPLE_REC_REVIEW) playTake();
}

// KEEP: the take is the client's now (it names and saves it).
void sampleRecKeep() {
    if (sState != SAMPLE_REC_REVIEW) return;
    if (sPitchState == SAMPLE_REC_PITCH_ANALYSING) {   // kept before it finished: finish now
        while (!pitchStep()) {}
        pitchFinish();
    }
    samplePlayerStop();
    micMonitor(false);
    freeBuf();
    sState = SAMPLE_REC_TAKE;
    Serial.println("[REC] kept - over to the client");
    sendTake(sState);
}

void sampleRecCancel() {
    if (sState != SAMPLE_REC_ARMED && sState != SAMPLE_REC_RECORDING &&
        sState != SAMPLE_REC_REVIEW) return;
    Serial.println("[REC] cancelled on the server");
    disarm(SAMPLE_REC_ERR_CANCEL);
}

bool sampleRecKeysNote(uint8_t note, uint8_t velocity) {
    int16_t* b = sKeysBuf;
    if (!b || note > 127) return false;
    if (!sKeyVoiceInit) {
        for (int i = 0; i < 128; i++) sKeyVoice[i] = -1;
        sKeyVoiceInit = true;
    }
    // Let go (or played again): fade out what this key started.
    if (sKeyVoice[note] >= 0) {
        sampleStopVoice(sKeyVoice[note]);
        sKeyVoice[note] = -1;
    }
    if (velocity > 0 && !organActive()) {
        // As the sample keys: note 60 = middle C once tuned; velocity 100 =
        // its own level, harder louder, softer quieter (squared taper).
        int vol = ((int)velocity * 127 + 50) / 100;
        sKeyVoice[note] = samplePlayBuffer(b, sKeysFrames, AUDIO_CODEC_RATE_HZ,
                                           (float)((int)note - 60) + sTune * 0.01f,
                                           (uint8_t)(vol > 255 ? 255 : vol));
    }
    return true;
}

uint8_t  sampleRecState()      { return sState; }
uint8_t  sampleRecPitchState() { return sPitchState; }
uint8_t  sampleRecPitchNote()  { return sPitchNote; }
int8_t   sampleRecPitchCents() { return sPitchCents; }
int16_t  sampleRecTuneCents()  { return sTune; }
uint8_t  sampleRecLevel()   { return sLevel; }
uint8_t  sampleRecLastErr() { return sLastErr; }
bool     sampleRecClipped() {
    if (sState == SAMPLE_REC_RECORDING) return micRecClipped();
    return sState == SAMPLE_REC_REVIEW && sTakeClip;
}
uint32_t sampleRecFrames()  {
    if (sState == SAMPLE_REC_RECORDING) return micRecFrames();
    return sState == SAMPLE_REC_REVIEW ? sTakeFrames : 0;
}

// Returns true when the sample list changed.
static bool saveTake(const char* name) {
    char fname[SAMPLE_NAME_LEN], path[48];
    if (!makeFileName(name, fname, sizeof(fname))) {
        sendStatus(sState, SAMPLE_REC_ERR_NAME, sTakePeak, sTakeClip, sTakeFrames);
        return false;
    }
    if (sampleManifestCount() >= SM_MAX_ENTRIES) {
        sendStatus(sState, SAMPLE_REC_ERR_FULL, sTakePeak, sTakeClip, sTakeFrames);
        return false;
    }
    snprintf(path, sizeof(path), "/samples/%s", fname);
    samplePlayerStop();
    bool exists, renamed = false;
    {
        SdLock _;
        exists = SD.exists(path);
        if (!exists) {
            if (!SD.exists("/samples")) SD.mkdir("/samples");
            renamed = SD.rename(REC_TMP_PATH, path);
        }
    }
    if (exists) {
        sendStatus(sState, SAMPLE_REC_ERR_NAME, sTakePeak, sTakeClip, sTakeFrames);
        return false;
    }
    if (!renamed) {
        sendStatus(sState, SAMPLE_REC_ERR_SD, sTakePeak, sTakeClip, sTakeFrames);
        return false;
    }
    sampleManifestSync();
    uint8_t id = 0;
    for (int i = 0; i < sampleManifestCount(); i++) {
        const SmEntry* e = sampleManifestAt(i);
        if (e && strcasecmp(e->name, fname) == 0) { id = e->id; break; }
    }
    if (id && (sTune || sPitchCHz)) {    // its TUNE: note 60 plays a middle C
        sampleTrimSet(id, 0, 0, 0, sTune, 0);
        if (sPitchCHz) sampleTrimSetPitch(id, sPitchCHz);   // kept for reference
        sampleTrimSave();
    }
    Serial.printf("[REC] saved as %s (id %u, tune %+d c)\n", path, (unsigned)id, (int)sTune);
    sHaveTake = false;
    sState = SAMPLE_REC_IDLE;
    sendStatus(SAMPLE_REC_SAVED, id ? SAMPLE_REC_OK : SAMPLE_REC_ERR_SD,
               sTakePeak, sTakeClip, sTakeFrames, id);
    return true;
}

bool sampleRecTick() {
    bool changed = sCacheDropped;
    sCacheDropped = false;

    // The take ends itself when it reaches the time limit.
    if (sState == SAMPLE_REC_RECORDING && !micRecRunning()) stopTake();

    // Its pitch: one FFT a pass while it plays back.
    if (sPitchState == SAMPLE_REC_PITCH_ANALYSING && sState == SAMPLE_REC_REVIEW && pitchStep()) {
        pitchFinish();
        sendTake(sState);
    }

    RecReq r;
    while (popReq(r)) {
        switch (r.op) {
            case SAMPLE_REC_ARM:
                if (sState == SAMPLE_REC_ARMED)
                    sendStatus(sState, SAMPLE_REC_OK, 0, false, 0);
                else if (sState == SAMPLE_REC_REVIEW)
                    sendTake(sState);            // already on the server's screen
                else
                    armTake();
                break;
            case SAMPLE_REC_PLAY:
                if (sState == SAMPLE_REC_TAKE || sState == SAMPLE_REC_REVIEW) playTake();
                break;
            case SAMPLE_REC_PLAY_STOP:
                if (sState == SAMPLE_REC_TAKE || sState == SAMPLE_REC_REVIEW) samplePlayerStop();
                break;
            case SAMPLE_REC_SAVE:
                if (sState == SAMPLE_REC_TAKE) changed |= saveTake(r.name);
                break;
            case SAMPLE_REC_DISCARD:
                disarm(SAMPLE_REC_OK);
                break;
        }
    }

    // The level: for the hold screen every 50 ms, to the client every 250 ms
    // while recording.
    if (sState == SAMPLE_REC_ARMED || sState == SAMPLE_REC_RECORDING) {
        uint32_t now = millis();
        if ((uint32_t)(now - sLastLevelMs) >= 50) {
            sLastLevelMs = now;
            int32_t pk = (sState == SAMPLE_REC_RECORDING) ? micRecPeakTake() : micMonitorPeak();
            sLevel = pct(pk);
            if (pk > sSendPeak) sSendPeak = pk;
        }
        if (sState == SAMPLE_REC_RECORDING &&
            (uint32_t)(now - sLastStatusMs) >= REC_STATUS_MS) {
            sLastStatusMs = now;
            sendStatus(sState, SAMPLE_REC_OK, sSendPeak, micRecClipped(), micRecFrames());
            sSendPeak = 0;
        }
    }
    return changed;
}

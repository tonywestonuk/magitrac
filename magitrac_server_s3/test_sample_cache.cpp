// Host tests for SamplePlayer's PSRAM sample cache: what gets loaded and in
// what order, the stepwise loading, the 2 MB / 6 MB limits, keeping unwanted
// samples until their room is needed, not removing anything while it could be
// playing, re-baking after a trim edit, and the RAM voices.
//
// Run from Terminal — NOT through the Arduino IDE:
//   cd magitrac_server_s3
//   g++ -std=c++11 -Wno-format -DUNIT_TEST -I test_mocks/cache -I test_mocks -I . \
//       -I ../magitrac_lib/src test_sample_cache.cpp -o test_sample_cache && ./test_sample_cache
//
// The #ifndef ARDUINO guard makes this file a no-op when the Arduino build
// compiles it as part of the sketch.
#ifndef ARDUINO

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

static uint32_t _mockMs = 0;
uint32_t millis() { return _mockMs; }
#include "test_mocks/Arduino.h"
HardwareSerial Serial;

// ── Mock state (declared extern in test_mocks/cache/*) ───────────────────────
int  gMockCriticalDepth = 0, gMockCriticalErrors = 0, gMockSemGives = 0;
long gMockHeapBytes = 0, gMockHeapBlocks = 0;
bool gMockHeapFail = false;
std::string gMockSdRoot;
long gMockSdOpens = 0, gMockSdReadBytes = 0, gMockSdMaxRead = 0, gMockSdOpenNow = 0;
#include "esp_heap_caps.h"
#include <SD.h>
SDClass SD;

// ── Things SamplePlayer.cpp needs from the rest of the sketch ────────────────
#include "sd_mutex.h"
SdLock::SdLock()  {}
SdLock::~SdLock() {}
#include "audio_codec.h"
bool audioCodecPlay(const uint8_t*, size_t) { return true; }
void crashSetAudioPhase(uint8_t) {}

// The manifest + trim table, as the test wants them.
#include "SampleManifest.h"
static std::vector<SmEntry>    gManifest;
static std::vector<SampleTrim> gTrims;
int            sampleManifestCount()      { return (int)gManifest.size(); }
const SmEntry* sampleManifestAt(int i)    { return (i >= 0 && i < (int)gManifest.size()) ? &gManifest[i] : nullptr; }
const char*    sampleManifestNameFor(uint8_t id) { for (auto& e : gManifest) if (e.id == id) return e.name; return nullptr; }
uint32_t       sampleManifestSizeFor(uint8_t id) { for (auto& e : gManifest) if (e.id == id) return e.size; return 0; }
const SampleTrim* sampleTrimFor(uint8_t id)      { for (auto& t : gTrims) if (t.id == id) return &t; return nullptr; }
void sampleTrimSetPitch(uint8_t id, uint32_t chz) { for (auto& t : gTrims) if (t.id == id) t.pitchCHz = chz; }
bool sampleTrimSave() { return true; }

// ── The code under test ──────────────────────────────────────────────────────
#include "SamplePlayer.cpp"
#include "psola.cpp"
#undef free              // pitch_detect allocates with plain malloc on the host
#include "pitch_detect.cpp"
#define free(p) mockFree(p)

// ── Helpers ───────────────────────────────────────────────────────────────────
static int gPassed = 0, gFailed = 0;
#define CHECK(cond, msg) do { if (cond) { gPassed++; printf("  PASS  %s\n", msg); } \
                              else { gFailed++; printf("  FAIL  %s   (line %d)\n", msg, __LINE__); } } while (0)

// The PCM value a test file holds at sample index i (so cached data can be checked).
static int16_t pcmAt(uint8_t id, uint32_t i) { return (int16_t)((i * 31u + id * 1009u) & 0x7FFF) - 16384; }

// Write a canonical 16-bit WAV of `bytes` of audio and add it to the manifest.
static void addSample(uint8_t id, const char* name, uint32_t bytes, int ch = 1, int bits = 16, uint32_t rate = 32000) {
    std::string path = gMockSdRoot + "/samples/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    uint8_t h[44] = {};
    memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = (uint8_t)ch;
    h[24] = (uint8_t)rate; h[25] = (uint8_t)(rate >> 8); h[26] = (uint8_t)(rate >> 16);
    h[34] = (uint8_t)bits; memcpy(h + 36, "data", 4);
    fwrite(h, 1, 44, f);
    std::vector<int16_t> d(bytes / 2);
    for (uint32_t i = 0; i < d.size(); i++) d[i] = pcmAt(id, i);
    fwrite(d.data(), 2, d.size(), f);
    fclose(f);
    SmEntry e = {}; e.id = id; strncpy(e.name, name, SM_NAME_LEN - 1); e.size = 44 + bytes;
    gManifest.push_back(e);
}

static Song* gSong = nullptr;
static void songUses(std::initializer_list<int> ids) {
    memset(gSong, 0, sizeof(Song));
    int c = 1;
    for (int id : ids) { gSong->columns[c].midiChannel = SFX_CHANNEL; gSong->columns[c].program = (uint8_t)id; c++; }
}

// Call the sync until it reports done; returns the number of calls (0 = never finished).
static int syncAll(const Song* song, int maxCalls = 100000) {
    for (int n = 1; n <= maxCalls; n++) if (samplePreloadSync(song)) return n;
    return 0;
}
static bool cached(uint8_t id) { return cacheFind(id) >= 0; }
static int  cachedCount()      { int n = 0; for (int i = 0; i < RAM_CACHE_MAX; i++) if (s_ram[i].id) n++; return n; }
static void setTrim(uint8_t id, uint32_t st, uint32_t en, uint8_t loop) {
    for (auto& t : gTrims) if (t.id == id) { t.startFrame = st; t.endFrame = en; t.loop = loop; return; }
    SampleTrim t = {}; t.id = id; t.startFrame = st; t.endFrame = en; t.loop = loop; gTrims.push_back(t);
}
// Let the "WAV task" render until every voice has finished.
static void renderUntilQuiet() {
    static int16_t buf[512];
    for (int i = 0; i < 100000 && sampleRamVoicesActive(); i++) { memset(buf, 0, sizeof(buf)); mixRamInto(buf, 256); }
}
static void emptyCache() {
    renderUntilQuiet();
    for (int i = 0; i < RAM_CACHE_MAX; i++) if (s_ram[i].id) cacheEvict(i);
    if (s_job.active) jobAbort(false);
    memset(s_failed, 0, sizeof(s_failed));
    samplePreloadSetExtra(0); samplePreloadSetPads(false);
    gTrims.clear();
}

static const uint32_t KB = 1024;

// ── Tests ─────────────────────────────────────────────────────────────────────
static void test_song_samples_load_in_steps() {
    printf("\ntest_song_samples_load_in_steps\n");
    emptyCache();
    songUses({ 9, 10 });                          // 150 KB and 350 KB
    gMockSdMaxRead = 0;
    int calls = syncAll(gSong);
    CHECK(calls > 0, "sync finishes");
    CHECK(cached(9) && cached(10) && cachedCount() == 2, "both of the song's samples are in, nothing else");
    CHECK(gMockSdMaxRead <= PRELOAD_STEP_BYTES, "no single SD read is bigger than one step (32 KB)");
    CHECK(calls >= (int)((150 + 350) * KB / PRELOAD_STEP_BYTES), "the load is spread over many calls, not one long one");
    CHECK(s_ramBytes == (150 + 350) * KB, "cache size is the two samples' audio");
    const RamSample& r = s_ram[cacheFind(10)];
    bool same = r.ch == 2 && r.frames == 350 * KB / 4;            // sample 10 is stereo
    for (uint32_t i = 0; same && i < r.frames * 2; i += 97) same = (r.data[i] == pcmAt(10, i));
    CHECK(same, "the cached audio is the file's audio");
    CHECK(gMockSdOpenNow == 0, "no file left open");
    CHECK(samplePreloadSync(gSong), "a second sync has nothing to do");
}

static void test_pads_load_shortest_first_within_the_limits() {
    printf("\ntest_pads_load_shortest_first_within_the_limits\n");
    emptyCache();
    samplePreloadSetPads(true);
    // Note the order things arrive in.
    std::vector<uint8_t> order;
    for (int n = 0; n < 100000; n++) {
        int before = cachedCount();
        bool done = samplePreloadSync(nullptr);
        if (cachedCount() > before)
            for (int i = 0; i < RAM_CACHE_MAX; i++) {
                uint8_t id = s_ram[i].id; if (!id) continue;
                bool known = false; for (uint8_t o : order) if (o == id) known = true;
                if (!known) order.push_back(id);
            }
        if (done) break;
    }
    bool ascending = true;
    for (size_t i = 1; i < order.size(); i++)
        if (sampleManifestSizeFor(order[i]) < sampleManifestSizeFor(order[i - 1])) ascending = false;
    CHECK(ascending, "samples arrive shortest first");
    CHECK(!cached(8) && !cached(6), "the two over 2 MB (ahhhhh, air_attack) are left to stream");
    CHECK(!cached(20), "an 8-bit WAV is not loaded");
    CHECK(s_ramBytes <= PRELOAD_TOTAL_CAP, "the cache stays within 6 MB");
    CHECK(cached(9) && cached(4) && cached(10) && cached(7) && cached(12) && cached(2) && cached(5),
          "everything up to jet_flyby (1.38 MB) is in");
    CHECK(!cached(1), "all_over (1.6 MB) would take it past 6 MB, so it streams");
    printf("        cached %d samples, %lu KB\n", cachedCount(), (unsigned long)(s_ramBytes / KB));
    CHECK(gMockSdOpenNow == 0, "no file left open");
}

static void test_voices_play_and_are_polyphonic() {
    printf("\ntest_voices_play_and_are_polyphonic\n");
    // (cache still holds the pads' samples from the previous test)
    CHECK(samplePlayRam(8, 0, 127) == -1, "a sample that is not cached reports -1 (caller streams it)");
    int h[RAM_VOICES + 1];
    for (int i = 0; i <= RAM_VOICES; i++) h[i] = samplePlayRam(9, (float)i, 127);
    bool allOk = true; for (int i = 0; i <= RAM_VOICES; i++) if (h[i] < 0) allOk = false;
    CHECK(allOk, "nine plays of a cached sample all start");
    int active = 0; for (int i = 0; i < RAM_VOICES; i++) if (s_voice[i].active) active++;
    CHECK(active == RAM_VOICES, "eight voices sound at once (the ninth took the oldest's place)");
    sampleStopVoice(h[0]);                        // stale: its voice was stolen
    int releasing = 0; for (int i = 0; i < RAM_VOICES; i++) if (s_voice[i].releasing) releasing++;
    CHECK(releasing == 0, "a stale handle stops nothing");
    sampleStopVoice(h[3]);
    releasing = 0; for (int i = 0; i < RAM_VOICES; i++) if (s_voice[i].releasing) releasing++;
    CHECK(releasing == 1, "a live handle releases exactly its own voice");
    renderUntilQuiet();
    CHECK(!sampleRamVoicesActive(), "the voices run out by themselves");
}

static void test_nothing_is_removed_while_playing() {
    printf("\ntest_nothing_is_removed_while_playing\n");
    // Edit sample 9's trim while a voice is playing it.
    int h = samplePlayRam(9, 0, 127);
    CHECK(h >= 0, "sample 9 is playing");
    const int16_t* before = s_ram[cacheFind(9)].data;
    setTrim(9, 1000, 5000, 0);
    bool done = false;
    for (int i = 0; i < 50; i++) done = samplePreloadSync(nullptr);
    CHECK(!done && cached(9) && s_ram[cacheFind(9)].data == before,
          "while it plays, the stale copy stays where it is (sync waits)");
    renderUntilQuiet();
    CHECK(syncAll(nullptr) > 0, "once it is quiet the sync finishes");
    const RamSample& r = s_ram[cacheFind(9)];
    CHECK(cached(9) && r.frames == 4000, "…and the sample has been re-baked to the trimmed region");
    bool same = true; for (uint32_t i = 0; i < r.frames; i += 13) if (r.data[i] != pcmAt(9, 1000 + i)) same = false;
    CHECK(same, "the re-baked audio starts at the trim's start");
}

static void test_loading_goes_on_while_voices_play_but_not_while_streaming() {
    printf("\ntest_loading_goes_on_while_voices_play_but_not_while_streaming\n");
    emptyCache();
    songUses({ 9 });
    syncAll(gSong);
    int h = samplePlayRam(9, -24, 127);           // a long, slow note
    CHECK(h >= 0, "a voice is sounding");
    songUses({ 9, 4 });                           // the song now also wants sample 4
    int calls = syncAll(gSong);
    CHECK(calls > 0 && cached(4), "a new sample loads while a voice plays");
    renderUntilQuiet();

    songUses({ 9, 4, 3 });
    s_streaming = true;                           // the WAV task is streaming from SD
    long reads = gMockSdReadBytes, opens = gMockSdOpens;
    bool done = false;
    for (int i = 0; i < 20; i++) done = samplePreloadSync(gSong);
    CHECK(!done && gMockSdReadBytes == reads && gMockSdOpens == opens,
          "while a sample streams, the preloader leaves the card alone");
    s_streaming = false;
    CHECK(syncAll(gSong) > 0 && cached(3), "…and carries on when the stream ends");
}

static void test_unwanted_samples_stay_until_room_is_needed() {
    printf("\ntest_unwanted_samples_stay_until_room_is_needed\n");
    emptyCache();
    samplePreloadSetPads(true);
    syncAll(nullptr);
    int before = cachedCount();
    size_t bytesBefore = s_ramBytes;
    samplePreloadSetPads(false);                  // the samples page closes
    syncAll(nullptr);
    CHECK(cachedCount() == before, "closing the samples page removes nothing");

    songUses({ 1 });                              // a song that needs all_over (1.6 MB): no room as it stands
    CHECK(syncAll(gSong) > 0 && cached(1), "a song's sample gets in by pushing out unwanted ones");
    CHECK(s_ramBytes <= PRELOAD_TOTAL_CAP, "the cache is still within 6 MB");
    CHECK(cachedCount() < before + 1, "something unwanted made way for it");
    printf("        %d samples / %lu KB before, %d / %lu KB after\n", before,
           (unsigned long)(bytesBefore / KB), cachedCount(), (unsigned long)(s_ramBytes / KB));

    samplePreloadSetPads(true);                   // pads again, with the song still loaded
    syncAll(gSong);
    CHECK(cached(1), "the song's sample is never the one pushed out");
}

// While a song plays the preloader takes smaller bites of the card.
static void test_gentle_steps_while_a_song_plays() {
    printf("\ntest_gentle_steps_while_a_song_plays\n");
    emptyCache();
    songUses({ 10 });
    gMockSdMaxRead = 0;
    int calls = 0;
    for (int n = 1; n <= 100000; n++) { calls = n; if (samplePreloadSync(gSong, /*gentle=*/true)) break; }
    CHECK(cached(10), "the sample still loads");
    CHECK(gMockSdMaxRead <= PRELOAD_STEP_GENTLE, "…in reads of no more than 8 KB");
    CHECK(calls >= (int)(350 * KB / PRELOAD_STEP_GENTLE), "…spread over correspondingly more calls");
}

// A voice starting between "is anything playing?" and the removal must stop the removal.
static void test_eviction_refuses_while_a_voice_sounds() {
    printf("\ntest_eviction_refuses_while_a_voice_sounds\n");
    emptyCache();
    songUses({ 9, 4 });
    syncAll(gSong);
    int h = samplePlayRam(4, 0, 127);
    CHECK(h >= 0 && !cacheEvict(cacheFind(9)) && cached(9),
          "nothing is removed while any voice sounds — even another sample's");
    renderUntilQuiet();
    CHECK(cacheEvict(cacheFind(9)) && !cached(9), "once quiet, it goes");
}

static void test_trim_edited_during_a_load() {
    printf("\ntest_trim_edited_during_a_load\n");
    emptyCache();
    songUses({ 2 });                              // 690 KB: many steps
    for (int i = 0; i < 5; i++) samplePreloadSync(gSong);
    CHECK(s_job.active && !cached(2), "a load is under way");
    setTrim(2, 0, 20000, 1);                      // edited while it loads
    CHECK(syncAll(gSong) > 0 && cached(2), "sync still finishes");
    const RamSample& r = s_ram[cacheFind(2)];
    CHECK(r.frames == 20000 && r.loop == 1, "what ends up cached is the edited region, not the stale load");
    CHECK(gMockSdOpenNow == 0, "no file left open");
}

static void test_failures_do_not_loop_or_leak() {
    printf("\ntest_failures_do_not_loop_or_leak\n");
    emptyCache();
    songUses({ 20, 99, 9 });                      // 8-bit WAV, an id with no file, a good one
    int calls = syncAll(gSong, 5000);
    CHECK(calls > 0 && cached(9) && !cached(20) && !cached(99), "the round ends with the good sample in");
    long opens = gMockSdOpens;
    CHECK(samplePreloadSync(gSong) && gMockSdOpens == opens + 1,
          "the next round tries the bad file once more, then is done");

    emptyCache();
    songUses({ 10 });
    gMockHeapFail = true;                         // PSRAM full
    CHECK(syncAll(gSong, 5000) > 0 && !cached(10), "no memory: the round still ends (it will stream)");
    gMockHeapFail = false;
    CHECK(syncAll(gSong) > 0 && cached(10), "…and loads next time there is memory");
    CHECK(gMockSdOpenNow == 0, "no file left open");
}

// KEEP LENGTH: a cached sample's pitch is found once, then other notes keep
// its length; without it they play faster / slower.
static uint32_t framesPlayed(uint8_t id, float semis) {
    static int16_t buf[512];
    int h = samplePlayRam(id, semis, 127);
    if (h < 0) return 0;
    uint32_t n = 0;
    while (sampleRamVoicesActive() && n < 60 * 32000) { memset(buf, 0, sizeof(buf)); mixRamInto(buf, 256); n += 256; }
    return n;
}
static void test_keep_length() {
    printf("\ntest_keep_length\n");
    emptyCache();
    samplePreloadSetExtra(9);
    CHECK(syncAll(nullptr) > 0 && cached(9), "sample 9 is cached");
    uint32_t native = framesPlayed(9, 0);
    uint32_t up     = framesPlayed(9, 12);
    CHECK(up < native * 0.55 && up > native * 0.45, "without KEEP LENGTH an octave up plays in half the time");

    setTrim(9, 0, 0, 0);
    for (auto& t : gTrims) if (t.id == 9) t.keepLen = 1;
    int steps = 0;
    _mockMs += 1000;
    while (steps < 200) { samplePitchTick(true); steps++; const SampleTrim* t = sampleTrimFor(9); if (t->pitchCHz) break; }
    CHECK(sampleTrimFor(9)->pitchCHz != 0, "its pitch is found (or marked unclear) by the pitch pass");
    CHECK(!s_pitchPin, "the pitch pass let go of the sample");

    uint32_t kUp   = framesPlayed(9, 12);
    uint32_t kDown = framesPlayed(9, -12);
    CHECK(kUp   > native * 0.97 && kUp   < native * 1.05, "with KEEP LENGTH an octave up lasts as long as native");
    CHECK(kDown > native * 0.97 && kDown < native * 1.05, "with KEEP LENGTH an octave down lasts as long as native");

    // AUTO TUNE: asked for a cached sample, answered once; not cached → NOT_READY.
    {
        sampleAutoTuneRequest(9);
        uint8_t id = 0, res = 99; float hz = 0; bool got = false;
        for (int n = 0; n < 200 && !got; n++) { samplePitchTick(true); got = sampleAutoTuneTake(id, res, hz); }
        CHECK(got && id == 9 && (res == SAMPLE_TUNE_FOUND || res == SAMPLE_TUNE_UNCLEAR),
              "AUTO TUNE answers for a cached sample");
        CHECK(!sampleAutoTuneTake(id, res, hz), "...once");
        sampleAutoTuneRequest(8);                           // not in the cache
        got = false;
        for (int n = 0; n < 50 && !got; n++) { _mockMs += 200; samplePitchTick(true); got = sampleAutoTuneTake(id, res, hz); }
        CHECK(got && id == 8 && res == SAMPLE_TUNE_NOT_READY, "AUTO TUNE gives up on a sample that never arrives");
    }

    // ADD SAMPLE drops the cache: the pass gives way rather than pinning it.
    for (auto& t : gTrims) if (t.id == 9) t.pitchCHz = 0;
    _mockMs += 1000;
    samplePitchTick(true);
    CHECK(s_pitchPin == 9, "a fresh pass pins the sample it is reading");
    CHECK(samplePreloadDropAllNow() && !cached(9) && !s_pitchPin && !pitchBusy(),
          "dropping the cache cancels the pass and frees the sample");
    emptyCache();
}

int main() {
    // A scratch "SD card" with the sizes of a real one.
    char tmpl[] = "/tmp/magitrac_cache_test_XXXXXX";
    gMockSdRoot = mkdtemp(tmpl);
    std::string dir = gMockSdRoot + "/samples";
    mkdir(dir.c_str(), 0755);
    addSample(1,  "all_over.wav",   1624 * KB);
    addSample(2,  "thunder1.wav",    690 * KB);
    addSample(3,  "thunder2.wav",    308 * KB);
    addSample(4,  "thunder3.wav",    332 * KB);
    addSample(5,  "jet_flyby.wav",  1378 * KB, 2);
    addSample(6,  "air_attack.wav", 4796 * KB, 2);
    addSample(7,  "take_cover.wav",  662 * KB, 2);
    addSample(8,  "ahhhhh.wav",     2826 * KB, 2);
    addSample(9,  "orch5.wav",       150 * KB);
    addSample(10, "SlapBass.wav",    350 * KB, 2);
    addSample(11, "SlapBass1.wav",   355 * KB, 2);
    addSample(12, "crashblue.wav",   689 * KB, 2);
    addSample(20, "eightbit.wav",     40 * KB, 1, 8);

    gSong = (Song*)calloc(1, sizeof(Song));
    samplePlayerInit();
    long tableBytes = gMockHeapBytes;

    test_song_samples_load_in_steps();
    test_pads_load_shortest_first_within_the_limits();
    test_voices_play_and_are_polyphonic();
    test_nothing_is_removed_while_playing();
    test_loading_goes_on_while_voices_play_but_not_while_streaming();
    test_unwanted_samples_stay_until_room_is_needed();
    test_gentle_steps_while_a_song_plays();
    test_eviction_refuses_while_a_voice_sounds();
    test_trim_edited_during_a_load();
    test_failures_do_not_loop_or_leak();
    test_keep_length();

    printf("\nfinal checks\n");
    emptyCache();
    CHECK(gMockHeapBytes == tableBytes && s_ramBytes == 0, "every sample buffer was freed");
    CHECK(gMockCriticalErrors == 0 && gMockCriticalDepth == 0, "the critical section was never nested or left held");

    for (auto& e : gManifest) unlink((dir + "/" + e.name).c_str());
    rmdir(dir.c_str()); rmdir(gMockSdRoot.c_str());
    printf("\n%d passed, %d failed\n", gPassed, gFailed);
    return gFailed > 0 ? 1 : 0;
}

#endif // ARDUINO

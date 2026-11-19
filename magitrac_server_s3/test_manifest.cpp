// test_manifest.cpp — host test for SampleManifest.cpp: the samples.txt id
// table, trim.txt, and that a rescan never leaves lookups with an empty or
// half-built table (the MIDI task looks song samples up while the loop task
// rescans for a sample-list request).
//
// Build + run from this directory (not through the Arduino IDE; it writes a
// test_manifest binary here — don't commit it):
//   g++ -std=c++11 -Wno-format -DUNIT_TEST -I test_mocks/manifest -I test_mocks/cache \
//       -I test_mocks -I . -I ../magitrac_lib/src test_manifest.cpp -o test_manifest && ./test_manifest
//
// The #ifndef ARDUINO guard makes this file a no-op when the Arduino build
// sweeps it up with the rest of the sketch folder.

#ifndef ARDUINO

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

#include "test_mocks/Arduino.h"
uint32_t       millis() { return 0; }
HardwareSerial Serial;

#include <SD.h>
std::string gMockSdRoot;
void (*gMockSdHook)() = nullptr;
SDClass SD;

#include "sd_mutex.h"
SemaphoreHandle_t sSdMutex = nullptr;
SdLock::SdLock()  {}
SdLock::~SdLock() {}

#include "SampleManifest.cpp"

// ── Harness ───────────────────────────────────────────────────────────────────
static int gFails = 0, gChecks = 0;
#define CHECK(cond, msg) do { gChecks++; if (!(cond)) { gFails++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
                              else printf("  ok:   %s\n", msg); } while (0)

static std::string samplesDir() { return gMockSdRoot + "/samples"; }

static void writeFile(const std::string& rel, const std::string& body) {
    FILE* f = fopen((gMockSdRoot + rel).c_str(), "wb");
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
}

static std::string readFile(const std::string& rel) {
    FILE* f = fopen((gMockSdRoot + rel).c_str(), "rb");
    if (!f) return "";
    std::string s; char b[256]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

// A "WAV" of `bytes` bytes — the manifest only looks at names and sizes.
static void addWav(const char* name, size_t bytes) {
    writeFile(std::string("/samples/") + name, std::string(bytes, 'x'));
}

static uint8_t idOf(const char* name) {
    for (int i = 0; i < sampleManifestCount(); i++) {
        const SmEntry* e = sampleManifestAt(i);
        if (e && strcasecmp(e->name, name) == 0) return e->id;
    }
    return 0;
}

// ── Tests ─────────────────────────────────────────────────────────────────────
static void test_first_sync_numbers_the_samples() {
    printf("\ntest_first_sync_numbers_the_samples\n");
    addWav("kick.wav",  1000);
    addWav("snare.wav", 2000);
    writeFile("/samples/notes.txt", "not a sample");
    CHECK(sampleManifestSync(), "sync succeeds");
    CHECK(sampleManifestCount() == 2, "two samples (the .txt is ignored)");
    uint8_t k = idOf("kick.wav"), s = idOf("snare.wav");
    CHECK(k && s && k != s, "each has its own id");
    CHECK(sampleManifestNameFor(k) && !strcmp(sampleManifestNameFor(k), "kick.wav"), "lookup by id");
    CHECK(sampleManifestSizeFor(s) == 2000, "size noted");
    CHECK(readFile("/samples/samples.txt").find("=snare.wav") != std::string::npos,
          "samples.txt written");
}

static void test_ids_are_stable_when_a_sample_is_added() {
    printf("\ntest_ids_are_stable_when_a_sample_is_added\n");
    uint8_t k = idOf("kick.wav"), s = idOf("snare.wav");
    addWav("hat.wav", 500);
    CHECK(sampleManifestSync(), "sync succeeds");
    CHECK(sampleManifestCount() == 3, "three samples");
    CHECK(idOf("kick.wav") == k && idOf("snare.wav") == s, "existing ids unchanged");
    uint8_t h = idOf("hat.wav");
    CHECK(h && h != k && h != s, "the new one gets a free id");
    bool sorted = true;
    for (int i = 1; i < sampleManifestCount(); i++)
        if (sampleManifestAt(i)->id < sampleManifestAt(i - 1)->id) sorted = false;
    CHECK(sorted, "table in id order (stable client paging)");
}

// The bug this guards: the old sync emptied the live table and refilled it in
// place, so a song sample looked up mid-rescan was not found (and played
// silent).  The mock calls this hook at every SD open, byte read and folder
// step — so from inside the sync, at every point it touches the card.
static uint8_t sWatchIds[3];
static int     sMidScanCalls = 0, sMidScanMisses = 0, sMidScanShort = 0;
static void lookUpMidScan() {
    sMidScanCalls++;
    if (sampleManifestCount() < 3) sMidScanShort++;
    for (uint8_t id : sWatchIds) {
        const char* n = sampleManifestNameFor(id);
        if (!n || !*n) sMidScanMisses++;
    }
}

static void test_lookups_during_a_rescan_still_find_samples() {
    printf("\ntest_lookups_during_a_rescan_still_find_samples\n");
    sWatchIds[0] = idOf("kick.wav"); sWatchIds[1] = idOf("snare.wav"); sWatchIds[2] = idOf("hat.wav");
    gMockSdHook = lookUpMidScan;
    CHECK(sampleManifestSync(), "sync succeeds");
    gMockSdHook = nullptr;
    CHECK(sMidScanCalls > 0, "lookups ran in the middle of the scan");
    CHECK(sMidScanMisses == 0, "every sample found by id throughout");
    CHECK(sMidScanShort == 0, "the list never looked short");
}

static void test_failed_sync_keeps_the_table() {
    printf("\ntest_failed_sync_keeps_the_table\n");
    std::string moved = gMockSdRoot + "/samples_away";
    rename(samplesDir().c_str(), moved.c_str());       // the folder can't be opened
    CHECK(!sampleManifestSync(), "sync reports failure");
    CHECK(sampleManifestCount() == 3, "the previous table is still there");
    CHECK(sampleManifestNameFor(idOf("kick.wav")) != nullptr, "and still answers lookups");
    rename(moved.c_str(), samplesDir().c_str());
}

static void test_trims_load_once_and_memory_wins() {
    printf("\ntest_trims_load_once_and_memory_wins\n");
    // (trim.txt was read by the first sync, when it didn't exist.)
    uint8_t k = idOf("kick.wav");
    CHECK(sampleTrimFor(k) == nullptr, "no trim stored yet");

    sampleTrimSet(k, 10, 900, 1, -250);
    const SampleTrim* t = sampleTrimFor(k);
    CHECK(t && t->startFrame == 10 && t->endFrame == 900 && t->loop == 1 && t->tuneCents == -250,
          "edit lands in memory");

    // A rescan before the edit is saved must not throw it away (the old sync
    // re-read trim.txt, so the server had to save first).
    CHECK(sampleManifestSync(), "rescan with the edit unsaved");
    t = sampleTrimFor(k);
    CHECK(t && t->tuneCents == -250, "edit survives the rescan");

    CHECK(sampleTrimSave(), "save");
    char want[64];
    snprintf(want, sizeof(want), "%u=10:900:1:-250:0", (unsigned)k);
    CHECK(readFile("/samples/trim.txt").find(want) != std::string::npos, "trim.txt has six fields");

    sampleTrimSet(k, 0, 0, 0, 0, 1);
    t = sampleTrimFor(k);
    CHECK(t && t->velOff == 1, "velocity switch alone keeps an entry");

    sampleTrimSet(k, 0, 0, 0, 0);
    CHECK(sampleTrimFor(k) == nullptr, "all defaults removes the entry");
}

static void test_three_field_trim_lines_still_parse() {
    printf("\ntest_three_field_trim_lines_still_parse\n");
    // Straight into the loader: trims are only read by the first sync.
    writeFile("/samples/trim.txt", "5=100:200:1\n6=0:0:0:+700\n7=1:2:0:99999\n8=0:0:0:0:1\nbad line\n");
    loadTrims();
    const SampleTrim* a = sampleTrimFor(5);
    const SampleTrim* b = sampleTrimFor(6);
    const SampleTrim* c = sampleTrimFor(7);
    CHECK(a && a->startFrame == 100 && a->endFrame == 200 && a->loop == 1 && a->tuneCents == 0,
          "three fields: tune 0");
    CHECK(b && b->tuneCents == 700, "four fields: tune read");
    CHECK(c && c->tuneCents == 4800, "tune clamped to four octaves");
    const SampleTrim* d = sampleTrimFor(8);
    CHECK(d && d->velOff == 1 && a->velOff == 0, "five fields: velocity switch read (default on)");
}

int main() {
    char tmpl[] = "/tmp/magitrac_manifest_XXXXXX";
    gMockSdRoot = mkdtemp(tmpl);
    mkdir(samplesDir().c_str(), 0755);

    test_first_sync_numbers_the_samples();
    test_ids_are_stable_when_a_sample_is_added();
    test_lookups_during_a_rescan_still_find_samples();
    test_failed_sync_keeps_the_table();
    test_trims_load_once_and_memory_wins();
    test_three_field_trim_lines_still_parse();

    std::string cmd = "rm -rf '" + gMockSdRoot + "'";
    system(cmd.c_str());
    printf("\n%d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}

#endif // ARDUINO

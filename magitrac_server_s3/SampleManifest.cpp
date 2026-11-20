#include "SampleManifest.h"
#include "sd_mutex.h"
#include "MagiMsg.h"         // SAMPLE_LEVEL_MIN/MAX_DB10
#include <math.h>
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include "esp_heap_caps.h"

static const char* SM_DIR  = "/samples";
static const char* SM_PATH = "/samples/samples.txt";

// The id ↔ filename table is double-buffered.  sampleManifestSync() (loop task)
// builds a fresh table in s_build, then publishes it by swapping one pointer, so
// the readers — the MIDI task looking a song sample up by id among them — always
// see a complete table, never an empty or half-written one.  The table it
// replaces becomes the next build buffer; that is safe because a reader holds
// its table only for one lookup (microseconds) and syncs are seconds apart (one
// per sample-list request).
//
// All tables (these two and s_trim below) live in PSRAM — control-rate data,
// and internal RAM is the radios'.  Allocated on first use by tablesReady();
// until the first sync publishes, there is no live table and lookups find
// nothing.
struct SmTable {
    int     count;
    uint8_t hiId;          // the highest id ever given out (#hi= line)
    SmEntry e[SM_MAX_ENTRIES];
};
static SmTable*    s_live  = nullptr;     // published — readers look only here
static SmTable*    s_build = nullptr;     // the next sync's workspace (loop task)
struct SampleTrim;                        // SampleManifest.h
static SampleTrim* s_trim = nullptr;      // SM_MAX_ENTRIES
static bool tablesReady();

static inline const SmTable* liveTable() {
    return __atomic_load_n(&s_live, __ATOMIC_ACQUIRE);
}

// ── Parsing ────────────────────────────────────────────────────────────────

static bool readLine(File& f, char* buf, int bufLen) {
    int n = 0;
    while (f.available()) {
        int c = f.read();
        if (c < 0)         break;
        if (c == '\r')     continue;
        if (c == '\n')     { buf[n] = '\0'; return true; }
        if (n < bufLen - 1) buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return n > 0;
}

// Parses "<id>=<filename>" into outId/outName.  Returns true on a valid line.
static bool parseLine(const char* line, uint8_t& outId, char* outName, int nameLen) {
    const char* eq = strchr(line, '=');
    if (!eq || eq == line) return false;
    int id = atoi(line);
    if (id < 1 || id > 127) return false;
    const char* name = eq + 1;
    if (*name == '\0') return false;
    strncpy(outName, name, nameLen - 1);
    outName[nameLen - 1] = '\0';
    outId = (uint8_t)id;
    return true;
}

static int findByName(const SmTable* t, const char* name) {
    for (int i = 0; i < t->count; i++) {
        if (strcasecmp(t->e[i].name, name) == 0) return i;
    }
    return -1;
}

// Past the highest id ever given out, so a deleted sample's id isn't reused
// while there are fresh ones; after 127, the lowest free.
static uint8_t nextFreeId(SmTable* t) {
    bool used[128] = {};
    for (int i = 0; i < t->count; i++) {
        used[t->e[i].id] = true;
        if (t->e[i].id > t->hiId) t->hiId = t->e[i].id;
    }
    for (int id = t->hiId + 1; id < 128; id++) if (!used[id]) { t->hiId = (uint8_t)id; return (uint8_t)id; }
    for (int id = 1; id < 128; id++) if (!used[id]) return (uint8_t)id;
    return 0;  // exhausted
}

// ── Manifest I/O ───────────────────────────────────────────────────────────

static void loadManifest(SmTable* t) {
    SdLock _;
    t->count = 0;
    t->hiId  = 0;
    File f = SD.open(SM_PATH, FILE_READ);
    if (!f) return;
    char line[SM_NAME_LEN + 16];
    while (t->count < SM_MAX_ENTRIES && readLine(f, line, sizeof(line))) {
        if (strncmp(line, "#hi=", 4) == 0) {
            int hi = atoi(line + 4);
            if (hi > 0 && hi < 128) t->hiId = (uint8_t)hi;
            continue;
        }
        uint8_t id;
        char    name[SM_NAME_LEN];
        if (!parseLine(line, id, name, SM_NAME_LEN)) continue;

        // Reject duplicates (id or name) — keep first occurrence.
        bool dup = false;
        for (int i = 0; i < t->count; i++) {
            if (t->e[i].id == id ||
                strcasecmp(t->e[i].name, name) == 0) { dup = true; break; }
        }
        if (dup) continue;

        SmEntry& e = t->e[t->count++];
        if (id > t->hiId) t->hiId = id;
        e.id = id;
        strncpy(e.name, name, SM_NAME_LEN - 1);
        e.name[SM_NAME_LEN - 1] = '\0';
        e.size = 0;                             // filled in by the folder scan
    }
    f.close();
}

static bool writeManifest(const SmTable* t) {
    SdLock _;
    SD.remove(SM_PATH);
    File f = SD.open(SM_PATH, FILE_WRITE);
    if (!f) return false;
    char line[SM_NAME_LEN + 16];
    if (t->hiId) {
        snprintf(line, sizeof(line), "#hi=%u\n", (unsigned)t->hiId);
        f.print(line);
    }
    for (int i = 0; i < t->count; i++) {
        snprintf(line, sizeof(line), "%u=%s\n", t->e[i].id, t->e[i].name);
        f.print(line);
    }
    f.close();
    return true;
}

// Walks /samples/ for .wav files and appends any not already in the manifest.
// Returns true if new entries were added.
static bool scanAndAppend(SmTable* t) {
    SdLock _;
    File dir = SD.open(SM_DIR);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }

    bool added = false;
    for (;;) {                   // every file: known ones still need their size noted
        File f = dir.openNextFile();
        if (!f) break;
        if (f.isDirectory()) { f.close(); continue; }

        const char* n = f.name();
        int len = (int)strlen(n);
        bool isWav = (n[0] != '.') && len > 4 &&
                     strcasecmp(n + len - 4, ".wav") == 0 &&
                     len < SM_NAME_LEN;
        if (!isWav) { f.close(); continue; }

        int at = findByName(t, n);
        if (at < 0) {
            if (t->count >= SM_MAX_ENTRIES) { f.close(); continue; }   // table full
            uint8_t id = nextFreeId(t);
            if (id == 0) { f.close(); continue; }   // 127 ids used up
            at = t->count++;
            t->e[at].id = id;
            strncpy(t->e[at].name, n, SM_NAME_LEN - 1);
            t->e[at].name[SM_NAME_LEN - 1] = '\0';
            added = true;
        }
        t->e[at].size = (uint32_t)f.size();   // the cache's "will it fit" figure
        f.close();
    }
    dir.close();
    return added;
}

// Sort by id ascending (stable order for client paging).
static void sortById(SmTable* t) {
    for (int i = 1; i < t->count; i++) {
        SmEntry e = t->e[i];
        int j = i;
        while (j > 0 && t->e[j - 1].id > e.id) {
            t->e[j] = t->e[j - 1];
            j--;
        }
        t->e[j] = e;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

// ── Trim/loop metadata (/samples/trim.txt) ───────────────────────────────────

static const char* ST_PATH = "/samples/trim.txt";
static int         s_trimCount = 0;
static bool        s_trimLoaded = false;   // trim.txt is read once, by the first sync

// PSRAM first, internal RAM as a fallback; zeroed.  False only if both fail.
static SmTable* s_tabs[2] = { nullptr, nullptr };   // the two buffers behind s_live / s_build

static SmTable* allocTable() {
    SmTable* t = (SmTable*)heap_caps_calloc(1, sizeof(SmTable), MALLOC_CAP_SPIRAM);
    if (!t) t = (SmTable*)calloc(1, sizeof(SmTable));
    return t;
}

static bool tablesReady() {
    for (int i = 0; i < 2; i++) if (!s_tabs[i]) s_tabs[i] = allocTable();
    if (!s_build && !s_live) s_build = s_tabs[0];   // first sync builds in [0]
    if (!s_trim) {
        s_trim = (SampleTrim*)heap_caps_calloc(SM_MAX_ENTRIES, sizeof(SampleTrim), MALLOC_CAP_SPIRAM);
        if (!s_trim) s_trim = (SampleTrim*)calloc(SM_MAX_ENTRIES, sizeof(SampleTrim));
    }
    return s_tabs[0] && s_tabs[1] && s_trim;
}

static void loadTrims() {
    SdLock _;
    s_trimCount = 0;
    File f = SD.open(ST_PATH);
    if (!f) return;
    char line[64];
    while (s_trimCount < SM_MAX_ENTRIES && readLine(f, line, sizeof(line))) {
        // <id>=<startFrame>:<endFrame>:<loop>[:<tuneCents>[:<velOff>[:<keepLen>[:<pitchCHz>[:<levelDb10>]]]]]
        const char* eq = strchr(line, '=');
        if (!eq) continue;
        int id = atoi(line);
        if (id < 1 || id > 127) continue;
        unsigned long st = 0, en = 0;   // %lu's own type (64-bit on the host test)
        int lp = 0, tune = 0, vo = 0, kl = 0, lv = 0;
        unsigned long pc = 0;
        // Fewer fields = a file written before TUNE / the velocity switch /
        // KEEP LENGTH / LEVEL existed (they stay 0).
        if (sscanf(eq + 1, "%lu:%lu:%d:%d:%d:%d:%lu:%d", &st, &en, &lp, &tune, &vo, &kl, &pc, &lv) < 3) continue;
        if (lv < SAMPLE_LEVEL_MIN_DB10) lv = SAMPLE_LEVEL_MIN_DB10;
        if (lv > SAMPLE_LEVEL_MAX_DB10) lv = SAMPLE_LEVEL_MAX_DB10;
        if (tune < -4800) tune = -4800;
        if (tune >  4800) tune =  4800;
        SampleTrim& t = s_trim[s_trimCount++];
        t.id = (uint8_t)id; t.startFrame = st; t.endFrame = en; t.loop = (uint8_t)(lp != 0);
        t.tuneCents = (int16_t)tune;
        t.velOff    = (uint8_t)(vo != 0);
        t.keepLen   = (uint8_t)(kl != 0);
        t.pitchCHz  = (uint32_t)pc;
        t.levelDb10 = (int16_t)lv;
    }
    f.close();
    Serial.printf("[SM] trim.txt: %d entr%s\n", s_trimCount, s_trimCount == 1 ? "y" : "ies");
}

const SampleTrim* sampleTrimFor(uint8_t id) {
    for (int i = 0; i < s_trimCount; i++)
        if (s_trim[i].id == id) return &s_trim[i];
    return nullptr;
}

float sampleLevelFor(uint8_t id) {
    const SampleTrim* t = sampleTrimFor(id);
    return (t && t->levelDb10) ? powf(10.0f, (float)t->levelDb10 / 200.0f) : 1.0f;
}

void sampleTrimSet(uint8_t id, uint32_t startFrame, uint32_t endFrame, uint8_t loop,
                   int16_t tuneCents, uint8_t velOff, uint8_t keepLen, int16_t levelDb10) {
    velOff  = velOff ? 1 : 0;
    keepLen = keepLen ? 1 : 0;
    if (!tablesReady()) return;
    if (tuneCents < -4800) tuneCents = -4800;
    if (tuneCents >  4800) tuneCents =  4800;
    if (levelDb10 < SAMPLE_LEVEL_MIN_DB10) levelDb10 = SAMPLE_LEVEL_MIN_DB10;
    if (levelDb10 > SAMPLE_LEVEL_MAX_DB10) levelDb10 = SAMPLE_LEVEL_MAX_DB10;
    bool deflt = (startFrame == 0 && endFrame == 0 && !loop &&
                  tuneCents == 0 && !velOff && !keepLen && levelDb10 == 0);   // back to defaults
    for (int i = 0; i < s_trimCount; i++) {
        if (s_trim[i].id != id) continue;
        if (deflt && s_trim[i].pitchCHz == 0) { s_trim[i] = s_trim[--s_trimCount]; }
        else        { s_trim[i].startFrame = startFrame;
                      s_trim[i].endFrame   = endFrame;
                      s_trim[i].loop       = loop;
                      s_trim[i].tuneCents  = tuneCents;
                      s_trim[i].velOff     = velOff;
                      s_trim[i].keepLen    = keepLen;
                      s_trim[i].levelDb10  = levelDb10; }
        return;
    }
    if (deflt || s_trimCount >= SM_MAX_ENTRIES) return;
    SampleTrim& t = s_trim[s_trimCount++];
    t.id = id; t.startFrame = startFrame; t.endFrame = endFrame; t.loop = loop;
    t.tuneCents = tuneCents;
    t.velOff    = velOff;
    t.keepLen   = keepLen;
    t.pitchCHz  = 0;
    t.levelDb10 = levelDb10;
}

void sampleTrimSetPitch(uint8_t id, uint32_t pitchCHz) {
    if (!tablesReady()) return;
    for (int i = 0; i < s_trimCount; i++)
        if (s_trim[i].id == id) { s_trim[i].pitchCHz = pitchCHz; return; }
    if (s_trimCount >= SM_MAX_ENTRIES) return;
    SampleTrim& t = s_trim[s_trimCount++];
    t = SampleTrim{};
    t.id = id;
    t.pitchCHz = pitchCHz;
}

bool sampleTrimSave() {
    SdLock _;
    if (SD.exists(ST_PATH)) SD.remove(ST_PATH);
    File f = SD.open(ST_PATH, FILE_WRITE);
    if (!f) return false;
    for (int i = 0; i < s_trimCount; i++)
        f.printf("%u=%lu:%lu:%u:%d:%u:%u:%lu:%d\n", (unsigned)s_trim[i].id,
                 (unsigned long)s_trim[i].startFrame,
                 (unsigned long)s_trim[i].endFrame,
                 (unsigned)s_trim[i].loop,
                 (int)s_trim[i].tuneCents,
                 (unsigned)s_trim[i].velOff,
                 (unsigned)s_trim[i].keepLen,
                 (unsigned long)s_trim[i].pitchCHz,
                 (int)s_trim[i].levelDb10);
    f.close();
    return true;
}

void sampleTrimReload() {
    if (!tablesReady()) return;
    loadTrims();
    s_trimLoaded = true;
}

bool sampleManifestSync() {
    if (!tablesReady()) return false;
    {
        SdLock _;
        File dir = SD.open(SM_DIR);
        bool dirOk = dir && dir.isDirectory();
        if (dir) dir.close();
        if (!dirOk) return false;          // the live table stays as it was
    }

    SmTable* t = s_build;
    loadManifest(t);
    bool changed = scanAndAppend(t);
    sortById(t);
    if (changed) writeManifest(t);

    // Publish: one pointer store.  The table it replaces is the next build
    // buffer (or, on the first sync, the second buffer is).
    SmTable* old = s_live ? s_live : s_tabs[1];
    __atomic_store_n(&s_live, t, __ATOMIC_RELEASE);
    s_build = old;

    // trim.txt only on the first sync.  After that the table in memory is the
    // authority (edits land there first, sampleTrimSave writes it out), and
    // nothing else changes the file while the server runs — so re-reading it
    // would only open a window in which song samples play untrimmed.
    if (!s_trimLoaded) {
        loadTrims();
        s_trimLoaded = true;
    }
    return true;
}

bool sampleManifestRemove(uint8_t id) {
    const SmTable* live = liveTable();
    if (!live || !tablesReady()) return false;
    SmTable* t = s_build;
    t->count = 0;
    t->hiId  = live->hiId;
    bool found = false;
    for (int i = 0; i < live->count; i++) {
        if (live->e[i].id > t->hiId) t->hiId = live->e[i].id;
        if (live->e[i].id == id) { found = true; continue; }
        t->e[t->count++] = live->e[i];
    }
    if (!found) return false;
    writeManifest(t);
    SmTable* old = s_live;
    __atomic_store_n(&s_live, t, __ATOMIC_RELEASE);
    s_build = old;

    for (int i = 0; i < s_trimCount; i++)
        if (s_trim[i].id == id) { s_trim[i] = s_trim[--s_trimCount]; break; }
    sampleTrimSave();
    return true;
}

int sampleManifestCount() {
    const SmTable* t = liveTable();
    return t ? t->count : 0;
}

const SmEntry* sampleManifestAt(int idx) {
    const SmTable* t = liveTable();
    if (!t || idx < 0 || idx >= t->count) return nullptr;
    return &t->e[idx];
}

uint32_t sampleManifestSizeFor(uint8_t id) {
    const SmTable* t = liveTable();
    if (!t) return 0;
    for (int i = 0; i < t->count; i++) {
        if (t->e[i].id == id) return t->e[i].size;
    }
    return 0;
}

const char* sampleManifestNameFor(uint8_t id) {
    const SmTable* t = liveTable();
    if (!t) return nullptr;
    for (int i = 0; i < t->count; i++) {
        if (t->e[i].id == id) return t->e[i].name;
    }
    return nullptr;
}

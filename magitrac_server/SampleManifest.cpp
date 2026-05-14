#include "SampleManifest.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>

static const char* SM_DIR  = "/samples";
static const char* SM_PATH = "/samples/samples.txt";

static SmEntry s_entries[SM_MAX_ENTRIES];
static int     s_count = 0;

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

static int findByName(const char* name) {
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_entries[i].name, name) == 0) return i;
    }
    return -1;
}

static uint8_t nextFreeId() {
    bool used[128] = {};
    for (int i = 0; i < s_count; i++) used[s_entries[i].id] = true;
    for (int id = 1; id < 128; id++) if (!used[id]) return (uint8_t)id;
    return 0;  // exhausted
}

// ── Manifest I/O ───────────────────────────────────────────────────────────

static void loadManifest() {
    s_count = 0;
    File f = SD.open(SM_PATH, FILE_READ);
    if (!f) return;
    char line[SM_NAME_LEN + 16];
    while (s_count < SM_MAX_ENTRIES && readLine(f, line, sizeof(line))) {
        uint8_t id;
        char    name[SM_NAME_LEN];
        if (!parseLine(line, id, name, SM_NAME_LEN)) continue;

        // Reject duplicates (id or name) — keep first occurrence.
        bool dup = false;
        for (int i = 0; i < s_count; i++) {
            if (s_entries[i].id == id ||
                strcasecmp(s_entries[i].name, name) == 0) { dup = true; break; }
        }
        if (dup) continue;

        s_entries[s_count].id = id;
        strncpy(s_entries[s_count].name, name, SM_NAME_LEN - 1);
        s_entries[s_count].name[SM_NAME_LEN - 1] = '\0';
        s_count++;
    }
    f.close();
}

static bool writeManifest() {
    SD.remove(SM_PATH);
    File f = SD.open(SM_PATH, FILE_WRITE);
    if (!f) return false;
    char line[SM_NAME_LEN + 16];
    for (int i = 0; i < s_count; i++) {
        snprintf(line, sizeof(line), "%u=%s\n", s_entries[i].id, s_entries[i].name);
        f.print(line);
    }
    f.close();
    return true;
}

// Walks /samples/ for .wav files and appends any not already in the manifest.
// Returns true if new entries were added.
static bool scanAndAppend() {
    File dir = SD.open(SM_DIR);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }

    bool added = false;
    while (s_count < SM_MAX_ENTRIES) {
        File f = dir.openNextFile();
        if (!f) break;
        if (f.isDirectory()) { f.close(); continue; }

        const char* n = f.name();
        int len = (int)strlen(n);
        bool isWav = (n[0] != '.') && len > 4 &&
                     strcasecmp(n + len - 4, ".wav") == 0 &&
                     len < SM_NAME_LEN;
        if (!isWav) { f.close(); continue; }

        if (findByName(n) < 0) {
            uint8_t id = nextFreeId();
            if (id == 0) { f.close(); break; }   // 127 ids used up
            s_entries[s_count].id = id;
            strncpy(s_entries[s_count].name, n, SM_NAME_LEN - 1);
            s_entries[s_count].name[SM_NAME_LEN - 1] = '\0';
            s_count++;
            added = true;
        }
        f.close();
    }
    dir.close();
    return added;
}

// Sort by id ascending (stable order for client paging).
static void sortById() {
    for (int i = 1; i < s_count; i++) {
        SmEntry e = s_entries[i];
        int j = i;
        while (j > 0 && s_entries[j - 1].id > e.id) {
            s_entries[j] = s_entries[j - 1];
            j--;
        }
        s_entries[j] = e;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

bool sampleManifestSync() {
    s_count = 0;
    File dir = SD.open(SM_DIR);
    bool dirOk = dir && dir.isDirectory();
    if (dir) dir.close();
    if (!dirOk) return false;

    loadManifest();
    bool changed = scanAndAppend();
    sortById();
    if (changed) writeManifest();
    return true;
}

int sampleManifestCount() { return s_count; }

const SmEntry* sampleManifestAt(int idx) {
    if (idx < 0 || idx >= s_count) return nullptr;
    return &s_entries[idx];
}

const char* sampleManifestNameFor(uint8_t id) {
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].id == id) return s_entries[i].name;
    }
    return nullptr;
}

#include "SetlistStorage.h"
#include <SD.h>
#include <string.h>
#include <stdio.h>

const char* SETLISTS_DIR = "/setlists";

static const char* MAGIC_HEADER = "MAGITRAC_SETLIST v1";

void buildSetlistPath(uint8_t slot, char* out, size_t outLen) {
    snprintf(out, outLen, "%s/setlist%u.set", SETLISTS_DIR, (unsigned)slot);
}

void initSetlist(Setlist* sl, uint8_t slot) {
    memset(sl, 0, sizeof(Setlist));
    snprintf(sl->name, SETLIST_NAME_LEN, "Setlist %u", (unsigned)slot);
    sl->count = 0;
}

// Strip trailing CR/LF/whitespace from an Arduino String in place by truncating.
static void rtrim(String& s) {
    int n = s.length();
    while (n > 0) {
        char c = s.charAt(n - 1);
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') n--;
        else break;
    }
    if (n != (int)s.length()) s.remove(n);
}

static void copyField(const String& src, int startIdx, char* dst, size_t dstLen) {
    int srcLen = src.length() - startIdx;
    if (srcLen < 0) srcLen = 0;
    if (srcLen > (int)dstLen - 1) srcLen = dstLen - 1;
    for (int i = 0; i < srcLen; i++) dst[i] = src.charAt(startIdx + i);
    dst[srcLen] = '\0';
}

bool loadSetlist(uint8_t slot, Setlist* sl) {
    char path[40];
    buildSetlistPath(slot, path, sizeof(path));
    if (!SD.exists(path)) return false;

    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    String line = f.readStringUntil('\n');
    rtrim(line);
    if (line != MAGIC_HEADER) { f.close(); return false; }

    // Header is valid — commit by parsing directly into *sl (no stack temp:
    // a Setlist is ~13 KB, which would blow the loop-task stack).
    initSetlist(sl, slot);
    sl->count = 0;

    int curIdx = -1;
    while (f.available()) {
        line = f.readStringUntil('\n');
        rtrim(line);
        if (line.length() == 0) continue;

        if (line == "SONG") {
            if (sl->count >= SETLIST_MAX_SONGS) { curIdx = -1; continue; }
            curIdx = sl->count;
            sl->count++;
            sl->songs[curIdx].name[0]  = '\0';
            sl->songs[curIdx].file[0]  = '\0';
            sl->songs[curIdx].notes[0] = '\0';
            continue;
        }
        if (line == "END") { curIdx = -1; continue; }

        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq);

        if (curIdx < 0) {
            if (key == "NAME") {
                copyField(line, eq + 1, sl->name, SETLIST_NAME_LEN);
            }
        } else {
            if (key == "NAME") {
                copyField(line, eq + 1, sl->songs[curIdx].name, SETLIST_SONG_NAME_LEN);
            } else if (key == "FILE") {
                copyField(line, eq + 1, sl->songs[curIdx].file, SETLIST_FILE_LEN);
            } else if (key == "NOTES") {
                copyField(line, eq + 1, sl->songs[curIdx].notes, SETLIST_NOTES_LEN);
            }
        }
    }
    f.close();
    return true;
}

bool saveSetlist(uint8_t slot, const Setlist* sl) {
    if (!SD.exists(SETLISTS_DIR)) SD.mkdir(SETLISTS_DIR);

    char path[40];
    buildSetlistPath(slot, path, sizeof(path));

    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;

    f.println(MAGIC_HEADER);
    f.print("NAME=");
    f.println(sl->name);

    uint8_t n = sl->count;
    if (n > SETLIST_MAX_SONGS) n = SETLIST_MAX_SONGS;
    for (uint8_t i = 0; i < n; i++) {
        const SetlistEntry& e = sl->songs[i];
        f.println("SONG");
        f.print("NAME=");
        f.println(e.name);
        f.print("FILE=");
        f.println(e.file);
        f.print("NOTES=");
        f.println(e.notes);
        f.println("END");
    }

    f.close();
    return true;
}

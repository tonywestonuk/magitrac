#include "SetlistStorage.h"
#include <string.h>
#include <stdio.h>

// ── helpers ──────────────────────────────────────────────────────────────────

static void copyField(const char* val, size_t valLen, char* dst, size_t dstCap) {
    size_t n = valLen;
    if (n > dstCap - 1) n = dstCap - 1;
    memcpy(dst, val, n);
    dst[n] = '\0';
}

// Length of the line starting at p (up to nl/end), trailing CR/space trimmed.
static size_t lineLen(const char* p, const char* end) {
    const char* nl = (const char*)memchr(p, '\n', end - p);
    size_t llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
    while (llen > 0) {
        char c = p[llen - 1];
        if (c == '\r' || c == ' ' || c == '\t') llen--;
        else break;
    }
    return llen;
}

// ── Master catalog ────────────────────────────────────────────────────────────

const char* masterListFilename() { return "master.txt"; }

void initMasterList(MasterList* ml) {
    ml->count = 0;
}

int masterFindByName(const MasterList* ml, const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < (int)ml->count; i++) {
        if (strcasecmp(ml->entries[i].name, name) == 0) return i;
    }
    return -1;
}

size_t serializeMasterList(const MasterList* ml, char* buf, size_t cap) {
    size_t off = 0;
    auto append = [&](const char* s, size_t n) {
        if (off + n <= cap) { memcpy(buf + off, s, n); off += n; }
    };
    auto appendStr = [&](const char* s) { append(s, strlen(s)); };

    int n = ml->count > MASTER_MAX_ENTRIES ? MASTER_MAX_ENTRIES : ml->count;
    for (int i = 0; i < n; i++) {
        const MasterEntry& e = ml->entries[i];
        if (e.name[0] == '\0') continue;        // skip blanks defensively
        appendStr(e.name); append(":", 1);
        appendStr(e.file); append(":", 1);
        appendStr(e.notes);
        append("\n", 1);
    }
    return off;
}

bool parseMasterList(const char* text, size_t len, MasterList* ml) {
    initMasterList(ml);
    const char* p   = text;
    const char* end = text + len;

    while (p < end && ml->count < MASTER_MAX_ENTRIES) {
        size_t llen = lineLen(p, end);
        const char* line = p;
        const char* nl = (const char*)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
        if (llen == 0) continue;

        // Split on the first two colons: name : file : notes(remainder).
        const char* c1 = (const char*)memchr(line, ':', llen);
        if (!c1) continue;                       // no colon — not a catalog line
        size_t rem = llen - (c1 + 1 - line);
        const char* c2 = (const char*)memchr(c1 + 1, ':', rem);

        MasterEntry& e = ml->entries[ml->count];
        copyField(line, c1 - line, e.name, SETLIST_SONG_NAME_LEN);
        if (e.name[0] == '\0') continue;
        if (c2) {
            copyField(c1 + 1, c2 - (c1 + 1), e.file, SETLIST_FILE_LEN);
            copyField(c2 + 1, (line + llen) - (c2 + 1), e.notes, SETLIST_NOTES_LEN);
        } else {
            copyField(c1 + 1, (line + llen) - (c1 + 1), e.file, SETLIST_FILE_LEN);
            e.notes[0] = '\0';
        }
        ml->count++;
    }
    return true;
}

// ── Setlists ──────────────────────────────────────────────────────────────────

void buildSetlistFilename(uint8_t slot, char* out, size_t outLen) {
    snprintf(out, outLen, "setlist%u.set", (unsigned)slot);
}

void initSetlist(Setlist* sl) {
    sl->count = 0;
}

int setlistFind(const Setlist* sl, const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < (int)sl->count; i++) {
        if (strcasecmp(sl->names[i], name) == 0) return i;
    }
    return -1;
}

bool setlistAppend(Setlist* sl, const char* name) {
    if (sl->count >= SETLIST_MAX_SONGS) return false;
    strncpy(sl->names[sl->count], name, SETLIST_SONG_NAME_LEN - 1);
    sl->names[sl->count][SETLIST_SONG_NAME_LEN - 1] = '\0';
    sl->count++;
    return true;
}

void setlistRemoveAt(Setlist* sl, int idx) {
    if (idx < 0 || idx >= (int)sl->count) return;
    for (int i = idx; i + 1 < (int)sl->count; i++)
        memcpy(sl->names[i], sl->names[i + 1], SETLIST_SONG_NAME_LEN);
    sl->count--;
    sl->names[sl->count][0] = '\0';
}

void setlistMoveAfter(Setlist* sl, int srcIdx, int afterIdx) {
    int n = (int)sl->count;
    if (srcIdx < 0 || srcIdx >= n) return;
    if (afterIdx < 0 || afterIdx >= n) return;
    if (srcIdx == afterIdx) return;

    char tmp[SETLIST_SONG_NAME_LEN];
    memcpy(tmp, sl->names[srcIdx], SETLIST_SONG_NAME_LEN);
    if (srcIdx < afterIdx) {
        // Moving down: close the gap, drop into afterIdx (now right after target).
        for (int i = srcIdx; i < afterIdx; i++)
            memcpy(sl->names[i], sl->names[i + 1], SETLIST_SONG_NAME_LEN);
        memcpy(sl->names[afterIdx], tmp, SETLIST_SONG_NAME_LEN);
    } else {
        // Moving up: open a gap just after the target and drop in.
        for (int i = srcIdx; i > afterIdx + 1; i--)
            memcpy(sl->names[i], sl->names[i - 1], SETLIST_SONG_NAME_LEN);
        memcpy(sl->names[afterIdx + 1], tmp, SETLIST_SONG_NAME_LEN);
    }
}

size_t serializeSetlist(const Setlist* sl, char* buf, size_t cap) {
    size_t off = 0;
    auto append = [&](const char* s, size_t n) {
        if (off + n <= cap) { memcpy(buf + off, s, n); off += n; }
    };
    int n = sl->count > SETLIST_MAX_SONGS ? SETLIST_MAX_SONGS : sl->count;
    for (int i = 0; i < n; i++) {
        if (sl->names[i][0] == '\0') continue;
        append(sl->names[i], strlen(sl->names[i]));
        append("\n", 1);
    }
    return off;
}

bool parseSetlist(const char* text, size_t len, Setlist* sl) {
    initSetlist(sl);
    const char* p   = text;
    const char* end = text + len;
    while (p < end && sl->count < SETLIST_MAX_SONGS) {
        size_t llen = lineLen(p, end);
        const char* line = p;
        const char* nl = (const char*)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
        if (llen == 0) continue;
        copyField(line, llen, sl->names[sl->count], SETLIST_SONG_NAME_LEN);
        if (sl->names[sl->count][0] != '\0') sl->count++;
    }
    return true;
}

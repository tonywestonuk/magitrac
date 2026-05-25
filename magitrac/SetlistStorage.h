#pragma once
#include <Arduino.h>

// ── Setlist storage — plain-text files on SD ─────────────────────────────────
//
// Files live at /setlists/setlist1.set ... /setlists/setlist4.set.
//
//   MAGITRAC_SETLIST v1
//   NAME=My Live Set 1
//   SONG
//   NAME=Tainted Love
//   FILE=TAINTED
//   NOTES=Key D, last verse twice
//   END
//   SONG
//   ...
//
// Each field value runs to end-of-line; everything after the first '='
// is the value (so '=' inside notes is fine).

#define SETLIST_COUNT          4
#define SETLIST_MAX_SONGS      50
#define SETLIST_NAME_LEN       24
#define SETLIST_SONG_NAME_LEN  32
#define SETLIST_FILE_LEN       24
#define SETLIST_NOTES_LEN      200

extern const char* SETLISTS_DIR;

struct SetlistEntry {
    char name [SETLIST_SONG_NAME_LEN];
    char file [SETLIST_FILE_LEN];
    char notes[SETLIST_NOTES_LEN];
};

struct Setlist {
    char         name[SETLIST_NAME_LEN];
    uint8_t      count;
    SetlistEntry songs[SETLIST_MAX_SONGS];
};

// slot is 1-based (1..SETLIST_COUNT).
void buildSetlistPath(uint8_t slot, char* out, size_t outLen);

void initSetlist(Setlist* sl, uint8_t slot);

// Returns true on success.  Leaves *sl untouched on failure.
bool loadSetlist(uint8_t slot, Setlist* sl);

bool saveSetlist(uint8_t slot, const Setlist* sl);

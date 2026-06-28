#pragma once
#include <Arduino.h>

// ── Setlist storage — master catalog + name-only setlists (server SD) ─────────
//
// Two things live under /setlists/ on the *server* (the client is server-only —
// see project_client_server_only):
//
//   master.txt        the catalog: every song that could appear in a setlist.
//                     One line per song, colon-separated, human-readable:
//                         Baker Street:BAKERST:Remember to start on the A chord
//                     fields = Name : File : Notes.  Name & File must not contain
//                     a ':'; Notes is the remainder of the line so it may.
//
//   setlist1.set      a setlist: just the song NAMES, one per line, in play order:
//   ...                   Baker Street
//   setlist4.set          Hungry Like a Wolf
//                     Each name references a master.txt entry (resolved at
//                     display time for the file + notes).  No header, no name —
//                     the four setlists are simply "Setlist 1".."Setlist 4".
//
// The client serialises/parses these text buffers here and ships them over
// MagiLink via the generic file server (FK_SETLISTS); this module never touches
// the SD card itself.

#define SETLIST_COUNT          4
#define SETLIST_MAX_SONGS      50
#define SETLIST_SONG_NAME_LEN  32     // display name — the key setlists reference
#define SETLIST_FILE_LEN       24     // server song filename (bare, no .mgt)
#define SETLIST_NOTES_LEN      200

#define MASTER_MAX_ENTRIES     80

// Worst-case serialised sizes — size static buffers from these.
//   master : 80 × (32 + 24 + 200 + 2 colons + nl) ≈ 80 × 260 ≈ 20.8 KB
//   setlist: 50 × (32 + nl) ≈ 1.7 KB
#define MASTER_SERIALIZE_MAX   24576
#define SETLIST_SERIALIZE_MAX  4096

// One catalog entry.
struct MasterEntry {
    char name [SETLIST_SONG_NAME_LEN];
    char file [SETLIST_FILE_LEN];
    char notes[SETLIST_NOTES_LEN];
};

// The single master catalog (server file /setlists/master.txt).
struct MasterList {
    uint16_t    count;
    MasterEntry entries[MASTER_MAX_ENTRIES];
};

// A setlist — an ordered list of names referencing the master catalog.
struct Setlist {
    uint8_t count;
    char    names[SETLIST_MAX_SONGS][SETLIST_SONG_NAME_LEN];
};

// ── Master catalog ────────────────────────────────────────────────────────────
const char* masterListFilename();      // "master.txt" (no directory)
void   initMasterList(MasterList* ml);
size_t serializeMasterList(const MasterList* ml, char* buf, size_t cap);
bool   parseMasterList(const char* text, size_t len, MasterList* ml);
// Find an entry by exact name (case-insensitive); returns index or -1.
int    masterFindByName(const MasterList* ml, const char* name);

// ── Setlists ──────────────────────────────────────────────────────────────────
void   buildSetlistFilename(uint8_t slot, char* out, size_t outLen);  // "setlistN.set", slot 1-based
void   initSetlist(Setlist* sl);
size_t serializeSetlist(const Setlist* sl, char* buf, size_t cap);
bool   parseSetlist(const char* text, size_t len, Setlist* sl);
// Index of `name` in the setlist (case-insensitive) or -1.
int    setlistFind(const Setlist* sl, const char* name);
bool   setlistAppend(Setlist* sl, const char* name);   // false if full
void   setlistRemoveAt(Setlist* sl, int idx);
// Move the entry at srcIdx to immediately after afterIdx (array shift+insert).
void   setlistMoveAfter(Setlist* sl, int srcIdx, int afterIdx);

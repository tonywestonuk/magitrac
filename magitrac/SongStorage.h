#pragma once
#include <Arduino.h>
#include "TrackerData.h"

// ── SongStorage ───────────────────────────────────────────────────────────────
//
// Binary .mgt format:  [ SongFileHeader (8 bytes) ] [ Song (raw struct) ]
//
// Call SD.begin(SD_CS_PIN) in setup() before using any of these.
// All paths should be absolute, e.g. "/songs/MYSONG.mgt"

// Maximum files shown in the browser
#define STORAGE_MAX_FILES   32
#define STORAGE_FILENAME_MAX 24   // enough for long FAT names + null

// Save *song to path.  Overwrites if it exists.
// Returns true on success.
bool saveSong(const char* path, const Song* song);

// Load from path into *out.
// Returns false if file missing, magic mismatch, or version mismatch.
bool loadSong(const char* path, Song* out);

// Scan dir for .mgt files; fill names[][] (each up to STORAGE_FILENAME_MAX).
// Returns number of files found (capped at maxFiles).
int listSongs(const char* dir,
              char names[][STORAGE_FILENAME_MAX],
              int maxFiles);

// ── Instrument bank ───────────────────────────────────────────────────────────
//
// Stored as /instruments.mgt at the SD root — shared across all songs.
// Call initInstruments() to fill defaults, then loadInstruments() on boot.
// If loadInstruments() returns false (file missing / version mismatch),
// call saveInstruments() to write the defaults to SD.

#define INSTRUMENTS_PATH "/instruments.mgt"

void initInstruments(Instrument* instruments);
bool saveInstruments(const Instrument* instruments);
bool loadInstruments(Instrument* instruments);

#pragma once
#include <stdint.h>

// ── Drum-track GM mapping ────────────────────────────────────────────────────
// Maps 2-letter instrument codes from drum-patterns.com text files (CH, OH,
// CY, CB, CP, RS, HT, MT, LT, SD, BD, AC, GH) to GM percussion MIDI notes +
// default velocities.  Hardcoded defaults at boot; the server's
// /drumtracks/gm_map.txt can override them at runtime (fetched once per
// session by DrumTrackImportPage and parsed via parseFromText()).

#define DRUM_GM_ENTRIES 13
#define DRUM_GM_CODE_LEN 3   // 2 chars + null
#define DRUM_GM_NAME_LEN 12  // 11 chars + null — matches INSTRUMENT_NAME_LEN

struct DrumGmEntry {
    char    code[DRUM_GM_CODE_LEN];   // e.g. "BD"
    uint8_t midiNote;                 // 0-127
    uint8_t velocity;                 // 0-127, default hit velocity
    char    name[DRUM_GM_NAME_LEN];   // display name written into ColumnSettings on import
};

class DrumGmMap {
public:
    DrumGmMap();   // initialises to hardcoded defaults

    // Parse the text format produced by writeDefaultGmMap() on the server.
    // Lines starting with '#' are comments; blank lines ignored.  Each
    // data line is `CODE midi_note velocity`.  Unrecognised codes are
    // skipped (defaults retained).  Returns true if at least one entry
    // was overridden.
    bool parseFromText(const char* text, uint32_t len);

    // Returns nullptr if no entry for this code.
    const DrumGmEntry* find(const char* code) const;

    // Iterate by index (0..DRUM_GM_ENTRIES-1).
    const DrumGmEntry* entry(int i) const {
        return (i >= 0 && i < DRUM_GM_ENTRIES) ? &_entries[i] : nullptr;
    }

private:
    DrumGmEntry _entries[DRUM_GM_ENTRIES];
};

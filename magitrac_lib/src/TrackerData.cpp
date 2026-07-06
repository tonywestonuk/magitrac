#include "TrackerData.h"
#include <string.h>

// ── Note name table ───────────────────────────────────────────────────────────

static const char NOTE_NAMES[12][3] = {
    "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
};

static const char HEX_CHARS[] = "0123456789ABCDEF";

// ── Helper implementations ────────────────────────────────────────────────────

void noteToString(uint8_t note, char* buf) {
    if (note == NOTE_OFF) {
        buf[0] = 'O'; buf[1] = 'F'; buf[2] = 'F'; buf[3] = '\0';
        return;
    }
    if (note == NOTE_ANY) {
        buf[0] = 'A'; buf[1] = 'N'; buf[2] = 'Y'; buf[3] = '\0';
        return;
    }
    if (note == NOTE_EMPTY || note > NOTE_MAX) {
        buf[0] = '-'; buf[1] = '-'; buf[2] = '-'; buf[3] = '\0';
        return;
    }
    int n        = note - 1;
    int semitone = n % 12;
    int octave   = n / 12;
    buf[0] = NOTE_NAMES[semitone][0];
    buf[1] = NOTE_NAMES[semitone][1];
    buf[2] = '0' + octave;
    buf[3] = '\0';
}

void effectToString(uint8_t effect, uint8_t param, char* buf) {
    if (effect == 0 && param == 0) {
        buf[0] = '-'; buf[1] = '-'; buf[2] = '-'; buf[3] = '\0';
        return;
    }
    buf[0] = HEX_CHARS[(effect >> 4) & 0xF];
    buf[1] = HEX_CHARS[effect & 0xF];
    buf[2] = HEX_CHARS[(param >> 4) & 0xF];
    buf[3] = HEX_CHARS[param & 0xF];
    buf[4] = '\0';
}

void velToString(uint8_t velocity, char* buf) {
    if (velocity & 0x80) {
        buf[0] = '-'; buf[1] = '-'; buf[2] = '\0';
    } else {
        buf[0] = HEX_CHARS[(velocity >> 4) & 0xF];
        buf[1] = HEX_CHARS[velocity & 0xF];
        buf[2] = '\0';
    }
}

void effectToString5(uint8_t effect, uint8_t param, char* buf) {
    if (effect == 0 && param == 0) {
        buf[0] = '-'; buf[1] = '-'; buf[2] = '-'; buf[3] = '-'; buf[4] = '\0';
        return;
    }
    buf[0] = HEX_CHARS[(effect >> 4) & 0xF];
    buf[1] = HEX_CHARS[effect & 0xF];
    buf[2] = HEX_CHARS[(param >> 4) & 0xF];
    buf[3] = HEX_CHARS[param & 0xF];
    buf[4] = '\0';
}

void col0MetaToString(const Note& n, char* buf) {
    if (n.effect == EFFECT_WAIT) {
        buf[0]='W'; buf[1]='A'; buf[2]='I'; buf[3]='T'; buf[4]='\0';
    } else if (n.effect == EFFECT_WAT1) {
        buf[0]='W'; buf[1]='A'; buf[2]='T'; buf[3]='1'; buf[4]='\0';
    } else if (n.effect == EFFECT_WAT2) {
        buf[0]='W'; buf[1]='A'; buf[2]='T'; buf[3]='2'; buf[4]='\0';
    } else if (n.effect == EFFECT_SYNC) {
        buf[0]='S'; buf[1]='Y'; buf[2]='N'; buf[3]='C'; buf[4]='\0';
    } else if (n.effect == EFFECT_AVRG) {
        buf[0]='A'; buf[1]='V'; buf[2]='R'; buf[3]='G'; buf[4]='\0';
    } else if (n.note != NOTE_EMPTY && n.note != NOTE_OFF) {
        // A present col-0 note with no cue effect is a PASS marker (plain
        // PASS = effect 0; PASA = effect 0x12).
        if (n.effect == EFFECT_PASA) {
            buf[0]='P'; buf[1]='A'; buf[2]='S'; buf[3]='A'; buf[4]='\0';
        } else {
            buf[0]='P'; buf[1]='A'; buf[2]='S'; buf[3]='S'; buf[4]='\0';
        }
    } else {
        buf[0]=' '; buf[1]=' '; buf[2]=' '; buf[3]=' '; buf[4]='\0';
    }
}

// ── Demo song — transposing major arpeggio ────────────────────────────────────
//
// Single 8-row pattern.  Column 1 plays C-E-G-C (major arpeggio).
// Column 0 (input track) has C-4 WAIT on row 0 — sequencer halts until the
// performer plays a note.  Each pitch class in inputNotes sets seqTranspose to
// its own semitone offset (C=0, C#=1, D=2, … B=11), so whatever note the
// performer hits becomes the new root: C plays C major, D plays D major, etc.
// On WAIT timeout (500 ms) the pattern restarts from row 0.

void initSong(Song* song) {
    memset(song, 0, sizeof(Song));

    // Build free list: chain all nodes together, tail sentinel = NOTE_NULL
    for (uint16_t i = 0; i < MAX_SONG_NOTES - 1; i++)
        song->notePool[i].next = i + 1;
    song->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    song->noteFreeHead = 0;

    // All patterns start empty
    for (int i = 0; i < MAX_PATTERNS; i++)
        song->patterns[i].noteHead = NOTE_NULL;

    song->bpm          = 120;
    song->minBPM       = 60;
    song->maxBPM       = 240;
    song->speed        = 6;
    song->numPatterns  = 1;
    song->startPattern = 0;
    song->midiInChannel   = 0;  // ANY
    song->midiInNoteMin   = 0;
    song->midiInNoteMax   = 127;
    song->transposeChMask = 0xFDFF;  // all 16 channels except ch 10 (drums)

    song->patterns[0].length = 16;

    // Default column volume = 100 (per-song, shared across all patterns)
    for (int c = 0; c < MAX_COLUMNS; c++)
        song->columns[c].volume = 100;
}

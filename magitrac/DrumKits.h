#pragma once
#include <stdint.h>

// ── SAM2695 drum kits ────────────────────────────────────────────────────────
// On MIDI channel 10 the synth selects a drum kit via a bare Program Change.
// The SAM2695 only populates these GS / GM2 kit slots (program is 0-based; the
// PC value shown 1-based in the comment); every other program number is silent.
// Shared by DrumTrackImportPage (audition + import) and ColumnEditor (per-column
// KIT selector), so the available kits live in exactly one place.

#define DRUM_MIDI_CHANNEL 10   // GM percussion (1-based)

struct DrumKit { uint8_t program; const char* name; };

static const DrumKit DRUM_KITS[] = {
    {0,   "Standard"},   // PC 1
    {16,  "Power"},      // PC 17
    {40,  "Brush"},      // PC 41
    {48,  "Orchestra"},  // PC 49
    {127, "CM-64"},      // PC 128
};
static const int DRUM_KIT_COUNT = (int)(sizeof(DRUM_KITS) / sizeof(DRUM_KITS[0]));

// Index of the kit whose program == p, or -1 if p isn't a kit slot.
static inline int drumKitIndexForProgram(uint8_t p) {
    for (int i = 0; i < DRUM_KIT_COUNT; i++)
        if (DRUM_KITS[i].program == p) return i;
    return -1;
}

// Index of the kit slot closest to program p (used to snap an arbitrary program
// onto a valid kit when a column is switched to the drum channel).
static inline int drumKitNearestIndex(uint8_t p) {
    int best = 0, bestDist = 256;
    for (int i = 0; i < DRUM_KIT_COUNT; i++) {
        int d = (int)p - (int)DRUM_KITS[i].program;
        if (d < 0) d = -d;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

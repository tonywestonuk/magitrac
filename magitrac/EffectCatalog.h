#pragma once
#include <Arduino.h>

// ── Output-column note-effect catalogue ──────────────────────────────────────
// Human-readable names for the "command" effects the server sequencer acts on
// for MIDI output columns (channel 1..16).  Drives the NoteEditor CHOOSE picker
// and the attribute-cell description.  The actual behaviour lives server-side
// in magitrac_server_s3/midi_player.cpp — keep this table in sync with it.
//
// param semantics for the value-taking commands:
//   Set Tempo  A0  param = BPM        (clamped to song min/max on the server)
//   Retrigger  1B  param = hit count  (2..8)
//   Pan        0A  param = position   (00 left … 40 centre … 7F right; MIDI CC 10)

struct EffectDef {
    uint8_t     effect;      // effect byte
    uint8_t     fixedParam;  // param written alongside (STOP needs 0xFF)
    bool        paramFixed;  // true = command owns the param; no value to enter
    const char* name;        // "Set Tempo"
};

static const EffectDef EFFECT_CATALOG[] = {
    { 0x00, 0x00, true,  "None"      },
    { 0xA0, 0x00, false, "Set Tempo" },
    { 0x1B, 0x00, false, "Retrigger" },
    { 0x0A, 0x00, false, "Pan"       },
    { 0xFF, 0xFF, true,  "Stop Song" },
};
static const int EFFECT_CATALOG_LEN =
    (int)(sizeof(EFFECT_CATALOG) / sizeof(EFFECT_CATALOG[0]));

// Description for an (effect,param) pair, or nullptr if not catalogued.
// 0xFF is "Stop Song" only when param is also 0xFF; 0x00 is "None" only when
// param is 0 (a 00xx with a non-zero param is a raw/unknown attribute, shown
// as hex).
inline const char* effectCatalogName(uint8_t effect, uint8_t param) {
    for (int i = 0; i < EFFECT_CATALOG_LEN; i++) {
        if (EFFECT_CATALOG[i].effect != effect) continue;
        if (EFFECT_CATALOG[i].paramFixed && EFFECT_CATALOG[i].fixedParam != param)
            continue;
        return EFFECT_CATALOG[i].name;
    }
    return nullptr;
}

#include "DrumGmMap.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

DrumGmMap::DrumGmMap() {
    // Defaults match the server's writeDefaultGmMap() output.  Velocity 100
    // for a normal hit, 40 for GH (ghost note).  AC routed to GM 31 (Sticks)
    // because it's the least bad guess for an unspecified accent.
    const DrumGmEntry defaults[DRUM_GM_ENTRIES] = {
        { "CH", 42, 100, "Closed Hat" },
        { "OH", 46, 100, "Open Hat"   },
        { "CY", 49, 100, "Cymbal"     },
        { "CB", 56, 100, "Cowbell"    },
        { "CP", 39, 100, "Clap"       },
        { "RS", 37, 100, "Rim Shot"   },
        { "HT", 50, 100, "High Tom"   },
        { "MT", 47, 100, "Mid Tom"    },
        { "LT", 41, 100, "Low Tom"    },
        { "SD", 38, 100, "Snare Drum" },
        { "BD", 36, 100, "Bass Drum"  },
        { "AC", 31, 100, "Accent"     },
        { "GH", 38, 40 , "Ghost"      },
    };
    memcpy(_entries, defaults, sizeof(defaults));
}

bool DrumGmMap::parseFromText(const char* text, uint32_t len) {
    if (!text || len == 0) return false;
    bool changed = false;
    uint32_t i = 0;
    while (i < len) {
        // Skip leading whitespace within this line.
        while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
        // Skip blank lines and comments.
        if (i >= len || text[i] == '\n' || text[i] == '\r' || text[i] == '#') {
            while (i < len && text[i] != '\n') i++;
            if (i < len) i++;
            continue;
        }
        // CODE — up to 2 alpha chars.
        char code[DRUM_GM_CODE_LEN] = {};
        int ci = 0;
        while (i < len && ci < DRUM_GM_CODE_LEN - 1 && isalpha((unsigned char)text[i])) {
            code[ci++] = (char)toupper((unsigned char)text[i]);
            i++;
        }
        code[ci] = '\0';
        // Whitespace.
        while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
        // midiNote.
        char numbuf[8] = {};
        int ni = 0;
        while (i < len && ni < (int)sizeof(numbuf) - 1 && isdigit((unsigned char)text[i])) {
            numbuf[ni++] = text[i++];
        }
        numbuf[ni] = '\0';
        int midiNote = atoi(numbuf);
        // Whitespace.
        while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
        // velocity.
        ni = 0;
        memset(numbuf, 0, sizeof(numbuf));
        while (i < len && ni < (int)sizeof(numbuf) - 1 && isdigit((unsigned char)text[i])) {
            numbuf[ni++] = text[i++];
        }
        numbuf[ni] = '\0';
        int velocity = atoi(numbuf);
        // Apply if the code matches one of our slots and values are sane.
        if (ci == 2 && midiNote >= 0 && midiNote <= 127
                    && velocity >  0 && velocity <= 127) {
            for (int k = 0; k < DRUM_GM_ENTRIES; k++) {
                if (strcmp(_entries[k].code, code) == 0) {
                    _entries[k].midiNote = (uint8_t)midiNote;
                    _entries[k].velocity = (uint8_t)velocity;
                    changed = true;
                    break;
                }
            }
        }
        // Skip to end of line.
        while (i < len && text[i] != '\n') i++;
        if (i < len) i++;
    }
    return changed;
}

const DrumGmEntry* DrumGmMap::find(const char* code) const {
    if (!code) return nullptr;
    for (int k = 0; k < DRUM_GM_ENTRIES; k++) {
        if (strcasecmp(_entries[k].code, code) == 0) return &_entries[k];
    }
    return nullptr;
}

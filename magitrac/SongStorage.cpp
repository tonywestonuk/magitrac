#include "SongStorage.h"
#include "SongMigration.h"
#include <SD.h>
#include <string.h>
#include <stdio.h>

// ── Instrument bank file header ───────────────────────────────────────────────

#define INSTR_FILE_MAGIC   0x494E5354UL  // "INST"
#define INSTR_FILE_VERSION 2

struct InstrFileHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  _pad[3];
};

bool saveSong(const char* path, const Song* song) {
    if (SD.exists(path)) SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[SD] saveSong open failed: %s\n", path);
        return false;
    }
    bool ok = songWriteCompact(f, song);
    uint32_t sz = (uint32_t)f.size();
    f.close();
    Serial.printf("[SD] saveSong %s: %s (%u bytes)\n", path, ok ? "OK" : "FAIL", sz);
    return ok;
}

bool loadSong(const char* path, Song* out) {
    File f = SD.open(path);
    if (!f) {
        Serial.printf("[SD] loadSong open failed: %s\n", path);
        return false;
    }

    SongFileHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        Serial.println("[SD] loadSong: header read short");
        f.close();
        return false;
    }
    if (hdr.magic != SONG_FILE_MAGIC) {
        Serial.printf("[SD] loadSong: bad magic 0x%08X\n", (unsigned)hdr.magic);
        f.close();
        return false;
    }
    bool ok = false;
    if (hdr.version == 11) {
        ok = songMigrateV11FromFile(f, out);
        Serial.printf("[SD] loadSong %s: migrated v11->v%d %s\n", path, SONG_FILE_VERSION, ok ? "OK" : "FAIL");
    } else if (hdr.version == 13) {
        ok = songMigrateV13FromFile(f, out);
        Serial.printf("[SD] loadSong %s: migrated v13->v%d %s\n", path, SONG_FILE_VERSION, ok ? "OK" : "FAIL");
    } else if (hdr.version == 14) {
        ok = songMigrateV14FromFile(f, out);
        Serial.printf("[SD] loadSong %s: migrated v14->v%d %s\n", path, SONG_FILE_VERSION, ok ? "OK" : "FAIL");
    } else if (hdr.version == 15) {
        ok = songMigrateV15FromFile(f, out);
        Serial.printf("[SD] loadSong %s: migrated v15->v%d %s\n", path, SONG_FILE_VERSION, ok ? "OK" : "FAIL");
    } else if (hdr.version == 16) {
        ok = songMigrateV16FromFile(f, out);
        Serial.printf("[SD] loadSong %s: migrated v16->v%d %s\n", path, SONG_FILE_VERSION, ok ? "OK" : "FAIL");
    } else if (hdr.version == 17) {
        ok = songMigrateV17FromFile(f, out);
        Serial.printf("[SD] loadSong %s: migrated v17->v%d %s\n", path, SONG_FILE_VERSION, ok ? "OK" : "FAIL");
    } else if (hdr.version == 18) {
        ok = songMigrateV18FromFile(f, out);
        Serial.printf("[SD] loadSong %s: migrated v18->v%d %s\n", path, SONG_FILE_VERSION, ok ? "OK" : "FAIL");
    } else if (hdr.version == SONG_FILE_VERSION) {
        ok = songReadCompact(f, out);
        Serial.printf("[SD] loadSong %s: %s\n", path, ok ? "OK" : "FAIL");
    } else {
        Serial.printf("[SD] loadSong: version %d unsupported\n", hdr.version);
    }
    f.close();
    return ok;
}

// ── Instrument bank ───────────────────────────────────────────────────────────

void initInstruments(Instrument* instruments) {
    for (int i = 0; i < MAX_INSTRUMENTS; i++) {
        Instrument& inst = instruments[i];
        memset(&inst, 0, sizeof(inst));
        snprintf(inst.name, INSTRUMENT_NAME_LEN, "INST %02X", i);
        inst.bankMSB     = 0;
        inst.program     = 0;
        inst.volume      = 100;
        inst.transpose   = 0;
    }
}

bool saveInstruments(const Instrument* instruments) {
    if (SD.exists(INSTRUMENTS_PATH)) SD.remove(INSTRUMENTS_PATH);
    File f = SD.open(INSTRUMENTS_PATH, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] saveInstruments: open failed");
        return false;
    }
    InstrFileHeader hdr;
    hdr.magic      = INSTR_FILE_MAGIC;
    hdr.version    = INSTR_FILE_VERSION;
    hdr._pad[0] = hdr._pad[1] = hdr._pad[2] = 0;

    size_t w = 0;
    w += f.write((const uint8_t*)&hdr, sizeof(hdr));
    w += f.write((const uint8_t*)instruments, sizeof(Instrument) * MAX_INSTRUMENTS);
    f.close();

    bool ok = (w == sizeof(hdr) + sizeof(Instrument) * MAX_INSTRUMENTS);
    Serial.printf("[SD] saveInstruments: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool loadInstruments(Instrument* instruments) {
    File f = SD.open(INSTRUMENTS_PATH);
    if (!f) {
        Serial.println("[SD] loadInstruments: file not found");
        return false;
    }
    InstrFileHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        hdr.magic   != INSTR_FILE_MAGIC ||
        hdr.version != INSTR_FILE_VERSION) {
        Serial.println("[SD] loadInstruments: bad header");
        f.close();
        return false;
    }
    bool ok = (f.read((uint8_t*)instruments,
                      sizeof(Instrument) * MAX_INSTRUMENTS)
               == (int)(sizeof(Instrument) * MAX_INSTRUMENTS));
    f.close();
    // (no sanitization needed — instruments no longer carry midiChannel)
    Serial.printf("[SD] loadInstruments: %s\n", ok ? "OK" : "short read");
    return ok;
}

int listSongs(const char* dir,
              char names[][STORAGE_FILENAME_MAX],
              int maxFiles) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) {
        Serial.printf("[SD] listSongs: cannot open dir '%s'\n", dir);
        return 0;
    }

    int count = 0;
    while (count < maxFiles) {
        File entry = d.openNextFile();
        if (!entry) break;

        if (!entry.isDirectory()) {
            const char* fullname = entry.name();
            // Strip directory prefix if the library returns full paths
            const char* slash = strrchr(fullname, '/');
            const char* fname = slash ? slash + 1 : fullname;
            int len = (int)strlen(fname);
            if (len >= 4) {
                const char* ext = fname + len - 4;
                if (ext[0] == '.' &&
                    (ext[1] == 'm' || ext[1] == 'M') &&
                    (ext[2] == 'g' || ext[2] == 'G') &&
                    (ext[3] == 't' || ext[3] == 'T')) {
                    strncpy(names[count], fname, STORAGE_FILENAME_MAX - 1);
                    names[count][STORAGE_FILENAME_MAX - 1] = '\0';
                    count++;
                }
            }
        }
        entry.close();
    }
    d.close();
    Serial.printf("[SD] listSongs '%s': %d files\n", dir, count);
    return count;
}

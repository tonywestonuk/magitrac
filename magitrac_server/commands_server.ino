// commands_server.ino — handles song commands from client
// Depends on: MagiComms (gComms), MagiMsg.h

#include <SD.h>
#include <WiFi.h>
#include <Preferences.h>
#include "TrackerData.h"
#include "NoteGrid.h"
#include "SongMigration.h"
#include "midi_player.h"
#include "SampleManifest.h"

// ── WiFi channel persistence ──────────────────────────────────────────────────
// Shared namespace name with the client side ("magitrac_wifi") so the channel
// setting is conceptually one thing across both devices, just stored locally.
static const char* WIFI_NVS_NS = "magitrac_wifi";
static uint8_t     sWifiChannelIdx = 0;   // 0/1/2 → 1/6/11

void wifiChannelInit() {
    Preferences prefs;
    prefs.begin(WIFI_NVS_NS, true);
    sWifiChannelIdx = prefs.getUChar("idx", 0);
    if (sWifiChannelIdx > 2) sWifiChannelIdx = 0;
    prefs.end();
    uint8_t ch = magiWifiChannelFromIdx(sWifiChannelIdx);
    WiFi.setChannel(ch);
    Serial.printf("[WIFI] boot channel: %u\n", (unsigned)ch);
}

static void wifiChannelApply(uint8_t idx) {
    if (idx > 2) return;
    sWifiChannelIdx = idx;
    Preferences prefs;
    prefs.begin(WIFI_NVS_NS, false);
    prefs.putUChar("idx", idx);
    prefs.end();
    uint8_t ch = magiWifiChannelFromIdx(idx);
    WiFi.setChannel(ch);
    Serial.printf("[WIFI] channel → %u\n", (unsigned)ch);
}

extern HardwareSerial midi;

// M5Stack Core Basic SD card shares VSPI (SCLK=18, MISO=19, MOSI=23) with display
// SD CS is GPIO 4; display CS is GPIO 14 — different CS, same bus
#define SRV_SD_CS      4
#define SRV_SONGS_DIR  "/songs"

// Song layout constants
#define SRV_HDR_SIZE      8      // sizeof(SongFileHeader)
#define SRV_SONG_XFER_MAX (sizeof(SongFileHeader) + sizeof(Song) + 64)

static bool srvSdOk = false;

// ── Instruments ───────────────────────────────────────────────────────────────

#define SRV_INSTRUMENTS_PATH "/instruments.mgt"
#define INSTR_FILE_MAGIC     0x494E5354UL   // "INST"
#define INSTR_FILE_VERSION   2

struct InstrFileHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  _pad[3];
};

Instrument srvInstruments[MAX_INSTRUMENTS];

static void initDefaultInstruments() {
    for (int i = 0; i < MAX_INSTRUMENTS; i++) {
        Instrument& inst = srvInstruments[i];
        memset(&inst, 0, sizeof(inst));
        snprintf(inst.name, INSTRUMENT_NAME_LEN, "INST %02X", i);
        inst.bankMSB     = 0;
        inst.program     = (uint8_t)(i % 128);  // instrument ID = MIDI program ID
        inst.volume      = 100;
        inst.transpose   = 0;
    }
}

static bool loadInstrumentsFromSD() {
    File f = SD.open(SRV_INSTRUMENTS_PATH);
    if (!f) return false;
    InstrFileHeader hdr;
    bool ok = (f.read((uint8_t*)&hdr, sizeof(hdr)) == (int)sizeof(hdr)
               && hdr.magic   == INSTR_FILE_MAGIC
               && hdr.version == INSTR_FILE_VERSION);
    if (ok)
        ok = (f.read((uint8_t*)srvInstruments, sizeof(srvInstruments))
              == (int)sizeof(srvInstruments));
    f.close();
    // (no sanitization needed — instruments no longer carry midiChannel)
    Serial.printf("[CMD] loadInstruments: %s\n", ok ? "OK" : "FAIL/missing");
    return ok;
}

static void saveInstrumentsToSD() {
    if (SD.exists(SRV_INSTRUMENTS_PATH)) SD.remove(SRV_INSTRUMENTS_PATH);
    File f = SD.open(SRV_INSTRUMENTS_PATH, FILE_WRITE);
    if (!f) { Serial.println("[CMD] saveInstruments: open failed"); return; }
    InstrFileHeader hdr;
    hdr.magic = INSTR_FILE_MAGIC; hdr.version = INSTR_FILE_VERSION;
    hdr._pad[0] = hdr._pad[1] = hdr._pad[2] = 0;
    f.write((const uint8_t*)&hdr, sizeof(hdr));
    f.write((const uint8_t*)srvInstruments, sizeof(srvInstruments));
    f.close();
    Serial.println("[CMD] saveInstruments: OK");
}

static bool srvInstSavePending   = false;
static bool srvInstLoadPending   = false;

// ── Active song buffer ─────────────────────────────────────────────────────────
// Holds the last song sent to the client so MSG_NOTE_UPDATE can be applied in-memory
uint8_t  srvActiveBuf[SRV_SONG_XFER_MAX];
uint32_t srvActiveBufLen  = 0;
static char     srvActiveName[SRV_FNAME_MAX] = {};  // filename (with .mgt)
bool     srvHasActive     = false;

// ── Upload (save) — streamed to temp file on SD ──────────────────────────────
#define SRV_UPLOAD_TMP "/songs/_upload.tmp"
#define SRV_RESTORE_TMP "/songs/_restore.tmp"
static char     srvSaveName[SRV_FNAME_MAX] = {};  // destination name (no extension)
static uint8_t  srvSaveTotal   = 0;
static uint8_t  srvSaveGot     = 0;
static bool     srvSaveActive     = false;
static bool     srvFinaliseNeeded = false;  // set when all chunks received; SD write deferred to main loop
static File     srvSaveFile;               // kept open during chunk reception to avoid open/close per chunk
static char     srvDeleteName[SRV_FNAME_MAX] = {};  // name pending delete (deferred to main loop)
static bool     srvDeletePending  = false;

// ── Deferred song-load request ─────────────────────────────────────────────────
// sendSongData is blocking (~750ms); calling it from the ESP-NOW receive callback
// stalls the WiFi task. Set a flag here and do the send from the main loop instead.
static bool    srvLoadPending   = false;
static uint8_t srvLoadPendingPage   = 0;
static uint8_t srvLoadPendingIdx    = 0;

// ── Deferred backup file send ──────────────────────────────────────────────────
static bool    srvBackupFilePending = false;
static char    srvBackupFileName[SRV_FNAME_MAX] = {};

// ── Restore upload state ──────────────────────────────────────────────────────
static bool    srvRestoreActive          = false;
static bool    srvRestoreFinaliseNeeded  = false;
static bool    srvRestoreIsInstruments   = false;
static char    srvRestoreName[SRV_FNAME_MAX] = {};
static uint8_t srvRestoreTotal   = 0;
static uint8_t srvRestoreGot     = 0;
static File    srvRestoreFile;

void commandsInit() {
    srvSdOk = SD.begin(SRV_SD_CS);
    Serial.printf("[CMD] SD: %s\n", srvSdOk ? "OK" : "not found");
    if (srvSdOk && !SD.exists(SRV_SONGS_DIR))
        SD.mkdir(SRV_SONGS_DIR);

    initDefaultInstruments();
    if (srvSdOk && !loadInstrumentsFromSD()) {
        saveInstrumentsToSD();  // write defaults so future boots load them
        Serial.println("[CMD] instruments: created defaults");
    }
}

// ── List .mgt files in /songs ──────────────────────────────────────────────────
int srvListSongs(char names[][SRV_FNAME_MAX], int maxFiles) {
    if (!srvSdOk) return 0;
    File d = SD.open(SRV_SONGS_DIR);
    if (!d || !d.isDirectory()) return 0;
    int count = 0;
    while (count < maxFiles) {
        File e = d.openNextFile();
        if (!e) break;
        if (!e.isDirectory()) {
            const char* fn = e.name();
            const char* sl = strrchr(fn, '/');
            if (sl) fn = sl + 1;
            int len = (int)strlen(fn);
            if (len >= 4) {
                const char* ext = fn + len - 4;
                if (ext[0] == '.' && (ext[1]=='m'||ext[1]=='M') &&
                    (ext[2]=='g'||ext[2]=='G') && (ext[3]=='t'||ext[3]=='T')) {
                    strncpy(names[count], fn, SRV_FNAME_MAX - 1);
                    names[count][SRV_FNAME_MAX - 1] = '\0';
                    count++;
                }
            }
        }
        e.close();
    }
    d.close();
    return count;
}

// ── Send song list page to client ──────────────────────────────────────────────
static void sendSongList(uint8_t page) {
    char files[SRV_MAX_FILES][SRV_FNAME_MAX];
    int total = srvListSongs(files, SRV_MAX_FILES);

    int totalPages = total > 0 ? (total + SL_PER_PKT - 1) / SL_PER_PKT : 1;
    if (page >= totalPages) page = totalPages - 1;

    int start    = page * SL_PER_PKT;
    int inPage   = total - start;
    if (inPage > SL_PER_PKT) inPage = SL_PER_PKT;
    if (inPage < 0)          inPage = 0;

    MsgSongListResp resp;
    resp.type       = MSG_SONG_LIST_RESP;
    resp.page       = page;
    resp.totalPages = (uint8_t)totalPages;
    resp.count      = (uint8_t)inPage;
    memset(resp.names, 0, sizeof(resp.names));

    for (int i = 0; i < inPage; i++) {
        // Strip .mgt extension for display
        strncpy(resp.names[i], files[start + i], SL_NAME_LEN - 1);
        resp.names[i][SL_NAME_LEN - 1] = '\0';
        int len = (int)strlen(resp.names[i]);
        if (len > 4 && resp.names[i][len - 4] == '.') resp.names[i][len - 4] = '\0';
    }

    gComms.send(&resp, sizeof(resp));
}

// ── Send sample list page to client ────────────────────────────────────────────
// Each request runs a full manifest sync first: scans /samples/*.wav, appends
// any new files to /samples/samples.txt with the next free id, then pages the
// updated list out.  This means the picker on the client is always fresh
// without needing a separate "refresh samples" command.
static void sendSampleList(uint8_t page) {
    sampleManifestSync();
    int total = sampleManifestCount();

    int totalPages = total > 0 ? (total + SAMPLES_PER_PKT - 1) / SAMPLES_PER_PKT : 1;
    if (page >= totalPages) page = totalPages - 1;

    int start  = page * SAMPLES_PER_PKT;
    int inPage = total - start;
    if (inPage > SAMPLES_PER_PKT) inPage = SAMPLES_PER_PKT;
    if (inPage < 0)               inPage = 0;

    MsgSampleListResp resp;
    memset(&resp, 0, sizeof(resp));
    resp.type         = MSG_SAMPLE_LIST_RESP;
    resp.page         = page;
    resp.totalPages   = (uint8_t)totalPages;
    resp.count        = (uint8_t)inPage;
    resp.totalEntries = (uint8_t)(total > 255 ? 255 : total);

    for (int i = 0; i < inPage; i++) {
        const SmEntry* e = sampleManifestAt(start + i);
        if (!e) break;
        resp.entries[i].id = e->id;
        strncpy(resp.entries[i].name, e->name, SAMPLE_NAME_LEN - 1);
        resp.entries[i].name[SAMPLE_NAME_LEN - 1] = '\0';
    }

    gComms.send(&resp, sizeof(resp));
}

// ── Send song file in chunks ───────────────────────────────────────────────────
// Loads/migrates the song into srvActiveBuf first, then sends from memory.
// This handles legacy v7/v8/v9 files transparently — the client always
// receives a v10-format payload regardless of the on-disk format.
static void sendSongData(uint8_t page, uint8_t index) {
    char files[SRV_MAX_FILES][SRV_FNAME_MAX];
    int total = srvListSongs(files, SRV_MAX_FILES);

    int fileIdx = (int)page * SL_PER_PKT + (int)index;
    if (fileIdx < 0 || fileIdx >= total) return;

    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, files[fileIdx]);

    File f = SD.open(path);
    if (!f) {
        Serial.printf("[CMD] sendSongData: open failed '%s'\n", path);
        return;
    }

    srvHasActive    = false;
    srvActiveBufLen = 0;
    bool loaded = false;

    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    SongFileHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) == (int)sizeof(hdr)
        && hdr.magic == SONG_FILE_MAGIC) {

        if (hdr.version == SONG_FILE_VERSION) {
            loaded = songReadCompact(f, song);
        } else if (hdr.version == 16) {
            loaded = songMigrateV16FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v16->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 15) {
            loaded = songMigrateV15FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v15->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 14) {
            loaded = songMigrateV14FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v14->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 13) {
            loaded = songMigrateV13FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v13->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 11) {
            loaded = songMigrateV11FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v11->v%d '%s'\n", SONG_FILE_VERSION, path);
        }
    }
    f.close();

    if (!loaded) {
        Serial.printf("[CMD] sendSongData: load failed '%s'\n", path);
        return;
    }

    // Stamp the in-memory header for wire transfer
    SongFileHeader* h2 = (SongFileHeader*)srvActiveBuf;
    h2->magic = SONG_FILE_MAGIC;
    h2->version = SONG_FILE_VERSION;
    h2->_pad[0] = h2->_pad[1] = h2->_pad[2] = 0;
    srvActiveBufLen = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);

    strncpy(srvActiveName, files[fileIdx], sizeof(srvActiveName) - 1);
    srvActiveName[sizeof(srvActiveName) - 1] = '\0';
    srvHasActive = true;

    // Send from srvActiveBuf in chunks
    uint32_t totalBytes  = srvActiveBufLen;
    uint8_t  totalChunks = (uint8_t)((totalBytes + SONG_CHUNK_SIZE - 1) / SONG_CHUNK_SIZE);
    Serial.printf("[CMD] sending '%s' %u bytes %u chunks\n", path, totalBytes, totalChunks);

    MsgSongData msg;
    msg.type        = MSG_SONG_DATA;
    msg.totalChunks = totalChunks;

    for (uint8_t chunk = 0; chunk < totalChunks; chunk++) {
        uint32_t offset    = (uint32_t)chunk * SONG_CHUNK_SIZE;
        uint32_t remaining = totalBytes - offset;
        int n = (int)(remaining < (uint32_t)SONG_CHUNK_SIZE ? remaining : (uint32_t)SONG_CHUNK_SIZE);
        memcpy(msg.payload, srvActiveBuf + offset, n);
        msg.chunk   = chunk;
        msg.dataLen = (uint8_t)n;
        gComms.sendReliable(&msg, 4 + n, chunk);
    }
    Serial.println("[CMD] sendSongData done");
}

// ── Apply a generic song memory patch to the active song buffer ───────────────
static void applySongPatch(const MsgSetSongData* msg) {
    if (!srvHasActive) return;
    uint32_t start = SRV_HDR_SIZE + msg->offset;
    if (start + msg->length > srvActiveBufLen) return;
    memcpy(srvActiveBuf + start, msg->data, msg->length);

    // If bpm was touched, update the live sequencer BPM immediately
    const Song* s = reinterpret_cast<const Song*>(srvActiveBuf + SRV_HDR_SIZE);
    uint16_t bpmOff = (uint16_t)offsetof(Song, bpm);
    if (msg->offset <= bpmOff && msg->offset + msg->length > bpmOff + 1) {
        sequencerSetBPM(s->bpm);
    }

    // If performerMask was touched, send CC 115 immediately
    uint16_t pmOff = (uint16_t)offsetof(Song, performerMask);
    if (msg->offset <= pmOff && msg->offset + msg->length > pmOff) {
        seqSendSlotEnable();
    }
}

// ── Load song into active buffer without sending to client ────────────────────
// Returns true if the song was loaded successfully into srvActiveBuf.
bool srvLoadSongLocal(int listIdx) {
    char raw[SRV_MAX_FILES][SRV_FNAME_MAX];
    int total = srvListSongs(raw, SRV_MAX_FILES);
    if (listIdx < 0 || listIdx >= total) return false;

    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, raw[listIdx]);

    File f = SD.open(path);
    if (!f) {
        Serial.printf("[CMD] srvLoadSongLocal: open failed '%s'\n", path);
        return false;
    }

    srvHasActive    = false;
    srvActiveBufLen = 0;
    bool loaded = false;

    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    SongFileHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) == (int)sizeof(hdr)
        && hdr.magic == SONG_FILE_MAGIC) {

        if (hdr.version == SONG_FILE_VERSION) {
            loaded = songReadCompact(f, song);
        } else if (hdr.version == 16) {
            loaded = songMigrateV16FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v16->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 15) {
            loaded = songMigrateV15FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v15->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 14) {
            loaded = songMigrateV14FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v14->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 13) {
            loaded = songMigrateV13FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v13->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 11) {
            loaded = songMigrateV11FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v11->v%d '%s'\n", SONG_FILE_VERSION, path);
        }
    }
    f.close();

    if (loaded) {
        SongFileHeader* h2 = (SongFileHeader*)srvActiveBuf;
        h2->magic = SONG_FILE_MAGIC;
        h2->version = SONG_FILE_VERSION;
        h2->_pad[0] = h2->_pad[1] = h2->_pad[2] = 0;
        srvActiveBufLen = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);
        strncpy(srvActiveName, raw[listIdx], sizeof(srvActiveName) - 1);
        srvActiveName[sizeof(srvActiveName) - 1] = '\0';
        srvHasActive = true;
    }

    Serial.printf("[CMD] srvLoadSongLocal: '%s' %u bytes %s\n",
                  path, srvActiveBufLen, loaded ? "OK" : "FAIL");
    return loaded;
}

// ── Flush active song buffer back to SD ───────────────────────────────────────
static void flushActiveSong() {
    if (!srvHasActive || srvActiveName[0] == '\0') return;
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, srvActiveName);
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("[CMD] flush: open failed '%s'\n", path); return; }
    const Song* song = (const Song*)(srvActiveBuf + sizeof(SongFileHeader));
    bool ok = songWriteCompact(f, song);
    uint32_t sz = (uint32_t)f.size();
    f.close();
    Serial.printf("[CMD] flushed '%s' %u bytes %s\n", path, sz, ok ? "OK" : "FAIL");
}

// ── Write uploaded song chunks to SD ──────────────────────────────────────────
static void finaliseSongSave() {
    if (srvSaveFile) srvSaveFile.close();
    if (!srvSaveActive) return;

    // The temp file contains the raw [Header + Song] wire format.
    // Read into srvActiveBuf (which we need for live playback anyway),
    // then optionally write compact format to SD if a name was given.
    File tmp = SD.open(SRV_UPLOAD_TMP);
    if (!tmp) { Serial.printf("[CMD] save: tmp open failed\n"); srvSaveActive = false; return; }

    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    SongFileHeader hdr;
    bool loaded = false;
    if (tmp.read((uint8_t*)&hdr, sizeof(hdr)) == (int)sizeof(hdr)
        && hdr.magic == SONG_FILE_MAGIC) {
        loaded = (tmp.read((uint8_t*)song, sizeof(Song)) == (int)sizeof(Song));
    }
    tmp.close();
    SD.remove(SRV_UPLOAD_TMP);

    if (!loaded) {
        Serial.println("[CMD] save: tmp read failed");
        srvSaveActive = false;
        return;
    }

    // Write compact format to SD only if a name was provided
    if (srvSaveName[0] != '\0') {
        char path[48];
        snprintf(path, sizeof(path), "%s/%s.mgt", SRV_SONGS_DIR, srvSaveName);
        SD.remove(path);
        File f = SD.open(path, FILE_WRITE);
        if (!f) { Serial.printf("[CMD] save: open failed '%s'\n", path); srvSaveActive = false; return; }
        bool ok = songWriteCompact(f, song);
        uint32_t sz = (uint32_t)f.size();
        f.close();
        Serial.printf("[CMD] saved '%s' %u bytes %s\n", path, sz, ok ? "OK" : "FAIL");
        snprintf(srvActiveName, sizeof(srvActiveName), "%s.mgt", srvSaveName);
    } else {
        Serial.println("[CMD] song pushed (no SD save)");
    }

    // Stamp the in-memory header for wire transfer
    SongFileHeader* h2 = (SongFileHeader*)srvActiveBuf;
    h2->magic = SONG_FILE_MAGIC;
    h2->version = SONG_FILE_VERSION;
    h2->_pad[0] = h2->_pad[1] = h2->_pad[2] = 0;
    srvActiveBufLen = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);
    srvHasActive = true;

    srvSaveActive = false;
}

// ── Send full instruments array in chunks ─────────────────────────────────────
static void sendInstrumentsData() {
    const uint8_t* raw   = (const uint8_t*)srvInstruments;
    uint32_t       total = (uint32_t)sizeof(srvInstruments);
    uint8_t totalChunks  = (uint8_t)((total + SONG_CHUNK_SIZE - 1) / SONG_CHUNK_SIZE);

    MsgInstrumentsData chunk;
    chunk.type        = MSG_INSTRUMENTS_DATA;
    chunk.totalChunks = totalChunks;

    for (uint8_t i = 0; i < totalChunks; i++) {
        uint32_t offset    = (uint32_t)i * SONG_CHUNK_SIZE;
        uint32_t remaining = total - offset;
        chunk.chunk   = i;
        chunk.dataLen = (uint8_t)(remaining < SONG_CHUNK_SIZE ? remaining : SONG_CHUNK_SIZE);
        memcpy(chunk.payload, raw + offset, chunk.dataLen);
        gComms.sendReliable(&chunk, 4 + chunk.dataLen, i);
    }
    Serial.printf("[CMD] sendInstruments: %u chunks done\n", totalChunks);
}

// ── Backup: send file list ────────────────────────────────────────────────────
static void sendBackupFileList(uint8_t page) {
    if (!srvSdOk) return;

    // Build full file list: songs + instruments
    char names[SRV_MAX_FILES + 1][SRV_FNAME_MAX];
    uint32_t sizes[SRV_MAX_FILES + 1];
    int total = 0;

    // Song files
    char songNames[SRV_MAX_FILES][SRV_FNAME_MAX];
    int songCount = srvListSongs(songNames, SRV_MAX_FILES);
    for (int i = 0; i < songCount && total < SRV_MAX_FILES; i++) {
        strncpy(names[total], songNames[i], SRV_FNAME_MAX - 1);
        names[total][SRV_FNAME_MAX - 1] = '\0';
        char path[48];
        snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, songNames[i]);
        File f = SD.open(path);
        sizes[total] = f ? (uint32_t)f.size() : 0;
        if (f) f.close();
        total++;
    }

    // Instruments file
    if (SD.exists(SRV_INSTRUMENTS_PATH) && total < SRV_MAX_FILES + 1) {
        strncpy(names[total], "instruments.mgt", SRV_FNAME_MAX - 1);
        names[total][SRV_FNAME_MAX - 1] = '\0';
        File f = SD.open(SRV_INSTRUMENTS_PATH);
        sizes[total] = f ? (uint32_t)f.size() : 0;
        if (f) f.close();
        total++;
    }

    int totalPages = total > 0 ? (total + BK_PER_PKT - 1) / BK_PER_PKT : 1;
    if (page >= totalPages) page = totalPages - 1;
    int start   = page * BK_PER_PKT;
    int inPage  = total - start;
    if (inPage > BK_PER_PKT) inPage = BK_PER_PKT;
    if (inPage < 0) inPage = 0;

    MsgBackupListResp resp;
    resp.type       = MSG_BACKUP_LIST_RESP;
    resp.page       = page;
    resp.totalPages = (uint8_t)totalPages;
    resp.count      = (uint8_t)inPage;
    resp.totalFiles = (uint8_t)total;
    memset(resp.entries, 0, sizeof(resp.entries));
    for (int i = 0; i < inPage; i++) {
        strncpy(resp.entries[i].name, names[start + i], SRV_FNAME_MAX - 1);
        resp.entries[i].name[SRV_FNAME_MAX - 1] = '\0';
        resp.entries[i].sizeHi = (uint16_t)(sizes[start + i] >> 16);
        resp.entries[i].sizeLo = (uint16_t)(sizes[start + i] & 0xFFFF);
    }
    gComms.send(&resp, sizeof(resp));
    Serial.printf("[CMD] backupList page=%d count=%d total=%d\n", page, inPage, total);
}

// ── Backup: send raw file in chunks (deferred — called from commandsTick) ────
static void sendBackupFileRaw(const char* name) {
    if (!srvSdOk) return;
    char path[48];
    if (strcmp(name, "instruments.mgt") == 0) {
        strncpy(path, SRV_INSTRUMENTS_PATH, sizeof(path) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, name);
    }
    path[sizeof(path) - 1] = '\0';

    File f = SD.open(path);
    if (!f) {
        Serial.printf("[CMD] backupFile: open failed '%s'\n", path);
        return;
    }
    uint32_t fileSize = (uint32_t)f.size();
    uint8_t totalChunks = (uint8_t)((fileSize + SONG_CHUNK_SIZE - 1) / SONG_CHUNK_SIZE);

    // Send start header
    MsgBackupFileStart startMsg;
    startMsg.type        = MSG_BACKUP_FILE_START;
    strncpy(startMsg.name, name, SRV_FNAME_MAX - 1);
    startMsg.name[SRV_FNAME_MAX - 1] = '\0';
    startMsg.totalSize   = (uint16_t)fileSize;
    startMsg.totalChunks = totalChunks;
    gComms.send(&startMsg, sizeof(startMsg));
    delay(10);

    // Stream data chunks directly from file (no RAM buffer needed)
    MsgBackupFileData chunk;
    chunk.type        = MSG_BACKUP_FILE_DATA;
    chunk.totalChunks = totalChunks;
    for (uint8_t i = 0; i < totalChunks; i++) {
        uint32_t remaining = fileSize - (uint32_t)i * SONG_CHUNK_SIZE;
        chunk.chunk   = i;
        chunk.dataLen = (uint8_t)(remaining < SONG_CHUNK_SIZE ? remaining : SONG_CHUNK_SIZE);
        f.read(chunk.payload, chunk.dataLen);
        gComms.sendReliable(&chunk, 4 + chunk.dataLen, i);
    }
    f.close();
    Serial.printf("[CMD] backupFile '%s' %u bytes %u chunks done\n", name, fileSize, totalChunks);
}

// ── Reload the active song from SD into srvActiveBuf and push to client ──────
static void reloadActiveSongFromSD() {
    extern bool sSongPushPending;

    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, srvActiveName);

    File f = SD.open(path);
    if (!f) {
        Serial.printf("[CMD] reload: open failed '%s'\n", path);
        return;
    }

    srvHasActive    = false;
    srvActiveBufLen = 0;
    bool loaded = false;

    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    SongFileHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) == (int)sizeof(hdr)
        && hdr.magic == SONG_FILE_MAGIC) {

        if (hdr.version == SONG_FILE_VERSION) {
            loaded = songReadCompact(f, song);
        } else if (hdr.version == 16) {
            loaded = songMigrateV16FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v16->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 15) {
            loaded = songMigrateV15FromFile(f, song);
            if (loaded) Serial.printf("[CMD] migrated v15->v%d '%s'\n", SONG_FILE_VERSION, path);
        } else if (hdr.version == 14) {
            loaded = songMigrateV14FromFile(f, song);
        } else if (hdr.version == 13) {
            loaded = songMigrateV13FromFile(f, song);
        } else if (hdr.version == 11) {
            loaded = songMigrateV11FromFile(f, song);
        }
    }
    f.close();

    if (!loaded) {
        Serial.printf("[CMD] reload: load failed '%s'\n", path);
        return;
    }

    SongFileHeader* h2 = (SongFileHeader*)srvActiveBuf;
    h2->magic = SONG_FILE_MAGIC;
    h2->version = SONG_FILE_VERSION;
    h2->_pad[0] = h2->_pad[1] = h2->_pad[2] = 0;
    srvActiveBufLen = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);
    srvHasActive = true;
    Serial.printf("[CMD] reloaded active song '%s' %u bytes\n", path, srvActiveBufLen);

    // Push to client
    if (pairingIsConnected()) {
        sSongPushPending = true;
    }
}

// ── Restore: finalise uploaded file (deferred — called from commandsTick) ────
static void finaliseRestore() {
    if (srvRestoreFile) srvRestoreFile.close();
    if (!srvRestoreActive || srvRestoreName[0] == '\0') return;
    char path[48];
    if (srvRestoreIsInstruments) {
        strncpy(path, SRV_INSTRUMENTS_PATH, sizeof(path) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, srvRestoreName);
    }
    path[sizeof(path) - 1] = '\0';

    // Rename temp file to final destination
    if (SD.exists(path)) SD.remove(path);
    SD.rename(SRV_RESTORE_TMP, path);
    Serial.printf("[CMD] restored '%s'\n", path);

    // If instruments, reload into memory
    if (srvRestoreIsInstruments) {
        loadInstrumentsFromSD();
        Serial.println("[CMD] instruments reloaded after restore");
    }

    // If the restored file is the currently active song, reload and push to client
    if (!srvRestoreIsInstruments && srvHasActive && srvActiveName[0] != '\0'
        && strcmp(srvRestoreName, srvActiveName) == 0) {
        Serial.printf("[CMD] restored file matches active song '%s', reloading\n", srvActiveName);
        reloadActiveSongFromSD();
    }

    srvRestoreActive = false;
}

// ── Send active song buffer to the connected client ───────────────────────────
// Called from commandsTick() when sSongPushPending is set.
// Sends from srvActiveBuf directly — no SD read needed.
static void sendActiveSongToClient() {
    if (!srvHasActive || srvActiveBufLen == 0) return;
    uint32_t totalBytes  = srvActiveBufLen;
    uint8_t  totalChunks = (uint8_t)((totalBytes + SONG_CHUNK_SIZE - 1) / SONG_CHUNK_SIZE);
    Serial.printf("[CMD] push song to client: %u bytes %u chunks\n", totalBytes, totalChunks);

    MsgSongData msg;
    msg.type        = MSG_SONG_DATA;
    msg.totalChunks = totalChunks;

    for (uint8_t chunk = 0; chunk < totalChunks; chunk++) {
        uint32_t offset    = (uint32_t)chunk * SONG_CHUNK_SIZE;
        uint32_t remaining = totalBytes - offset;
        int n = (int)(remaining < (uint32_t)SONG_CHUNK_SIZE ? remaining : (uint32_t)SONG_CHUNK_SIZE);
        memcpy(msg.payload, srvActiveBuf + offset, n);
        msg.chunk   = chunk;
        msg.dataLen = (uint8_t)n;
        gComms.sendReliable(&msg, 4 + n, chunk);
    }
    Serial.println("[CMD] push song done");
}

// ── Tick — call from main loop() to run deferred operations ──────────────────
void commandsTick() {
    // Push active song to client on connect or song change (deferred from pairing/main)
    extern bool sSongPushPending;
    if (sSongPushPending) {
        sSongPushPending = false;
        if (srvHasActive) {
            sendActiveSongToClient();
        } else if (pairingIsConnected()) {
            // No song loaded — tell client to show "NO SONG" overlay
            uint8_t msg = (uint8_t)MSG_NO_SONG;
            gComms.send(&msg, 1);
        }
    }

    if (srvLoadPending) {
        srvLoadPending = false;
        int fileIdx = (int)srvLoadPendingPage * SL_PER_PKT + (int)srvLoadPendingIdx;
        sendSongData(srvLoadPendingPage, srvLoadPendingIdx);
        if (srvHasActive) {
            sequencerStop();
            sequencerReset();
            // Sync the server display cursor to the song the client just loaded
            int newCursor = fileIdx + SONG_IDX_OFFSET;
            if (newCursor < 0) newCursor = 0;
            if (newCursor >= numSongs + SONG_IDX_OFFSET)
                newCursor = numSongs + SONG_IDX_OFFSET - 1;
            cursor = newCursor;
            if (cursor >= scrollOffset + UI_VISIBLE_ROWS)
                scrollOffset = cursor - UI_VISIBLE_ROWS + 1;
            if (cursor < scrollOffset)
                scrollOffset = cursor;
            needsFullRedraw = true;
        }
    }
    if (srvFinaliseNeeded) {
        srvFinaliseNeeded = false;
        finaliseSongSave();  // loads into srvActiveBuf, writes compact to SD
        if (srvHasActive) { sequencerStop(); sequencerReset(); }
    }
    if (srvInstLoadPending) {
        srvInstLoadPending = false;
        sendInstrumentsData();
    }
    if (srvInstSavePending) {
        srvInstSavePending = false;
        saveInstrumentsToSD();
    }
    if (srvDeletePending) {
        srvDeletePending = false;
        char path[48];
        snprintf(path, sizeof(path), "%s/%s.mgt", SRV_SONGS_DIR, srvDeleteName);
        if (SD.exists(path)) {
            SD.remove(path);
            Serial.printf("[CMD] deleted '%s'\n", path);
        } else {
            snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, srvDeleteName);
            if (SD.exists(path)) {
                SD.remove(path);
                Serial.printf("[CMD] deleted '%s'\n", path);
            }
        }
        char expectedActive[SRV_FNAME_MAX];
        snprintf(expectedActive, sizeof(expectedActive), "%s.mgt", srvDeleteName);
        if (srvHasActive && strcmp(expectedActive, srvActiveName) == 0)
            srvHasActive = false;
    }
    if (srvBackupFilePending) {
        srvBackupFilePending = false;
        sendBackupFileRaw(srvBackupFileName);
    }
    if (srvRestoreFinaliseNeeded) {
        srvRestoreFinaliseNeeded = false;
        finaliseRestore();
    }
}

// ── Dispatch incoming commands ─────────────────────────────────────────────────
void handleCommand(MagiMsgType type, const uint8_t* data, int len) {
    switch (type) {

        case MSG_SONG_LIST_REQ:
            if (len < (int)sizeof(MsgSongListReq)) return;
            sendSongList(((const MsgSongListReq*)data)->page);
            break;

        case MSG_SAMPLE_LIST_REQ:
            if (len < (int)sizeof(MsgSampleListReq)) return;
            sendSampleList(((const MsgSampleListReq*)data)->page);
            break;

        case MSG_SONG_LOAD_REQ:
            if (len < (int)sizeof(MsgSongLoadReq)) return;
            {
                const MsgSongLoadReq* req = (const MsgSongLoadReq*)data;
                srvLoadPendingPage = req->page;
                srvLoadPendingIdx  = req->index;
                srvLoadPending     = true;
            }
            break;

        case MSG_SET_SONG_DATA:
            if (len < 4) return;
            {
                const MsgSetSongData* p = (const MsgSetSongData*)data;
                if (len < 4 + p->length) return;
                applySongPatch(p);
            }
            break;

        case MSG_NOTE_SET:
            if (len < (int)sizeof(MsgNoteSet)) return;
            if (srvHasActive) {
                const MsgNoteSet* m = (const MsgNoteSet*)data;
                Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
                if (m->pattern < song->numPatterns) {
                    NoteGrid grid(song->notePool, &song->noteFreeHead,
                                  &song->patterns[m->pattern].noteHead);
                    // Always set — NoteGrid::set clears internally when *all*
                    // fields are empty, but persists attr-only / vel-only rows.
                    grid.set(m->row, m->col, m->note);
                }
                // ACK so client can pace bulk sends (e.g. block duplicate)
                uint8_t ack[2] = { (uint8_t)MSG_CHUNK_ACK, m->row };
                gComms.send(ack, 2);
            }
            break;

        case MSG_SONG_SAVE:
            if (len < (int)sizeof(MsgSongSaveStart)) return;
            {
                const MsgSongSaveStart* s = (const MsgSongSaveStart*)data;
                strncpy(srvSaveName, s->name, sizeof(srvSaveName) - 1);
                srvSaveName[sizeof(srvSaveName) - 1] = '\0';
                srvSaveTotal  = s->totalChunks;
                srvSaveGot    = 0;
                srvSaveActive = true;
                // Open temp file and keep it open for all chunks
                if (srvSaveFile) srvSaveFile.close();
                SD.remove(SRV_UPLOAD_TMP);
                srvSaveFile = SD.open(SRV_UPLOAD_TMP, FILE_WRITE);
                Serial.printf("[CMD] save start '%s' %u chunks, file=%d\n",
                              srvSaveName, srvSaveTotal, (bool)srvSaveFile);
            }
            break;

        case MSG_SONG_SAVE_DATA:
            if (!srvSaveActive) return;
            if (len < 4) return;
            {
                const MsgSongSaveData* d = (const MsgSongSaveData*)data;
                if (srvSaveFile) {
                    srvSaveFile.write(d->payload, d->dataLen);
                    srvSaveGot++;
                }
                // ACK this chunk so the client can send the next one
                { uint8_t ack[2] = { (uint8_t)MSG_CHUNK_ACK, d->chunk };
                  gComms.send(ack, 2); }
                if (srvSaveGot >= srvSaveTotal && srvSaveTotal > 0) {
                    srvSaveFile.close();
                    srvFinaliseNeeded = true;
                }
            }
            break;

        case MSG_SONG_DELETE:
            if (len < (int)sizeof(MsgSongDelete)) return;
            {
                const MsgSongDelete* d = (const MsgSongDelete*)data;
                strncpy(srvDeleteName, d->name, sizeof(srvDeleteName) - 1);
                srvDeleteName[sizeof(srvDeleteName) - 1] = '\0';
                srvDeletePending = true;  // defer SD delete to main loop
            }
            break;

        case MSG_INSTRUMENTS_REQ:
            srvInstLoadPending = true;
            break;

        case MSG_INSTRUMENTS_PATCH:
            if (len < 4) return;
            {
                const MsgInstrumentsPatch* p = (const MsgInstrumentsPatch*)data;
                uint32_t end = (uint32_t)p->offset + p->length;
                if (end <= sizeof(srvInstruments)) {
                    memcpy((uint8_t*)srvInstruments + p->offset, p->data, p->length);
                    srvInstSavePending = true;
                }
            }
            break;

        case MSG_MIDI_DATA:
            if (len < (int)sizeof(MsgMidiData)) return;
            {
                const MsgMidiData* m = (const MsgMidiData*)data;
                for (uint8_t i = 0; i < m->len && i < 3; i++)
                    midi.write(m->data[i]);
            }
            break;

        case MSG_PLAY:
            sequencerResume();
            break;

        case MSG_STOP:
            if (sequencerIsRunning()) {
                sequencerStop();    // stop in place
            } else {
                sequencerPanic();   // all notes off on all channels
                sequencerReset();   // snap to top of block
            }
            break;

        case MSG_PAUSE:
            sequencerPause();
            break;

        case MSG_UNPAUSE:
            sequencerUnpause();
            break;

        case MSG_SET_WIFI_CHANNEL:
            if (len < (int)sizeof(MsgSetWifiChannel)) return;
            wifiChannelApply(((const MsgSetWifiChannel*)data)->idx);
            break;

        case MSG_SEEK:
            if (len < (int)sizeof(MsgSeek)) return;
            {
                const MsgSeek* s = (const MsgSeek*)data;
                sequencerSeek(s->pattern, s->row);
            }
            break;

        case MSG_GOTO:
            if (len < (int)sizeof(MsgGoto)) return;
            {
                const MsgGoto* g = (const MsgGoto*)data;
                sequencerGoto(g->pattern, g->row);
            }
            break;

        case MSG_QUEUE_BLOCK:
            if (len < (int)sizeof(MsgQueueBlock)) return;
            sequencerQueueBlock(((const MsgQueueBlock*)data)->pattern);
            break;

        case MSG_CANCEL_QUEUE:
            sequencerCancelQueue();
            break;

        case MSG_PREVIEW_START:
            if (len < (int)sizeof(MsgPreviewStart)) return;
            {
                const MsgPreviewStart* p = (const MsgPreviewStart*)data;
                sequencerStartPreview(p->pattern, p->col);
            }
            break;

        case MSG_PREVIEW_STOP:
            sequencerStopPreview();
            break;

        case MSG_NOTE_AUDITION:
            if (len < (int)sizeof(MsgNoteAudition)) return;
            {
                const MsgNoteAudition* a = (const MsgNoteAudition*)data;
                sequencerAuditionNote(a->pattern, a->row, a->col);
            }
            break;

        case MSG_BACKUP_LIST_REQ:
            if (len < (int)sizeof(MsgBackupListReq)) return;
            sendBackupFileList(((const MsgBackupListReq*)data)->page);
            break;

        case MSG_BACKUP_FILE_REQ:
            if (len < (int)sizeof(MsgBackupFileReq)) return;
            strncpy(srvBackupFileName, ((const MsgBackupFileReq*)data)->name,
                    SRV_FNAME_MAX - 1);
            srvBackupFileName[SRV_FNAME_MAX - 1] = '\0';
            srvBackupFilePending = true;
            break;

        case MSG_RESTORE_FILE_START:
            if (len < (int)sizeof(MsgRestoreFileStart)) return;
            {
                // If previous restore hasn't been finalised yet, do it now
                if (srvRestoreFinaliseNeeded) {
                    srvRestoreFinaliseNeeded = false;
                    finaliseRestore();
                }
                const MsgRestoreFileStart* r = (const MsgRestoreFileStart*)data;
                strncpy(srvRestoreName, r->name, SRV_FNAME_MAX - 1);
                srvRestoreName[SRV_FNAME_MAX - 1] = '\0';
                srvRestoreIsInstruments = (r->isInstruments != 0);
                srvRestoreTotal  = r->totalChunks;
                srvRestoreGot    = 0;
                srvRestoreActive = true;
                // Open temp file and keep open for all chunks
                if (srvRestoreFile) srvRestoreFile.close();
                SD.remove(SRV_RESTORE_TMP);
                srvRestoreFile = SD.open(SRV_RESTORE_TMP, FILE_WRITE);
                Serial.printf("[CMD] restore start '%s' %u chunks instr=%d\n",
                              srvRestoreName, srvRestoreTotal, srvRestoreIsInstruments);
            }
            break;

        case MSG_RESTORE_FILE_DATA:
            if (!srvRestoreActive) return;
            if (len < 4) return;
            {
                const MsgRestoreFileData* d = (const MsgRestoreFileData*)data;
                if (srvRestoreFile) {
                    srvRestoreFile.write(d->payload, d->dataLen);
                    srvRestoreGot++;
                }
                // ACK this chunk
                { uint8_t ack[2] = { (uint8_t)MSG_CHUNK_ACK, d->chunk };
                  gComms.send(ack, 2); }
                if (srvRestoreGot >= srvRestoreTotal && srvRestoreTotal > 0) {
                    srvRestoreFile.close();
                    srvRestoreFinaliseNeeded = true;
                }
            }
            break;

        default:
            break;
    }
}

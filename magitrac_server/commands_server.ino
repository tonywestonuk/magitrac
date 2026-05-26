// commands_server.ino — handles song commands from client
// Depends on: MagiComms (gComms), MagiMsg.h

#include <SD.h>
#include "sd_mutex.h"
#include <WiFi.h>
#include <Preferences.h>
#include "TrackerData.h"
#include "NoteGrid.h"
#include "SongMigration.h"
#include "midi_player.h"
#include "SampleManifest.h"

// ── REPRO: set to 1 to stub the SD reads in sendBackupFileRaw — pretend
// every backup file is 4096 fake bytes (memset 0xAB) instead of reading
// real data from the SD card.  Pairs with the magitrac-side
// REPRO_SKIP_SD_WRITE so both sides are SD-free during backup.  Lets us
// test whether the wedge involves SD/SDIO contention or current draw on
// either device.  Revert to 0 when done.
#define REPRO_SKIP_SERVER_SD_READ 1

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
    SdLock _;
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
    SdLock _;
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
static bool    srvLoadByNamePending = false;
static char    srvLoadByNameStr[SL_NAME_LEN] = {};
static bool    srvListPending       = false;
static uint8_t srvListPendingPage   = 0;

// ── Deferred backup file send ──────────────────────────────────────────────────
static bool    srvBackupFilePending = false;
static bool     srvTcpTestRunning    = false;   // set by MSG_TCP_TEST_START, cleared by MSG_TCP_TEST_STOP
static uint8_t  srvTcpTestPattern    = 0;       // incrementing-u8 pattern, persists across blobs
static char    srvBackupFileName[SRV_FNAME_MAX] = {};
static bool    srvBackupListPending = false;

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
    SdLock _;
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

    gMagiLink.acquireMutex();
    gMagiLink.send(&resp, sizeof(resp));
    gMagiLink.releaseMutex();
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
// This handles legacy v7..v16 files transparently — the client always
// receives a current-version payload regardless of the on-disk format.
static void sendSongDataFromPath(const char* path, const char* displayName) {
    srvHasActive    = false;
    srvActiveBufLen = 0;
    bool loaded = false;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));

    {
        SdLock _;
        File f = SD.open(path);
        if (!f) {
            Serial.printf("[CMD] sendSongData: open failed '%s'\n", path);
            return;
        }
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
    }

    if (!loaded) {
        Serial.printf("[CMD] sendSongData: load failed '%s'\n", path);
        return;
    }

    SongFileHeader* h2 = (SongFileHeader*)srvActiveBuf;
    h2->magic = SONG_FILE_MAGIC;
    h2->version = SONG_FILE_VERSION;
    h2->_pad[0] = h2->_pad[1] = h2->_pad[2] = 0;
    srvActiveBufLen = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);

    strncpy(srvActiveName, displayName, sizeof(srvActiveName) - 1);
    srvActiveName[sizeof(srvActiveName) - 1] = '\0';
    srvHasActive = true;

    // Push the loaded song to the client via the MagiLink HEADER+BODY stream.
    // Same path as the unsolicited push after handshake.
    Serial.printf("[CMD] sendSongData '%s' %u bytes — pushing\n",
                  path, (unsigned)srvActiveBufLen);
    sendActiveSongToClient();
}

static void sendSongData(uint8_t page, uint8_t index) {
    char files[SRV_MAX_FILES][SRV_FNAME_MAX];
    int total = srvListSongs(files, SRV_MAX_FILES);

    int fileIdx = (int)page * SL_PER_PKT + (int)index;
    if (fileIdx < 0 || fileIdx >= total) return;

    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, files[fileIdx]);
    sendSongDataFromPath(path, files[fileIdx]);
}

static void sendSongDataByName(const char* name) {
    char path[48];
    snprintf(path, sizeof(path), "%s/%s.mgt", SRV_SONGS_DIR, name);
    char display[SRV_FNAME_MAX];
    snprintf(display, sizeof(display), "%s.mgt", name);
    sendSongDataFromPath(path, display);
}

// ── Apply a generic song memory patch to the active song buffer ───────────────
static void applySongPatch(const MsgSetSongData* msg) {
    if (!srvHasActive) return;
    uint32_t start = SRV_HDR_SIZE + msg->offset;
    if (start + msg->dataLen > srvActiveBufLen) return;
    memcpy(srvActiveBuf + start, msg->data, msg->dataLen);

    // If bpm was touched, update the live sequencer BPM immediately
    const Song* s = reinterpret_cast<const Song*>(srvActiveBuf + SRV_HDR_SIZE);
    uint16_t bpmOff = (uint16_t)offsetof(Song, bpm);
    if (msg->offset <= bpmOff && msg->offset + msg->dataLen > bpmOff + 1) {
        sequencerSetBPM(s->bpm);
    }

    // If performerMask was touched, send CC 115 immediately
    uint16_t pmOff = (uint16_t)offsetof(Song, performerMask);
    if (msg->offset <= pmOff && msg->offset + msg->dataLen > pmOff) {
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

    srvHasActive    = false;
    srvActiveBufLen = 0;
    bool loaded = false;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));

    {
        SdLock _;
        File f = SD.open(path);
        if (!f) {
            Serial.printf("[CMD] srvLoadSongLocal: open failed '%s'\n", path);
            return false;
        }
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
    }

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
    SdLock _;
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
    SdLock _;
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

// ── Send full instruments array as HEADER + N×BODY over MagiLink ───────────
// Triggered by MSG_INSTRUMENTS_REQ — server streams the in-memory
// srvInstruments array to the client (~4-5 KB).  Reuses the shared
// sStreamBody for the body chunks (id rewritten per-use).
static void sendInstrumentsData() {
    if (!gMagiLink.isConnected()) {
        Serial.println("[CMD] sendInstruments: link not connected");
        return;
    }
    const uint8_t* raw   = (const uint8_t*)srvInstruments;
    uint32_t       total = (uint32_t)sizeof(srvInstruments);

    gMagiLink.acquireMutex();

    MsgInstrumentsPushHeader hdr;
    hdr.total_size = total;
    bool ok = gMagiLink.send(&hdr, sizeof(hdr));

    sStreamBody.id = MSG_INSTRUMENTS_PUSH_BODY;

    uint32_t sent = 0;
    while (ok && sent < total) {
        uint32_t remain = total - sent;
        uint16_t chunk  = remain > 1024 ? 1024 : (uint16_t)remain;
        sStreamBody.data_len = chunk;
        memcpy(sStreamBody.data, raw + sent, chunk);
        ok = gMagiLink.send(&sStreamBody, sizeof(sStreamBody));
        sent += chunk;
    }

    gMagiLink.releaseMutex();
    Serial.printf("[CMD] sendInstruments: %u bytes (%s)\n",
                  (unsigned)total, ok ? "OK" : "FAIL");
}

// ── Backup: stream the full file list as a single framed blob ───────────────
//
// Wire payload: type(1) + numFiles(1) + BkFileEntry × numFiles.
// All entries arrive in one round-trip — no paging.  Server enumerates
// songs + instruments, stat()s each for its size, packs the entries into
// the BkFileEntry layout, and ships them.  At SRV_MAX_FILES+1 = 33
// entries the blob is 1+1+33*28 = 926 bytes — well under MAX_FRAME.
static void sendBackupFileList(uint8_t /*unused*/) {
    if (!srvSdOk) return;
    extern MagiCommsTcp gTransportTcp;

    BkFileEntry entries[SRV_MAX_FILES + 1];
    int total = 0;

    // Enumerate songs + instruments under a single SD lock — srvListSongs
    // takes its own lock internally so we release ours around it.
    char songNames[SRV_MAX_FILES][SRV_FNAME_MAX];
    int songCount = srvListSongs(songNames, SRV_MAX_FILES);
    {
        SdLock _;
        for (int i = 0; i < songCount && total < SRV_MAX_FILES; i++) {
            memset(&entries[total], 0, sizeof(entries[total]));
            strncpy(entries[total].name, songNames[i], SRV_FNAME_MAX - 1);
            char path[48];
            snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, songNames[i]);
            File f = SD.open(path);
            uint32_t sz = f ? (uint32_t)f.size() : 0;
            if (f) f.close();
            entries[total].sizeHi = (uint16_t)(sz >> 16);
            entries[total].sizeLo = (uint16_t)(sz & 0xFFFF);
            total++;
        }
        if (SD.exists(SRV_INSTRUMENTS_PATH) && total < (int)(sizeof(entries)/sizeof(entries[0]))) {
            memset(&entries[total], 0, sizeof(entries[total]));
            strncpy(entries[total].name, "instruments.mgt", SRV_FNAME_MAX - 1);
            File f = SD.open(SRV_INSTRUMENTS_PATH);
            uint32_t sz = f ? (uint32_t)f.size() : 0;
            if (f) f.close();
            entries[total].sizeHi = (uint16_t)(sz >> 16);
            entries[total].sizeLo = (uint16_t)(sz & 0xFFFF);
            total++;
        }
    }

    size_t totalLen = 1 + 1 + (size_t)total * sizeof(BkFileEntry);
    if (!gTransportTcp.streamBegin(totalLen)) {
        Serial.printf("[CMD] backupList: streamBegin failed (len=%u)\n", (unsigned)totalLen);
        return;
    }
    bool sendOk = true;
    uint8_t type    = (uint8_t)MSG_BACKUP_LIST_BLOB;
    uint8_t numByte = (uint8_t)total;
    sendOk &= gTransportTcp.streamMore(&type,    1);
    sendOk &= gTransportTcp.streamMore(&numByte, 1);
    if (total > 0) {
        sendOk &= gTransportTcp.streamMore(entries, (size_t)total * sizeof(BkFileEntry));
    }
    gTransportTcp.streamEnd();
    Serial.printf("[CMD] backupList total=%d streamed=%s\n", total, sendOk ? "OK" : "FAIL");
}

// ── Backup: stream raw file as a single framed message ──────────────────────
//
// Wire payload (totalPayloadLen = 1 + 24 + 4 + fileSize):
//   [type=MSG_BACKUP_FILE_BLOB][name 24B null-padded][fileSize u32 LE][bytes]
//
// SD is read in 1 KB pieces and shovelled straight into the TCP socket via
// the streaming API — peer.write() blocks when the kernel send buffer fills,
// so TCP flow control paces the SD reads naturally.  Peak RAM: one 1 KB
// read buffer on the stack.
static void sendBackupFileRaw(const char* name) {
    if (!srvSdOk) return;
    extern MagiCommsTcp gTransportTcp;

    char path[48];
    if (strcmp(name, "instruments.mgt") == 0) {
        strncpy(path, SRV_INSTRUMENTS_PATH, sizeof(path) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, name);
    }
    path[sizeof(path) - 1] = '\0';

#if REPRO_SKIP_SERVER_SD_READ
    uint32_t fileSize = 4096;   // fake — actual SD never touched
    Serial.printf("[CMD][REPRO] skip-SD-read '%s' (fake %u bytes)\n",
                  name, (unsigned)fileSize);
#else
    File f;
    uint32_t fileSize = 0;
    {
        SdLock _;
        f = SD.open(path);
        if (!f) {
            Serial.printf("[CMD] backupFile: open failed '%s'\n", path);
            return;
        }
        fileSize = (uint32_t)f.size();
    }
#endif
    size_t totalLen = 1 + SRV_FNAME_MAX + 4 + fileSize;

    if (!gTransportTcp.streamBegin(totalLen)) {
        Serial.printf("[CMD] backupFile: streamBegin failed (len=%u)\n", (unsigned)totalLen);
#if !REPRO_SKIP_SERVER_SD_READ
        { SdLock _; f.close(); }
#endif
        return;
    }
    bool    sendOk = true;
    uint8_t type = (uint8_t)MSG_BACKUP_FILE_BLOB;
    char    namePadded[SRV_FNAME_MAX] = {};
    strncpy(namePadded, name, SRV_FNAME_MAX - 1);
    sendOk &= gTransportTcp.streamMore(&type, 1);
    sendOk &= gTransportTcp.streamMore(namePadded, SRV_FNAME_MAX);
    sendOk &= gTransportTcp.streamMore(&fileSize, 4);

    uint8_t  buf[1024];
    uint32_t sent = 0;
    while (sent < fileSize && sendOk) {
        size_t want = sizeof(buf);
        if (fileSize - sent < want) want = fileSize - sent;
        int got;
#if REPRO_SKIP_SERVER_SD_READ
        memset(buf, 0xAB, want);
        got = (int)want;
#else
        { SdLock _; got = f.read(buf, want); }
#endif
        if (got <= 0) break;
        sendOk = gTransportTcp.streamMore(buf, (size_t)got);
        sent += got;
    }
    gTransportTcp.streamEnd();
#if !REPRO_SKIP_SERVER_SD_READ
    { SdLock _; f.close(); }
#endif
    Serial.printf("[CMD] backupFile '%s' %u bytes streamed=%s\n",
                  name, fileSize, sendOk ? "OK" : "FAIL");
}


// Shared 1029-byte streaming body buffer for backup and song push.
// MsgBackupBody and MsgSongPushBody have identical wire shape — only the
// id byte differs — so we keep one buffer and rewrite the id per-use.
// Worker stack is 8 KB so this struct is too big for it; the mutex
// serializes use sites so a single shared static is safe.  Saves ~1 KB
// of DRAM vs. a separate static body per function (matters on the
// M5Stack ESP32 classic — its DRAM is tight).
static MsgBackupBody sStreamBody;

// ── MagiLink song save (client → server) state ─────────────────────────────
// Populated by the MSG_SAVE_SONG_HEADER and _BODY callbacks (worker task).
// On stream completion, the BODY callback sets sMagiSavePending; the main
// loop's commandsTick consumes it and writes to SD if a name was given.
// SD writes can't run on the worker task (SamplePlayer contention rule)
// so deferring to main loop is mandatory.
static char     sMagiSaveName[SRV_FNAME_MAX] = {};
static uint32_t sMagiSaveExpectedBytes       = 0;  // Song bytes expected
static uint32_t sMagiSaveReceivedBytes       = 0;  // Song bytes received so far
static bool     sMagiSavePending             = false;

void onMagiLinkSaveHeader(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgSongSaveHeader)) return;
    const MsgSongSaveHeader* h = (const MsgSongSaveHeader*)msg;
    if (h->song_bytes > sizeof(Song)) {
        Serial.printf("[CMD] save header rejected: %u bytes > max %u\n",
                      (unsigned)h->song_bytes, (unsigned)sizeof(Song));
        sMagiSaveExpectedBytes = 0;
        return;
    }
    // Stop the sequencer up-front: the receive writes incrementally into
    // srvActiveBuf over many ms.  If the sequencer keeps reading it could
    // follow a half-overwritten NoteNode pointer into garbage.
    sequencerStop();
    // Stamp SongFileHeader at the front of srvActiveBuf.
    memcpy(srvActiveBuf, &h->song_file_header, sizeof(SongFileHeader));
    strncpy(sMagiSaveName, h->name, sizeof(sMagiSaveName) - 1);
    sMagiSaveName[sizeof(sMagiSaveName) - 1] = '\0';
    sMagiSaveExpectedBytes = h->song_bytes;
    sMagiSaveReceivedBytes = 0;
    Serial.printf("[CMD] save start: name='%s' bytes=%u\n",
                  sMagiSaveName, (unsigned)sMagiSaveExpectedBytes);
}

void onMagiLinkSaveBody(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (sMagiSaveExpectedBytes == 0) return;          // stray body
    if (len < sizeof(MsgSongSaveBody)) return;
    const MsgSongSaveBody* b = (const MsgSongSaveBody*)msg;
    if (b->data_len > 1024) return;
    if (sMagiSaveReceivedBytes + b->data_len > sMagiSaveExpectedBytes) {
        Serial.println("[CMD] save body overshoot — dropping");
        sMagiSaveExpectedBytes = 0;
        return;
    }
    memcpy(srvActiveBuf + sizeof(SongFileHeader) + sMagiSaveReceivedBytes,
           b->data, b->data_len);
    sMagiSaveReceivedBytes += b->data_len;
    if (sMagiSaveReceivedBytes == sMagiSaveExpectedBytes) {
        sMagiSaveExpectedBytes = 0;
        sMagiSavePending       = true;
    }
}

// Called from commandsTick when sMagiSavePending fires.  Writes the
// in-memory song to SD if a name was provided; either way, marks
// srvHasActive and resets the sequencer.
static void finaliseMagiLinkSongSave() {
    srvActiveBufLen = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);
    srvHasActive    = true;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));

    if (sMagiSaveName[0] != '\0') {
        char path[48];
        snprintf(path, sizeof(path), "%s/%s.mgt", SRV_SONGS_DIR, sMagiSaveName);
        SdLock _;
        SD.remove(path);
        File f = SD.open(path, FILE_WRITE);
        if (!f) {
            Serial.printf("[CMD] save: open failed '%s'\n", path);
            return;
        }
        bool ok = songWriteCompact(f, song);
        uint32_t sz = (uint32_t)f.size();
        f.close();
        Serial.printf("[CMD] save: wrote '%s' %u bytes %s\n",
                      path, sz, ok ? "OK" : "FAIL");
        snprintf(srvActiveName, sizeof(srvActiveName), "%s.mgt", sMagiSaveName);
    } else {
        Serial.println("[CMD] save: in-memory push (no SD write)");
    }
    sequencerStop();
    sequencerReset();
}

// ── MagiLink backup — streams every /songs/*.mgt + instruments.mgt ─────────
//
// Called from the MagiLink MSG_START_BACKUP handler (worker task context,
// transaction mutex already held).  Sends:
//   MsgBackupHeader, MsgBackupBody×N    ← for each file
//   MsgEndOfData                         ← run complete
//
// SD reads gated by SdLock so SamplePlayer / other tasks can interleave
// safely.  Sends are fire-and-forget — TCP backpressure paces via
// blocking peer.write in MagiLink::send.
void sendBackupToClient() {
    if (!srvSdOk) {
        Serial.println("[BK-SRV] SD not available — sending bare END_OF_DATA");
        MsgEndOfData eod;
        gMagiLink.send(&eod, sizeof(eod));
        return;
    }

    // Step 1: enumerate files (one SdLock window, kept tight).
    // names/sizes/body are static — they're large (~2 KB combined) and
    // sendBackupToClient is only ever called from one task at a time,
    // so static keeps the worker task's stack usage low.
    static char     names[SRV_MAX_FILES][SRV_FNAME_MAX];
    static uint32_t sizes[SRV_MAX_FILES];
    uint16_t count = 0;
    {
        SdLock _;
        File d = SD.open(SRV_SONGS_DIR);
        if (d && d.isDirectory()) {
            while (count < SRV_MAX_FILES) {
                File entry = d.openNextFile();
                if (!entry) break;
                if (!entry.isDirectory()) {
                    const char* nm = entry.name();
                    if (nm && strstr(nm, ".mgt")) {
                        strncpy(names[count], nm, SRV_FNAME_MAX - 1);
                        names[count][SRV_FNAME_MAX - 1] = '\0';
                        sizes[count] = (uint32_t)entry.size();
                        count++;
                    }
                }
                entry.close();
            }
            d.close();
        }
        // Add instruments.mgt at the end.
        if (count < SRV_MAX_FILES && SD.exists(SRV_INSTRUMENTS_PATH)) {
            File ins = SD.open(SRV_INSTRUMENTS_PATH);
            if (ins) {
                strncpy(names[count], "instruments.mgt", SRV_FNAME_MAX - 1);
                names[count][SRV_FNAME_MAX - 1] = '\0';
                sizes[count] = (uint32_t)ins.size();
                count++;
                ins.close();
            }
        }
    }
    Serial.printf("[BK-SRV] sending %u files\n", (unsigned)count);

    // Step 2: send each file as header + N×body.
    // Use the shared sStreamBody buffer.  Set id explicitly since song
    // push may have left it as MSG_SONG_PUSH_BODY.
    sStreamBody.id = MSG_BACKUP_BODY;

    bool ok = true;
    for (uint16_t i = 0; i < count && ok; i++) {
        MsgBackupHeader hdr;
        memset(hdr.filename, 0, sizeof(hdr.filename));
        strncpy(hdr.filename, names[i], sizeof(hdr.filename) - 1);
        hdr.file_size  = sizes[i];
        hdr.file_index = (uint16_t)(i + 1);
        hdr.file_total = count;
        ok = gMagiLink.send(&hdr, sizeof(hdr));
        if (!ok) {
            Serial.printf("[BK-SRV] header send failed at file %u\n", (unsigned)i);
            break;
        }

        // Open the file.
        char path[64];
        if (strcmp(names[i], "instruments.mgt") == 0) {
            strncpy(path, SRV_INSTRUMENTS_PATH, sizeof(path) - 1);
        } else {
            snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, names[i]);
        }
        path[sizeof(path) - 1] = '\0';

        File f;
        { SdLock _; f = SD.open(path); }
        if (!f) {
            Serial.printf("[BK-SRV] open failed '%s'\n", path);
            continue;
        }

        // Stream body chunks.
        uint32_t sent = 0;
        while (ok && sent < sizes[i]) {
            uint32_t want = sizes[i] - sent;
            if (want > sizeof(sStreamBody.data)) want = sizeof(sStreamBody.data);
            int got;
            { SdLock _; got = f.read(sStreamBody.data, want); }
            if (got <= 0) break;
            sStreamBody.data_len = (uint16_t)got;
            ok = gMagiLink.send(&sStreamBody, sizeof(sStreamBody));
            if (!ok) {
                Serial.printf("[BK-SRV] body send failed at file %u sent=%u\n",
                              (unsigned)i, (unsigned)sent);
                break;
            }
            sent += got;
        }
        { SdLock _; f.close(); }
    }

    // Step 3: end-of-data marker.
    MsgEndOfData eod;
    gMagiLink.send(&eod, sizeof(eod));
    Serial.printf("[BK-SRV] backup done (ok=%d)\n", ok ? 1 : 0);
}

// ── TCP/IP diagnostic test — sends a 4096-byte incrementing-u8 blob ────────
//
// Wire payload (totalPayloadLen = 1 + 4 + 4096 = 4101):
//   [type=MSG_TCP_TEST_BLOB][size u32 LE = 4096][4096 incrementing u8 bytes]
//
// No SD, no I/O — same streamBegin/streamMore/streamEnd path the real backup
// uses, but isolated from any subsystem outside WiFi+TCP.  Lets us tell
// whether the wedge sits in the network stack under the actual magitrac
// runtime environment (EPD task, MIDI, etc.) vs in something specific to
// the backup data path (which we've already stubbed and which still wedges).
static void sendTcpTestBlob() {
    extern MagiCommsTcp gTransportTcp;

    static const uint32_t PAYLOAD = 4096;
    size_t totalLen = 1 + 4 + PAYLOAD;

    if (!gTransportTcp.streamBegin(totalLen)) {
        Serial.printf("[CMD] tcpTest: streamBegin failed (len=%u)\n",
                      (unsigned)totalLen);
        return;
    }

    bool sendOk = true;
    uint8_t  type = (uint8_t)MSG_TCP_TEST_BLOB;
    uint32_t size = PAYLOAD;
    sendOk &= gTransportTcp.streamMore(&type, 1);
    sendOk &= gTransportTcp.streamMore(&size, 4);

    uint8_t buf[1024];
    uint32_t sent = 0;
    while (sent < PAYLOAD && sendOk) {
        size_t want = sizeof(buf);
        if (PAYLOAD - sent < want) want = PAYLOAD - sent;
        for (size_t i = 0; i < want; ++i) buf[i] = srvTcpTestPattern++;
        sendOk = gTransportTcp.streamMore(buf, want);
        sent  += want;
    }
    gTransportTcp.streamEnd();
    Serial.printf("[CMD] tcpTest %u bytes streamed=%s\n",
                  (unsigned)PAYLOAD, sendOk ? "OK" : "FAIL");
}

// ── Reload the active song from SD into srvActiveBuf and push to client ──────
static void reloadActiveSongFromSD() {
    extern bool sSongPushPending;

    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, srvActiveName);

    srvHasActive    = false;
    srvActiveBufLen = 0;
    bool loaded = false;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));

    {
        SdLock _;
        File f = SD.open(path);
        if (!f) {
            Serial.printf("[CMD] reload: open failed '%s'\n", path);
            return;
        }
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
    }

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
    if (!srvRestoreActive || srvRestoreName[0] == '\0') {
        SdLock _;
        if (srvRestoreFile) srvRestoreFile.close();
        return;
    }
    char path[48];
    if (srvRestoreIsInstruments) {
        strncpy(path, SRV_INSTRUMENTS_PATH, sizeof(path) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, srvRestoreName);
    }
    path[sizeof(path) - 1] = '\0';

    {
        SdLock _;
        if (srvRestoreFile) srvRestoreFile.close();
        // Rename temp file to final destination
        if (SD.exists(path)) SD.remove(path);
        SD.rename(SRV_RESTORE_TMP, path);
    }
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
// Called from commandsTick() when sSongPushPending is set.  Streams from
// srvActiveBuf over MagiLink as HEADER + N×BODY (no SD read needed).
// Body is `static` because 1029 bytes is too big for any caller's stack.
static void sendActiveSongToClient() {
    if (!srvHasActive || srvActiveBufLen == 0) return;
    if (!gMagiLink.isConnected()) {
        Serial.println("[CMD] song push: link not connected");
        return;
    }

    gMagiLink.acquireMutex();

    MsgSongPushHeader hdr;
    hdr.total_size = srvActiveBufLen;
    bool ok = gMagiLink.send(&hdr, sizeof(hdr));

    // Reuse the shared streaming body buffer; flip its id since backup may
    // have left it as MSG_BACKUP_BODY.
    sStreamBody.id = MSG_SONG_PUSH_BODY;

    uint32_t sent = 0;
    while (ok && sent < srvActiveBufLen) {
        uint32_t remain = srvActiveBufLen - sent;
        uint16_t chunk  = remain > 1024 ? 1024 : (uint16_t)remain;
        sStreamBody.data_len = chunk;
        memcpy(sStreamBody.data, srvActiveBuf + sent, chunk);
        ok = gMagiLink.send(&sStreamBody, sizeof(sStreamBody));
        sent += chunk;
    }

    gMagiLink.releaseMutex();
    Serial.printf("[CMD] song push: %u bytes (%s)\n",
                  (unsigned)srvActiveBufLen, ok ? "OK" : "FAIL");
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
            MsgNoSong msg;
            gMagiLink.acquireMutex();
            gMagiLink.send(&msg, sizeof(msg));
            gMagiLink.releaseMutex();
        }
    }

    if (srvLoadByNamePending) {
        srvLoadByNamePending = false;
        sendSongDataByName(srvLoadByNameStr);
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
    if (sMagiSavePending) {
        sMagiSavePending = false;
        finaliseMagiLinkSongSave();
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
        {
            SdLock _;
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
    if (srvBackupListPending) {
        srvBackupListPending = false;
        sendBackupFileList(0);
    }
    // Pure-streaming test — emit one blob per tick while running.
    // TCP backpressure naturally paces via blocking peer.write.
    if (srvTcpTestRunning) {
        sendTcpTestBlob();
    }

    if (srvRestoreFinaliseNeeded) {
        srvRestoreFinaliseNeeded = false;
        finaliseRestore();
    }
    // Process list request AFTER finalize/delete so a save-then-list or
    // delete-then-list sequence returns the up-to-date directory.
    if (srvListPending) {
        srvListPending = false;
        sendSongList(srvListPendingPage);
    }
}

// ── Dispatch incoming commands ─────────────────────────────────────────────────
void handleCommand(MagiMsgType type, const uint8_t* data, int len) {
    switch (type) {

        case MSG_SONG_LIST_REQ:
            if (len < (int)sizeof(MsgSongListReq)) return;
            // Deferred to main loop — runs after any pending save finalize so
            // a save-then-list sequence returns the file the user just saved.
            srvListPendingPage = ((const MsgSongListReq*)data)->page;
            srvListPending     = true;
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

        case MSG_SONG_LOAD_NAME:
            if (len < (int)sizeof(MsgSongLoadNameReq)) return;
            {
                const MsgSongLoadNameReq* req = (const MsgSongLoadNameReq*)data;
                strncpy(srvLoadByNameStr, req->name, sizeof(srvLoadByNameStr) - 1);
                srvLoadByNameStr[sizeof(srvLoadByNameStr) - 1] = '\0';
                srvLoadByNamePending = true;
            }
            break;

        case MSG_SET_SONG_DATA:
            // MagiLink wire: id(1) + length(2) + offset(2) + dataLen(1) + data[dataLen]
            if (len < 6) return;
            {
                const MsgSetSongData* p = (const MsgSetSongData*)data;
                if (len < (int)(6 + p->dataLen)) return;
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
            }
            break;

        // MSG_SONG_SAVE / MSG_SONG_SAVE_DATA (chunked upload) retired —
        // replaced by streaming MSG_SONG_SAVE_BLOB handled in
        // onSongSaveBlobStream() below.

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
            // MagiLink wire: id(1) + length(2) + offset(2) + dataLen(1) + data[dataLen]
            if (len < 6) return;
            {
                const MsgInstrumentsPatch* p = (const MsgInstrumentsPatch*)data;
                if (len < (int)(6 + p->dataLen)) return;
                uint32_t end = (uint32_t)p->offset + p->dataLen;
                if (end <= sizeof(srvInstruments)) {
                    memcpy((uint8_t*)srvInstruments + p->offset, p->data, p->dataLen);
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
            // Defer SD enumeration to the main loop — SD/SPI isn't
            // thread-safe and inline access from the TCP reader task
            // races with SamplePlayer SD reads on the MIDI task.
            srvBackupListPending = true;
            break;

        case MSG_BACKUP_FILE_REQ:
            if (len < (int)sizeof(MsgBackupFileReq)) return;
            strncpy(srvBackupFileName, ((const MsgBackupFileReq*)data)->name,
                    SRV_FNAME_MAX - 1);
            srvBackupFileName[SRV_FNAME_MAX - 1] = '\0';
            srvBackupFilePending = true;
            break;

        case MSG_TCP_TEST_START:
            srvTcpTestRunning = true;
            break;

        case MSG_TCP_TEST_STOP:
            srvTcpTestRunning = false;
            break;

        // MSG_RESTORE_FILE_START / MSG_RESTORE_FILE_DATA (chunked upload)
        // retired — replaced by streaming MSG_RESTORE_FILE_BLOB handled in
        // onRestoreFileBlobStream() below.

        default:
            break;
    }
}

// ── Streaming receive: song save (client → server) ──────────────────────────
//
// Wire payload (remaining after the type byte was consumed by the reader):
//   name(SRV_NAME_MAX) + SongFileHeader + Song
//
// We pull each section straight off the socket and write to a temp file
// on SD as bytes arrive — no full-buffer copy of the 35 KB Song.  Marks
// srvFinaliseNeeded so commandsTick's main-loop finalizer renames the
// temp file and reloads the active song.
void onSongSaveBlobStream(size_t remainingLen, void* /*ctx*/) {
    extern MagiCommsTcp gTransportTcp;

    if (remainingLen < SRV_NAME_MAX + sizeof(SongFileHeader) + sizeof(Song)) {
        // Bad frame — drain so framing stays aligned
        uint8_t junk[256];
        while (remainingLen > 0) {
            size_t n = remainingLen > sizeof(junk) ? sizeof(junk) : remainingLen;
            size_t got = gTransportTcp.streamReadRecv(junk, n);
            if (got == 0) break;
            remainingLen -= got;
        }
        Serial.println("[CMD] SONG_SAVE_BLOB: undersized, dropped");
        return;
    }

    char nameBuf[SRV_NAME_MAX] = {};
    gTransportTcp.streamReadRecv((uint8_t*)nameBuf, SRV_NAME_MAX);
    SongFileHeader hdr;
    gTransportTcp.streamReadRecv((uint8_t*)&hdr, sizeof(hdr));

    size_t expectedSong = remainingLen - SRV_NAME_MAX - sizeof(SongFileHeader);

    SdLock _;
    SD.remove(SRV_UPLOAD_TMP);
    File f = SD.open(SRV_UPLOAD_TMP, FILE_WRITE);
    if (!f) {
        Serial.println("[CMD] SONG_SAVE_BLOB: tmp open failed");
        // Drain remaining
        uint8_t junk[512];
        while (expectedSong > 0) {
            size_t n = expectedSong > sizeof(junk) ? sizeof(junk) : expectedSong;
            size_t got = gTransportTcp.streamReadRecv(junk, n);
            if (got == 0) break;
            expectedSong -= got;
        }
        return;
    }
    // Write header
    f.write((const uint8_t*)&hdr, sizeof(hdr));
    // Stream the Song bytes from socket → SD
    uint8_t buf[1024];
    size_t  remain = expectedSong;
    while (remain > 0) {
        size_t want = remain > sizeof(buf) ? sizeof(buf) : remain;
        size_t got  = gTransportTcp.streamReadRecv(buf, want);
        if (got == 0) break;
        f.write(buf, got);
        remain -= got;
    }
    f.close();

    strncpy(srvSaveName, nameBuf, sizeof(srvSaveName) - 1);
    srvSaveName[sizeof(srvSaveName) - 1] = '\0';
    srvSaveActive     = true;
    srvFinaliseNeeded = true;
    Serial.printf("[CMD] SONG_SAVE_BLOB '%s' %u bytes received\n",
                  srvSaveName, (unsigned)expectedSong);
}

// ── Streaming receive: restore upload (client → server) ─────────────────────
//
// Wire payload (after type byte):
//   name(SRV_FNAME_MAX) + isInstruments(1) + size(u32 LE) + bytes
void onRestoreFileBlobStream(size_t remainingLen, void* /*ctx*/) {
    extern MagiCommsTcp gTransportTcp;

    const size_t HDR = SRV_FNAME_MAX + 1 + 4;
    if (remainingLen < HDR) {
        uint8_t junk[256];
        while (remainingLen > 0) {
            size_t n = remainingLen > sizeof(junk) ? sizeof(junk) : remainingLen;
            size_t got = gTransportTcp.streamReadRecv(junk, n);
            if (got == 0) break;
            remainingLen -= got;
        }
        Serial.println("[CMD] RESTORE_FILE_BLOB: undersized, dropped");
        return;
    }
    char nameBuf[SRV_FNAME_MAX] = {};
    gTransportTcp.streamReadRecv((uint8_t*)nameBuf, SRV_FNAME_MAX);
    uint8_t isInstr;
    gTransportTcp.streamReadRecv(&isInstr, 1);
    uint32_t fileSize;
    gTransportTcp.streamReadRecv((uint8_t*)&fileSize, 4);

    size_t remain = remainingLen - HDR;
    if (remain != fileSize) {
        Serial.printf("[CMD] RESTORE_FILE_BLOB: size mismatch frame=%u hdr=%u\n",
                      (unsigned)remain, (unsigned)fileSize);
    }

    // If a prior restore is still waiting to finalize, finish it first
    if (srvRestoreFinaliseNeeded) {
        srvRestoreFinaliseNeeded = false;
        finaliseRestore();
    }

    SdLock _;
    SD.remove(SRV_RESTORE_TMP);
    File f = SD.open(SRV_RESTORE_TMP, FILE_WRITE);
    if (!f) {
        Serial.println("[CMD] RESTORE_FILE_BLOB: tmp open failed");
        uint8_t junk[512];
        while (remain > 0) {
            size_t n = remain > sizeof(junk) ? sizeof(junk) : remain;
            size_t got = gTransportTcp.streamReadRecv(junk, n);
            if (got == 0) break;
            remain -= got;
        }
        return;
    }
    uint8_t buf[1024];
    while (remain > 0) {
        size_t want = remain > sizeof(buf) ? sizeof(buf) : remain;
        size_t got  = gTransportTcp.streamReadRecv(buf, want);
        if (got == 0) break;
        f.write(buf, got);
        remain -= got;
    }
    f.close();

    strncpy(srvRestoreName, nameBuf, SRV_FNAME_MAX - 1);
    srvRestoreName[SRV_FNAME_MAX - 1] = '\0';
    srvRestoreIsInstruments  = (isInstr != 0);
    srvRestoreActive         = true;
    srvRestoreFinaliseNeeded = true;
    Serial.printf("[CMD] RESTORE_FILE_BLOB '%s' %u bytes instr=%d received\n",
                  srvRestoreName, (unsigned)fileSize, srvRestoreIsInstruments);
}

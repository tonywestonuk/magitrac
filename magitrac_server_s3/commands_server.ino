// commands_server.ino — handles song commands from client
// Depends on: MagiLink (gMagiLink), MagiMsg.h

#include <SD.h>
#include "sd_mutex.h"
#include <WiFi.h>
#include <Preferences.h>
#include "TrackerData.h"
#include "NoteGrid.h"
#include "SongMigration.h"
#include "midi_player.h"
#include "SampleManifest.h"
#include "esp32s3_dsp.h"   // ESP32S3_FFT — spectrogram (same engine as pixelpost bands)

// ── Direct-from-Mac PixelPost OTA (client FW button) ──────────────────────────
// The client's FW button triggers MSG_PIXELPOST_FIRMWARE_UPDATE; the server then
// tells the posts to join the home WiFi and OTA from the Mac's nginx box.  This
// is the bench/home route — the gig route is the server's own FLASH screen +
// field_flash.ino (self-served off the server softAP, no external router).
// Creds live in wifi_secrets.h (gitignored — copy wifi_secrets.h.example)
// because the trigger message is a bare button-press.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define PP_MAC_OTA_SSID  "your-wifi-ssid"
#define PP_MAC_OTA_PSK   "your-wifi-password"
#define PP_MAC_OTA_URL   "http://192.168.1.100/upd/pixel_post.ino.bin"
#endif

// ── WiFi channel persistence ──────────────────────────────────────────────────
// Shared namespace name with the client side ("magitrac_wifi") so the channel
// setting is conceptually one thing across both devices, just stored locally.
static const char* WIFI_NVS_NS = "magitrac_wifi";
static uint8_t     sWifiChannelIdx = 0;   // 0/1/2 → 1/6/11

// Load the persisted channel idx without touching WiFi.  Used by setup()
// to pick the softAP channel before WiFi.softAP() is called.
uint8_t wifiChannelLoad() {
    Preferences prefs;
    prefs.begin(WIFI_NVS_NS, true);
    sWifiChannelIdx = prefs.getUChar("idx", 0);
    if (sWifiChannelIdx > 2) sWifiChannelIdx = 0;
    prefs.end();
    return sWifiChannelIdx;
}

static void wifiChannelApply(uint8_t idx) {
    if (idx > 2) return;
    sWifiChannelIdx = idx;
    Preferences prefs;
    prefs.begin(WIFI_NVS_NS, false);
    prefs.putUChar("idx", idx);
    prefs.end();
    // Live channel-switch via WiFi.softAP(ssid, psk, ch) is unreliable on
    // Arduino-ESP32 — the call can silently fail when the AP is already
    // running, leaving the radio in a default fallback state (the AP
    // disappears or comes back as "ESP_xxxxxx" with no SSID/PSK).  Persist
    // the new index and reboot so setup() runs its full softAP +
    // softAPConfig + dhcps_stop sequence cleanly on the new channel.
    Serial.printf("[WIFI] channel idx=%u saved — rebooting to apply\n",
                  (unsigned)idx);
    delay(200);   // serial flush
    ESP.restart();
}

// CoreS3 SD: SCLK=G36, MISO=G35, MOSI=G37, CS=G4.  SPI.begin() with those
// pins runs in setup() before commandsInit(); SD.begin(cs) then picks up
// the default SPI bus we just configured.
#define SRV_SD_CS      4
#define SRV_SONGS_DIR  "/songs"
// Power-loss recovery: autosave writes land here, NOT /songs/.  On boot any
// file in this dir is promoted to /songs/<same name>.mgt (overwriting), then
// the directory is empty until the next autosave.  Explicit Save also writes
// to /songs/ and clears the matching /autosave/ file.
#define SRV_AUTOSAVE_DIR "/autosave"

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
// PSRAM — 36 KB of control-rate data (sequencer row walks, patches, CRC);
// internal RAM is reserved for the radios + audio hot loops (see
// feedback_internal_ram_budget).  Allocated in commandsInit.
uint8_t* srvActiveBuf     = nullptr;
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

// MSG_NOTE_SET queue — produced on the MagiLink worker task, consumed by
// commandsTick on the loop task, so note-pool relinks never race a playing
// sequencer walk.  SPSC ring: head written only by the worker, tail only by
// the loop; uint8_t indices wrap at 256, a multiple of the queue length, so
// the masks stay consistent across wrap.
#define NOTE_SET_Q_LEN 64
static MsgNoteSet       sNoteSetQ[NOTE_SET_Q_LEN];
static volatile uint8_t sNoteSetQHead = 0;
static volatile uint8_t sNoteSetQTail = 0;

// Cell-tap audition, deferred behind the note-set queue (see MSG_NOTE_AUDITION).
static volatile bool sAuditionPending = false;
static uint8_t       sAuditionPat = 0, sAuditionRow = 0, sAuditionCol = 0;

// PSRAM sample-preload sync: set whenever the active song (or its column
// settings) changes; commandsTick runs samplePreloadSync on the loop task when
// the sequencer is stopped and the player is quiet (SD reads + buffer frees).
static volatile bool gSamplePreloadDirty = false;

// Sample-editor requests, deferred to the loop task (overview = a windowed
// SD read; trim save = an SD write).  Latest-wins single slots.
static volatile bool     sSampleInfoPending = false;
static volatile uint8_t  sSampleInfoId      = 0;
static volatile uint32_t sSampleInfoVs      = 0;   // requested view window
static volatile uint32_t sSampleInfoVe      = 0;   // 0,0 = whole file
static volatile bool     sTrimSavePending   = false;
static volatile bool     sSampleSpecPending = false;   // spectrogram request
static volatile uint8_t  sSampleSpecId      = 0;

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

// ── Restore upload state ──────────────────────────────────────────────────────
static bool    srvRestoreActive          = false;
static bool    srvRestoreFinaliseNeeded  = false;
static bool    srvRestoreIsInstruments   = false;
static char    srvRestoreName[SRV_FNAME_MAX] = {};
static uint8_t srvRestoreTotal   = 0;
static uint8_t srvRestoreGot     = 0;
static File    srvRestoreFile;

// ── Generic file server ───────────────────────────────────────────────────────
// FileKind enum (MagiMsg.h) → SD path lookup.  Whitelist controls what the
// client can enumerate / fetch.  Currently just /drumtracks/; add a row
// here and a FileKind entry to expose another directory.
#define SRV_DRUMTRACKS_DIR  "/drumtracks"
#define SRV_SETLISTS_DIR    "/setlists"
#define SRV_GM_MAP_NAME     "gm_map.txt"

// Returns nullptr for an unknown / out-of-range kind.
static const char* fileKindToPath(uint8_t kind) {
    switch (kind) {
        case FK_DRUMTRACKS: return SRV_DRUMTRACKS_DIR;
        case FK_SETLISTS:   return SRV_SETLISTS_DIR;
        default:            return nullptr;
    }
}

// Maps a plain backup filename to its SD directory by extension.  Keeps the
// backup (read) and restore (write) sides in lockstep so each file round-trips
// to the directory it came from.  instruments.mgt is handled separately (it
// lives at the root path, not under a directory).
//   .txt → /drumtracks   .set → /setlists   everything else → /songs
static const char* backupDirForName(const char* name) {
    int n = (int)strlen(name);
    // The setlist master catalog is a .txt but belongs with the setlists, not
    // the drumtracks — route it by name before the extension rules below.
    if (strcasecmp(name, "master.txt") == 0) return SRV_SETLISTS_DIR;
    if (n >= 4 && name[n - 4] == '.') {
        char c3 = name[n - 3], c2 = name[n - 2], c1 = name[n - 1];
        if ((c3 == 't' || c3 == 'T') && (c2 == 'x' || c2 == 'X') && (c1 == 't' || c1 == 'T'))
            return SRV_DRUMTRACKS_DIR;
        if ((c3 == 's' || c3 == 'S') && (c2 == 'e' || c2 == 'E') && (c1 == 't' || c1 == 'T'))
            return SRV_SETLISTS_DIR;
    }
    return SRV_SONGS_DIR;
}

// True when this filename should be excluded from the user-facing list
// for `kind`.  Currently only used to hide /drumtracks/gm_map.txt.
static bool fileKindHidesFromList(uint8_t kind, const char* name) {
    if (kind == FK_DRUMTRACKS && strcasecmp(name, SRV_GM_MAP_NAME) == 0) return true;
    return false;
}

static bool    srvFileListPending      = false;
static uint8_t srvFileListPendingKind  = 0;
static uint8_t srvFileListPendingPage  = 0;
static bool    srvFileLoadPending      = false;
static uint8_t srvFileLoadPendingKind  = 0;
static char    srvFileLoadPendingName[FILE_NAME_LEN] = {};

static void writeDefaultGmMap() {
    SdLock _;
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SRV_DRUMTRACKS_DIR, SRV_GM_MAP_NAME);
    if (SD.exists(path)) return;
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.println("[CMD] gm_map.txt create failed"); return; }
    f.println("# Drum-track GM mapping — one entry per instrument code.");
    f.println("# Format: CODE midi_note default_velocity");
    f.println("# Edit then re-import to apply.");
    f.println("CH 42 100");
    f.println("OH 46 100");
    f.println("CY 49 100");
    f.println("CB 56 100");
    f.println("CP 39 100");
    f.println("RS 37 100");
    f.println("HT 50 100");
    f.println("MT 47 100");
    f.println("LT 41 100");
    f.println("SD 38 100");
    f.println("BD 36 100");
    f.println("AC 31 100");
    f.println("GH 38 40");
    f.close();
    Serial.println("[CMD] wrote default gm_map.txt");
}

// Autosave policy: the 30 s autosave tick drops a draft in /autosave/<name>.mgt
// for every song being worked on.  These are crash / power-cut survivors ONLY —
// the server NEVER auto-promotes or reloads them on boot.  A draft sits
// untouched until the next autosave of the same name overwrites it, or an
// explicit Save / Delete of that song clears it (see finaliseSaveActive +
// the delete handler).  Recovering a draft, if ever wanted, must be a
// deliberate user action — never a silent boot step.

// After a USB-MSC session the SD card is left in SPI mode by SdFat (25 MHz,
// DEDICATED_SPI) and an ESP.restart() does NOT power-cycle it.  If the host
// ejected while the card was mid (multi-)block read it keeps streaming data and
// swallows the next CMD0 as payload, so a single cold SD.begin() can never get
// the card to idle — mounting then fails until a full power cycle.  Recover the
// way the SD spec prescribes: deassert CS and clock a long 0xFF burst (>1 block)
// at a slow rate to flush any pending stream and return the card to idle.
static void sdRecoverBus() {
    pinMode(SRV_SD_CS, OUTPUT);
    digitalWrite(SRV_SD_CS, HIGH);                 // deselect the card
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 1024; i++) SPI.transfer(0xFF);   // >1 block of clocks
    SPI.endTransaction();
}

// Hard power-cycle the SD card.  On CoreS3 the TF-card slot Vdd is on AXP2101
// ALDO4 (reg 0x90 bit 3) — confirmed in M5Unified Power_Class.  After a USB-MSC
// session SdFat can leave the card's own state machine wedged mid-operation, and
// NOTHING short of removing Vcc resets it: not sdRecoverBus()'s clock burst, not
// even a full digital-core reset (esp_rom_software_reset_system).  Only pulling
// the rail works — which is exactly the manual unplug the card used to need.
//
// Drive the shared SPI lines (SCK/MOSI/CS) low first so the card can't stay
// alive on phantom power through its protection diodes while the rail is down,
// then cut ALDO4, drain, restore, and re-init the bus.  (SCK/MOSI are shared
// with the LCD but driving them static-low just means no LCD update meanwhile —
// no clock edges, no harm.)
#define AXP2101_I2C_ADDR  0x34
#define AXP2101_LDO_EN0   0x90
#define AXP2101_ALDO4_BIT 0x08
static void sdPowerCycle() {
    // Toggle ONLY the AXP2101 ALDO4 rail (TF-card Vdd) — touch nothing else.
    // The AXP holds ALDO4 up across any ESP32 reset, so cutting it here is the
    // only thing that resets a card SdFat left wedged after a USB-MSC session.
    // Do NOT hand-drive the SD SPI pins: SCK(36)/MOSI(37) are shared with the LCD
    // bus and driving them here hangs boot — the rail toggle alone is enough.
    M5.In_I2C.bitOff(AXP2101_I2C_ADDR, AXP2101_LDO_EN0, AXP2101_ALDO4_BIT, 400000);
    delay(400);           // let the slot caps discharge
    M5.In_I2C.bitOn (AXP2101_I2C_ADDR, AXP2101_LDO_EN0, AXP2101_ALDO4_BIT, 400000);
    delay(100);           // Vdd ramp + card internal power-up
}

void commandsInit() {
    // Active-song buffer in PSRAM (internal fallback so a bad PSRAM day
    // still boots — the wrong-board incident proved that can happen).
    srvActiveBuf = (uint8_t*)heap_caps_malloc(SRV_SONG_XFER_MAX, MALLOC_CAP_SPIRAM);
    if (!srvActiveBuf) {
        srvActiveBuf = (uint8_t*)malloc(SRV_SONG_XFER_MAX);
        Serial.println("[CMD] srvActiveBuf: PSRAM alloc FAILED — using internal");
    }

    sdPowerCycle();                                // real Vcc cycle: unwedge the card
    srvSdOk = false;
    for (int attempt = 0; attempt < 4 && !srvSdOk; ++attempt) {
        sdRecoverBus();                            // idle the card before each try
        srvSdOk = SD.begin(SRV_SD_CS);
        if (!srvSdOk) {
            Serial.printf("[CMD] SD.begin attempt %d failed\n", attempt + 1);
            delay(100);
        }
    }
    Serial.printf("[CMD] SD: %s\n", srvSdOk ? "OK" : "not found");
    if (srvSdOk && !SD.exists(SRV_SONGS_DIR))
        SD.mkdir(SRV_SONGS_DIR);
    if (srvSdOk && !SD.exists(SRV_AUTOSAVE_DIR))
        SD.mkdir(SRV_AUTOSAVE_DIR);
    if (srvSdOk && !SD.exists(SRV_DRUMTRACKS_DIR)) {
        SD.mkdir(SRV_DRUMTRACKS_DIR);
        Serial.println("[CMD] created /drumtracks/");
    }
    if (srvSdOk && !SD.exists(SRV_SETLISTS_DIR)) {
        SD.mkdir(SRV_SETLISTS_DIR);
        Serial.println("[CMD] created /setlists/");
    }
    if (srvSdOk) writeDefaultGmMap();

    initDefaultInstruments();
    if (srvSdOk && !loadInstrumentsFromSD()) {
        saveInstrumentsToSD();  // write defaults so future boots load them
        Serial.println("[CMD] instruments: created defaults");
    }
}

// Case-insensitive name compare for qsort over the fixed-width name rows.
static int srvNameCmp(const void* a, const void* b) {
    return strcasecmp((const char*)a, (const char*)b);
}

// ── List .mgt files in /songs ──────────────────────────────────────────────────
// Returns names sorted alphabetically (case-insensitive) so every paginated
// list the client assembles is in a stable, sorted order.
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
    qsort(names, count, SRV_FNAME_MAX, srvNameCmp);
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

// ── Generic file list ────────────────────────────────────────────────────────
// Scans `dir` for files, applying the per-kind hide filter.  Returns count
// collected into `out` (capped at maxFiles).
static int srvListFiles(uint8_t kind, const char* dir,
                        char out[][FILE_NAME_LEN], int maxFiles) {
    if (!srvSdOk || !dir) return 0;
    SdLock _;
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return 0;
    int count = 0;
    while (count < maxFiles) {
        File e = d.openNextFile();
        if (!e) break;
        if (!e.isDirectory()) {
            const char* fn = e.name();
            const char* sl = strrchr(fn, '/');
            if (sl) fn = sl + 1;
            if (fn[0] != '.' && !fileKindHidesFromList(kind, fn)) {
                strncpy(out[count], fn, FILE_NAME_LEN - 1);
                out[count][FILE_NAME_LEN - 1] = '\0';
                count++;
            }
        }
        e.close();
    }
    d.close();
    return count;
}

static void sendFileList(uint8_t kind, uint8_t page) {
    const char* dir = fileKindToPath(kind);
    if (!dir) {
        Serial.printf("[CMD] file list: unknown kind=%u\n", (unsigned)kind);
        return;
    }
    char files[SRV_MAX_FILES][FILE_NAME_LEN];
    int total = srvListFiles(kind, dir, files, SRV_MAX_FILES);

    int totalPages = total > 0 ? (total + FILE_LIST_PER_PKT - 1) / FILE_LIST_PER_PKT : 1;
    if (page >= (uint8_t)totalPages) page = totalPages - 1;
    int start  = page * FILE_LIST_PER_PKT;
    int inPage = total - start;
    if (inPage > FILE_LIST_PER_PKT) inPage = FILE_LIST_PER_PKT;
    if (inPage < 0) inPage = 0;

    MsgFileListResp resp;
    resp.kind       = kind;
    resp.page       = page;
    resp.totalPages = (uint8_t)totalPages;
    resp.count      = (uint8_t)inPage;
    memset(resp.names, 0, sizeof(resp.names));
    for (int i = 0; i < inPage; i++) {
        strncpy(resp.names[i], files[start + i], FILE_NAME_LEN - 1);
        resp.names[i][FILE_NAME_LEN - 1] = '\0';
    }

    gMagiLink.acquireMutex();
    gMagiLink.send(&resp, sizeof(resp));
    gMagiLink.releaseMutex();
}

// Shared 1029-byte streaming buffer used by every chunked-body sender below
// (backup, song push, instruments push, file load).  Declared up here
// because sendFile() — the first user — would otherwise see it undeclared.
// Mutex serialises use sites so one shared instance is safe.
static MsgBackupBody sStreamBody;

// ── Send a {kind}/{name} file as HEADER + chunked BODY + EndOfData ───────────
// `name` is taken verbatim (with extension); the kind selects the dir via
// the whitelist.  Bad kind ⇒ header sent with found=0, no body.
static void sendFile(uint8_t kind, const char* name) {
    const char* dir = fileKindToPath(kind);

    File f;
    uint32_t fsize = 0;
    bool found = false;
    if (dir) {
        char path[80];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        SdLock _;
        f = SD.open(path);
        if (f) {
            found = !f.isDirectory();
            if (found) fsize = (uint32_t)f.size();
        }
    }

    gMagiLink.acquireMutex();

    MsgFileLoadHeader hdr;
    hdr.kind       = kind;
    hdr.total_size = fsize;
    hdr.found      = found ? 1 : 0;
    gMagiLink.send(&hdr, sizeof(hdr));

    if (found) {
        sStreamBody.id = MSG_FILE_LOAD_BODY;
        uint32_t sent = 0;
        bool ok = true;
        while (ok && sent < fsize) {
            uint32_t remain = fsize - sent;
            uint16_t chunk  = remain > 1024 ? 1024 : (uint16_t)remain;
            { SdLock _; f.read(sStreamBody.data, chunk); }
            sStreamBody.data_len = chunk;
            ok = gMagiLink.send(&sStreamBody, sizeof(sStreamBody));
            sent += chunk;
        }
        MsgEndOfData eod;
        gMagiLink.send(&eod, sizeof(eod));
    }

    gMagiLink.releaseMutex();
    { SdLock _; if (f) f.close(); }
    Serial.printf("[CMD] file send kind=%u '%s' %u bytes (%s)\n",
                  (unsigned)kind, name, (unsigned)fsize, found ? "OK" : "MISSING");
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
    // NSDMI sets id+length; zero the dynamic fields ourselves so
    // unused entries don't carry garbage.
    resp.page         = page;
    resp.totalPages   = (uint8_t)totalPages;
    resp.count        = (uint8_t)inPage;
    resp.totalEntries = (uint8_t)(total > 255 ? 255 : total);
    memset(resp.entries, 0, sizeof(resp.entries));

    for (int i = 0; i < inPage; i++) {
        const SmEntry* e = sampleManifestAt(start + i);
        if (!e) break;
        resp.entries[i].id = e->id;
        strncpy(resp.entries[i].name, e->name, SAMPLE_NAME_LEN - 1);
        resp.entries[i].name[SAMPLE_NAME_LEN - 1] = '\0';
    }

    gMagiLink.acquireMutex();
    gMagiLink.send(&resp, sizeof(resp));
    gMagiLink.releaseMutex();
}

// ── Send song file in chunks ───────────────────────────────────────────────────
// Loads/migrates the song into srvActiveBuf first, then sends from memory.
// This handles legacy v7..v16 files transparently — the client always
// receives a current-version payload regardless of the on-disk format.
static void sendSongDataFromPath(const char* path, const char* displayName) {
    // Explicit load is the user's "discard draft" path.
    // Clear the autosave draft for:
    //   (a) the song being loaded — they asked for the canonical /songs/
    //       version, so any session draft is abandoned;
    //   (b) the song we're moving away from — they didn't hit Save, so the
    //       draft would otherwise resurrect at next boot recovery.
    {
        SdLock _;
        char autopath[48];
        if (srvActiveName[0] != '\0') {
            snprintf(autopath, sizeof(autopath), "%s/%s",
                     SRV_AUTOSAVE_DIR, srvActiveName);    // already carries .mgt
            if (SD.exists(autopath)) {
                SD.remove(autopath);
                Serial.printf("[CMD] abandoned autosave '%s'\n", autopath);
            }
        }
        snprintf(autopath, sizeof(autopath), "%s/%s",
                 SRV_AUTOSAVE_DIR, displayName);          // also has .mgt
        if (SD.exists(autopath)) {
            SD.remove(autopath);
            Serial.printf("[CMD] discarded draft on reload '%s'\n", autopath);
        }
    }

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
            } else if (hdr.version == 19) {
                loaded = songMigrateV19FromFile(f, song);
                if (loaded) Serial.printf("[CMD] migrated v19->v%d '%s'\n", SONG_FILE_VERSION, path);
            } else if (hdr.version == 18) {
                loaded = songMigrateV18FromFile(f, song);
                if (loaded) Serial.printf("[CMD] migrated v18->v%d '%s'\n", SONG_FILE_VERSION, path);
            } else if (hdr.version == 17) {
                loaded = songMigrateV17FromFile(f, song);
                if (loaded) Serial.printf("[CMD] migrated v17->v%d '%s'\n", SONG_FILE_VERSION, path);
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
    srvHasActive = true;  gSamplePreloadDirty = true;

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

// ── Sample editor: build + send meta + waveform overview ──────────────────────
// Windowed SD read (min/max peaks per bucket over [vs, ve); 0,0 = whole file)
// — called from commandsTick on the loop task only.  A zoomed-in client asks
// for its visible window and gets full-resolution peaks for it.
static void sendSampleInfo(uint8_t id, uint32_t vs, uint32_t ve) {
    const char* fname = sampleManifestNameFor(id);
    if (!fname) return;
    char path[64];
    snprintf(path, sizeof(path), "/samples/%s", fname);

    // Transient PSRAM — internal RAM is the radios' budget (see the
    // spectrogram builder below for the incident that taught us this).
    MsgSampleInfo* infoP =
        (MsgSampleInfo*)heap_caps_malloc(sizeof(MsgSampleInfo), MALLOC_CAP_SPIRAM);
    int16_t* rdP = (int16_t*)heap_caps_malloc(1024 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!infoP || !rdP) { free(infoP); free(rdP); return; }
    MsgSampleInfo& info = *infoP;
    info = MsgSampleInfo();      // NSDMI: id + length

    uint32_t totalFrames = 0, rate = 0;
    uint16_t numCh = 0;
    bool ok = false;
    memset(info.peaks, 0, sizeof(info.peaks));
    {
        SdLock _;
        File f = SD.open(path);
        if (f) do {
            uint8_t hdr[44];
            if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) break;
            rate  = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8)
                  | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
            numCh = (uint16_t)hdr[22] | ((uint16_t)hdr[23] << 8);
            uint16_t bitsPer = (uint16_t)hdr[34] | ((uint16_t)hdr[35] << 8);
            if ((numCh != 1 && numCh != 2) || bitsPer != 16) break;
            uint32_t frameBytes = (uint32_t)numCh * 2;
            totalFrames = ((uint32_t)f.size() - sizeof(hdr)) / frameBytes;

            // Clamp the requested window.
            if (vs >= totalFrames) vs = 0;
            if (ve == 0 || ve > totalFrames || ve <= vs) ve = totalFrames;
            uint32_t viewFrames = ve - vs;
            if (vs && !f.seek(sizeof(hdr) + vs * frameBytes)) break;

            // Min/max of the LEFT channel per bucket, scaled int16 → int8.
            uint32_t bucket = (viewFrames + SAMPLE_OVERVIEW_N - 1) / SAMPLE_OVERVIEW_N;
            if (bucket == 0) bucket = 1;
            uint32_t frame = 0;
            int      col   = 0;
            int16_t  mn = 32767, mx = -32768;
            while (frame < viewFrames && col < SAMPLE_OVERVIEW_N) {
                int got = f.read((uint8_t*)rdP, 1024 * sizeof(int16_t));
                if (got <= 0) break;
                int n = got / (int)frameBytes;
                for (int i = 0; i < n && col < SAMPLE_OVERVIEW_N; i++) {
                    if (frame >= viewFrames) break;
                    int16_t s = rdP[i * numCh];
                    if (s < mn) mn = s;
                    if (s > mx) mx = s;
                    if (++frame % bucket == 0) {
                        info.peaks[col * 2]     = (int8_t)(mn >> 8);
                        info.peaks[col * 2 + 1] = (int8_t)(mx >> 8);
                        col++;
                        mn = 32767; mx = -32768;
                    }
                }
            }
            if (col < SAMPLE_OVERVIEW_N && mn != 32767) { // final partial bucket
                info.peaks[col * 2]     = (int8_t)(mn >> 8);
                info.peaks[col * 2 + 1] = (int8_t)(mx >> 8);
            }
            info.viewStart = vs;
            info.viewEnd   = ve;
            ok = true;
        } while (false);
        if (f) f.close();
    }

    if (ok) {
        const SampleTrim* tr = sampleTrimFor(id);
        info.sampleId    = id;
        info.channels    = (uint8_t)numCh;
        info.loop        = tr ? tr->loop : 0;
        info.kind        = 0;              // waveform overview
        info.totalFrames = totalFrames;
        info.rate        = rate;
        info.startFrame  = tr ? tr->startFrame : 0;
        info.endFrame    = tr ? tr->endFrame   : 0;

        gMagiLink.acquireMutex();
        gMagiLink.send(&info, sizeof(info));
        gMagiLink.releaseMutex();
    }
    free(infoP);
    free(rdP);
}

// ── Sample editor: spectrogram (Fairlight Page-D waterfall, display only) ─────
// SAMPLE_SPEC_SLICES time slices across the TRIMMED region; per slice one
// 1024-point FFT via the S3's DSP engine (ESP32S3_FFT, esp32s3_dsp.cpp — the
// same accelerated path the pixelpost bands use, own instance so it never
// touches the mic path's buffers), folded into SAMPLE_SPEC_BINS log-spaced
// display bins (60 Hz .. 8 kHz), normalised to bytes and shipped as
// SAMPLE_SPEC_PAGES MsgSampleInfo packets (kind = page+1, payload in
// peaks[]).  Loop task only — SD reads + ~2 ms/slice of FFT.
static void sendSampleSpectrum(uint8_t id) {
    const char* fname = sampleManifestNameFor(id);
    if (!fname) return;
    char path[64];
    snprintf(path, sizeof(path), "/samples/%s", fname);

    // All working memory is TRANSIENT PSRAM — ~30 KB of static internal RAM
    // here starved WiFi ("Connecting..." forever).  Internal RAM is the
    // radios' budget; big one-shot buffers go to PSRAM and are freed on exit.
    const int FFTN = 1024;
    uint8_t* spec = (uint8_t*)heap_caps_malloc(SAMPLE_SPEC_BYTES, MALLOC_CAP_SPIRAM);
    float*   mag  = (float*)heap_caps_malloc(SAMPLE_SPEC_BYTES * sizeof(float), MALLOC_CAP_SPIRAM);
    int16_t* rd   = (int16_t*)heap_caps_malloc(2048 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    float*   fin  = (float*)heap_caps_malloc(FFTN * sizeof(float), MALLOC_CAP_SPIRAM);
    float*   fout = (float*)heap_caps_malloc(FFTN * sizeof(float), MALLOC_CAP_SPIRAM);
    MsgSampleInfo* infoP =
        (MsgSampleInfo*)heap_caps_malloc(sizeof(MsgSampleInfo), MALLOC_CAP_SPIRAM);
    static ESP32S3_FFT sFft;                 // instance itself is small; its
                                             // buffers are PSRAM via init()
    bool fftUp = spec && mag && rd && fin && fout && infoP &&
                 sFft.init(FFTN, FFTN, SPECTRAL_AVERAGE);
    if (!fftUp) {
        free(spec); free(mag); free(rd); free(fin); free(fout); free(infoP);
        return;
    }
    memset(spec, 0, SAMPLE_SPEC_BYTES);
    memset(mag,  0, SAMPLE_SPEC_BYTES * sizeof(float));

    bool ok = false;
    {
        SdLock _;
        File f = SD.open(path);
        if (f) do {
        uint8_t hdr[44];
        if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) break;
        uint32_t rate  = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8)
                       | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
        uint16_t numCh = (uint16_t)hdr[22] | ((uint16_t)hdr[23] << 8);
        uint16_t bits  = (uint16_t)hdr[34] | ((uint16_t)hdr[35] << 8);
        if ((numCh != 1 && numCh != 2) || bits != 16 ||
            rate < 8000 || rate > 48000) break;
        uint32_t frameBytes  = (uint32_t)numCh * 2;
        uint32_t totalFrames = ((uint32_t)f.size() - sizeof(hdr)) / frameBytes;

        const SampleTrim* tr = sampleTrimFor(id);
        uint32_t st = tr ? tr->startFrame : 0;
        uint32_t en = (tr && tr->endFrame) ? tr->endFrame : totalFrames;
        if (st >= totalFrames) st = 0;
        if (en > totalFrames || en <= st) en = totalFrames;

        uint32_t segFrames = (en - st) / SAMPLE_SPEC_SLICES;
        if (segFrames == 0) segFrames = 1;

        // Log-spaced display-bin edges → linear FFT bin ranges.
        float fTop = (rate / 2 > 8500) ? 8000.0f : (float)rate * 0.45f;

        for (int s = 0; s < SAMPLE_SPEC_SLICES; s++) {
            uint32_t frame0 = st + (uint32_t)s * segFrames;
            if (!f.seek(sizeof(hdr) + frame0 * frameBytes)) break;
            uint32_t want = segFrames;
            if (want > (uint32_t)FFTN) want = FFTN;
            int got = f.read((uint8_t*)rd, want * frameBytes);
            int n = got / (int)frameBytes;
            if (n < 32) continue;
            for (int i = 0; i < n; i++)    fin[i] = (float)rd[i * numCh];
            for (int i = n; i < FFTN; i++) fin[i] = 0.0f;

            sFft.compute(fin, fout);       // hardware-accelerated, hann window

            for (int b = 0; b < SAMPLE_SPEC_BINS; b++) {
                float f0 = 60.0f * powf(fTop / 60.0f, (float)b / SAMPLE_SPEC_BINS);
                float f1 = 60.0f * powf(fTop / 60.0f, (float)(b + 1) / SAMPLE_SPEC_BINS);
                int k0 = (int)(f0 * FFTN / rate);
                int k1 = (int)(f1 * FFTN / rate);
                if (k0 < 1) k0 = 1;
                if (k1 <= k0) k1 = k0 + 1;
                if (k1 > FFTN / 2) k1 = FFTN / 2;
                float pk = 0;
                for (int k = k0; k < k1; k++)
                    if (fout[k] > pk) pk = fout[k];
                mag[s * SAMPLE_SPEC_BINS + b] = pk;
            }
        }
        ok = true;
        } while (false);
        if (f) f.close();
    }
    sFft.end();

    if (ok) {
        // Log-compress and normalise to the loudest bin in the whole plot.
        float mx = 1.0f;
        for (int i = 0; i < SAMPLE_SPEC_BYTES; i++) if (mag[i] > mx) mx = mag[i];
        float lmx = logf(mx);
        const float RANGE = 5.5f;                // ~48 dB of visible dynamics
        for (int i = 0; i < SAMPLE_SPEC_BYTES; i++) {
            float l = (mag[i] > 1.0f) ? logf(mag[i]) : 0.0f;
            float v = (l - (lmx - RANGE)) / RANGE;
            if (v < 0) v = 0;
            spec[i] = (uint8_t)(v * 255.0f);
        }

        MsgSampleInfo& info = *infoP;
        info = MsgSampleInfo();                  // NSDMI: id + length
        for (int p = 0; p < SAMPLE_SPEC_PAGES; p++) {
            info.sampleId = id;
            info.kind     = (uint8_t)(p + 1);
            memcpy(info.peaks, spec + p * SAMPLE_SPEC_PAGE_BYTES,
                   SAMPLE_SPEC_PAGE_BYTES);
            gMagiLink.acquireMutex();
            gMagiLink.send(&info, sizeof(info));
            gMagiLink.releaseMutex();
            delay(5);   // let the client's dispatcher drain between pages
        }
    }
    free(spec); free(mag); free(rd); free(fin); free(fout); free(infoP);
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

    // If column settings were touched (sample re-pick, channel change), the
    // PSRAM preload cache may need to follow.
    uint32_t colsOff = (uint32_t)offsetof(Song, columns);
    uint32_t colsEnd = colsOff + sizeof(s->columns);
    if (msg->offset < colsEnd && msg->offset + msg->dataLen > colsOff) {
        gSamplePreloadDirty = true;
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
            } else if (hdr.version == 19) {
                loaded = songMigrateV19FromFile(f, song);
                if (loaded) Serial.printf("[CMD] migrated v19->v%d '%s'\n", SONG_FILE_VERSION, path);
            } else if (hdr.version == 18) {
                loaded = songMigrateV18FromFile(f, song);
                if (loaded) Serial.printf("[CMD] migrated v18->v%d '%s'\n", SONG_FILE_VERSION, path);
            } else if (hdr.version == 17) {
                loaded = songMigrateV17FromFile(f, song);
                if (loaded) Serial.printf("[CMD] migrated v17->v%d '%s'\n", SONG_FILE_VERSION, path);
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
        srvHasActive = true;  gSamplePreloadDirty = true;
    }

    Serial.printf("[CMD] srvLoadSongLocal: '%s' %u bytes %s\n",
                  path, srvActiveBufLen, loaded ? "OK" : "FAIL");
    return loaded;
}

// ── Verified song write ───────────────────────────────────────────────────────
// Writes `song` to `path`, then reads the bytes straight back off the card and
// CRC-checks them against the in-memory song before committing.  The write goes
// to a sibling _save.tmp first; only a byte-verified temp is atomically renamed
// over `path`, so a bad write (dying card, truncation, power glitch) can never
// clobber the previous good file.  Retries once.  Returns true only when the
// committed file is verified.
//
// Caller MUST already hold SdLock — sSdMutex is non-recursive, do not re-lock.
static bool writeSongVerified(const char* path, const Song* song) {
    const uint32_t crcMem = songCrc32(song);

    // Temp file in the same directory as the target so the rename stays intra-FS.
    char tmp[64];
    const char* sl = strrchr(path, '/');
    if (sl) snprintf(tmp, sizeof(tmp), "%.*s/_save.tmp", (int)(sl - path), path);
    else    snprintf(tmp, sizeof(tmp), "/_save.tmp");

    for (int attempt = 1; attempt <= 2; attempt++) {
        SD.remove(tmp);
        File f = SD.open(tmp, FILE_WRITE);
        if (!f) { Serial.printf("[CMD] save-verify: tmp open failed '%s'\n", tmp); continue; }
        bool ok = songWriteCompact(f, song);
        uint32_t sz = (uint32_t)f.size();
        f.close();
        if (!ok) {
            Serial.printf("[CMD] save-verify: write failed (try %d)\n", attempt);
            SD.remove(tmp);
            continue;
        }

        // Read it straight back and CRC the bytes that actually landed.
        uint32_t crcDisk = 0;
        bool readOk = false;
        File rf = SD.open(tmp, FILE_READ);
        if (rf) {
            readOk = true;
            uint8_t buf[256];
            int r;
            while ((r = rf.read(buf, sizeof(buf))) > 0)
                crcDisk = magiCrc32(crcDisk, buf, (size_t)r);
            rf.close();
        }
        if (!readOk || crcDisk != crcMem) {
            Serial.printf("[CMD] save-verify: VERIFY FAIL (try %d) crcMem=%08X crcDisk=%08X sz=%u\n",
                          attempt, (unsigned)crcMem, (unsigned)crcDisk, (unsigned)sz);
            SD.remove(tmp);
            continue;
        }

        // Verified — atomically swap the temp in for the canonical file.
        SD.remove(path);
        if (!SD.rename(tmp, path)) {
            Serial.printf("[CMD] save-verify: rename '%s' -> '%s' FAILED (try %d)\n", tmp, path, attempt);
            SD.remove(tmp);
            continue;
        }
        Serial.printf("[CMD] save-verify: '%s' %u bytes OK (crc=%08X)\n",
                      path, (unsigned)sz, (unsigned)crcDisk);
        return true;
    }
    Serial.printf("[CMD] save-verify: GAVE UP on '%s' — existing file left intact\n", path);
    return false;
}

// ── Flush active song buffer back to SD ───────────────────────────────────────
static void flushActiveSong() {
    if (!srvHasActive || srvActiveName[0] == '\0') return;
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, srvActiveName);
    const Song* song = (const Song*)(srvActiveBuf + sizeof(SongFileHeader));
    SdLock _;
    if (!writeSongVerified(path, song))
        Serial.printf("[CMD] flush: verify failed '%s' — kept previous file\n", path);
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
        if (!writeSongVerified(path, song)) {
            Serial.printf("[CMD] save: verify failed '%s' — kept previous file\n", path);
            srvSaveActive = false;
            return;
        }
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
    srvHasActive = true;  gSamplePreloadDirty = true;

    srvSaveActive = false;
}

// Shared 1029-byte streaming body buffer for backup, song push, and
// `sStreamBody` is declared earlier in this file (just above sendDrumtrackFile)
// so the drum-track send path can reuse the same buffer.  All consumers
// (backup / song push / instruments push / drum-track load) write the
// matching id byte before sending.

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

// ── MagiLink restore (client → server) state ───────────────────────────────
// Streams bytes incrementally to SRV_RESTORE_TMP on SD as they arrive on
// the worker task — same approach as the legacy onRestoreFileBlobStream.
// SdLock serializes against SamplePlayer.  This avoids a 35 KB RAM
// buffer that would blow DRAM on the M5 ESP32.
static File     sMagiRestoreFile;
static char     sMagiRestoreName[SRV_FNAME_MAX] = {};
static bool     sMagiRestoreIsInstruments       = false;
static uint32_t sMagiRestoreExpectedBytes       = 0;
static uint32_t sMagiRestoreReceivedBytes       = 0;
static bool     sMagiRestorePending             = false;

void onMagiLinkRestoreHeader(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgRestoreHeader)) return;
    const MsgRestoreHeader* h = (const MsgRestoreHeader*)msg;
    strncpy(sMagiRestoreName, h->name, sizeof(sMagiRestoreName) - 1);
    sMagiRestoreName[sizeof(sMagiRestoreName) - 1] = '\0';
    sMagiRestoreIsInstruments = (h->isInstruments != 0);
    sMagiRestoreExpectedBytes = h->total_size;
    sMagiRestoreReceivedBytes = 0;
    {
        SdLock _;
        if (sMagiRestoreFile) sMagiRestoreFile.close();
        SD.remove(SRV_RESTORE_TMP);
        sMagiRestoreFile = SD.open(SRV_RESTORE_TMP, FILE_WRITE);
    }
    if (!sMagiRestoreFile) {
        Serial.println("[CMD] restore: tmp open failed");
        sMagiRestoreExpectedBytes = 0;
        return;
    }
    Serial.printf("[CMD] restore start: name='%s' instr=%d bytes=%u\n",
                  sMagiRestoreName, sMagiRestoreIsInstruments ? 1 : 0,
                  (unsigned)sMagiRestoreExpectedBytes);
}

void onMagiLinkRestoreBody(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (sMagiRestoreExpectedBytes == 0) return;
    if (len < sizeof(MsgRestoreBody)) return;
    if (!sMagiRestoreFile) return;
    const MsgRestoreBody* b = (const MsgRestoreBody*)msg;
    if (b->data_len > 1024) return;
    if (sMagiRestoreReceivedBytes + b->data_len > sMagiRestoreExpectedBytes) {
        Serial.println("[CMD] restore body overshoot — closing & dropping");
        {
            SdLock _;
            sMagiRestoreFile.close();
        }
        sMagiRestoreExpectedBytes = 0;
        return;
    }
    {
        SdLock _;
        sMagiRestoreFile.write(b->data, b->data_len);
    }
    sMagiRestoreReceivedBytes += b->data_len;
    if (sMagiRestoreReceivedBytes >= sMagiRestoreExpectedBytes) {
        {
            SdLock _;
            sMagiRestoreFile.close();
        }
        sMagiRestoreExpectedBytes = 0;
        sMagiRestorePending       = true;
    }
}

// Called from commandsTick when sMagiRestorePending fires.  Moves the
// temp file to its destination, reloads instruments if relevant, and
// reloads the active song if the restored file matches.
static void finaliseMagiLinkRestore() {
    char path[64];
    if (sMagiRestoreIsInstruments) {
        strncpy(path, SRV_INSTRUMENTS_PATH, sizeof(path) - 1);
    } else {
        // Route by extension so setlists/drumtracks go back to their own dirs,
        // not into /songs.  (Cross-dir rename from the /songs temp is fine —
        // same volume.)
        snprintf(path, sizeof(path), "%s/%s",
                 backupDirForName(sMagiRestoreName), sMagiRestoreName);
    }
    path[sizeof(path) - 1] = '\0';

    {
        SdLock _;
        if (SD.exists(path)) SD.remove(path);
        SD.rename(SRV_RESTORE_TMP, path);
    }
    Serial.printf("[CMD] restored '%s' (%u bytes)\n",
                  path, (unsigned)sMagiRestoreReceivedBytes);

    if (sMagiRestoreIsInstruments) {
        loadInstrumentsFromSD();
        Serial.println("[CMD] instruments reloaded after restore");
    }

    if (!sMagiRestoreIsInstruments && srvHasActive && srvActiveName[0] != '\0'
        && strcmp(sMagiRestoreName, srvActiveName) == 0) {
        Serial.printf("[CMD] restored file is the active song — reloading\n");
        reloadActiveSongFromSD();
    }
}

// ── MagiLink generic file save (client → server) state ─────────────────────
// Streams an uploaded {kind}/{name} file to a temp file in the target dir on
// the worker task (SdLock-serialised, like restore); the main loop then
// renames it into place and ACKs.  Completion is detected by byte count — the
// stream has no terminator.  Used by setlists; the planned PC uploader shares
// it.  Only one upload in flight at a time (mutex-serialised send side).
static File     sMagiFileSaveFile;
static uint8_t  sMagiFileSaveKind              = 0;
static char     sMagiFileSaveName[FILE_NAME_LEN] = {};
static char     sMagiFileSaveTmp[96]           = {};
static uint32_t sMagiFileSaveExpectedBytes     = 0;
static uint32_t sMagiFileSaveReceivedBytes     = 0;
static bool     sMagiFileSavePending           = false;

void onMagiLinkFileSaveHeader(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgFileSaveHeader)) return;
    const MsgFileSaveHeader* h = (const MsgFileSaveHeader*)msg;
    const char* dir = fileKindToPath(h->kind);
    if (!dir) {
        Serial.printf("[CMD] file save: unknown kind=%u\n", (unsigned)h->kind);
        sMagiFileSaveExpectedBytes = 0;
        return;
    }
    sMagiFileSaveKind = h->kind;
    strncpy(sMagiFileSaveName, h->name, sizeof(sMagiFileSaveName) - 1);
    sMagiFileSaveName[sizeof(sMagiFileSaveName) - 1] = '\0';
    sMagiFileSaveExpectedBytes = h->total_size;
    sMagiFileSaveReceivedBytes = 0;
    snprintf(sMagiFileSaveTmp, sizeof(sMagiFileSaveTmp), "%s/_save.tmp", dir);
    {
        SdLock _;
        if (!SD.exists(dir)) SD.mkdir(dir);
        if (sMagiFileSaveFile) sMagiFileSaveFile.close();
        SD.remove(sMagiFileSaveTmp);
        sMagiFileSaveFile = SD.open(sMagiFileSaveTmp, FILE_WRITE);
    }
    if (!sMagiFileSaveFile) {
        Serial.println("[CMD] file save: tmp open failed");
        sMagiFileSaveExpectedBytes = 0;
        return;
    }
    Serial.printf("[CMD] file save start: kind=%u name='%s' bytes=%u\n",
                  (unsigned)sMagiFileSaveKind, sMagiFileSaveName,
                  (unsigned)sMagiFileSaveExpectedBytes);
}

void onMagiLinkFileSaveBody(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (sMagiFileSaveExpectedBytes == 0) return;
    if (len < sizeof(MsgFileSaveBody)) return;
    if (!sMagiFileSaveFile) return;
    const MsgFileSaveBody* b = (const MsgFileSaveBody*)msg;
    if (b->data_len > 1024) return;
    if (sMagiFileSaveReceivedBytes + b->data_len > sMagiFileSaveExpectedBytes) {
        Serial.println("[CMD] file save body overshoot — dropping");
        { SdLock _; sMagiFileSaveFile.close(); }
        sMagiFileSaveExpectedBytes = 0;
        return;
    }
    { SdLock _; sMagiFileSaveFile.write(b->data, b->data_len); }
    sMagiFileSaveReceivedBytes += b->data_len;
    if (sMagiFileSaveReceivedBytes >= sMagiFileSaveExpectedBytes) {
        { SdLock _; sMagiFileSaveFile.close(); }
        sMagiFileSaveExpectedBytes = 0;
        sMagiFileSavePending       = true;
    }
}

// Called from commandsTick when sMagiFileSavePending fires.  Renames the temp
// into place (same dir, so atomic) and ACKs the client.
static void finaliseMagiLinkFileSave() {
    const char* dir = fileKindToPath(sMagiFileSaveKind);
    bool ok = false;
    if (dir) {
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", dir, sMagiFileSaveName);
        SdLock _;
        if (SD.exists(path)) SD.remove(path);
        ok = SD.rename(sMagiFileSaveTmp, path);
        if (!ok) SD.remove(sMagiFileSaveTmp);
    }
    Serial.printf("[CMD] file saved kind=%u '%s' (%u bytes) %s\n",
                  (unsigned)sMagiFileSaveKind, sMagiFileSaveName,
                  (unsigned)sMagiFileSaveReceivedBytes, ok ? "OK" : "FAIL");

    MsgFileSaveAck ack;
    ack.kind = sMagiFileSaveKind;
    ack.ok   = ok ? 1 : 0;
    gMagiLink.acquireMutex();
    gMagiLink.send(&ack, sizeof(ack));
    gMagiLink.releaseMutex();
}

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
    Serial.printf("[STOPSRC] save-header (full song push) name='%s' t=%lu\n",
                  h->name, millis());
    crashSetPhase(CP_SONG_PUSH);   // breadcrumb until finalise re-arms to idle
    sequencerStop();
    // Also drop srvHasActive so sequencerTick's early-return guards the
    // buffer even if a tick was already past the seqRunning check when this
    // header arrived.  Restored in finaliseMagiLinkSongSave().
    srvHasActive = false;
    // Those guards only stop NEW walks.  A tick/MIDI-poll already in flight on
    // the loop task can be parked mid-walk in a midiTx FIFO wait for several
    // ms; the body chunks that follow overwrite the note pool it is walking,
    // and a torn `next` index reads far out of bounds (LoadProhibited) or
    // breaks list circularity (watchdog).  Wait the in-flight walk out —
    // dispatch is sequential on this task, so once we return no walker can
    // race the body writes.  Bounded: a walk is µs–ms, never near 100 ms.
    for (int i = 0; i < 100 && sequencerSongReaderBusy(); i++) delay(1);
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

// ── Save-active (autosave shortcut): client → server ─────────────────────
// Tells us to write the current srvActiveBuf to SD under the given name.
// No song bytes on the wire — patches + note-sets have kept srvActiveBuf
// in sync with the client's memory.  SD work deferred to commandsTick.
static char sSaveActiveName[SRV_NAME_MAX] = {};
static bool sSaveActivePending            = false;
static bool sSaveActiveIsAutosave         = false;

void onMagiLinkSaveActive(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgSaveActive)) return;
    const MsgSaveActive* m = (const MsgSaveActive*)msg;
    strncpy(sSaveActiveName, m->name, sizeof(sSaveActiveName) - 1);
    sSaveActiveName[sizeof(sSaveActiveName) - 1] = '\0';
    sSaveActiveIsAutosave = (m->is_autosave != 0);
    sSaveActivePending = true;
}

// Called from commandsTick when sSaveActivePending fires.
static void finaliseSaveActive() {
    if (!srvHasActive) {
        Serial.println("[CMD] save-active: no active song — ignoring");
        return;
    }
    if (sSaveActiveName[0] == '\0') {
        Serial.println("[CMD] save-active: empty name — ignoring");
        return;
    }
    crashSetPhase(CP_AUTOSAVE);
    // Autosave goes to /autosave/; explicit Save goes to /songs/ AND wipes the
    // matching /autosave/ draft (it's now superseded by the committed file).
    // Drafts are never auto-recovered on boot — see the autosave-policy note.
    const char* dir = sSaveActiveIsAutosave ? SRV_AUTOSAVE_DIR : SRV_SONGS_DIR;
    char path[48];
    snprintf(path, sizeof(path), "%s/%s.mgt", dir, sSaveActiveName);
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    SdLock _;
    if (!writeSongVerified(path, song)) {
        Serial.printf("[CMD] save-active: verify failed '%s' — kept previous file\n", path);
        crashSetPhase(CP_IDLE);
        return;
    }
    if (!sSaveActiveIsAutosave) {
        // Explicit save committed — any pending autosave draft is now redundant.
        char autopath[48];
        snprintf(autopath, sizeof(autopath), "%s/%s.mgt",
                 SRV_AUTOSAVE_DIR, sSaveActiveName);
        if (SD.exists(autopath)) {
            SD.remove(autopath);
            Serial.printf("[CMD] cleared autosave '%s'\n", autopath);
        }
        snprintf(srvActiveName, sizeof(srvActiveName), "%s.mgt", sSaveActiveName);
    }
    crashSetPhase(CP_IDLE);   // autosave/save complete
}

// ── New-song: client → server ────────────────────────────────────────────
// Server runs initSong() on its in-memory copy so it matches the
// freshly-wiped client copy.  No bytes streamed.
static bool sNewSongPending = false;

void onMagiLinkNewSong(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgNewSong)) return;
    sNewSongPending = true;
}

// ── Set-OFF: client → server "-- OFF -- / No Song" ────────────────────────────
static bool sSetOffPending = false;

void onMagiLinkSetNoSong(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgSetNoSong)) return;
    sSetOffPending = true;   // deferred to commandsTick — same as loadSong(0)
}

static void finaliseNewSong() {
    Serial.printf("[STOPSRC] finaliseNewSong t=%lu\n", millis());
    sequencerStop();
    // Stamp a fresh SongFileHeader at the front of srvActiveBuf, then
    // initSong() the Song body that follows.
    SongFileHeader hdr;
    hdr.magic   = SONG_FILE_MAGIC;
    hdr.version = SONG_FILE_VERSION;
    memset(hdr._pad, 0, sizeof(hdr._pad));
    memcpy(srvActiveBuf, &hdr, sizeof(SongFileHeader));
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    initSong(song);
    srvActiveBufLen = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);
    srvHasActive    = true;  gSamplePreloadDirty = true;
    srvActiveName[0] = '\0';
    sequencerReset();
    Serial.println("[CMD] new-song: server memory wiped");
}

// Called from commandsTick when sMagiSavePending fires.  Writes the
// in-memory song to SD if a name was provided; either way, marks
// srvHasActive and resets the sequencer.
static void finaliseMagiLinkSongSave() {
    srvActiveBufLen = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);
    srvHasActive    = true;  gSamplePreloadDirty = true;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));

    if (sMagiSaveName[0] != '\0') {
        char path[48];
        snprintf(path, sizeof(path), "%s/%s.mgt", SRV_SONGS_DIR, sMagiSaveName);
        SdLock _;
        if (writeSongVerified(path, song))
            snprintf(srvActiveName, sizeof(srvActiveName), "%s.mgt", sMagiSaveName);
        else
            Serial.printf("[CMD] save: verify failed '%s' — kept previous file\n", path);
    } else {
        Serial.println("[CMD] save: in-memory push (no SD write)");
    }
    Serial.printf("[STOPSRC] finaliseMagiLinkSongSave name='%s' t=%lu\n",
                  sMagiSaveName, millis());
    sequencerStop();
    sequencerReset();
    crashSetPhase(CP_IDLE);   // song-push complete
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
        // Append every /drumtracks/*.txt (including gm_map.txt — it's a
        // legitimate part of the user's drum-import config).  The client
        // restore path routes by extension: .mgt → /songs/, .txt → /drumtracks/.
        File dt = SD.open(SRV_DRUMTRACKS_DIR);
        if (dt && dt.isDirectory()) {
            while (count < SRV_MAX_FILES) {
                File entry = dt.openNextFile();
                if (!entry) break;
                if (!entry.isDirectory()) {
                    const char* nm = entry.name();
                    const char* sl = nm ? strrchr(nm, '/') : nullptr;
                    if (sl) nm = sl + 1;
                    int nlen = nm ? (int)strlen(nm) : 0;
                    if (nlen >= 4 && (nm[nlen-4] == '.')
                        && (nm[nlen-3] == 't' || nm[nlen-3] == 'T')
                        && (nm[nlen-2] == 'x' || nm[nlen-2] == 'X')
                        && (nm[nlen-1] == 't' || nm[nlen-1] == 'T')) {
                        strncpy(names[count], nm, SRV_FNAME_MAX - 1);
                        names[count][SRV_FNAME_MAX - 1] = '\0';
                        sizes[count] = (uint32_t)entry.size();
                        count++;
                    }
                }
                entry.close();
            }
            dt.close();
        }
        // Append every /setlists/*.set plus the master.txt catalog so the
        // performer's setlists travel with the backup.  Restore routes .set →
        // /setlists by extension, and master.txt → /setlists by name.
        File sld = SD.open(SRV_SETLISTS_DIR);
        if (sld && sld.isDirectory()) {
            while (count < SRV_MAX_FILES) {
                File entry = sld.openNextFile();
                if (!entry) break;
                if (!entry.isDirectory()) {
                    const char* nm = entry.name();
                    const char* sl = nm ? strrchr(nm, '/') : nullptr;
                    if (sl) nm = sl + 1;
                    int nlen = nm ? (int)strlen(nm) : 0;
                    bool isSet = (nlen >= 4 && (nm[nlen-4] == '.')
                        && (nm[nlen-3] == 's' || nm[nlen-3] == 'S')
                        && (nm[nlen-2] == 'e' || nm[nlen-2] == 'E')
                        && (nm[nlen-1] == 't' || nm[nlen-1] == 'T'));
                    bool isMaster = (nm && strcasecmp(nm, "master.txt") == 0);
                    if (isSet || isMaster) {
                        strncpy(names[count], nm, SRV_FNAME_MAX - 1);
                        names[count][SRV_FNAME_MAX - 1] = '\0';
                        sizes[count] = (uint32_t)entry.size();
                        count++;
                    }
                }
                entry.close();
            }
            sld.close();
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

        // Open the file.  Path depends on extension (same mapping as restore):
        // instruments.mgt → root, .txt → /drumtracks, .set → /setlists,
        // everything else → /songs.
        char path[80];
        if (strcmp(names[i], "instruments.mgt") == 0) {
            strncpy(path, SRV_INSTRUMENTS_PATH, sizeof(path) - 1);
        } else {
            snprintf(path, sizeof(path), "%s/%s",
                     backupDirForName(names[i]), names[i]);
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
            } else if (hdr.version == 19) {
                loaded = songMigrateV19FromFile(f, song);
                if (loaded) Serial.printf("[CMD] migrated v19->v%d '%s'\n", SONG_FILE_VERSION, path);
            } else if (hdr.version == 18) {
                loaded = songMigrateV18FromFile(f, song);
                if (loaded) Serial.printf("[CMD] migrated v18->v%d '%s'\n", SONG_FILE_VERSION, path);
            } else if (hdr.version == 17) {
                loaded = songMigrateV17FromFile(f, song);
                if (loaded) Serial.printf("[CMD] migrated v17->v%d '%s'\n", SONG_FILE_VERSION, path);
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
    srvHasActive = true;  gSamplePreloadDirty = true;
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
    // Apply queued note edits first, so anything below that reads or saves the
    // song (save-active, autosave) sees them.  Skipped-but-consumed while a
    // full push is in flight (srvHasActive false) — the push supersedes them.
    while (sNoteSetQTail != sNoteSetQHead) {
        const MsgNoteSet& m = sNoteSetQ[sNoteSetQTail & (NOTE_SET_Q_LEN - 1)];
        if (srvHasActive) {
            Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
            if (m.pattern < song->numPatterns) {
                NoteGrid grid(song->notePool, &song->noteFreeHead,
                              &song->patterns[m.pattern].noteHead);
                // Always set — NoteGrid::set clears internally when *all*
                // fields are empty, but persists attr-only / vel-only rows.
                grid.set(m.row, m.col, m.note);
            }
        }
        sNoteSetQTail++;
    }

    // Audition fires after the drain so it plays the just-set cell value.
    if (sAuditionPending) {
        sAuditionPending = false;
        sequencerAuditionNote(sAuditionPat, sAuditionRow, sAuditionCol);
    }

    // PSRAM sample preload follows the active song.  Deferred until the
    // sequencer is stopped and the player is quiet — samplePreloadSync
    // returns false (retry next tick) while anything is still sounding.
    if (gSamplePreloadDirty && !sequencerIsRunning() && srvHasActive) {
        Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
        if (samplePreloadSync(song)) gSamplePreloadDirty = false;
    }

    // Sample editor: persist trim edits, serve meta/overview requests.
    if (sTrimSavePending) {
        sTrimSavePending = false;
        sampleTrimSave();
    }
    if (sSampleInfoPending) {
        sSampleInfoPending = false;
        sendSampleInfo(sSampleInfoId, sSampleInfoVs, sSampleInfoVe);
    }
    if (sSampleSpecPending) {
        sSampleSpecPending = false;
        sendSampleSpectrum(sSampleSpecId);
    }

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
        // Mirror the load-by-index path: stop+reset the sequencer and
        // sync the on-screen cursor so the row highlight tracks the
        // song the client just pushed to us via the setlist.
        if (srvHasActive) {
            Serial.printf("[STOPSRC] load-by-name '%s' t=%lu\n",
                          srvLoadByNameStr, millis());
            sequencerStop();
            sequencerReset();
            int matchIdx = -1;
            for (int i = 0; i < numSongs; i++) {
                if (strcmp(songNames[i], srvLoadByNameStr) == 0) {
                    matchIdx = i;
                    break;
                }
            }
            if (matchIdx >= 0) {
                cursor = matchIdx + SONG_IDX_OFFSET;
                if (cursor >= scrollOffset + UI_VISIBLE_ROWS)
                    scrollOffset = cursor - UI_VISIBLE_ROWS + 1;
                if (cursor < scrollOffset)
                    scrollOffset = cursor;
                needsFullRedraw = true;
            }
        }
    }
    if (srvLoadPending) {
        srvLoadPending = false;
        int fileIdx = (int)srvLoadPendingPage * SL_PER_PKT + (int)srvLoadPendingIdx;
        sendSongData(srvLoadPendingPage, srvLoadPendingIdx);
        if (srvHasActive) {
            Serial.printf("[STOPSRC] load-by-index fileIdx=%d t=%lu\n",
                          fileIdx, millis());
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
        if (srvHasActive) {
            Serial.printf("[STOPSRC] finaliseSongSave t=%lu\n", millis());
            sequencerStop();
            sequencerReset();
        }
    }
    if (sMagiSavePending) {
        sMagiSavePending = false;
        finaliseMagiLinkSongSave();
    }
    if (sSaveActivePending) {
        sSaveActivePending = false;
        finaliseSaveActive();
    }
    if (sNewSongPending) {
        sNewSongPending = false;
        finaliseNewSong();
    }
    if (sSetOffPending) {
        sSetOffPending = false;
        // Same as the server's local "-- OFF --" selection: stop, drop the
        // active song, tell the client, and sync the on-screen cursor to OFF.
        sequencerStop();
        srvHasActive    = false;
        srvActiveName[0] = '\0';
        if (pairingIsConnected()) {
            MsgNoSong msg;
            gMagiLink.acquireMutex();
            gMagiLink.send(&msg, sizeof(msg));
            gMagiLink.releaseMutex();
        }
        cursor          = 0;   // 0 = OFF (SONG_IDX_OFFSET)
        needsFullRedraw = true;
    }
    if (sMagiRestorePending) {
        sMagiRestorePending = false;
        finaliseMagiLinkRestore();
    }
    if (sMagiFileSavePending) {
        sMagiFileSavePending = false;
        finaliseMagiLinkFileSave();
    }
    if (srvInstLoadPending) {
        srvInstLoadPending = false;
        sendInstrumentsData();
    }
    if (srvInstSavePending) {
        srvInstSavePending = false;
        saveInstrumentsToSD();
    }
    if (srvFileListPending) {
        srvFileListPending = false;
        sendFileList(srvFileListPendingKind, srvFileListPendingPage);
    }
    if (srvFileLoadPending) {
        srvFileLoadPending = false;
        sendFile(srvFileLoadPendingKind, srvFileLoadPendingName);
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
            // Also wipe any matching autosave draft so a recovered file
            // can't resurrect the song we just deleted.
            char autopath[48];
            snprintf(autopath, sizeof(autopath), "%s/%s.mgt",
                     SRV_AUTOSAVE_DIR, srvDeleteName);
            if (SD.exists(autopath)) {
                SD.remove(autopath);
                Serial.printf("[CMD] deleted autosave '%s'\n", autopath);
            }
        }
        char expectedActive[SRV_FNAME_MAX];
        snprintf(expectedActive, sizeof(expectedActive), "%s.mgt", srvDeleteName);
        if (srvHasActive && strcmp(expectedActive, srvActiveName) == 0)
            srvHasActive = false;
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

        case MSG_FILE_LIST_REQ:
            if (len < (int)sizeof(MsgFileListReq)) return;
            {
                const MsgFileListReq* req = (const MsgFileListReq*)data;
                srvFileListPendingKind = req->kind;
                srvFileListPendingPage = req->page;
                srvFileListPending     = true;
            }
            break;

        case MSG_FILE_LOAD_REQ:
            if (len < (int)sizeof(MsgFileLoadReq)) return;
            {
                const MsgFileLoadReq* req = (const MsgFileLoadReq*)data;
                srvFileLoadPendingKind = req->kind;
                strncpy(srvFileLoadPendingName, req->name, FILE_NAME_LEN - 1);
                srvFileLoadPendingName[FILE_NAME_LEN - 1] = '\0';
                srvFileLoadPending = true;
            }
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
                // NoteGrid::set relinks the live note pool the sequencer walks
                // on the loop task — applying it here (MagiLink worker task)
                // races a playing walker.  Queue it; commandsTick applies it
                // on the loop task, serialised with sequencerTick.
                // Queue full is effectively impossible (client paces edits at
                // touch speed), but never drop silently — wait for a slot.
                while ((uint8_t)(sNoteSetQHead - sNoteSetQTail) >= NOTE_SET_Q_LEN)
                    delay(1);
                sNoteSetQ[sNoteSetQHead & (NOTE_SET_Q_LEN - 1)] =
                    *(const MsgNoteSet*)data;
                sNoteSetQHead++;
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
                midiSendRawBytes(m->data, m->midiLen < 3 ? m->midiLen : 3);
            }
            break;

        case MSG_PLAY:
            sequencerResume();
            break;

        case MSG_STOP:
            Serial.printf("[STOPSRC] MSG_STOP from client running=%d t=%lu\n",
                          (int)sequencerIsRunning(), millis());
            if (sequencerIsRunning()) {
                sequencerStop();    // stop in place
            } else {
                sequencerPanic();   // all notes off on all channels
                sequencerReset();   // snap to top of block
            }
            break;

        case MSG_PIXELPOST_SET_EFFECT:
            if (len < (int)sizeof(MsgPixelpostSetEffect)) return;
            {
                extern void pixelpostSetEffect(uint8_t idx);
                pixelpostSetEffect(((const MsgPixelpostSetEffect*)data)->effectIdx);
            }
            break;

        case MSG_ORGAN:
            if (len < (int)sizeof(MsgOrgan)) return;
            {
                extern volatile int      gOrganScreenReq;
                extern volatile uint16_t gOrganBarDirty;
                const MsgOrgan* m = (const MsgOrgan*)data;
                if (m->op == ORGAN_OP_ENTER)      gOrganScreenReq = 1;
                else if (m->op == ORGAN_OP_EXIT)  gOrganScreenReq = 2;
                else if (m->op == ORGAN_OP_SET) {
                    organSetDrawbar(m->index, m->value);   // task-safe (volatile write)
                    if (m->index < 16) gOrganBarDirty |= (uint16_t)(1u << m->index);
                }
                else if (m->op == ORGAN_OP_TYPE)      organSetType(m->value);
                else if (m->op == ORGAN_OP_VIBCHORUS) organSetVibChorus(m->value);
                else if (m->op == ORGAN_OP_LESLIE)    organSetLeslie(m->value);
                else if (m->op == ORGAN_OP_DRIVE)     organSetDrive(m->value);
                else if (m->op == ORGAN_OP_PARAM)     organSetParam(m->index, m->value);
                else if (m->op == ORGAN_OP_PROCSEL)   organSetProcSound(m->value);
                else if (m->op == ORGAN_OP_REVERB)    organSetReverb(m->value);
            }
            break;

        case MSG_PIXELPOST_SET_SLIDER:
            if (len < (int)sizeof(MsgPixelpostSetSlider)) return;
            {
                extern void pixelpostSetSlider(uint8_t value);
                pixelpostSetSlider(((const MsgPixelpostSetSlider*)data)->value);
            }
            break;

        case MSG_PIXELPOST_SET_TOUCHPAD:
            if (len < (int)sizeof(MsgPixelpostSetTouchpad)) return;
            {
                extern void pixelpostSetTouchpad(uint8_t x, uint8_t y, bool touched);
                const MsgPixelpostSetTouchpad* m = (const MsgPixelpostSetTouchpad*)data;
                pixelpostSetTouchpad(m->x, m->y, m->touched != 0);
            }
            break;

        case MSG_PIXELPOST_POWER_OFF:
            if (len < (int)sizeof(MsgPixelpostPowerOff)) return;
            {
                extern void pixelpostSetPowerOff(bool off);
                pixelpostSetPowerOff(((const MsgPixelpostPowerOff*)data)->off != 0);
            }
            break;

        case MSG_PIXELPOST_SET_POST_COUNT:
            if (len < (int)sizeof(MsgPixelpostSetPostCount)) return;
            {
                extern void pixelpostSetPostCount(uint8_t count);
                pixelpostSetPostCount(((const MsgPixelpostSetPostCount*)data)->count);
            }
            break;

        case MSG_PIXELPOST_FIRMWARE_UPDATE:
            {
                // Client FW button → tell the posts to OTA directly from the
                // Mac's nginx box over the home WiFi (bench/home route).  The gig
                // route is the server's own FLASH screen (field_flash.ino); the
                // two don't collide — this one points the posts at an external
                // network, not the server softAP.
                extern void pixelpostSendFirmwareUpdate(const char* ssid, const char* pwd, const char* url);
                pixelpostSendFirmwareUpdate(PP_MAC_OTA_SSID, PP_MAC_OTA_PSK, PP_MAC_OTA_URL);
                Serial.println("[PP-TX] client FW trigger → posts OTA from Mac nginx");
            }
            break;

        case MSG_PIXELPOST_SET_FLASH_CTRL:
            if (len < (int)sizeof(MsgPixelpostSetFlashCtrl)) return;
            {
                extern void pixelpostSetFlashCtrl(uint8_t value);
                pixelpostSetFlashCtrl(((const MsgPixelpostSetFlashCtrl*)data)->flashCtrl);
            }
            break;

        case MSG_PIXELPOST_OVERRIDE:
            if (len < (int)sizeof(MsgPixelpostOverride)) return;
            {
                extern void pixelpostManualCycle(int8_t delta);
                extern void pixelpostManualWhite(bool on);
                extern void pixelpostManualRelease();
                switch (((const MsgPixelpostOverride*)data)->op) {
                    case PPO_NEXT:      pixelpostManualCycle(+1);    break;
                    case PPO_PREV:      pixelpostManualCycle(-1);    break;
                    case PPO_WHITE_ON:  pixelpostManualWhite(true);  break;
                    case PPO_WHITE_OFF: pixelpostManualWhite(false); break;
                    case PPO_RELEASE:   pixelpostManualRelease();    break;
                    default: break;
                }
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
                // Deferred behind the note-set queue: the client sends
                // NOTE_SET then NOTE_AUDITION for a cell tap, and the set is
                // now applied from commandsTick — auditioning inline here
                // would play the cell's OLD value.  Latest-wins single slot.
                const MsgNoteAudition* a = (const MsgNoteAudition*)data;
                sAuditionPat     = a->pattern;
                sAuditionRow     = a->row;
                sAuditionCol     = a->col;
                sAuditionPending = true;
            }
            break;

        case MSG_AUDITION_RAW_NOTE:
            if (len < (int)sizeof(MsgAuditionRawNote)) return;
            {
                const MsgAuditionRawNote* a = (const MsgAuditionRawNote*)data;
                sequencerAuditionRawNote(a->channel, a->note, a->velocity, a->col);
            }
            break;

        case MSG_AUDITION_PROGRAM:
            if (len < (int)sizeof(MsgAuditionProgram)) return;
            {
                const MsgAuditionProgram* a = (const MsgAuditionProgram*)data;
                sequencerAuditionProgram(a->channel, a->program);
            }
            break;

        case MSG_SAMPLE_EDIT:
            if (len < (int)sizeof(MsgSampleEdit)) return;
            {
                const MsgSampleEdit* m = (const MsgSampleEdit*)data;
                if (m->op == SAMPLE_EDIT_GET) {
                    sSampleInfoId      = m->sampleId;   // overview reads SD —
                    sSampleInfoVs      = m->startFrame; // deferred to the loop
                    sSampleInfoVe      = m->endFrame;
                    sSampleInfoPending = true;
                } else if (m->op == SAMPLE_EDIT_SET) {
                    // RAM table updates inline (playback picks it up on the
                    // next trigger); the SD write + cache re-bake defer.
                    sampleTrimSet(m->sampleId, m->startFrame, m->endFrame, m->loop);
                    sTrimSavePending    = true;
                    gSamplePreloadDirty = true;
                } else if (m->op == SAMPLE_EDIT_STOP) {
                    samplePlayerStop();                 // ends a looping audition
                } else if (m->op == SAMPLE_EDIT_SPEC) {
                    sSampleSpecId      = m->sampleId;   // Goertzel over SD data —
                    sSampleSpecPending = true;          // deferred to the loop
                }
            }
            break;

        default:
            break;
    }
}

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

// Load the persisted channel idx (0/1/2 → ch 1/6/11) without touching WiFi.
// Used by serverWifiBringUp() to pick the softAP channel before
// WiFi.softAP() is called.
uint8_t wifiChannelLoad() {
    Preferences prefs;
    prefs.begin(WIFI_NVS_NS, true);
    uint8_t idx = prefs.getUChar("idx", 0);
    if (idx > 2) idx = 0;
    prefs.end();
    return idx;
}

static void wifiChannelApply(uint8_t idx) {
    if (idx > 2) return;
    Preferences prefs;
    prefs.begin(WIFI_NVS_NS, false);
    prefs.putUChar("idx", idx);
    prefs.end();
    // Live channel-switch via WiFi.softAP(ssid, psk, ch) is unreliable on
    // Arduino-ESP32 — the call can silently fail when the AP is already
    // running, leaving the radio in a default fallback state (the AP
    // disappears or comes back as "ESP_xxxxxx" with no SSID/PSK).  Persist
    // the new index and reboot so serverWifiBringUp() runs its full softAP +
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
// Autosave drafts land here, NOT /songs/.  They are never promoted or
// recovered automatically (see the autosave-policy note below) — an explicit
// Save writes /songs/ and clears the matching draft; an explicit load or
// delete clears it too.
#define SRV_AUTOSAVE_DIR "/autosave"

// Song layout constants
#define SRV_HDR_SIZE      ((uint32_t)sizeof(SongFileHeader))   // Song starts here in srvActiveBuf
#define SRV_SONG_XFER_MAX (sizeof(SongFileHeader) + sizeof(Song) + 64)

static bool srvSdOk = false;

// ── Instruments ───────────────────────────────────────────────────────────────

#define SRV_INSTRUMENTS_PATH "/instruments.mgt"
#define SRV_INSTRUMENTS_TMP  "/instruments.tmp"   // save lands here, then renames
#define INSTR_FILE_MAGIC     0x494E5354UL   // "INST"
#define INSTR_FILE_VERSION   2

struct InstrFileHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  _pad[3];
};

// The instrument bank: MAX_INSTRUMENTS entries in PSRAM (4.6 KB — control-rate
// data, so not internal RAM).  Allocated at the top of commandsInit(); every
// user below checks for the nullptr a double allocation failure would leave.
Instrument* srvInstruments = nullptr;
#define SRV_INSTRUMENTS_BYTES (sizeof(Instrument) * MAX_INSTRUMENTS)
// Scratch for a full /songs enumeration (see "Shared scratch" further down).
static char (*srvSongScratch)[SRV_FNAME_MAX] = nullptr;

// True when the instruments file was on the card at boot but would not load
// (SD fault, bad header).  srvInstruments then holds the built-in defaults, and
// writing those out would replace the user's instruments with them — so
// saveInstrumentsToSD refuses until a load succeeds (e.g. after a restore of
// instruments.mgt).
static bool srvInstFileUnreadable = false;

static void initDefaultInstruments() {
    if (!srvInstruments) return;
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

// Reads the file into a scratch copy and only overwrites srvInstruments once
// the whole file has been read and its header checked — a missing file, a bad
// header or a short read leaves the in-memory instruments exactly as they were.
static bool loadInstrumentsFromSD() {
    if (!srvInstruments) return false;
    uint8_t* tmp = (uint8_t*)heap_caps_malloc(SRV_INSTRUMENTS_BYTES, MALLOC_CAP_SPIRAM);
    if (!tmp) tmp = (uint8_t*)malloc(SRV_INSTRUMENTS_BYTES);
    if (!tmp) {
        Serial.println("[CMD] loadInstruments: FAIL (no scratch memory)");
        return false;
    }
    bool ok = false;
    {
        SdLock _;
        File f = SD.open(SRV_INSTRUMENTS_PATH);
        if (f) {
            InstrFileHeader hdr;
            ok = (f.read((uint8_t*)&hdr, sizeof(hdr)) == (int)sizeof(hdr)
                  && hdr.magic   == INSTR_FILE_MAGIC
                  && hdr.version == INSTR_FILE_VERSION);
            if (ok)
                ok = (f.read(tmp, SRV_INSTRUMENTS_BYTES)
                      == (int)SRV_INSTRUMENTS_BYTES);
            f.close();
        }
    }
    // (no sanitization needed — instruments no longer carry midiChannel)
    if (ok) {
        memcpy(srvInstruments, tmp, SRV_INSTRUMENTS_BYTES);
        srvInstFileUnreadable = false;   // what is in memory is the file again
    }
    free(tmp);
    Serial.printf("[CMD] loadInstruments: %s\n", ok ? "OK" : "FAIL/missing");
    return ok;
}

// Written to a temp file first and renamed into place only if every byte went
// out, so a failed write can't leave a truncated instruments file behind.
static void saveInstrumentsToSD() {
    if (!srvInstruments) return;
    if (srvInstFileUnreadable) {
        Serial.println("[CMD] saveInstruments: REFUSED — " SRV_INSTRUMENTS_PATH
                       " was unreadable at boot and memory holds defaults; "
                       "file left untouched (restore or delete it to recover)");
        return;
    }
    SdLock _;
    SD.remove(SRV_INSTRUMENTS_TMP);
    File f = SD.open(SRV_INSTRUMENTS_TMP, FILE_WRITE);
    if (!f) { Serial.println("[CMD] saveInstruments: open failed"); return; }
    InstrFileHeader hdr;
    hdr.magic = INSTR_FILE_MAGIC; hdr.version = INSTR_FILE_VERSION;
    hdr._pad[0] = hdr._pad[1] = hdr._pad[2] = 0;
    bool ok = (f.write((const uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr));
    if (ok)
        ok = (f.write((const uint8_t*)srvInstruments, SRV_INSTRUMENTS_BYTES)
              == SRV_INSTRUMENTS_BYTES);
    f.close();
    if (!ok) {
        SD.remove(SRV_INSTRUMENTS_TMP);
        Serial.println("[CMD] saveInstruments: write FAILED — previous file kept");
        return;
    }
    SD.remove(SRV_INSTRUMENTS_PATH);
    if (!SD.rename(SRV_INSTRUMENTS_TMP, SRV_INSTRUMENTS_PATH)) {
        Serial.println("[CMD] saveInstruments: rename FAILED — data left in "
                       SRV_INSTRUMENTS_TMP);
        return;
    }
    Serial.println("[CMD] saveInstruments: OK");
}

static bool srvInstSavePending   = false;
static bool srvInstLoadPending   = false;

// ── Active song buffer ─────────────────────────────────────────────────────────
// The active song: what the sequencer plays and what client patches /
// note-sets are applied to.
// PSRAM — 36 KB of control-rate data (sequencer row walks, patches, CRC);
// internal RAM is reserved for the radios + audio hot loops (see
// feedback_internal_ram_budget).  Allocated in commandsInit.
uint8_t* srvActiveBuf     = nullptr;
uint32_t srvActiveBufLen  = 0;
// False when srvActiveBuf could not be allocated at all — every path that
// writes a Song through that pointer must bail rather than fault.
bool     srvBufOk         = false;
static uint8_t* srvLoadScratch = nullptr;           // sizeof(Song) staging, see commandsInit
static uint8_t* srvPushScratch = nullptr;           // sizeof(Song) staging for full-song pushes
static char     srvActiveName[SRV_FNAME_MAX] = {};  // filename (with .mgt)
bool     srvHasActive     = false;

#define SRV_RESTORE_TMP "/songs/_restore.tmp"       // restore upload lands here, then renames

// MSG_NOTE_SET queue — produced on the MagiLink worker task, consumed by
// commandsTick on the loop task, which keeps note edits in arrival order with
// the song operations below.  The loop task does not touch the note pool
// itself: it hands each edit to the MIDI task (sequencerApplyNoteSet), the one
// task that walks the pool, so a relink can never land under a playing walk.
// SPSC ring: head written only by the worker, tail only by
// the loop; uint8_t indices wrap at 256, a multiple of the queue length, so
// the masks stay consistent across wrap.
#define NOTE_SET_Q_LEN 64
static MsgNoteSet       sNoteSetQ[NOTE_SET_Q_LEN];
static uint16_t         sNoteSetSeq[NOTE_SET_Q_LEN];   // arrival stamp per entry
static volatile uint8_t sNoteSetQHead = 0;
static volatile uint8_t sNoteSetQTail = 0;

// ── Ordered song operations ───────────────────────────────────────────────────
// Everything the client sends that reads, replaces or saves the active song is
// queued here by the MagiLink worker and run by commandsTick on the loop task
// IN ARRIVAL ORDER.  The client sends pairs back to back — "save X, then load
// Y"; "new song, patch its name, save it" — and both land before the loop task
// runs once.  With one flag per kind, serviced in a fixed order, the load ran
// first and Y was written out as X.mgt.
//
// Note edits keep their own ring (they come in bursts) but carry the same
// arrival stamp, so commandsTick merges the two streams into one sequence.  A
// song patch is applied inline for immediacy unless an operation is queued or
// running, in which case it waits its turn here too.
//
// Single producer (the worker), single consumer (the loop task): head is
// written only by the worker, tail only by the loop.  The tail advances AFTER
// an operation has run, so "queue not empty" also means "one is in progress".
enum {
    OP_LOAD_NAME = 1,   // name
    OP_LOAD_INDEX,      // a = page, b = index
    OP_SAVE_ACTIVE,     // name, a = isAutosave
    OP_NEW_SONG,
    OP_SET_OFF,
    OP_PUSH_DONE,       // a full-song push finished streaming in
    OP_DELETE,          // name
    OP_PATCH,           // offset, len, data
};
struct SongOp {
    uint16_t seq;
    uint8_t  kind, a, b, len;
    uint16_t offset;
    union {
        char    name[SRV_NAME_MAX];
        uint8_t data[SONG_PATCH_MAX];
    };
};
#define SONG_OP_Q_LEN 16                        // power of two (uint8 index wrap)
static SongOp*           sSongOpQ    = nullptr; // PSRAM, see commandsInit
static volatile uint8_t  sSongOpHead = 0;
static volatile uint8_t  sSongOpTail = 0;
static uint16_t          sArrivalSeq = 0;       // worker only

// /songs changed — refresh the on-screen list.  Set by the loop task and by
// the worker (a finished restore); consumed by commandsTick.
static volatile bool sSongListDirty = false;

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

// ── Deferred active-song pull + song-list request ──────────────────────────────
// Streaming the active song is ~35 KB (~750ms on a good link) and a list is an
// SD directory scan; doing either inside the MagiLink callback would hold up
// the link worker.  The handler sets a flag and commandsTick does the work on
// the loop task.  (Loads themselves are queued as song operations, above.)
static bool    srvActiveSongReqPending = false;  // client pulled the active song
static bool    srvListPending       = false;
static uint8_t srvListPendingPage   = 0;
static bool    srvSampleListPending     = false;  // manifest sync + page, SD work
static uint8_t srvSampleListPendingPage = 0;

// ── Generic file server ───────────────────────────────────────────────────────
// FileKind enum (MagiMsg.h) → SD path lookup.  Whitelist controls what the
// client can enumerate / fetch (see fileKindToPath).  Add a row there and a
// FileKind entry to expose another directory.
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

// Most names one directory listing returns to the client (the client caches
// the same number).  The scratch is PSRAM, allocated in commandsInit: 128
// names x 48 bytes is far too big for the loop-task stack and too big to want
// in internal RAM.  Loop task only (sendFileList), so one buffer is race-free.
#define SRV_FILE_LIST_MAX 128
static char (*srvFileScratch)[FILE_NAME_LEN] = nullptr;

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

// (History: these two were written for the USB-drive mode, since removed.  They
// stay as cheap insurance against a card left wedged by a reset mid-transfer.)
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
    // Report what the PSRAM chip actually gave us before asking for any of it —
    // a dead or unconfigured PSRAM reads as size 0 here, which is the one fact
    // that distinguishes "bad hardware" from "heap merely full".
    Serial.printf("[CMD] PSRAM: %u bytes total, %u free\n",
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());

    // Control-rate tables (see srvAllocPsram): before anything below uses them.
    srvInstruments = (Instrument*)srvAllocPsram(SRV_INSTRUMENTS_BYTES);
    srvSongScratch = (char (*)[SRV_FNAME_MAX])srvAllocPsram(SRV_MAX_SONGS * SRV_FNAME_MAX);

    srvActiveBuf = (uint8_t*)heap_caps_malloc(SRV_SONG_XFER_MAX, MALLOC_CAP_SPIRAM);
    if (!srvActiveBuf) {
        srvActiveBuf = (uint8_t*)malloc(SRV_SONG_XFER_MAX);
        Serial.println("[CMD] srvActiveBuf: PSRAM alloc FAILED — using internal");
    }
    // Both allocators can fail (dead PSRAM *and* a tight internal heap).  Every
    // song path casts this buffer straight to a Song* and writes through it, so
    // a null here is not a degraded mode — it is a StoreProhibited panic on the
    // first load, with the song list still working because it needs no buffer.
    // Every other PSRAM site in this sketch already degrades gracefully; this
    // one has to as well.
    srvBufOk = (srvActiveBuf != nullptr);
    // Staging area for client-requested loads: the file is read + migrated
    // here and only copied over the active song once it has loaded cleanly,
    // so a missing / unreadable / corrupt file leaves the current song
    // playing.  PSRAM only — internal RAM is the radio's.  Without it loads
    // fall back to reading in place (a failed load then drops to No Song).
    srvLoadScratch = (uint8_t*)heap_caps_malloc(sizeof(Song), MALLOC_CAP_SPIRAM);
    if (!srvLoadScratch)
        Serial.println("[CMD] srvLoadScratch: PSRAM alloc FAILED — loads read in place");
    // Same idea for a full-song push from the client: the bytes stream into
    // this buffer and replace the active song only once all of them are in,
    // so a link drop mid-push leaves the current song loaded and playing.
    // PSRAM only; without it pushes are received in place (see
    // onMagiLinkSaveHeader).
    srvPushScratch = (uint8_t*)heap_caps_malloc(sizeof(Song), MALLOC_CAP_SPIRAM);
    if (!srvPushScratch)
        Serial.println("[CMD] srvPushScratch: PSRAM alloc FAILED — pushes received in place");
    // Name scratch for the generic file lists (see sendFileList).
    srvFileScratch = (char (*)[FILE_NAME_LEN])
        heap_caps_malloc(SRV_FILE_LIST_MAX * FILE_NAME_LEN, MALLOC_CAP_SPIRAM);
    if (!srvFileScratch)
        srvFileScratch = (char (*)[FILE_NAME_LEN])malloc(SRV_FILE_LIST_MAX * FILE_NAME_LEN);
    if (!srvFileScratch)
        Serial.println("[CMD] srvFileScratch: ALLOC FAILED — file lists will be empty");
    sSongOpQ =(SongOp*)heap_caps_malloc(SONG_OP_Q_LEN * sizeof(SongOp), MALLOC_CAP_SPIRAM);
    if (!sSongOpQ) sSongOpQ = (SongOp*)malloc(SONG_OP_Q_LEN * sizeof(SongOp));
    if (!sSongOpQ)
        Serial.println("[CMD] song-op queue: ALLOC FAILED — load/save commands DISABLED");
    if (!srvBufOk) {
        Serial.printf("[CMD] srvActiveBuf: ALLOC FAILED for %u bytes — "
                      "song load/save DISABLED (internal free=%u, largest=%u)\n",
                      (unsigned)SRV_SONG_XFER_MAX,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
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
        // Defaults are only written when there is no file at all.  A file that
        // is present but did not load (read error, bad header) is left alone —
        // overwriting it here would replace the user's instruments with
        // defaults on a transient SD fault.
        bool missing;
        { SdLock _; missing = !SD.exists(SRV_INSTRUMENTS_PATH); }
        if (missing) {
            saveInstrumentsToSD();  // write defaults so future boots load them
            Serial.println("[CMD] instruments: created defaults");
        } else {
            srvInstFileUnreadable = true;   // later saves must not clobber it
            Serial.println("[CMD] instruments: " SRV_INSTRUMENTS_PATH
                           " present but unreadable — running on defaults, file untouched");
        }
    }
}

// Case-insensitive name compare for qsort over the fixed-width name rows.
static int srvNameCmp(const void* a, const void* b) {
    return strcasecmp((const char*)a, (const char*)b);
}

// Shared scratch for a full /songs enumeration.  Every user below (list, load,
// load-local) is deferred to the loop task, so they never run concurrently and
// a single static buffer is race-free — and at SRV_MAX_SONGS entries it is too
// big (~3 KB) to want on the loop-task stack.  Same "one task only" rationale
// as the backup path's static names[] buffer.  It is srvSongScratch, declared
// with the instrument bank near the top of this file: SRV_MAX_SONGS rows in
// PSRAM, allocated in commandsInit(); srvListSongs returns 0 if it is missing.

// ── List .mgt files in /songs ──────────────────────────────────────────────────
// Returns names sorted alphabetically (case-insensitive) so every paginated
// list the client assembles is in a stable, sorted order.
int srvListSongs(char names[][SRV_FNAME_MAX], int maxFiles) {
    if (!srvSdOk || !names) return 0;
    SdLock _;
    File d = SD.open(SRV_SONGS_DIR);
    if (!d || !d.isDirectory()) {
        Serial.println("[CMD] srvListSongs: can't open " SRV_SONGS_DIR " — SD read failed");
        return 0;
    }
    int count = 0;
    while (count < maxFiles) {
        File e = d.openNextFile();
        if (!e) break;
        if (!e.isDirectory()) {
            const char* fn = e.name();
            const char* sl = strrchr(fn, '/');
            if (sl) fn = sl + 1;
            int len = (int)strlen(fn);
            // Dot-files are skipped: macOS leaves a "._Name.mgt" sidecar beside
            // every file copied over USB-MSC, and it is not a song.
            if (fn[0] != '.' && len >= 4) {
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
    char (*files)[SRV_FNAME_MAX] = srvSongScratch;
    int total = srvListSongs(files, SRV_MAX_SONGS);

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
        // Strip the .mgt extension FIRST, then fit what is left to the field —
        // truncating first could cut the extension in half and leave a stub
        // of it in the name the client shows and asks to load.
        const char* fn  = files[start + i];
        int         len = (int)strlen(fn);
        if (len > 4 && fn[len - 4] == '.') len -= 4;
        if (len > SL_NAME_LEN - 1) len = SL_NAME_LEN - 1;
        memcpy(resp.names[i], fn, (size_t)len);   // resp.names is pre-zeroed
        resp.names[i][len] = '\0';
    }

    gMagiLink.acquireMutex();
    gMagiLink.send(&resp, sizeof(resp));
    gMagiLink.releaseMutex();
}

// ── Generic file list ────────────────────────────────────────────────────────
// Scans `dir` for files, applying the per-kind hide filter.  Returns count
// collected into `out` (capped at maxFiles), sorted case-insensitively like
// the song list so the pages the client assembles are in a stable order.
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
    qsort(out, count, FILE_NAME_LEN, srvNameCmp);
    return count;
}

static void sendFileList(uint8_t kind, uint8_t page) {
    const char* dir = fileKindToPath(kind);
    if (!dir) {
        Serial.printf("[CMD] file list: unknown kind=%u\n", (unsigned)kind);
        return;
    }
    char (*files)[FILE_NAME_LEN] = srvFileScratch;
    int total = 0;
    if (files) total = srvListFiles(kind, dir, files, SRV_FILE_LIST_MAX);
    else       Serial.println("[CMD] file list: no name scratch — sending an empty list");

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
// Mutex serialises use sites so one shared instance is safe.  The four body
// structs share one layout (id, length, data_len, data[1024]).  Nothing is
// carried over between users: each sets `id` before its loop and `length`
// to match what it actually puts on the wire — the truncated senders per
// chunk, the backup once to the full struct size.
static MsgBackupBody sStreamBody;

// Payload bytes per chunk for the song push, instruments push and file load.
// With the 5-byte body header that is a 517-byte frame on the wire rather than
// the full 1029-byte struct — see sendActiveSongToClient for why.
#define SRV_PUSH_CHUNK 512

// ── Send a {kind}/{name} file as HEADER + chunked BODY + EndOfData ───────────
// `name` is taken verbatim (with extension); the kind selects the dir via
// the whitelist.  Bad kind ⇒ header sent with found=0, no body.
static void sendFile(uint8_t kind, const char* name) {
    const char* dir = fileKindToPath(kind);

    File f;
    uint32_t fsize = 0;
    bool found = false;
    bool readFailed = false;
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
    memset(hdr.name, 0, sizeof(hdr.name));
    strncpy(hdr.name, name, sizeof(hdr.name) - 1);
    gMagiLink.send(&hdr, sizeof(hdr));

    if (found) {
        // Truncated 512B-chunk frames, same RF rationale as
        // sendActiveSongToClient; the wire `length` is set per chunk below.
        sStreamBody.id = MSG_FILE_LOAD_BODY;
        uint32_t sent = 0;
        bool ok = true;
        while (ok && sent < fsize) {
            uint32_t remain = fsize - sent;
            uint16_t chunk  = remain > SRV_PUSH_CHUNK ? SRV_PUSH_CHUNK : (uint16_t)remain;
            int got;
            { SdLock _; got = f.read(sStreamBody.data, chunk); }
            if (got <= 0) {
                // SD read failed mid-file.  Stop: the EndOfData below then
                // arrives short of total_size and the client reports an error,
                // instead of being handed stale buffer bytes as file content.
                Serial.printf("[CMD] file send READ FAILED got=%d at %u/%u '%s'\n",
                              got, (unsigned)sent, (unsigned)fsize, name);
                readFailed = true;
                break;
            }
            // Framed on what was actually read, so a short read still goes
            // out as a well-formed frame.
            uint16_t wire = (uint16_t)(offsetof(MsgFileLoadBody, data) + (size_t)got);
            sStreamBody.data_len = (uint16_t)got;
            sStreamBody.length   = wire;
            ok = gMagiLink.send(&sStreamBody, wire);
            sent += (uint32_t)got;
        }
        MsgEndOfData eod;
        gMagiLink.send(&eod, sizeof(eod));
    }

    gMagiLink.releaseMutex();
    { SdLock _; if (f) f.close(); }
    Serial.printf("[CMD] file send kind=%u '%s' %u bytes (%s)\n",
                  (unsigned)kind, name, (unsigned)fsize,
                  !found ? "MISSING" : readFailed ? "READ FAILED" : "OK");
}

// ── Send sample list page to client ────────────────────────────────────────────
// Each request runs a full manifest sync first: scans /samples/*.wav, appends
// any new files to /samples/samples.txt with the next free id, then pages the
// updated list out.  This means the picker on the client is always fresh
// without needing a separate "refresh samples" command.
// Loop task only (commandsTick) — a directory scan plus a manifest rewrite is
// too much SD work for the link worker.  The sync also re-reads trim.txt, so
// commandsTick writes out any pending trim edit before calling this.
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

// Quiesce all sequencer reads of srvActiveBuf before rewriting it.  Same
// hazard the save-push path guards against (see onMagiLinkSaveHeader): the
// MIDI task on Core 1 walks the note pool concurrently, and a torn NoteNode
// `next` mid-rebuild is a LoadProhibited crash or a broken-circularity
// watchdog.  Stop + flag only block NEW walks; the bounded wait lets an
// in-flight walk (which can park ms in a midiTx FIFO wait) finish first.
static void quiesceSongReaders() {
    sequencerStop();
    srvHasActive    = false;
    srvActiveBufLen = 0;
    for (int i = 0; i < 100 && sequencerSongReaderBusy(); i++) delay(1);
}

// The counterpart of quiesceSongReaders: the Song body in srvActiveBuf is now
// complete, so stamp the current file header in front of it and let readers
// back in.  Shared by load, reload and new-song.  (The full-song push does NOT
// use it — that path keeps the header the client sent.)
static void commitActiveSong() {
    SongFileHeader* h = (SongFileHeader*)srvActiveBuf;
    h->magic   = SONG_FILE_MAGIC;
    h->version = SONG_FILE_VERSION;
    h->_pad[0] = h->_pad[1] = h->_pad[2] = 0;
    srvActiveBufLen     = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);
    srvHasActive        = true;
    gSamplePreloadDirty = true;
}

// Queue one song operation (worker task).  Never blocks: these arrive at human
// pace, so a full queue means the loop task has been stuck for seconds — the
// operation is dropped and logged rather than wedging the link.
static bool songOpPush(uint8_t kind, const char* name, uint8_t nameMax,
                       uint8_t a, uint8_t b) {
    if (!sSongOpQ || (uint8_t)(sSongOpHead - sSongOpTail) >= SONG_OP_Q_LEN) {
        Serial.printf("[CMD] song-op queue full — DROPPED op %u\n", (unsigned)kind);
        return false;
    }
    SongOp* op = &sSongOpQ[sSongOpHead & (SONG_OP_Q_LEN - 1)];
    memset(op, 0, sizeof(*op));
    op->kind = kind;
    op->a    = a;
    op->b    = b;
    if (name) {
        size_t n = nameMax < sizeof(op->name) - 1 ? nameMax : sizeof(op->name) - 1;
        strncpy(op->name, name, n);             // op->name is pre-zeroed
    }
    op->seq = sArrivalSeq++;
    sSongOpHead++;
    return true;
}

static bool songOpPushPatch(uint16_t offset, const uint8_t* data, uint8_t len) {
    if (len > SONG_PATCH_MAX) return false;
    if (!sSongOpQ || (uint8_t)(sSongOpHead - sSongOpTail) >= SONG_OP_Q_LEN) {
        Serial.println("[CMD] song-op queue full — DROPPED patch");
        return false;
    }
    SongOp* op = &sSongOpQ[sSongOpHead & (SONG_OP_Q_LEN - 1)];
    memset(op, 0, sizeof(*op));
    op->kind   = OP_PATCH;
    op->offset = offset;
    op->len    = len;
    memcpy(op->data, data, len);
    op->seq = sArrivalSeq++;
    sSongOpHead++;
    return true;
}

// Tell the client the server has no active song: set to OFF, nothing loaded
// yet, or an in-place load that failed and took the active song with it.
void sendNoSongToClient() {
    if (!pairingIsConnected()) return;
    MsgNoSong msg;
    gMagiLink.acquireMutex();
    gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
}

// "-- OFF --": stop, drop the active song and its name, tell the client.  The
// one routine behind the client's Set-OFF, the server screen's OFF row and a
// delete of the active song.  Loop task only.  The caller looks after its own
// cursor / redraw.
void srvSetOff() {
    sequencerStop();
    srvHasActive     = false;
    srvActiveName[0] = '\0';
    sendNoSongToClient();
}

// Failure reply for an explicit load.  The client sits on its "receiving"
// page until song data or this arrives — every path that gives up on a load
// request must send it, or the client waits out its 8s timeout and cannot
// tell an SD problem from a slow link.
static void sendLoadFailedToClient(uint8_t reason) {
    if (!gMagiLink.isConnected()) return;
    MsgSongLoadFailed msg;
    msg.reason = reason;
    gMagiLink.acquireMutex();
    gMagiLink.send(&msg, sizeof(msg));
    gMagiLink.releaseMutex();
}

// ── Load a song file into the active buffer ────────────────────────────────────
// The one place a song file becomes the active song — shared by the
// client-requested load and the server-screen load.  Reads + migrates `path`
// (legacy versions are upgraded on the way in) and, only if that succeeds,
// commits it as the active song named `fileName` (with .mgt).
//
// Returns 0 on success, else a SongLoadFailReason.  Nothing is sent to the
// client and nothing is drawn — callers report.  *activeKept says whether the
// previous active song survived a failure: true when the read was staged,
// false when there was no staging buffer and it had to be read in place.
static uint8_t srvLoadSongFile(const char* path, const char* fileName, bool* activeKept) {
    *activeKept = true;
    if (!srvBufOk) {
        Serial.println("[CMD] load refused — no song buffer (see PSRAM report at boot)");
        return SONG_LOAD_FAIL_NO_MEMORY;
    }

    // Read into the staging buffer so nothing is committed until the file has
    // loaded — the current song keeps playing meanwhile.  No staging buffer →
    // read in place, which means the sequencer must be out of the active song
    // first (and a failure loses that song).
    Song* active = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    Song* song   = srvLoadScratch ? (Song*)srvLoadScratch : active;
    bool  staged = (song != active);
    *activeKept  = staged;
    if (!staged) quiesceSongReaders();

    bool    loaded = false;
    uint8_t fail   = SONG_LOAD_FAIL_NOT_FOUND;
    {
        SdLock _;
        File f = SD.open(path);
        SongFileHeader hdr;
        bool hdrRead = f && f.read((uint8_t*)&hdr, sizeof(hdr)) == (int)sizeof(hdr);
        // No file at all stays NOT_FOUND.  A short header is an SD read
        // problem; a whole header with the wrong magic is simply not a song
        // (e.g. a macOS "._name.mgt" sidecar).
        if (f) fail = hdrRead ? SONG_LOAD_FAIL_BAD_FILE : SONG_LOAD_FAIL_READ;
        if (hdrRead && hdr.magic == SONG_FILE_MAGIC) {
            // From here a reader that gives up means the body came up short
            // or failed its sanity checks.
            fail = SONG_LOAD_FAIL_READ;

            bool known = true;
            loaded = songReadAnyVersion(f, hdr.version, song, &known);
            if (!known) {
                Serial.printf("[CMD] song load: unknown version %u '%s'\n",
                              (unsigned)hdr.version, path);
                fail = SONG_LOAD_FAIL_BAD_FILE;
            } else if (loaded && hdr.version != SONG_FILE_VERSION) {
                Serial.printf("[CMD] migrated v%u->v%d '%s'\n",
                              (unsigned)hdr.version, SONG_FILE_VERSION, path);
            }
        }
        if (f) f.close();
    }

    if (!loaded) {
        static const char* const why[] = {
            "?", "not found / open failed", "read failed / corrupt body", "not a song file" };
        Serial.printf("[CMD] song load FAILED '%s' — %s; %s\n", path,
                      why[fail <= 3 ? fail : 0],
                      staged ? "active song kept" : "active song LOST (read in place)");
        return fail;
    }

    if (staged) {
        quiesceSongReaders();
        memcpy(active, song, sizeof(Song));
    }

    strncpy(srvActiveName, fileName, sizeof(srvActiveName) - 1);
    srvActiveName[sizeof(srvActiveName) - 1] = '\0';
    commitActiveSong();
    Serial.printf("[CMD] song load OK '%s' %u bytes\n", path, (unsigned)srvActiveBufLen);
    return 0;
}

// ── Client-requested load: load, then send the song in chunks ──────────────────
// Returns true only if the song loaded and became the active song.  On any
// failure the client is told (MSG_SONG_LOAD_FAILED) and, when the staging
// buffer exists, the previously active song is left untouched and playing.
static bool sendSongDataFromPath(const char* path, const char* displayName) {
    char prevName[SRV_FNAME_MAX];                 // the song we may be leaving
    strncpy(prevName, srvActiveName, sizeof(prevName) - 1);
    prevName[sizeof(prevName) - 1] = '\0';

    bool    activeKept = true;
    uint8_t fail = srvLoadSongFile(path, displayName, &activeKept);
    if (fail) {
        if (!activeKept) sendNoSongToClient();    // the in-place read wiped it — say so
        sendLoadFailedToClient(fail);
        return false;
    }

    // The load is good — only now do the drafts go.
    //
    // Explicit load is the user's "discard draft" path.
    // Clear the autosave draft for:
    //   (a) the song being loaded — they asked for the canonical /songs/
    //       version, so any session draft is abandoned;
    //   (b) the song we're moving away from — they didn't hit Save, so its
    //       draft is abandoned with it.
    {
        SdLock _;
        char autopath[48];
        if (prevName[0] != '\0') {
            snprintf(autopath, sizeof(autopath), "%s/%s",
                     SRV_AUTOSAVE_DIR, prevName);         // already carries .mgt
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

    // Push the loaded song to the client via the MagiLink HEADER+BODY stream.
    // Same path as the unsolicited push after handshake.
    Serial.printf("[CMD] sendSongData '%s' %u bytes — pushing\n",
                  path, (unsigned)srvActiveBufLen);
    sendActiveSongToClient();
    return true;
}

static bool sendSongData(uint8_t page, uint8_t index) {
    char (*files)[SRV_FNAME_MAX] = srvSongScratch;
    int total = srvListSongs(files, SRV_MAX_SONGS);

    int fileIdx = (int)page * SL_PER_PKT + (int)index;
    if (fileIdx < 0 || fileIdx >= total) {
        // The client indexed a list this scan no longer matches — files
        // changed underneath it, or the directory read came back short.
        Serial.printf("[CMD] sendSongData: index %d out of range (scan found %d songs)\n",
                      fileIdx, total);
        sendLoadFailedToClient(SONG_LOAD_FAIL_NOT_FOUND);
        return false;
    }

    // Copy the name out of the shared scratch before the send call — keeps this
    // independent of whether sendSongDataFromPath ever re-enumerates.
    char name[SRV_FNAME_MAX];
    strncpy(name, files[fileIdx], sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, name);
    return sendSongDataFromPath(path, name);
}

static bool sendSongDataByName(const char* name) {
    char path[48];
    snprintf(path, sizeof(path), "%s/%s.mgt", SRV_SONGS_DIR, name);
    char display[SRV_FNAME_MAX];
    snprintf(display, sizeof(display), "%s.mgt", name);
    return sendSongDataFromPath(path, display);
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
// The server-screen load, by the name shown on the SONGS screen (no .mgt).
// Returns 0 on success, else a SongLoadFailReason (NOT a bool — zero is the
// good result).  A failure leaves the previously active song in place and
// playing, exactly as for a client-requested load.
//
// By name, not by row number: the row only means something against the
// directory scan the list was built from, and a fresh scan can differ.
uint8_t srvLoadSongLocal(const char* bareName) {
    if (!bareName || bareName[0] == '\0') return SONG_LOAD_FAIL_NOT_FOUND;
    char file[SRV_FNAME_MAX + 8];
    snprintf(file, sizeof(file), "%s.mgt", bareName);
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, file);
    bool activeKept = true;
    return srvLoadSongFile(path, file, &activeKept);
}

// ── Verified song write ───────────────────────────────────────────────────────
// Writes `song` to `path`, then reads the bytes straight back off the card and
// CRC-checks them against the in-memory song before committing.  The write goes
// to a sibling _save.tmp first; only a byte-verified temp is swapped in for
// `path`, so a bad write (dying card, truncation, power glitch) can never
// clobber the previous good file.  The swap itself keeps the old file too: it
// is renamed to `<path>.bak`, the temp is renamed into place, and only then is
// the .bak removed — if the temp will not rename, the .bak is renamed back.
// Retries once.  Returns true only when the committed file is verified.
//
// Caller MUST already hold SdLock — sSdMutex is non-recursive, do not re-lock.
static bool writeSongVerified(const char* path, const Song* song) {
    // Temp file in the same directory as the target so the rename stays intra-FS.
    char tmp[64];
    const char* sl = strrchr(path, '/');
    if (sl) snprintf(tmp, sizeof(tmp), "%.*s/_save.tmp", (int)(sl - path), path);
    else    snprintf(tmp, sizeof(tmp), "/_save.tmp");
    // Where the previous version waits while the temp is renamed in.  Not a
    // .mgt name, so the song list and the backup never pick one up.
    char bak[64];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    bool oldStranded = false;   // previous version left in `bak`, `path` missing

    for (int attempt = 1; attempt <= 2; attempt++) {
        SD.remove(tmp);
        File f = SD.open(tmp, FILE_WRITE);
        if (!f) { Serial.printf("[CMD] save-verify: tmp open failed '%s'\n", tmp); continue; }
        // Reference CRC taken per attempt, right before the write.  The song
        // can change under a save (the link worker applies a song patch
        // inline, or — without the staging buffer — streams a full-song push
        // into it); with one CRC taken up front the retry
        // would be comparing against the old bytes and could never pass.
        const uint32_t crcMem = songCrc32(song);
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

        // Verified — swap the temp in for the canonical file.  The old file is
        // moved aside, not deleted, until the new one is in place.
        bool hadOld = SD.exists(path);
        if (hadOld) {
            SD.remove(bak);                    // a leftover would block the rename
            if (!SD.rename(path, bak)) {
                Serial.printf("[CMD] save-verify: could not move '%s' aside (try %d)\n",
                              path, attempt);
                SD.remove(tmp);
                continue;                      // old file still in place, untouched
            }
        }
        if (!SD.rename(tmp, path)) {
            Serial.printf("[CMD] save-verify: rename '%s' -> '%s' FAILED (try %d)\n", tmp, path, attempt);
            if (hadOld) {
                if (SD.rename(bak, path)) {
                    oldStranded = false;
                } else {
                    oldStranded = true;
                    Serial.printf("[CMD] save-verify: could NOT put '%s' back — previous version is in '%s'\n",
                                  path, bak);
                }
            }
            SD.remove(tmp);
            continue;
        }
        // On a retry after a stranded swap `path` did not exist, so `bak` was
        // not touched above — it still holds the older version; clear it now
        // that a newer one is committed.
        if (hadOld || oldStranded) SD.remove(bak);
        Serial.printf("[CMD] save-verify: '%s' %u bytes OK (crc=%08X)\n",
                      path, (unsigned)sz, (unsigned)crcDisk);
        return true;
    }
    if (oldStranded)
        Serial.printf("[CMD] save-verify: GAVE UP on '%s' — nothing at that path now; "
                      "the previous version is in '%s'\n", path, bak);
    else
        Serial.printf("[CMD] save-verify: GAVE UP on '%s' — previous file (if any) left in place\n", path);
    return false;
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
    if (!srvInstruments) return;
    const uint8_t* raw   = (const uint8_t*)srvInstruments;
    uint32_t       total = (uint32_t)SRV_INSTRUMENTS_BYTES;

    gMagiLink.acquireMutex();

    MsgInstrumentsPushHeader hdr;
    hdr.total_size = total;
    bool ok = gMagiLink.send(&hdr, sizeof(hdr));

    sStreamBody.id = MSG_INSTRUMENTS_PUSH_BODY;

    // Truncated 512B-chunk frames, same RF rationale as sendActiveSongToClient
    // (a 4.6KB instruments push has been seen stalling in the same spells).
    uint32_t sent = 0;
    while (ok && sent < total) {
        uint32_t remain = total - sent;
        uint16_t chunk  = remain > SRV_PUSH_CHUNK ? SRV_PUSH_CHUNK : (uint16_t)remain;
        uint16_t wire   = (uint16_t)(offsetof(MsgBackupBody, data) + chunk);
        sStreamBody.data_len = chunk;
        sStreamBody.length   = wire;
        memcpy(sStreamBody.data, raw + sent, chunk);
        ok = gMagiLink.send(&sStreamBody, wire);
        sent += chunk;
    }

    gMagiLink.releaseMutex();
    Serial.printf("[CMD] sendInstruments: %u bytes (%s)\n",
                  (unsigned)total, ok ? "OK" : "FAIL");
}

// ── MagiLink restore (client → server) state ───────────────────────────────
// Streams bytes incrementally to SRV_RESTORE_TMP on SD as they arrive on
// the worker task, under SdLock (which serialises against SamplePlayer) —
// no 35 KB RAM staging buffer needed.  The upload is finished on the worker
// too: the frame that completes the byte count renames the temp into place.
// A restore sends file after file with nothing in between, so per-file work
// left for the loop task would be overtaken by the next HEADER reusing this
// state and the temp file.  Only the follow-ups that touch srvInstruments /
// srvActiveBuf are deferred to commandsTick, as the idempotent flags below.
static File     sMagiRestoreFile;
static char     sMagiRestoreName[BK_FNAME_MAX] = {};
static bool     sMagiRestoreIsInstruments       = false;
static uint32_t sMagiRestoreExpectedBytes       = 0;
static uint32_t sMagiRestoreReceivedBytes       = 0;
static bool     sMagiRestoreWriteOk             = true;   // every chunk written in full
static volatile bool sRestoreInstReloadPending  = false;  // instruments.mgt was replaced
static volatile bool sRestoreSongReloadPending  = false;  // the active song's file was replaced

// Runs on the worker once the last byte of a restore upload has been written
// and the temp closed.  Moves the temp to its destination and flags the
// follow-up work (reload instruments / reload the active song / refresh the
// on-screen list) for the loop task.
static void finaliseMagiLinkRestore() {
    char path[BK_FNAME_MAX + 32];
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

    bool renamed    = false;
    bool removedOld = false;
    {
        SdLock _;
        if (sMagiRestoreWriteOk) {
            if (SD.exists(path)) removedOld = SD.remove(path);
            renamed = SD.rename(SRV_RESTORE_TMP, path);
        }
        if (!renamed) SD.remove(SRV_RESTORE_TMP);
    }
    if (!renamed) {
        Serial.printf("[CMD] restore FAILED '%s' (%u bytes) — %s\n",
                      path, (unsigned)sMagiRestoreReceivedBytes,
                      !sMagiRestoreWriteOk ? "SD write came up short; existing file kept"
                      : removedOld         ? "rename into place failed AFTER the old file was removed"
                                           : "rename into place failed");
        return;
    }
    Serial.printf("[CMD] restored '%s' (%u bytes)\n",
                  path, (unsigned)sMagiRestoreReceivedBytes);

    if (sMagiRestoreIsInstruments) {
        sRestoreInstReloadPending = true;
    } else {
        sSongListDirty = true;
        // srvActiveName belongs to the loop task; this is an unlocked read of
        // it.  Worst case (a load or save renaming the active song at this
        // very moment) is one missed or one needless reload.  The loop task
        // re-checks that a song is still active before reloading.
        if (strcmp(sMagiRestoreName, srvActiveName) == 0)
            sRestoreSongReloadPending = true;
    }
}

void onMagiLinkRestoreHeader(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgRestoreHeader)) return;
    const MsgRestoreHeader* h = (const MsgRestoreHeader*)msg;
    if (sMagiRestoreExpectedBytes != 0)
        Serial.printf("[CMD] restore: '%s' abandoned at %u/%u bytes — new upload starting\n",
                      sMagiRestoreName, (unsigned)sMagiRestoreReceivedBytes,
                      (unsigned)sMagiRestoreExpectedBytes);
    strncpy(sMagiRestoreName, h->name, sizeof(sMagiRestoreName) - 1);
    sMagiRestoreName[sizeof(sMagiRestoreName) - 1] = '\0';
    sMagiRestoreIsInstruments = (h->isInstruments != 0);
    sMagiRestoreExpectedBytes = h->total_size;
    sMagiRestoreReceivedBytes = 0;
    sMagiRestoreWriteOk       = true;
    if (sMagiRestoreExpectedBytes == 0) {
        // Nothing will follow, so there is nothing to complete on.  An empty
        // file is never worth replacing a real one with — leave the card alone.
        SdLock _;
        if (sMagiRestoreFile) sMagiRestoreFile.close();
        SD.remove(SRV_RESTORE_TMP);
        Serial.printf("[CMD] restore: '%s' is empty — ignored, existing file kept\n",
                      sMagiRestoreName);
        return;
    }
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
    if (b->data_len > sizeof(b->data)) return;
    if (sMagiRestoreReceivedBytes + b->data_len > sMagiRestoreExpectedBytes) {
        Serial.println("[CMD] restore body overshoot — closing & dropping");
        {
            SdLock _;
            sMagiRestoreFile.close();
            SD.remove(SRV_RESTORE_TMP);
        }
        sMagiRestoreExpectedBytes = 0;
        return;
    }
    {
        SdLock _;
        if (sMagiRestoreFile.write(b->data, b->data_len) != (size_t)b->data_len)
            sMagiRestoreWriteOk = false;
    }
    sMagiRestoreReceivedBytes += b->data_len;
    if (sMagiRestoreReceivedBytes >= sMagiRestoreExpectedBytes) {
        {
            SdLock _;
            sMagiRestoreFile.close();
        }
        sMagiRestoreExpectedBytes = 0;
        // Here, not on the loop task — see the note at the top of this section.
        finaliseMagiLinkRestore();
    }
}

// ── MagiLink generic file save (client → server) state ─────────────────────
// Streams an uploaded {kind}/{name} file to a temp file in the target dir on
// the worker task (SdLock-serialised, like restore).  Completion is detected
// by byte count — the stream has no terminator — and the frame that completes
// it renames the temp into place and ACKs, still on the worker.  It has to be
// there: the client sends two saves back to back (the master catalog, then the
// setlist), and a finalise left for the loop task was overtaken by the second
// HEADER, which reused this state and the temp file before the first had been
// renamed.  Used by setlists; the planned PC uploader shares it.  Only one
// upload in flight at a time (mutex-serialised send side).
static File     sMagiFileSaveFile;
static uint8_t  sMagiFileSaveKind              = 0;
static char     sMagiFileSaveName[FILE_NAME_LEN] = {};
static char     sMagiFileSaveTmp[96]           = {};
static uint32_t sMagiFileSaveExpectedBytes     = 0;
static uint32_t sMagiFileSaveReceivedBytes     = 0;
static bool     sMagiFileSaveWriteOk           = true;   // every chunk written in full

// Worker task only, from inside a MagiLink callback: the dispatcher already
// holds the link mutex for the callback, so this is a bare send — no
// acquire/release (a release here would drop the dispatcher's hold early).
static void sendFileSaveAck(uint8_t kind, bool ok) {
    MsgFileSaveAck ack;
    ack.kind = kind;
    ack.ok   = ok ? 1 : 0;
    gMagiLink.send(&ack, sizeof(ack));
}

// Runs on the worker once the temp holds the whole upload and is closed.
// Renames it into place (same dir, so atomic) and ACKs the client.
static void finaliseMagiLinkFileSave() {
    const char* dir = fileKindToPath(sMagiFileSaveKind);
    bool ok = false;
    if (dir) {
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", dir, sMagiFileSaveName);
        SdLock _;
        if (sMagiFileSaveWriteOk && sMagiFileSaveName[0] != '\0') {
            if (SD.exists(path)) SD.remove(path);
            ok = SD.rename(sMagiFileSaveTmp, path);
        }
        if (!ok) SD.remove(sMagiFileSaveTmp);
    }
    Serial.printf("[CMD] file saved kind=%u '%s' (%u bytes) %s\n",
                  (unsigned)sMagiFileSaveKind, sMagiFileSaveName,
                  (unsigned)sMagiFileSaveReceivedBytes,
                  ok ? "OK" : !sMagiFileSaveWriteOk ? "FAIL (SD write came up short)" : "FAIL");
    sendFileSaveAck(sMagiFileSaveKind, ok);
}

void onMagiLinkFileSaveHeader(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgFileSaveHeader)) return;
    const MsgFileSaveHeader* h = (const MsgFileSaveHeader*)msg;
    const uint8_t  kind  = h->kind;         // copied out before any send below
    const uint32_t total = h->total_size;
    const char* dir = fileKindToPath(kind);
    if (sMagiFileSaveExpectedBytes != 0)
        Serial.printf("[CMD] file save: '%s' abandoned at %u/%u bytes — new upload starting\n",
                      sMagiFileSaveName, (unsigned)sMagiFileSaveReceivedBytes,
                      (unsigned)sMagiFileSaveExpectedBytes);
    if (!dir) {
        Serial.printf("[CMD] file save: unknown kind=%u\n", (unsigned)kind);
        sMagiFileSaveExpectedBytes = 0;
        sendFileSaveAck(kind, false);       // the client is waiting on an ACK
        return;
    }
    sMagiFileSaveKind = kind;
    strncpy(sMagiFileSaveName, h->name, sizeof(sMagiFileSaveName) - 1);
    sMagiFileSaveName[sizeof(sMagiFileSaveName) - 1] = '\0';
    sMagiFileSaveExpectedBytes = total;
    sMagiFileSaveReceivedBytes = 0;
    sMagiFileSaveWriteOk       = true;
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
        sendFileSaveAck(kind, false);
        return;
    }
    Serial.printf("[CMD] file save start: kind=%u name='%s' bytes=%u\n",
                  (unsigned)sMagiFileSaveKind, sMagiFileSaveName,
                  (unsigned)sMagiFileSaveExpectedBytes);
    if (total == 0) {
        // An empty file (e.g. a setlist with every entry removed): the client
        // sends no BODY at all, so nothing would ever complete the count.
        // The empty temp is the whole file — finish it now.
        { SdLock _; sMagiFileSaveFile.close(); }
        finaliseMagiLinkFileSave();
    }
}

void onMagiLinkFileSaveBody(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (sMagiFileSaveExpectedBytes == 0) return;
    if (len < sizeof(MsgFileSaveBody)) return;
    if (!sMagiFileSaveFile) return;
    const MsgFileSaveBody* b = (const MsgFileSaveBody*)msg;
    if (b->data_len > sizeof(b->data)) return;
    if (sMagiFileSaveReceivedBytes + b->data_len > sMagiFileSaveExpectedBytes) {
        Serial.println("[CMD] file save body overshoot — dropping");
        {
            SdLock _;
            sMagiFileSaveFile.close();
            SD.remove(sMagiFileSaveTmp);
        }
        sMagiFileSaveExpectedBytes = 0;
        sendFileSaveAck(sMagiFileSaveKind, false);
        return;
    }
    {
        SdLock _;
        if (sMagiFileSaveFile.write(b->data, b->data_len) != (size_t)b->data_len)
            sMagiFileSaveWriteOk = false;
    }
    sMagiFileSaveReceivedBytes += b->data_len;
    if (sMagiFileSaveReceivedBytes >= sMagiFileSaveExpectedBytes) {
        { SdLock _; sMagiFileSaveFile.close(); }
        sMagiFileSaveExpectedBytes = 0;
        // Rename + ACK here, not on the loop task — see the note at the top
        // of this section.
        finaliseMagiLinkFileSave();
    }
}

// ── Save result (server → client) ─────────────────────────────────────────────
// The answer to a save the client asked for — an OP_SAVE_ACTIVE, or a
// full-song push that carried a name — so it can report "saved" only when the
// file really is on the card.  `name` is echoed as requested (no extension).
//
// Never call this holding SdLock: the link worker takes the link mutex first
// and SdLock second, so a send made under SdLock can deadlock against it.
// Nor from a link callback that still holds the dispatcher's mutex (the
// release below would drop that hold early).
static void sendSaveResult(const char* name, bool isAutosave, bool ok) {
    if (!gMagiLink.isConnected()) return;
    MsgSaveResult res;
    res.ok          = ok ? 1 : 0;
    res.is_autosave = isAutosave ? 1 : 0;
    memset(res.name, 0, sizeof(res.name));
    strncpy(res.name, name, sizeof(res.name) - 1);
    gMagiLink.acquireMutex();
    gMagiLink.send(&res, sizeof(res));
    gMagiLink.releaseMutex();
}

// ── MagiLink song save (client → server) state ─────────────────────────────
// Populated by the MSG_SAVE_SONG_HEADER and _BODY callbacks (worker task).
// The Song bytes stream into srvPushScratch — NOT into the active song — so
// the current song stays loaded and playing until the whole push has arrived;
// a link drop part-way through just abandons the scratch.  On completion the
// BODY callback queues OP_PUSH_DONE, and finaliseMagiLinkSongSave (loop task,
// which owns srvActiveBuf and the verified-save path) quiesces the sequencer,
// copies the scratch over the active song and writes it to SD if a name was
// given.  (Restore and file-save do stream to SD from the worker, under
// SdLock.)
//
// No srvPushScratch (its PSRAM allocation failed at boot) → the old in-place
// receive: the HEADER stops the sequencer and takes the song inactive, the
// bodies land straight in srvActiveBuf, and a push that never completes
// leaves the server with no song.
static char           sMagiSaveName[SRV_FNAME_MAX] = {};
static SongFileHeader sMagiSaveFileHdr;                  // as sent by the client (staged path)
static uint32_t       sMagiSaveExpectedBytes       = 0;  // Song bytes expected; 0 = no push in flight
static uint32_t       sMagiSaveReceivedBytes       = 0;  // Song bytes received so far
// A complete push is waiting for (or is in) finaliseMagiLinkSongSave.  Until
// the loop task clears this, the receive buffer, sMagiSaveName and
// sMagiSaveFileHdr are its to read — the next HEADER waits for it.
static volatile bool     sPushCommitPending = false;
// The CP_SONG_PUSH breadcrumb is up for a push that has not been finalised.
// sPushGen is the link generation the push arrived on, so commandsTick can
// tell that the link dropped under it and take the breadcrumb down.
static volatile bool     sPushPhaseArmed    = false;
static volatile uint32_t sPushGen           = 0;

// Take the CP_SONG_PUSH breadcrumb down, if it is up.
static void pushPhaseEnd() {
    if (!sPushPhaseArmed) return;
    sPushPhaseArmed = false;
    crashSetPhase(CP_IDLE);
}

void onMagiLinkSaveHeader(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (!srvBufOk) return;   // nowhere to receive the song into
    if (len < sizeof(MsgSongSaveHeader)) return;
    // Copied out: the wait below can give up the link mutex, and after that
    // `msg` (the link's receive buffer) is no longer ours to read.
    MsgSongSaveHeader hdr = *(const MsgSongSaveHeader*)msg;
    hdr.name[sizeof(hdr.name) - 1] = '\0';

    // The previous push has all arrived but the loop task has not committed it
    // yet.  Starting this one now would overwrite the bytes (and the name) it
    // is about to commit, so wait for it — but NOT holding the link mutex: the
    // loop task may have to send before it reaches that commit (same reasoning
    // as the note-set queue in handleCommand).
    if (sPushCommitPending) {
        gMagiLink.releaseMutex();
        uint32_t t0 = millis();
        while (sPushCommitPending && millis() - t0 < 3000) delay(1);
        if (sPushCommitPending) {
            // sMagiSaveExpectedBytes is 0 (that push completed), so the bodies
            // of this one are discarded as strays.
            Serial.printf("[CMD] save header DROPPED name='%s' — previous push "
                          "still not committed after 3s\n", hdr.name);
            if (hdr.name[0] != '\0') sendSaveResult(hdr.name, false, false);
            return;
        }
    }

    if (sMagiSaveExpectedBytes != 0)
        Serial.printf("[CMD] save header: previous push incomplete (%u/%u bytes) — restarting\n",
                      (unsigned)sMagiSaveReceivedBytes, (unsigned)sMagiSaveExpectedBytes);

    // The push is a raw Song struct, committed as a whole song on completion —
    // anything but exactly sizeof(Song) is refused.  (A short one would be
    // committed with a stale tail; zero would never complete.)
    if (hdr.song_bytes != sizeof(Song)) {
        Serial.printf("[CMD] save header rejected: %u bytes, expected %u\n",
                      (unsigned)hdr.song_bytes, (unsigned)sizeof(Song));
        sMagiSaveExpectedBytes = 0;
        pushPhaseEnd();
        return;
    }

    // Breadcrumb until the push is finalised or abandoned (overshoot, a
    // rejected restart, or a dropped link — see commandsTick).
    sPushGen        = gMagiLink.generation();
    sPushPhaseArmed = true;
    crashSetPhase(CP_SONG_PUSH);

    if (srvPushScratch) {
        // Staged: nothing the sequencer reads is touched until the commit in
        // finaliseMagiLinkSongSave.  Keep the file header for it.
        memcpy(&sMagiSaveFileHdr, &hdr.song_file_header, sizeof(SongFileHeader));
    } else {
        // In place.  Stop the sequencer up-front: the receive writes
        // incrementally into srvActiveBuf over many ms.  If the sequencer
        // keeps reading it could follow a half-overwritten NoteNode pointer
        // into garbage.
        Serial.printf("[STOPSRC] save-header (full song push, in place) name='%s' t=%lu\n",
                      hdr.name, millis());
        sequencerStop();
        // Also drop srvHasActive so sequencerTick's early-return guards the
        // buffer even if a tick was already past the seqRunning check when
        // this header arrived.  Restored in finaliseMagiLinkSongSave().
        srvHasActive = false;
        // Those guards only stop NEW walks.  A tick/MIDI-poll already in
        // flight on the MIDI task can be parked mid-walk in a midiTx FIFO wait
        // for several ms; the body chunks that follow overwrite the note pool
        // it is walking, and a torn `next` index reads far out of bounds
        // (LoadProhibited) or breaks list circularity (watchdog).  Wait the
        // in-flight walk out — dispatch is sequential on this task, so once we
        // return no walker can race the body writes.  Bounded: a walk is
        // µs–ms, never near 100 ms.
        for (int i = 0; i < 100 && sequencerSongReaderBusy(); i++) delay(1);
        // Stamp SongFileHeader at the front of srvActiveBuf.
        memcpy(srvActiveBuf, &hdr.song_file_header, sizeof(SongFileHeader));
    }
    strncpy(sMagiSaveName, hdr.name, sizeof(sMagiSaveName) - 1);
    sMagiSaveName[sizeof(sMagiSaveName) - 1] = '\0';
    sMagiSaveExpectedBytes = hdr.song_bytes;
    sMagiSaveReceivedBytes = 0;
    Serial.printf("[CMD] save start: name='%s' bytes=%u (%s)\n",
                  sMagiSaveName, (unsigned)sMagiSaveExpectedBytes,
                  srvPushScratch ? "staged" : "in place");
}

void onMagiLinkSaveBody(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (sMagiSaveExpectedBytes == 0) return;          // stray body
    if (len < sizeof(MsgSongSaveBody)) return;
    const MsgSongSaveBody* b = (const MsgSongSaveBody*)msg;
    if (b->data_len > sizeof(b->data)) return;
    if (sMagiSaveReceivedBytes + b->data_len > sMagiSaveExpectedBytes) {
        Serial.println("[CMD] save body overshoot — dropping");
        sMagiSaveExpectedBytes = 0;
        pushPhaseEnd();
        return;
    }
    // Expected is exactly sizeof(Song) (checked in the header), so this stays
    // inside whichever buffer it is.
    uint8_t* dst = srvPushScratch ? srvPushScratch
                                  : srvActiveBuf + sizeof(SongFileHeader);
    memcpy(dst + sMagiSaveReceivedBytes, b->data, b->data_len);
    sMagiSaveReceivedBytes += b->data_len;
    if (sMagiSaveReceivedBytes == sMagiSaveExpectedBytes) {
        sMagiSaveExpectedBytes = 0;
        sPushCommitPending = true;        // up before the loop task can see the op
        if (!songOpPush(OP_PUSH_DONE, nullptr, 0, 0, 0)) {
            // No commit will ever run for this push (songOpPush has logged it).
            sPushCommitPending = false;
            pushPhaseEnd();
        }
    }
}

// ── Save-active (autosave shortcut): client → server ─────────────────────
// Tells us to write the current srvActiveBuf to SD under the given name.
// No song bytes on the wire — patches + note-sets have kept srvActiveBuf
// in sync with the client's memory.  SD work deferred to commandsTick.

void onMagiLinkSaveActive(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgSaveActive)) return;
    const MsgSaveActive* m = (const MsgSaveActive*)msg;
    songOpPush(OP_SAVE_ACTIVE, m->name, sizeof(m->name) - 1, m->is_autosave != 0, 0);
}

// Runs as OP_SAVE_ACTIVE.  Every way out answers the client with one
// MsgSaveResult — ok only once the file is written and verified.
static void finaliseSaveActive(const char* saveName, bool isAutosave) {
    if (!srvHasActive) {
        Serial.println("[CMD] save-active: no active song — ignoring");
        sendSaveResult(saveName, isAutosave, false);
        return;
    }
    if (saveName[0] == '\0') {
        Serial.println("[CMD] save-active: empty name — ignoring");
        sendSaveResult(saveName, isAutosave, false);
        return;
    }
    crashSetPhase(CP_AUTOSAVE);
    // Autosave goes to /autosave/; explicit Save goes to /songs/ AND wipes the
    // matching /autosave/ draft (it's now superseded by the committed file).
    // Drafts are never auto-recovered on boot — see the autosave-policy note.
    const char* dir = isAutosave ? SRV_AUTOSAVE_DIR : SRV_SONGS_DIR;
    char path[48];
    snprintf(path, sizeof(path), "%s/%s.mgt", dir, saveName);
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    bool saved;
    {
        SdLock _;
        saved = writeSongVerified(path, song);
        if (saved && !isAutosave) {
            // Explicit save committed — any pending autosave draft is now redundant.
            char autopath[48];
            snprintf(autopath, sizeof(autopath), "%s/%s.mgt",
                     SRV_AUTOSAVE_DIR, saveName);
            if (SD.exists(autopath)) {
                SD.remove(autopath);
                Serial.printf("[CMD] cleared autosave '%s'\n", autopath);
            }
        }
    }
    if (!saved) {
        Serial.printf("[CMD] save-active: verify failed '%s'\n", path);
    } else if (!isAutosave) {
        snprintf(srvActiveName, sizeof(srvActiveName), "%s.mgt", saveName);
        // A name the on-screen list doesn't have yet is a new file.
        bool listed = false;
        for (int i = 0; i < numSongs && !listed; i++)
            listed = (strcasecmp(songNames[i], saveName) == 0);
        if (!listed) sSongListDirty = true;
    }
    crashSetPhase(CP_IDLE);   // autosave/save complete (or failed)
    sendSaveResult(saveName, isAutosave, saved);   // SdLock is released by now
}

// ── New-song: client → server ────────────────────────────────────────────
// Server runs initSong() on its in-memory copy so it matches the
// freshly-wiped client copy.  No bytes streamed.

void onMagiLinkNewSong(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgNewSong)) return;
    songOpPush(OP_NEW_SONG, nullptr, 0, 0, 0);
}

// ── Set-OFF: client → server "-- OFF -- / No Song" ────────────────────────────

void onMagiLinkSetNoSong(const uint8_t* msg, size_t len, void* /*ctx*/) {
    if (len < sizeof(MsgSetNoSong)) return;
    songOpPush(OP_SET_OFF, nullptr, 0, 0, 0);   // run on the loop task — same as loadSong(0)
}

static void finaliseNewSong() {
    if (!srvBufOk) {
        Serial.println("[CMD] new-song refused — no song buffer (see PSRAM report at boot)");
        return;
    }
    Serial.printf("[STOPSRC] finaliseNewSong t=%lu\n", millis());
    quiesceSongReaders();
    // initSong() the Song body, then commit it behind a fresh SongFileHeader.
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    initSong(song);
    srvActiveName[0] = '\0';
    commitActiveSong();
    sequencerReset();
    Serial.println("[CMD] new-song: server memory wiped");
}

// Runs as OP_PUSH_DONE.  Makes the pushed song the active song — a staged
// push is copied in from the scratch here, with the sequencer quiesced; an
// in-place one is already sitting in srvActiveBuf — then writes it to SD if a
// name was provided (and tells the client how that went) and resets the
// sequencer.
static void finaliseMagiLinkSongSave() {
    crashSetPhase(CP_SONG_PUSH);   // a save that ran since the HEADER will have cleared it
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    if (srvPushScratch) {
        Serial.printf("[STOPSRC] full song push commit name='%s' t=%lu\n",
                      sMagiSaveName, millis());
        quiesceSongReaders();
        // The header the client sent, then the song (commitActiveSong is not
        // used here — see its comment).
        memcpy(srvActiveBuf, &sMagiSaveFileHdr, sizeof(SongFileHeader));
        memcpy(song, srvPushScratch, sizeof(Song));
    }
    srvActiveBufLen = (uint32_t)sizeof(SongFileHeader) + sizeof(Song);
    srvHasActive    = true;  gSamplePreloadDirty = true;

    if (sMagiSaveName[0] != '\0') {
        char path[48];
        snprintf(path, sizeof(path), "%s/%s.mgt", SRV_SONGS_DIR, sMagiSaveName);
        bool saved;
        {
            SdLock _;
            saved = writeSongVerified(path, song);
        }
        if (saved) {
            snprintf(srvActiveName, sizeof(srvActiveName), "%s.mgt", sMagiSaveName);
            sSongListDirty = true;
        } else {
            Serial.printf("[CMD] save: verify failed '%s'\n", path);
        }
        sendSaveResult(sMagiSaveName, false, saved);   // SdLock released above
    } else {
        Serial.println("[CMD] save: in-memory push (no SD write)");
    }
    Serial.printf("[STOPSRC] finaliseMagiLinkSongSave name='%s' t=%lu\n",
                  sMagiSaveName, millis());
    sequencerStop();
    sequencerReset();
    sPushPhaseArmed = false;
    crashSetPhase(CP_IDLE);       // song-push complete
    sPushCommitPending = false;   // last: the worker may now start receiving the next push
}

// ── MagiLink backup — streams the whole library (songs, instruments, ──────
//                       drumtracks, setlists), with NO fixed capacity.
//
// Called from the MagiLink MSG_START_BACKUP handler (worker task context,
// transaction mutex already held).  Sends:
//   MsgBackupHeader, MsgBackupBody×N    ← for each file
//   MsgEndOfData                         ← run complete
//
// The file list is never materialised — files are counted (pass 1, for an
// accurate "X of Y") then streamed straight off SD (pass 2).  So the backup
// captures EVERYTHING; the old SRV_MAX_FILES=32 array silently dropped songs
// past the 32nd.  SD reads are gated by SdLock so SamplePlayer / other tasks
// interleave safely; sends are fire-and-forget, TCP backpressure paces via
// blocking peer.write in MagiLink::send.

// Per-source backup filters (operate on the bare basename).
// Common guard: skip macOS "._" AppleDouble sidecars (Finder junk) and any
// name too long for the wire header — it would otherwise be truncated and
// fail to open (or restore under the wrong name).
static bool backupNameOk(const char* nm) {
    if (nm[0] == '.' && nm[1] == '_') return false;
    if (strlen(nm) >= BK_FNAME_MAX) {
        Serial.printf("[BK-SRV] name too long (%u >= %u), skipped: '%s'\n",
                      (unsigned)strlen(nm), (unsigned)BK_FNAME_MAX, nm);
        return false;
    }
    return true;
}
static bool backupIsSong(const char* nm) {
    if (!backupNameOk(nm)) return false;
    int n = (int)strlen(nm);
    return n >= 4 && nm[n-4] == '.'
        && (nm[n-3]=='m'||nm[n-3]=='M') && (nm[n-2]=='g'||nm[n-2]=='G')
        && (nm[n-1]=='t'||nm[n-1]=='T');
}
static bool backupIsDrumtrack(const char* nm) {
    if (!backupNameOk(nm)) return false;
    int n = (int)strlen(nm);
    return n >= 4 && nm[n-4] == '.'
        && (nm[n-3]=='t'||nm[n-3]=='T') && (nm[n-2]=='x'||nm[n-2]=='X')
        && (nm[n-1]=='t'||nm[n-1]=='T');
}
static bool backupIsSetlist(const char* nm) {
    if (!backupNameOk(nm)) return false;
    int n = (int)strlen(nm);
    bool isSet = n >= 4 && nm[n-4] == '.'
        && (nm[n-3]=='s'||nm[n-3]=='S') && (nm[n-2]=='e'||nm[n-2]=='E')
        && (nm[n-1]=='t'||nm[n-1]=='T');
    return isSet || strcasecmp(nm, "master.txt") == 0;
}

// Diagnostic: print a raw name as text + hex so a corrupt directory entry
// (non-printable bytes, lost LFN → 8.3 alias) is unmistakable on the log.
static void backupDumpName(const char* tag, const char* nm, uint32_t size, bool match) {
    bool clean = true;
    for (const char* c = nm; *c; c++) if (*c < 0x20 || *c > 0x7E) { clean = false; break; }
    Serial.printf("[BK-SRV] %s '%s' len=%u size=%u match=%d%s",
                  tag, nm, (unsigned)strlen(nm), (unsigned)size, match ? 1 : 0,
                  clean ? "" : " NONPRINTABLE hex=");
    if (!clean) for (const char* c = nm; *c; c++) Serial.printf("%02X", (uint8_t)*c);
    Serial.println();
}

// Pass 1: count matching files in `dir` (0 if it isn't a directory).
static uint16_t backupCountDir(const char* dir, bool (*pred)(const char*)) {
    uint16_t c = 0;
    SdLock _;
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return 0; }
    for (;;) {
        File e = d.openNextFile();
        if (!e) break;
        if (!e.isDirectory()) {
            const char* nm = e.name();
            const char* sl = strrchr(nm, '/'); if (sl) nm = sl + 1;
            bool m = pred(nm);
            backupDumpName("count", nm, (uint32_t)e.size(), m);
            if (m) c++;
        } else {
            Serial.printf("[BK-SRV] count subdir '%s' (skipped)\n", e.name());
        }
        e.close();
    }
    d.close();
    Serial.printf("[BK-SRV] count dir %s -> %u\n", dir, (unsigned)c);
    return c;
}

// Stream one file (header + body chunks).  Returns false only on a wire
// failure; a local open failure is non-fatal (skips the body — the client
// tolerates a header followed straight by the next header).
static uint16_t sBkFileFailures = 0;   // open/size/read failures (non-fatal to the stream)

static bool backupStreamOne(const char* name, const char* fullPath,
                            uint32_t size, uint16_t index, uint16_t total) {
    MsgBackupHeader hdr;
    memset(hdr.filename, 0, sizeof(hdr.filename));
    strncpy(hdr.filename, name, sizeof(hdr.filename) - 1);
    hdr.file_size  = size;
    hdr.file_index = index;
    hdr.file_total = total;
    if (!gMagiLink.send(&hdr, sizeof(hdr))) {
        Serial.printf("[BK-SRV] header send failed at file %u\n", (unsigned)index);
        return false;
    }

    File f;
    { SdLock _; f = SD.open(fullPath); }
    if (!f) {
        sBkFileFailures++;
        Serial.printf("[BK-SRV] file %u/%u OPEN FAILED '%s' (dir said size=%u)\n",
                      (unsigned)index, (unsigned)total, fullPath, (unsigned)size);
        return true;   // skip body, keep the run going
    }
    uint32_t openSize;
    { SdLock _; openSize = (uint32_t)f.size(); }
    if (openSize != size) {
        sBkFileFailures++;
        Serial.printf("[BK-SRV] file %u/%u SIZE MISMATCH dir=%u open=%u '%s'\n",
                      (unsigned)index, (unsigned)total, (unsigned)size,
                      (unsigned)openSize, fullPath);
    }
    sStreamBody.id     = MSG_BACKUP_BODY;      // song push may have left another id
    sStreamBody.length = sizeof(sStreamBody);  // …and a short wire length
    bool ok = true;
    uint32_t sent = 0;
    uint32_t t0 = millis();
    while (ok && sent < size) {
        uint32_t want = size - sent;
        if (want > sizeof(sStreamBody.data)) want = sizeof(sStreamBody.data);
        int got;
        { SdLock _; got = f.read(sStreamBody.data, want); }
        if (got <= 0) {
            sBkFileFailures++;
            Serial.printf("[BK-SRV] file %u/%u READ FAILED got=%d at %u/%u '%s'\n",
                          (unsigned)index, (unsigned)total, got,
                          (unsigned)sent, (unsigned)size, fullPath);
            break;
        }
        if ((uint32_t)got < want)
            Serial.printf("[BK-SRV] file %u/%u short read want=%u got=%d at %u\n",
                          (unsigned)index, (unsigned)total, (unsigned)want, got,
                          (unsigned)sent);
        sStreamBody.data_len = (uint16_t)got;
        ok = gMagiLink.send(&sStreamBody, sizeof(sStreamBody));
        if (!ok)
            Serial.printf("[BK-SRV] body send failed at file %u sent=%u\n",
                          (unsigned)index, (unsigned)sent);
        sent += got;
    }
    { SdLock _; f.close(); }
    Serial.printf("[BK-SRV] file %u/%u %s sent=%u/%u in %ums '%s'\n",
                  (unsigned)index, (unsigned)total,
                  sent == size ? "ok" : "INCOMPLETE",
                  (unsigned)sent, (unsigned)size, (unsigned)(millis() - t0), name);
    return ok;
}

// Pass 2: stream every matching file in `dir` (paths built as dir/name),
// advancing *idx.  The directory handle stays open across the per-file sends
// — the same open-across-SdLock-windows pattern backupStreamOne uses for the
// body, so no in-memory file list is ever needed.
static bool backupStreamDir(const char* dir, bool (*pred)(const char*),
                            uint16_t total, uint16_t* idx) {
    File d;
    { SdLock _; d = SD.open(dir); }
    if (!d || !d.isDirectory()) { if (d) { SdLock _; d.close(); } return true; }
    bool ok = true;
    for (;;) {
        char name[BK_FNAME_MAX];
        uint32_t size = 0;
        bool have = false, end = false;
        {
            SdLock _;
            File e = d.openNextFile();
            if (!e) {
                end = true;
            } else {
                if (!e.isDirectory()) {
                    const char* nm = e.name();
                    const char* sl = strrchr(nm, '/'); if (sl) nm = sl + 1;
                    bool m = pred(nm);
                    backupDumpName("stream", nm, (uint32_t)e.size(), m);
                    if (m) {
                        strncpy(name, nm, sizeof(name) - 1);
                        name[sizeof(name) - 1] = '\0';
                        size = (uint32_t)e.size();
                        have = true;
                    }
                }
                e.close();
            }
        }
        if (end) break;
        if (have) {
            char full[BK_FNAME_MAX + 32];
            snprintf(full, sizeof(full), "%s/%s", dir, name);
            (*idx)++;
            ok = backupStreamOne(name, full, size, *idx, total);
            if (!ok) break;
        }
    }
    { SdLock _; d.close(); }
    return ok;
}

void sendBackupToClient() {
    if (!srvSdOk) {
        Serial.println("[BK-SRV] SD not available — sending bare END_OF_DATA");
        MsgEndOfData eod;
        gMagiLink.send(&eod, sizeof(eod));
        return;
    }

    // Pass 1 — count so each header carries an accurate "X of Y".  Same
    // sources, same order as the streaming pass below.
    uint16_t total = 0;
    sBkFileFailures = 0;
    total += backupCountDir(SRV_SONGS_DIR, backupIsSong);
    { SdLock _; if (SD.exists(SRV_INSTRUMENTS_PATH)) total++; }
    total += backupCountDir(SRV_DRUMTRACKS_DIR, backupIsDrumtrack);
    total += backupCountDir(SRV_SETLISTS_DIR, backupIsSetlist);
    Serial.printf("[BK-SRV] sending %u files\n", (unsigned)total);

    // Pass 2 — stream everything, in the same order the count visited it.
    // The client restore routes each file back by extension: .mgt → /songs
    // (instruments.mgt → root), .txt → /drumtracks, .set → /setlists.
    uint16_t idx = 0;
    bool ok = backupStreamDir(SRV_SONGS_DIR, backupIsSong, total, &idx);
    if (ok) {                                   // instruments.mgt (root)
        uint32_t isz = 0; bool exists = false;
        { SdLock _; File ins = SD.open(SRV_INSTRUMENTS_PATH);
          if (ins) { exists = true; isz = (uint32_t)ins.size(); ins.close(); } }
        if (exists) { idx++; ok = backupStreamOne("instruments.mgt",
                                    SRV_INSTRUMENTS_PATH, isz, idx, total); }
    }
    if (ok) ok = backupStreamDir(SRV_DRUMTRACKS_DIR, backupIsDrumtrack, total, &idx);
    if (ok) ok = backupStreamDir(SRV_SETLISTS_DIR, backupIsSetlist, total, &idx);

    if (idx != total)
        Serial.printf("[BK-SRV] COUNT/STREAM MISMATCH counted=%u streamed=%u\n",
                      (unsigned)total, (unsigned)idx);
    MsgEndOfData eod;
    gMagiLink.send(&eod, sizeof(eod));
    if (sBkFileFailures) ok = false;        // a backup with unreadable files is not OK
    Serial.printf("[BK-SRV] backup done (ok=%d) streamed=%u/%u failed=%u free=%u\n",
                  ok ? 1 : 0, (unsigned)idx, (unsigned)total,
                  (unsigned)sBkFileFailures, (unsigned)ESP.getFreeHeap());
}

// ── Reload the active song from SD into srvActiveBuf; notify client ─────────
// Used when a restore has replaced the active song's file.  Goes through the
// staged loader, so a file that will not load leaves the song in memory as it
// is (and playing) instead of dropping the server to No Song.
static void reloadActiveSongFromSD() {
    // Own copy of the name: the loader writes srvActiveName on success.
    char name[SRV_FNAME_MAX];
    strncpy(name, srvActiveName, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, name);

    bool    activeKept = true;
    uint8_t fail = srvLoadSongFile(path, name, &activeKept);
    if (fail) {
        Serial.printf("[CMD] reload: load failed '%s' (reason %u) — %s\n",
                      path, (unsigned)fail,
                      activeKept ? "song in memory kept" : "active song LOST");
        if (!activeKept) sendNoSongToClient();   // the in-place read wiped it — say so
        return;
    }
    // The loader left the sequencer stopped; put it at the top of the reloaded
    // song (its old position may not exist in it) with its patches up, as
    // every other load does.
    sequencerReset();
    sequencerSendPatchesNow();
    Serial.printf("[CMD] reloaded active song '%s' %u bytes\n", path, (unsigned)srvActiveBufLen);

    // Tiny changed-event via the kick socket; the client pulls the song
    // with MSG_ACTIVE_SONG_REQ when it is ready.  Fire-and-forget: if the
    // event is lost the client recovers by pulling on the next connect.
    if (pairingIsConnected()) {
        MsgActiveSongChanged ev;
        gMagiLink.postToPeer(&ev, sizeof(ev));
    }
}

// ── Send active song buffer to the connected client ───────────────────────────
// Called from commandsTick() as the response to a client pull or an explicit
// load command — never volunteered.  Streams from
// srvActiveBuf over MagiLink as HEADER + N×BODY (no SD read needed).
// Body is `static` because 1029 bytes is too big for any caller's stack.
static void sendActiveSongToClient() {
    if (!srvHasActive || srvActiveBufLen == 0) return;
    if (!gMagiLink.isConnected()) {
        Serial.println("[CMD] song push: link not connected");
        return;
    }

    // Quiet our own radio for the duration: the PixelPost audio broadcasts
    // share the WiFi TX buffer pool and the channel with this stream.
    pixelpostAudioHold(true);
    gMagiLink.acquireMutex();

    MsgSongPushHeader hdr;
    hdr.total_size = srvActiveBufLen;
    // The file this song is (with .mgt); empty for a song with no file yet.
    memset(hdr.name, 0, sizeof(hdr.name));
    strncpy(hdr.name, srvActiveName, sizeof(hdr.name) - 1);
    bool ok = gMagiLink.send(&hdr, sizeof(hdr));

    // Reuse the shared streaming body buffer; flip its id since backup may
    // have left it as MSG_BACKUP_BODY.
    sStreamBody.id = MSG_SONG_PUSH_BODY;

    // 512-byte chunks sent as truncated frames (~517B on air, not the full
    // 1029B struct): during RF noise spells ~1KB frames were lost on air
    // for 10s+ while sub-512B frames kept passing (with 11b disabled there
    // is no slow DSSS fallback).  The wire `length` field must match the
    // truncated size or the receiver's framing desyncs.
    uint32_t sent = 0;
    while (ok && sent < srvActiveBufLen) {
        uint32_t remain = srvActiveBufLen - sent;
        uint16_t chunk  = remain > SRV_PUSH_CHUNK ? SRV_PUSH_CHUNK : (uint16_t)remain;
        uint16_t wire   = (uint16_t)(offsetof(MsgBackupBody, data) + chunk);
        sStreamBody.data_len = chunk;
        sStreamBody.length   = wire;
        memcpy(sStreamBody.data, srvActiveBuf + sent, chunk);
        ok = gMagiLink.send(&sStreamBody, wire);
        sent += chunk;
    }

    gMagiLink.releaseMutex();
    pixelpostAudioHold(false);
    Serial.printf("[CMD] song push: %u bytes (%s)\n",
                  (unsigned)srvActiveBufLen, ok ? "OK" : "FAIL");
}

// ── Run one queued note edit (loop task) ──────────────────────────────────────
// Validated here, applied on the MIDI task: sequencerApplyNoteSet does the
// NoteGrid::set there, serialised with playback, and re-seats the play cursor
// — the loop task never relinks the note pool under a playing walk.
// Skipped-but-consumed while no song is active (set to OFF, or an in-place
// full push in flight — the push supersedes them).
static void applyQueuedNoteSet(uint8_t slot) {
    const MsgNoteSet& m = sNoteSetQ[slot];
    if (!srvHasActive) return;
    const Song* song = (const Song*)(srvActiveBuf + sizeof(SongFileHeader));
    // The sequencer indexes its per-column state by these — refuse a cell
    // that is outside the grid rather than store it.
    if (m.pattern >= song->numPatterns || m.pattern >= MAX_PATTERNS ||
        m.row >= MAX_ROWS || m.col >= MAX_COLUMNS) return;
    // Always passed on — NoteGrid::set clears internally when *all* fields
    // are empty, but persists attr-only / vel-only rows.
    sequencerApplyNoteSet(m.pattern, m.row, m.col,
                          m.note.note, m.note.velocity, m.note.effect, m.note.param);
}

// ── The on-screen song list follows the card ──────────────────────────────────
// The SONGS screen shows a list read at boot.  After a save-as / new / delete /
// restore it no longer matches /songs, so re-read it and put the highlight
// back on whatever is actually loaded (OFF if that has no file yet).
static void srvRefreshScreenSongList() {
    loadSongList();
    cursor = 0;
    if (srvHasActive && srvActiveName[0] != '\0') {
        for (int i = 0; i < numSongs; i++) {
            char fn[SRV_FNAME_MAX + 8];
            snprintf(fn, sizeof(fn), "%s.mgt", songNames[i]);
            if (strcasecmp(fn, srvActiveName) == 0) { cursor = i + SONG_IDX_OFFSET; break; }
        }
    }
    if (cursor >= scrollOffset + UI_VISIBLE_ROWS)
        scrollOffset = cursor - UI_VISIBLE_ROWS + 1;
    needsFullRedraw = true;
}

// ── Run one queued song operation (loop task) ─────────────────────────────────
static void runSongOp(uint8_t slot) {
    const SongOp& op = sSongOpQ[slot];
    switch (op.kind) {

    case OP_LOAD_NAME: {
        // Mirror the load-by-index path: stop+reset the sequencer and
        // sync the on-screen cursor so the row highlight tracks the
        // song the client just pushed to us via the setlist.  Only when
        // the load actually took — a failed one leaves the previous song
        // active and playing, and must not reset it.
        if (sendSongDataByName(op.name)) {
            Serial.printf("[STOPSRC] load-by-name '%s' t=%lu\n",
                          op.name, millis());
            sequencerStop();
            sequencerReset();
            sequencerSendPatchesNow();   // patches up before the first note
            int matchIdx = -1;
            for (int i = 0; i < numSongs; i++) {
                if (strcmp(songNames[i], op.name) == 0) {
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
        break;
    }

    case OP_LOAD_INDEX: {
        int fileIdx = (int)op.a * SL_PER_PKT + (int)op.b;
        if (sendSongData(op.a, op.b)) {
            Serial.printf("[STOPSRC] load-by-index fileIdx=%d t=%lu\n",
                          fileIdx, millis());
            sequencerStop();
            sequencerReset();
            sequencerSendPatchesNow();   // patches up before the first note
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
        break;
    }

    case OP_PUSH_DONE:
        finaliseMagiLinkSongSave();
        break;

    case OP_SAVE_ACTIVE:
        finaliseSaveActive(op.name, op.a != 0);
        break;

    case OP_NEW_SONG:
        finaliseNewSong();
        break;

    case OP_SET_OFF:
        // Same as the server's local "-- OFF --" selection (srvSetOff: stop,
        // drop the active song, tell the client), then sync the on-screen
        // cursor to OFF.
        srvSetOff();
        cursor          = 0;   // 0 = OFF (SONG_IDX_OFFSET)
        needsFullRedraw = true;
        break;

    case OP_DELETE: {
        char path[48];
        snprintf(path, sizeof(path), "%s/%s.mgt", SRV_SONGS_DIR, op.name);
        {
            SdLock _;
            if (SD.exists(path)) {
                SD.remove(path);
                Serial.printf("[CMD] deleted '%s'\n", path);
            } else {
                snprintf(path, sizeof(path), "%s/%s", SRV_SONGS_DIR, op.name);
                if (SD.exists(path)) {
                    SD.remove(path);
                    Serial.printf("[CMD] deleted '%s'\n", path);
                }
            }
            // Also wipe any matching autosave draft, so no copy of the song
            // we just deleted is left on the card.  (Drafts are never loaded
            // automatically — see the autosave-policy note.)
            char autopath[48];
            snprintf(autopath, sizeof(autopath), "%s/%s.mgt",
                     SRV_AUTOSAVE_DIR, op.name);
            if (SD.exists(autopath)) {
                SD.remove(autopath);
                Serial.printf("[CMD] deleted autosave '%s'\n", autopath);
            }
        }
        // Deleting the song that is loaded sets the server to OFF, the same
        // as choosing "-- OFF --": sequencer stopped, name cleared (so a later
        // save or restore can't be matched against a file that is gone), the
        // client told, the highlight on the OFF row.
        char expectedActive[SRV_FNAME_MAX];
        snprintf(expectedActive, sizeof(expectedActive), "%s.mgt", op.name);
        if (srvHasActive && strcasecmp(expectedActive, srvActiveName) == 0) {
            Serial.printf("[STOPSRC] deleted the active song '%s' t=%lu\n",
                          expectedActive, millis());
            srvSetOff();
            cursor          = 0;   // 0 = OFF (SONG_IDX_OFFSET)
            needsFullRedraw = true;
        }
        sSongListDirty = true;
        break;
    }

    case OP_PATCH: {
        MsgSetSongData m;
        m.offset  = op.offset;
        m.dataLen = op.len;
        memcpy(m.data, op.data, op.len);
        applySongPatch(&m);
        break;
    }

    default:
        break;
    }
}

// ── Tick — call from main loop() to run deferred operations ──────────────────
void commandsTick() {
    // Note edits and song operations, merged into the order they arrived.
    // An operation can take a second (SD read + 35 KB stream); anything that
    // arrives meanwhile simply queues behind it.
    for (;;) {
        bool haveNote = (sNoteSetQTail != sNoteSetQHead);
        bool haveOp   = (sSongOpTail  != sSongOpHead);
        if (!haveNote && !haveOp) break;
        bool noteFirst = haveNote;
        if (haveNote && haveOp) {
            uint16_t ns = sNoteSetSeq[sNoteSetQTail & (NOTE_SET_Q_LEN - 1)];
            uint16_t os = sSongOpQ[sSongOpTail & (SONG_OP_Q_LEN - 1)].seq;
            noteFirst = (int16_t)(ns - os) < 0;          // wrap-safe "ns is older"
        }
        if (noteFirst) {
            applyQueuedNoteSet(sNoteSetQTail & (NOTE_SET_Q_LEN - 1));
            sNoteSetQTail++;
        } else {
            runSongOp(sSongOpTail & (SONG_OP_Q_LEN - 1));
            sSongOpTail++;                                // only now is it "done"
        }
    }
    if (sSongListDirty) {
        sSongListDirty = false;
        srvRefreshScreenSongList();
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
    // Sample list AFTER the trim save: the manifest sync it runs re-reads
    // trim.txt, which would throw away a trim edit not yet written out.
    if (srvSampleListPending) {
        srvSampleListPending = false;
        sendSampleList(srvSampleListPendingPage);
    }
    if (sSampleInfoPending) {
        sSampleInfoPending = false;
        sendSampleInfo(sSampleInfoId, sSampleInfoVs, sSampleInfoVe);
    }
    if (sSampleSpecPending) {
        sSampleSpecPending = false;
        sendSampleSpectrum(sSampleSpecId);
    }

    // Client pulled the active song (MSG_ACTIVE_SONG_REQ — sent on its
    // connect edge and whenever we post MSG_ACTIVE_SONG_CHANGED).  Deferred
    // here so the ~750ms stream runs on the main task, which also owns all
    // srvActiveBuf mutation — no torn reads.  This is the ONLY way the song
    // stream leaves the server besides an explicit load command: bulk data
    // flows solely as the response to something the client asked for.
    if (srvActiveSongReqPending) {
        srvActiveSongReqPending = false;
        if (srvHasActive) {
            sendActiveSongToClient();
        } else {
            sendNoSongToClient();   // nothing loaded — client shows "NO SONG"
        }
    }

    // A full-song push whose link dropped mid-stream never reaches
    // OP_PUSH_DONE — take its crash breadcrumb down.  (A completed push that
    // is merely waiting its turn in the queue is not abandoned.)
    if (sPushPhaseArmed && !sPushCommitPending &&
        (!gMagiLink.isConnected() || gMagiLink.generation() != sPushGen)) {
        sPushPhaseArmed = false;
        crashSetPhase(CP_IDLE);
        Serial.printf("[CMD] full-song push abandoned — link dropped mid-stream (%s)\n",
                      srvPushScratch ? "current song kept" : "received in place: no active song");
    }

    // Follow-ups to a restore.  The files themselves were renamed into place
    // on the worker as each upload completed (see finaliseMagiLinkRestore);
    // these touch srvInstruments / srvActiveBuf, so they run here.
    if (sRestoreInstReloadPending) {
        sRestoreInstReloadPending = false;
        bool ok = loadInstrumentsFromSD();
        Serial.printf("[CMD] instruments %s after restore\n",
                      ok ? "reloaded" : "NOT reloaded — restored file did not load, memory unchanged");
    }
    if (sRestoreSongReloadPending) {
        sRestoreSongReloadPending = false;
        if (srvHasActive && srvActiveName[0] != '\0') {
            Serial.printf("[CMD] restored file is the active song — reloading\n");
            reloadActiveSongFromSD();
        }
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
            // Deferred to the loop task — the list is a directory scan plus a
            // manifest rewrite.  Latest-wins, like the other list requests.
            srvSampleListPendingPage = ((const MsgSampleListReq*)data)->page;
            srvSampleListPending     = true;
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
                songOpPush(OP_LOAD_INDEX, nullptr, 0, req->page, req->index);
            }
            break;

        case MSG_ACTIVE_SONG_REQ:
            srvActiveSongReqPending = true;
            break;

        case MSG_SONG_LOAD_NAME:
            if (len < (int)sizeof(MsgSongLoadNameReq)) return;
            {
                const MsgSongLoadNameReq* req = (const MsgSongLoadNameReq*)data;
                songOpPush(OP_LOAD_NAME, req->name, sizeof(req->name) - 1, 0, 0);
            }
            break;

        case MSG_SET_SONG_DATA:
            // MagiLink wire: id(1) + length(2) + offset(2) + dataLen(1) + data[dataLen]
            {
                const int hdrLen = (int)offsetof(MsgSetSongData, data);
                if (len < hdrLen) return;
                const MsgSetSongData* p = (const MsgSetSongData*)data;
                if (len < hdrLen + (int)p->dataLen) return;
                // Inline for immediacy (live BPM / mute edits) — unless a song
                // operation is queued or running, when it must keep its place.
                if (sSongOpHead != sSongOpTail) songOpPushPatch(p->offset, p->data, p->dataLen);
                else                            applySongPatch(p);
            }
            break;

        case MSG_NOTE_SET:
            if (len < (int)sizeof(MsgNoteSet)) return;
            {
                // A note edit relinks the live note pool.  Queue it:
                // commandsTick takes it in arrival order with saves and
                // loads and hands it to the MIDI task to apply.
                MsgNoteSet m = *(const MsgNoteSet*)data;   // `data` is the link's buffer
                if ((uint8_t)(sNoteSetQHead - sNoteSetQTail) >= NOTE_SET_Q_LEN) {
                    // Full: a paste / drum import sends more than one loop pass
                    // drains.  Wait for room — but NOT holding the link mutex:
                    // the loop task that empties this queue may itself be
                    // blocked waiting to send, and then neither side moves.
                    gMagiLink.releaseMutex();
                    uint32_t t0 = millis();
                    while ((uint8_t)(sNoteSetQHead - sNoteSetQTail) >= NOTE_SET_Q_LEN &&
                           millis() - t0 < 2000)
                        delay(1);
                }
                if ((uint8_t)(sNoteSetQHead - sNoteSetQTail) >= NOTE_SET_Q_LEN) {
                    Serial.println("[CMD] note-set queue stuck full for 2s — DROPPED an edit");
                    break;
                }
                uint8_t slot = sNoteSetQHead & (NOTE_SET_Q_LEN - 1);
                sNoteSetQ[slot]   = m;
                sNoteSetSeq[slot] = sArrivalSeq++;
                sNoteSetQHead++;
            }
            break;

        case MSG_SONG_DELETE:
            if (len < (int)sizeof(MsgSongDelete)) return;
            {
                const MsgSongDelete* d = (const MsgSongDelete*)data;
                songOpPush(OP_DELETE, d->name, sizeof(d->name) - 1, 0, 0);   // SD work → loop task
            }
            break;

        case MSG_INSTRUMENTS_REQ:
            srvInstLoadPending = true;
            break;

        case MSG_INSTRUMENTS_PATCH:
            // MagiLink wire: id(1) + length(2) + offset(2) + dataLen(1) + data[dataLen]
            {
                const int hdrLen = (int)offsetof(MsgInstrumentsPatch, data);
                if (len < hdrLen) return;
                const MsgInstrumentsPatch* p = (const MsgInstrumentsPatch*)data;
                if (len < hdrLen + (int)p->dataLen) return;
                uint32_t end = (uint32_t)p->offset + p->dataLen;
                if (srvInstruments && end <= SRV_INSTRUMENTS_BYTES) {
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
                else if (m->op == ORGAN_OP_VOCODER)   organSetVocoder(m->value);
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
                    sSampleSpecId      = m->sampleId;   // FFT over SD data —     
                    sSampleSpecPending = true;          // deferred to the loop
                }
            }
            break;

        default:
            break;
    }
}

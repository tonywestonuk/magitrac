// MagiTrac — ProTracker-style MIDI sequencer for LilyGo T5 S3
// Display: 960×540 e-paper via EPD_Painter (Adafruit GFX binding)
// Touch:   GT911_Lite (polling, no interrupts)
// Audio:   MIDI over WiFi (future) — UI only for now


//#define EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS

#include <magitrac_lib.h>
#include <WiFi.h>
#include "EPD_Painter_presets.h"
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerEngine.h"
#include "TrackerUI.h"
#include "TouchHandler.h"
#include "NoteEditor.h"
#include "PageManager.h"
#include "BlockSettingsPage.h"
#include "ColumnNoteEditorPage.h"
#include "SongStorage.h"
#include "SongPage.h"
#include "ServerPairing.h"
#include "SongSource.h"
#include "BootMenu.h"
#include "InstrumentsPage.h"
#include "SongConfigPage.h"
#include "ColumnEditor.h"
#include "SettingsPage.h"
#include "BackupRestorePage.h"
#include "PerformancePage.h"
#include "Battery.h"
#include <SD.h>
#include <I2C_BM8563.h>



// ── Backlight + boot button ───────────────────────────────────────────────────
#define BOARD_BL_EN   11   // PWM backlight enable — LilyGo T5 S3
#define BOOT_BTN_PIN   0   // GPIO 0 = BOOT button (active LOW, internal pull-up)
static bool gBacklightOn    = true;
static bool gBootBtnWasDown = false;

// ── SD card pins ──────────────────────────────────────────────────────────────
// LilyGo T5 4.7" S3
#define SD_CS_LILYGO    12
#define SD_SCLK_LILYGO  14
#define SD_MISO_LILYGO  21
#define SD_MOSI_LILYGO  13
// M5PaperS3
#define SD_CS_M5        47
#define SD_SCLK_M5      39
#define SD_MISO_M5      40
#define SD_MOSI_M5      38

// ── Hardware objects ──────────────────────────────────────────────────────────

EPD_PainterAdafruit display(EPD_PAINTER_PRESET);   // AUTO board detection
GT911_Lite          touch;

// ── Application objects ───────────────────────────────────────────────────────

// Heap-allocated so CONFIG_SPIRAM_USE_MALLOC routes them to PSRAM (>4 KB threshold).
// Using Song& keeps all call sites unchanged.
Song&         song         = *new Song();
Instrument*   gInstruments =  new Instrument[MAX_INSTRUMENTS]();
TrackerEngine engine(&song);
TrackerUI     ui(display, song, engine);
TouchHandler  handler(touch, ui, engine, song);
NoteEditor         noteEditor(display, touch);
PageManager        pageManager(display, touch, song);
BlockSettingsPage  blockSettings(display, touch, song);
ColumnNoteEditorPage noteEditorPage(display, touch, song);
SongPage           songPage(display, touch, song);
BootMenu           bootMenu(display, touch);
InstrumentsPage    instrumentsPage(display, touch, gInstruments);
SongConfigPage     songConfigPage(display, touch, song);
ColumnEditor       columnEditor(display, touch, song, gInstruments);
PerformancePage    performancePage(display, touch, song);
I2C_BM8563*        rtc = nullptr;
SettingsPage*      settingsPage = nullptr;
BackupRestorePage* backupRestorePage = nullptr;

bool gSdAvailable = false;
int  gSdCs        = -1;   // chip-select pin used for SD; set in setup()
SongSource gSongSource = SongSource::NONE;

// ── MIDI passthrough state (client → server) ──────────────────────────────────
// Tracker note → MIDI note: midi = tracker + 11  (tracker C4=49, MIDI C4=60)
#define TRACKER_TO_MIDI  11
static uint8_t gMidiActiveNote[MAX_COLUMNS] = {};  // 0 = no note active on that channel

static void sendMidiRow(uint8_t patIdx, uint8_t row) {
    if (!gServerPairing.isPaired()) return;
    NoteGrid grid(song.notePool, &song.noteFreeHead, &song.patterns[patIdx].noteHead);
    grid.forRow(row, [](uint8_t col, const Note& n, void* /*ctx*/) {
        if (col == INPUT_COLUMN) return;
        if (n.note == NOTE_EMPTY) return;
        if (gMidiActiveNote[col] != 0) {
            uint8_t off[3] = { 0x80, gMidiActiveNote[col], 0 };
            gServerPairing.sendMidi(off, 3);
        }
        uint8_t midiNote = (uint8_t)(n.note + TRACKER_TO_MIDI);
        uint8_t on[3] = { 0x90, midiNote, 100 };
        gServerPairing.sendMidi(on, 3);
        gMidiActiveNote[col] = midiNote;
    }, nullptr);
}

static void sendMidiAllOff() {
    if (!gServerPairing.isPaired()) return;
    for (int col = 0; col < MAX_COLUMNS; col++) {
        if (gMidiActiveNote[col] != 0) {
            uint8_t off[3] = { 0x80, gMidiActiveNote[col], 0 };  // NoteOff, channel 1
            gServerPairing.sendMidi(off, 3);
            gMidiActiveNote[col] = 0;
        }
    }
}

// ── Pairing ceremony state (declared here so drawPairingScreen/pollPairingUi can use them)
static bool            gPairingUiWasDown = false;
static PairClientState gLastPairState    = PairClientState::IDLE;

// ── Pairing ceremony UI ───────────────────────────────────────────────────────

static void drawPairingScreen() {
    PairClientState state = gServerPairing.pairState();
    display.fillScreen(COL_WHITE);

    // Header bar
    display.fillRect(0, 0, 960, 80, COL_BLACK);
    uiButton(display, 0, 0, 960, 80, "PAIRING SETUP", COL_BLACK, COL_WHITE, 4);

    if (state == PairClientState::PAIRING_REQUEST) {
        uiButton(display, 100, 200, 760, 100, "Waiting for server...", COL_WHITE, COL_BLACK, 3);

    } else if (state == PairClientState::PAIRING_CONFIRM) {
        uiButton(display, 100, 140, 760, 80,  "Check code matches on server:", COL_WHITE, COL_BLACK, 3);
        // Large code display
        const uint8_t* code = gServerPairing.pairCode();
        char codeStr[6];
        snprintf(codeStr, sizeof(codeStr), "%c%c%c%c",
                 code[0], code[1], code[2], code[3]);
        uiButton(display, 280, 240, 400, 120, codeStr, COL_LTGREY, COL_BLACK, 6);
        // Confirm button
        uiButton(display, 280, 390, 400, 100, "CONFIRM", COL_BLACK, COL_WHITE, 4);

    } else if (state == PairClientState::PAIRING_WAITING) {
        uiButton(display, 100, 200, 760, 100, "Completing pairing...", COL_WHITE, COL_BLACK, 3);

    } else {
        // SUCCESS or IDLE — close the UI from loop()
    }

    // Cancel button (bottom-left)
    uiButton(display, 20, 440, 200, 80, "CANCEL", COL_WHITE, COL_BLACK, 3);

    display.paint();
}

// Handle touch while pairing UI is open. Returns true when the UI should close.
static bool pollPairingUi() {
    PairClientState state = gServerPairing.pairState();

    // Auto-close when ceremony finishes or connection established
    if (state == PairClientState::SUCCESS ||
        state == PairClientState::AUTO_CONNECTING ||
        state == PairClientState::IDLE) {
        return true;
    }

    if (!touch.read()) return false;
    bool down = touch.isTouched;
    int rx = touch.x, ry = touch.y;
    // rawToScreen: sx = ry, sy = 540 - rx  (same as BootMenu)
    int sx = ry, sy = 540 - rx;

    if (!down && gPairingUiWasDown) {
        gPairingUiWasDown = false;

        // CANCEL button (x=20..220, y=440..520)
        if (sx >= 20 && sx < 220 && sy >= 440 && sy < 520) {
            gServerPairing.cancelPairing();
            return true;
        }

        // CONFIRM button — only shown in PAIRING_CONFIRM state (x=280..680, y=390..490)
        if (state == PairClientState::PAIRING_CONFIRM &&
            sx >= 280 && sx < 680 && sy >= 390 && sy < 490) {
            gServerPairing.confirmPairCode();
            drawPairingScreen();   // redraw to show "Completing..." state
        }
    }
    if (down && !gPairingUiWasDown) gPairingUiWasDown = true;

    // Redraw if state changed (e.g. code arrived while waiting)
    PairClientState cur = gServerPairing.pairState();
    if (cur != gLastPairState) {
        gLastPairState = cur;
        drawPairingScreen();
    }

    return false;
}

// ── Redraw flags ──────────────────────────────────────────────────────────────

static bool     gWasPaired         = false;  // track connect edge to trigger instrument request
static bool     needsFullRedraw    = false;
static bool     gPairingUiOpen     = false;  // pairing ceremony screen is showing
static bool     needsHeaderRedraw  = false;
static bool     needsGridRedraw    = false;
static bool     needsStatusRedraw  = false;
static Page     lastPage          = Page::TRACKER;
static bool     songPageOpen        = false;
static bool     instrumentsPageOpen = false;
static bool     songConfigPageOpen  = false;
static bool     columnEditorOpen    = false;
static bool     noteEditorPageOpen  = false;
static bool     settingsPageOpen    = false;
static bool     backupRestorePageOpen = false;
static bool     performancePageOpen   = false;

// ── Grid pause state ─────────────────────────────────────────────────────
// When the user touches the grid while the server is playing we PAUSE (not STOP)
// so the server freezes in place.  On finger-up we UNPAUSE to resume.
// gGridPaused stays true across the note editor so UNPAUSE fires when it closes.
static bool     gGridPaused    = false;
static uint32_t gLastBattMs    = 0;
static int      gLastBattPct   = -2;   // -2 = never read
static uint32_t gLastClockMs   = 0;
static int8_t   gLastClockMin  = -1;   // force first update
static bool     gLastBattChg   = false;


// ── setup() ───────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    Serial.printf("[MEM] Before display.begin() — internal: %u  PSRAM: %u\n",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!display.begin()) {
        Serial.printf("[MEM] After fail — internal: %u  PSRAM: %u\n",
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        Serial.println("Display init failed — halting");
        while (1) {}
    }
    display.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
    display.clear();

    // Backlight — only available on LilyGo T5 boards (BOARD_BL_EN conflicts
    // with display pins on M5PaperS3)
    if (display.getPreset() != &EPD_M5PAPER_S3_PRESET) {
        pinMode(BOARD_BL_EN,  OUTPUT);
        analogWrite(BOARD_BL_EN, 255);   // backlight on at full brightness
    }

    TwoWire* wire = display.getConfig().i2c.wire;
    touch.begin(wire, 20);

    // RTC — BM8563 shares the display's I2C bus
    if (wire) {
        rtc = new I2C_BM8563(I2C_BM8563_DEFAULT_ADDRESS, *wire);
        rtc->begin();
        settingsPage = new SettingsPage(display, touch, *rtc);

        // Read initial clock for header display
        I2C_BM8563_TimeTypeDef t;
        I2C_BM8563_DateTypeDef d;
        rtc->getTime(&t);
        rtc->getDate(&d);
        ui.setClock(t.hours, t.minutes, d.date, d.month, d.year);
        gLastClockMin = t.minutes;
    }

    backupRestorePage = new BackupRestorePage(display, touch, rtc);

#if defined(EPD_PAINTER_PRESET_M5PAPER_S3)
    bool m5paper = true;
#elif defined(EPD_PAINTER_PRESET_AUTO)
    bool m5paper = (display.getPreset() == &EPD_M5PAPER_S3_PRESET);
#else
    bool m5paper = false;
#endif
    gSdCs        = m5paper ? SD_CS_M5    : SD_CS_LILYGO;
    int sdSclk   = m5paper ? SD_SCLK_M5  : SD_SCLK_LILYGO;
    int sdMiso   = m5paper ? SD_MISO_M5  : SD_MISO_LILYGO;
    int sdMosi   = m5paper ? SD_MOSI_M5  : SD_MOSI_LILYGO;
    SPI.begin(sdSclk, sdMiso, sdMosi, gSdCs);
    gSdAvailable = SD.begin(gSdCs);
    Serial.printf("[SD] %s\n", gSdAvailable ? "OK" : "not found / no card?");

    gServerPairing.begin();
    loadMidiLimits();
    loadWifiChannel();
    WiFi.setChannel(magiWifiChannelFromIdx(gWifiChannelIdx));
    Serial.printf("[WIFI] boot channel: %u\n",
                  (unsigned)magiWifiChannelFromIdx(gWifiChannelIdx));

    // Battery — LilyGo T5 S3 has BQ25896 PMIC on I2C with ADC fallback;
    // M5PaperS3 has neither, so leave battery backend as NONE.
    if (!m5paper) {
        battery_begin_adc(4, 2.0f);
        if (display.getConfig().i2c.wire)
            battery_begin_bq25896(display.getConfig().i2c.wire);
    }

    initSong(&song);

    initInstruments(gInstruments);
    if (gSdAvailable && !loadInstruments(gInstruments)) {
        saveInstruments(gInstruments);
    }

    ui.drawAll();
    display.paintLater();

    Serial.println("MagiTrac ready.");
}

// ── loop() ────────────────────────────────────────────────────────────────────

void loop() {
    uint32_t now = millis();

    // ── Boot button — toggle backlight ────────────────────────────────────────
    {
        bool down = (digitalRead(BOOT_BTN_PIN) == LOW);
        if (down && !gBootBtnWasDown) {
            gBacklightOn = !gBacklightOn;
            analogWrite(BOARD_BL_EN, gBacklightOn ? 255 : 0);
        }
        gBootBtnWasDown = down;
    }

    // ── Connection status text in header ─────────────────────────────────────
    {
        static char sStatusBuf[32]     = {};
        static char sLastStatusBuf[32] = {};

        PairClientState ps = gServerPairing.pairState();
        BrowseState     bs = gServerPairing.browseState();

        if (ps == PairClientState::AUTO_CONNECTING ||
            ps == PairClientState::REQUESTING      ||
            ps == PairClientState::PAIRING_REQUEST ||
            ps == PairClientState::PAIRING_CONFIRM ||
            ps == PairClientState::PAIRING_WAITING) {
            strncpy(sStatusBuf, "Connecting...", 31);
        } else if (ps == PairClientState::SUCCESS && bs == BrowseState::WAITING_SONG) {
            strncpy(sStatusBuf, "Downloading...", 31);
        } else if (ps == PairClientState::SUCCESS) {
            strncpy(sStatusBuf, "Connected", 31);
        } else {
            strncpy(sStatusBuf, "Standalone", 31);
        }
        sStatusBuf[31] = '\0';

        if (strcmp(sStatusBuf, sLastStatusBuf) != 0) {
            strncpy(sLastStatusBuf, sStatusBuf, 31);
            sLastStatusBuf[31] = '\0';
            ui.setStatus(sStatusBuf);
            needsHeaderRedraw = true;
        }
    }

    // ── Server pairing tick + instrument sync ────────────────────────────────
    {
        gServerPairing.tick();
        bool nowPaired = gServerPairing.isPaired();
        if (nowPaired && !gWasPaired) {
            // Just connected — request instruments from server
            gServerPairing.requestInstruments();
        }
        gWasPaired = nowPaired;

        // Copy instruments once the server's array has fully arrived
        if (gServerPairing.instrumentsReady()) {
            if (gServerPairing.copyInstruments(gInstruments)) {
                Serial.println("[INST] received instruments from server");
            }
            gServerPairing.resetInstruments();
        }
    }

    // ── Battery poll — every 30 s, redraw header if value changed ────────────
    if (now - gLastBattMs >= 30000 || gLastBattPct == -2) {
        gLastBattMs = now;
        int  pct = battery_read_pct();
        bool chg = battery_is_charging();
        if (pct >= 0 && (pct != gLastBattPct || chg != gLastBattChg)) {
            gLastBattPct = pct;
            gLastBattChg = chg;
            ui.setBattery(pct, chg);
            needsHeaderRedraw = true;
        } else if (pct < 0 && gLastBattPct == -2) {
            gLastBattPct = -1;   // mark first attempt complete; stop spamming retries
        }
    }

    // ── Clock poll — every 10 s, redraw header if minute changed ──────────────
    if (rtc && (now - gLastClockMs >= 10000 || gLastClockMin == -1)) {
        gLastClockMs = now;
        I2C_BM8563_TimeTypeDef t;
        I2C_BM8563_DateTypeDef d;
        rtc->getTime(&t);
        rtc->getDate(&d);
        if (t.minutes != gLastClockMin) {
            gLastClockMin = t.minutes;
            ui.setClock(t.hours, t.minutes, d.date, d.month, d.year);
            needsHeaderRedraw = true;
        }
    }

    // ── Page manager — stub page HOME button ─────────────────────────────────
    pageManager.poll();

    if (pageManager.trackerRedrawNeeded()) {
        ui.drawAll();
        display.paintLater();
    }

    // ── Boot menu overlay ─────────────────────────────────────────────────────
    if (bootMenu.isOpen()) {
        BootMenuResult bmr = bootMenu.poll();
        switch (bmr) {
            case BootMenuResult::SONG:
                songConfigPageOpen = true;
                songConfigPage.open();
                display.clear();
                songConfigPage.draw();
                display.paint();
                break;
            case BootMenuResult::INSTRUMENTS:
                instrumentsPageOpen = true;
                instrumentsPage.open();
                display.clear();
                instrumentsPage.draw();
                display.paint();
                break;
            case BootMenuResult::SETTINGS:
                if (settingsPage) {
                    settingsPageOpen = true;
                    settingsPage->open();
                    display.clear();
                    settingsPage->draw();
                    display.paint();
                }
                break;
            case BootMenuResult::BACKUP:
                if (backupRestorePage) {
                    backupRestorePageOpen = true;
                    backupRestorePage->open();
                    display.clear();
                    backupRestorePage->draw();
                    display.paint();
                }
                break;
            case BootMenuResult::PERFORM:
                if (gServerPairing.isPaired() && !gServerPairing.serverPlaying()) {
                    gServerPairing.sendControl(MSG_PLAY);
                }
                performancePageOpen = true;
                performancePage.open(engine.currentPattern());
                display.clear();
                performancePage.draw();
                display.paint();
                break;
            case BootMenuResult::PAIR:
                gPairingUiOpen    = true;
                gPairingUiWasDown = touch.isTouched;
                gLastPairState    = gServerPairing.pairState();
                gServerPairing.startPairCeremony();
                display.clear();
                drawPairingScreen();
                break;
            case BootMenuResult::DISMISSED:
                display.clear();
                ui.drawAll();
                display.paintLater();
                break;
            default:
                break;
        }
        return;
    }

    // ── Pairing ceremony UI ───────────────────────────────────────────────────
    if (gPairingUiOpen) {
        if (pollPairingUi()) {
            gPairingUiOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Server set to OFF — clear song and show message ──────────────────────
    if (gServerPairing.noSongPending()) {
        gServerPairing.clearNoSong();
        engine.stop();
        initSong(&song);
        ui.setSelected(0, 0);
        ui.setNoSong(true);
        gSongSource = SongSource::NONE;
        needsFullRedraw = true;
    }

    // ── Unsolicited song push (server pushed song on connect or song change) ──
    if (!songPageOpen &&
        gServerPairing.browseState() == BrowseState::SONG_READY) {
        if (gServerPairing.copySong(&song)) {
            engine.stop();
            ui.setSelected(0, 0);
            ui.setNoSong(false);
            gSongSource = SongSource::SERVER;
            songPage.setServerLoadedName(song.name);
            Serial.println("[SONG] applied server-pushed song");
        }
        gServerPairing.resetBrowse();
        needsFullRedraw = true;
    }

    // ── Backup/Restore page ─────────────────────────────────────────────────
    if (backupRestorePageOpen && backupRestorePage) {
        if (backupRestorePage->poll()) {
            backupRestorePageOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Performance page ──────────────────────────────────────────────────────
    if (performancePageOpen) {
        // Forward server position updates to the performance page
        {
            uint8_t rPat, rRow;
            if (gServerPairing.remoteSeqPos(&rPat, &rRow)) {
                performancePage.setPlayingPattern(rPat);
                engine.setPosition(rPat, rRow);
            }
        }
        // Sync perfPad config to server when changed
        if (gServerPairing.isPaired() && performancePage.patchPending()) {
            const uint8_t* padStart = (const uint8_t*)&song.perfPads;
            uint16_t padSize = (uint16_t)sizeof(song.perfPads);
            uint16_t off = 0;
            while (off < padSize) {
                uint8_t chunk = (padSize - off) > SONG_PATCH_MAX ? SONG_PATCH_MAX : (uint8_t)(padSize - off);
                gServerPairing.sendSongPatch(song, padStart + off, chunk);
                off += chunk;
            }
            performancePage.clearPatch();
        }
        if (performancePage.poll()) {
            performancePageOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Song config page ──────────────────────────────────────────────────────
    if (songConfigPageOpen) {
        if (gServerPairing.isPaired() && songConfigPage.patchPending()) {
            // Send all editable song-level fields in one patch (bpm..performerMask = 59 bytes)
            const uint8_t* start = (const uint8_t*)&song.bpm;
            const uint8_t* end   = (const uint8_t*)&song.performerMask + 1;
            gServerPairing.sendSongPatch(song, start, (uint8_t)(end - start));
            songConfigPage.clearPatch();
        }
        if (songConfigPage.poll()) {
            songConfigPageOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Instruments page ──────────────────────────────────────────────────────
    if (instrumentsPageOpen) {
        if (instrumentsPage.poll()) {
            instrumentsPageOpen = false;
            if (instrumentsPage.saveOnExit()) {
                if (gServerPairing.isPaired()) {
                    for (int i = 0; i < MAX_INSTRUMENTS; i++)
                        gServerPairing.sendInstrumentPatch(gInstruments, i);
                } else if (gSdAvailable) {
                    saveInstruments(gInstruments);
                }
            } else {
                // User chose NO — discard in-memory edits
                if (gServerPairing.isPaired()) {
                    gServerPairing.requestInstruments();  // async reload from server
                } else if (gSdAvailable) {
                    loadInstruments(gInstruments);
                }
            }
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Settings page ─────────────────────────────────────────────────────────
    if (settingsPageOpen && settingsPage) {
        if (settingsPage->poll()) {
            settingsPageOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Column editor page ──────────────────────────────────────────────────
    if (columnEditorOpen) {
        if (gServerPairing.isPaired() && columnEditor.patchPending()) {
            // Sync the edited column's settings to the server (per-song)
            uint8_t c = columnEditor.editCol();
            const uint8_t* colStart = (const uint8_t*)&song.columns[c];
            uint16_t colSize = (uint16_t)sizeof(ColumnSettings);
            uint16_t off = 0;
            while (off < colSize) {
                uint8_t chunk = (colSize - off) > SONG_PATCH_MAX ? SONG_PATCH_MAX : (uint8_t)(colSize - off);
                gServerPairing.sendSongPatch(song, colStart + off, chunk);
                off += chunk;
            }
            columnEditor.clearPatch();
        }
        if (gServerPairing.isPaired() && columnEditor.resyncMask()) {
            // After COPY / SWAP / CLEAR: push settings + every cell of every
            // affected column across all patterns up to its used length.
            uint16_t mask = columnEditor.resyncMask();
            for (uint8_t c = 0; c < MAX_COLUMNS; c++) {
                if (!(mask & (1u << c))) continue;
                const uint8_t* colStart = (const uint8_t*)&song.columns[c];
                uint16_t colSize = (uint16_t)sizeof(ColumnSettings);
                uint16_t off = 0;
                while (off < colSize) {
                    uint8_t chunk = (colSize - off) > SONG_PATCH_MAX
                                  ? SONG_PATCH_MAX : (uint8_t)(colSize - off);
                    gServerPairing.sendSongPatch(song, colStart + off, chunk);
                    off += chunk;
                }
                for (uint8_t p = 0; p < song.numPatterns; p++) {
                    uint8_t len = song.patterns[p].length;
                    for (uint8_t r = 0; r < len; r++) {
                        gServerPairing.sendNoteSet(song, p, r, c);
                    }
                }
            }
            columnEditor.clearResync();
        }
        if (columnEditor.poll()) {
            columnEditorOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Column note editor page ───────────────────────────────────────────────
    if (noteEditorPageOpen) {
        if (gServerPairing.isPaired() && noteEditorPage.notePending()) {
            uint8_t p = noteEditorPage.pendingPat();
            uint8_t r = noteEditorPage.pendingRow();
            uint8_t c = noteEditorPage.pendingCol();
            gServerPairing.sendNoteSet(song, p, r, c);
            noteEditorPage.clearPending();
        }
        if (gServerPairing.isPaired() && noteEditorPage.auditionPending()) {
            gServerPairing.sendAuditionNote(noteEditorPage.auditionPat(),
                                            noteEditorPage.auditionRow(),
                                            noteEditorPage.auditionCol());
            noteEditorPage.clearAuditionPending();
        }
        if (gServerPairing.isPaired() && noteEditorPage.bulkPending()) {
            gServerPairing.sendSongToServer("", &song);
            noteEditorPage.clearBulkPending();
        }
        uint8_t prevRow;
        if (gServerPairing.pollPreviewRow(&prevRow)) {
            noteEditorPage.setPreviewRow(prevRow);
        }
        bool exiting = noteEditorPage.poll();
        // Drain preview pending flags AFTER poll() — taps, HOME, and segment
        // navigation all set these from inside poll().
        if (gServerPairing.isPaired() && noteEditorPage.previewStartPending()) {
            gServerPairing.sendPreviewStart(noteEditorPage.previewStartPat(),
                                            noteEditorPage.previewStartCol());
            noteEditorPage.clearPreviewStartPending();
        }
        if (gServerPairing.isPaired() && noteEditorPage.previewStopPending()) {
            gServerPairing.sendPreviewStop();
            noteEditorPage.clearPreviewStopPending();
        }
        if (exiting) {
            noteEditorPageOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Block settings page ───────────────────────────────────────────────────
    if (pageManager.currentPage() == Page::BLOCKS) {
        // First frame on this page — open and draw
        if (lastPage != Page::BLOCKS) {
            lastPage = Page::BLOCKS;
            blockSettings.open(engine.currentPattern());
            blockSettings.draw();
            display.clear();
            display.paint();
        }
        if (gServerPairing.isPaired() && blockSettings.keyChangePending()) {
            uint8_t p = blockSettings.keyChangePat();
            gServerPairing.sendSongPatch(song, &song.patterns[p].keyChangeMode,
                                         sizeof(song.patterns[p].keyChangeMode));
            blockSettings.clearKeyChange();
        }
        if (gServerPairing.isPaired() && blockSettings.navChangePending()) {
            uint8_t p = blockSettings.navChangePat();
            gServerPairing.sendSongPatch(song, &song.patterns[p].blockEndNav,
                                         sizeof(song.patterns[p].blockEndNav));
            blockSettings.clearNavChange();
        }
        if (gServerPairing.isPaired() && blockSettings.didSplit()) {
            // Split modifies source pattern (truncated, notes removed) AND creates
            // a new pattern with notes. Sync immediately so server sees it.
            gServerPairing.sendSongToServer("", &song);
            blockSettings.clearSplit();
        }
        if (blockSettings.poll()) {
            if (gServerPairing.isPaired()) {
                // Structural changes (new, delete, duplicate, split, length)
                // affect noteHead links and note pool topology — send full song
                // so the server has a consistent copy.
                gServerPairing.sendSongToServer("", &song);
            }
            // HOME tapped — return to tracker
            lastPage = Page::TRACKER;
            pageManager.goHome();
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Song save/load page ───────────────────────────────────────────────────
    if (songPageOpen) {
        SongPageResult res = songPage.poll();
        if (res == SongPageResult::HOME) {
            songPageOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        } else if (res == SongPageResult::SONG_LOADED) {
            songPageOpen = false;
            engine.stop();
            ui.setSelected(0, 0);
            ui.setNoSong(false);
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    lastPage = pageManager.currentPage();

    // Skip tracker logic when not on the tracker page
    if (pageManager.currentPage() != Page::TRACKER) return;

    // ── Note editor popup — takes over all input while open ───────────────────
    if (noteEditor.isOpen()) {
        bool closed = noteEditor.pollTouch();
        // Sync any committed note (row navigation or OK) to server
        if (noteEditor.pendingSync() && gServerPairing.isPaired()) {
            uint8_t p = noteEditor.editPattern();
            uint8_t r = noteEditor.editRow();
            uint8_t c = noteEditor.editCol();
            gServerPairing.sendNoteSet(song, p, r, c);
            noteEditor.clearPendingSync();
        }
        if (closed) {
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;  // skip tracker input/tick while popup is open
    }

    // ── Server connection state ───────────────────────────────────────────────
    {
        bool paired = gServerPairing.isPaired();
        if (paired != ui.serverConnected()) {
            ui.setServerConnected(paired);
            if (!paired) ui.setServerPlaying(false);  // clear play state on disconnect
            needsFullRedraw = true;
        }
    }

    // ── Server play state ─────────────────────────────────────────────────────
    {
        bool playing = gServerPairing.serverPlaying();
        if (playing != ui.serverPlaying()) {
            ui.setServerPlaying(playing);
            needsFullRedraw = true;
        }
    }

    // ── Server position update ────────────────────────────────────────────────
    // Only follow server position while playing — when stopped the client owns
    // the view so the user can freely browse blocks for editing.
    {
        uint8_t rPat, rRow;
        if (gServerPairing.remoteSeqPos(&rPat, &rRow) &&
            gServerPairing.serverPlaying()) {
            engine.setPosition(rPat, rRow);
            needsGridRedraw   = true;
            needsHeaderRedraw = true;
            needsStatusRedraw = true;
        }
    }

    // ── Handle touch ──────────────────────────────────────────────────────────
    TouchResult t = handler.poll();

    // ── Grid pause/unpause — finger press/release while server is playing ─────
    // Check isGridDown() AFTER poll() so state reflects the current frame.
    // PAUSE on grid press; UNPAUSE on grid release (unless note editor takes over).
    {
        bool gridDown = handler.isGridDown();
        if (gridDown && !gGridPaused && gServerPairing.isPaired() && gServerPairing.serverPlaying()) {
            gServerPairing.sendControl(MSG_PAUSE);
            gGridPaused = true;
        }
        // Unpause whenever the finger lifts — always, no exceptions.
        if (!gridDown && gGridPaused) {
            gServerPairing.sendControl(MSG_UNPAUSE);
            gGridPaused = false;
        }
    }

    // When no song is loaded, only allow song browser tap — skip everything else
    if (ui.noSong() && t.action != TouchAction::SONG_NAME_TAP
                     && t.action != TouchAction::MENU_TAP)
        t.action = TouchAction::NONE;

    switch (t.action) {

        case TouchAction::PLAY:
            if (gServerPairing.isPaired()) {
                gServerPairing.sendControl(MSG_PLAY);
            }
            break;

        case TouchAction::STOP:
            if (gServerPairing.isPaired()) {
                gServerPairing.sendControl(MSG_STOP);
                gGridPaused = false;
            }
            break;

        case TouchAction::BLOCK_PREV:
            if (engine.currentPattern() > 0) {
                engine.setPattern(engine.currentPattern() - 1);
                if (gServerPairing.isPaired())
                    gServerPairing.sendGoto(engine.currentPattern(), engine.currentRow());
                ui.setSelected(0, ui.selectedCol());
                needsHeaderRedraw = true;
                needsGridRedraw   = true;
                needsStatusRedraw = true;
            }
            break;

        case TouchAction::BLOCK_NEXT:
            if (engine.currentPattern() + 1 < song.numPatterns) {
                engine.setPattern(engine.currentPattern() + 1);
                if (gServerPairing.isPaired())
                    gServerPairing.sendGoto(engine.currentPattern(), engine.currentRow());
                ui.setSelected(0, ui.selectedCol());
                needsHeaderRedraw = true;
                needsGridRedraw   = true;
                needsStatusRedraw = true;
            }
            break;

        case TouchAction::CELL_SELECT:
            ui.setSelected(t.row, t.col);
            needsGridRedraw = true;
            // Scrub: send seek to server when paused (jammed) or stopped
            if (gServerPairing.isPaired() &&
                (gGridPaused || !gServerPairing.serverPlaying())) {
                gServerPairing.sendSeek(engine.currentPattern(), (uint8_t)t.row);
            }
            break;

        case TouchAction::COL_SCROLL:
            needsFullRedraw = true;
            break;

        case TouchAction::SONG_NAME_TAP:
            songPageOpen = true;
            songPage.open(gSdAvailable);
            display.clear();
            display.fillScreen(COL_WHITE);
            songPage.draw();
            display.paint();
            return;

        case TouchAction::BLOCK_LABEL_TAP:
            pageManager.goBlocks();
            return;

        case TouchAction::MENU_TAP:
            bootMenu.open(ui.noSong());
            display.paintLater();
            return;

        case TouchAction::MUTE_TAP: {
            ColumnSettings& cs = song.columns[t.col];
            cs.mute = cs.mute ? 0 : 1;
            Serial.printf("[MUTE] col %d -> %s\n", t.col, cs.mute ? "MUTED" : "UNMUTED");
            // Redraw column headers to show updated icon
            ui.drawAll();
            display.paintLater();
            // Sync to server
            if (gServerPairing.isPaired()) {
                gServerPairing.sendSongPatch(song, &cs.mute, sizeof(cs.mute));
            }
            break;
        }

        case TouchAction::COLUMN_HEADER_TAP:
            columnEditorOpen = true;
            columnEditor.open(engine.currentPattern(), t.col);
            display.clear();
            columnEditor.draw();
            display.paint();
            return;

        case TouchAction::COLUMN_HEADER_HOLD:
            noteEditorPageOpen = true;
            noteEditorPage.open(engine.currentPattern(), t.col,
                                (uint8_t)ui.selectedRow());
            display.clear();
            noteEditorPage.draw();
            display.paint();
            return;

        case TouchAction::EDIT_MODE_TAP:
            ui.toggleEditMode();
            needsStatusRedraw = true;
            break;

        case TouchAction::STEP_ADVANCE_TAP:
            ui.cycleStepAdvance();
            needsStatusRedraw = true;
            break;

        case TouchAction::VEL_CAPTURE_TAP:
            ui.toggleVelCapture();
            needsStatusRedraw = true;
            break;

        case TouchAction::CELL_TAP:
            // Only open note editor when server is stopped — while playing, a tap
            // is purely a "pause while touching / unpause on release" gesture.
            if (gServerPairing.serverPlaying()) break;
            // Track last-edited output column for MIDI step-input
            if (t.col >= 1) {
                ui.setMidiInputCol((uint8_t)t.col);
                needsFullRedraw = true;  // redraw to update column header indicator
            }
            // Open note editor — clear first (page change)
            display.clear();
            noteEditor.open(&song, engine.currentPattern(), t.row, t.col, nullptr, gInstruments);
            noteEditor.draw();
            display.paintLater();
            return;  // skip redraw logic below

        default:
            break;
    }

    // ── MIDI step-input: place note at selected column when stopped ───────────
    {
        uint8_t midiNote;
        uint8_t midiVelocity;
        if (!gServerPairing.serverPlaying() &&
            !ui.noSong() &&
            ui.editMode() &&
            gServerPairing.pollMidiNoteIn(&midiNote, &midiVelocity))
        {
            uint8_t col = ui.midiInputCol();
            uint8_t pat = engine.currentPattern();
            uint8_t row = (uint8_t)ui.selectedRow();
            Pattern& p  = song.patterns[pat];

            if (col >= 1 && col < MAX_COLUMNS && row < p.length) {
                NoteGrid grid(song.notePool, &song.noteFreeHead, &p.noteHead);
                bool handled = false;

                if (midiNote == 21) {
                    // A-0: delete note at current cell
                    grid.clear(row, col);
                    if (gServerPairing.isPaired())
                        gServerPairing.sendNoteSet(song, pat, row, col);
                    handled = true;
                } else if (midiNote == 22) {
                    // A#0: insert NOTE_OFF
                    Note n;
                    n.note      = NOTE_OFF;
                    n.velocity  = VEL_DEFAULT;
                    n.effect    = 0;
                    n.param     = 0;
                    grid.set(row, col, n);
                    if (gServerPairing.isPaired())
                        gServerPairing.sendNoteSet(song, pat, row, col);
                    handled = true;
                } else {
                    // Normal note — convert MIDI to tracker note
                    int trackerNote = (int)midiNote - 11;
                    if (trackerNote >= 1 && trackerNote <= NOTE_MAX) {
                        Note existing = grid.get(row, col);
                        Note n;
                        n.note      = (uint8_t)trackerNote;
                        n.velocity  = ui.velCapture()
                                    ? midiVelocity
                                    : ((existing.note == NOTE_EMPTY) ? VEL_DEFAULT : existing.velocity);
                        n.effect    = existing.effect;
                        n.param     = existing.param;
                        grid.set(row, col, n);
                        if (gServerPairing.isPaired())
                            gServerPairing.sendNoteSet(song, pat, row, col);
                        handled = true;
                    }
                }

                if (handled) {
                    // Clear skipped rows (between current and next) when step > 1
                    uint8_t step = ui.stepAdvance();
                    for (uint8_t s = 1; s < step; s++) {
                        uint8_t clearRow = (row + s) % p.length;
                        if (grid.has(clearRow, col)) {
                            grid.clear(clearRow, col);
                            if (gServerPairing.isPaired())
                                gServerPairing.sendNoteSet(song, pat, clearRow, col);
                        }
                    }

                    // Advance cursor by step amount
                    uint8_t nextRow = (row + step) % p.length;
                    ui.setSelected((int8_t)nextRow, ui.selectedCol());
                    needsGridRedraw = true;
                }
            }
        }
    }

    // ── Redraw changed regions ────────────────────────────────────────────────
    if (needsFullRedraw) {
        { uint32_t t0=millis(); ui.drawAll();         DBG("[TIMING] drawAll      %lums\n", millis()-t0); }
        { uint32_t t0=millis(); display.paintLater(); DBG("[TIMING] paintLater   %lums\n", millis()-t0); }
        needsFullRedraw   = false;
        needsHeaderRedraw = false;
        needsGridRedraw   = false;
        needsStatusRedraw = false;
        return;
    }

    bool painted = false;

    if (needsHeaderRedraw) {
        { uint32_t t0=millis(); ui.drawHeader();      DBG("[TIMING] drawHeader   %lums\n", millis()-t0); }
        needsHeaderRedraw = false;
        painted = true;
    }
    if (needsStatusRedraw) {
        { uint32_t t0=millis(); ui.drawStatusBar();   DBG("[TIMING] drawStatus   %lums\n", millis()-t0); }
        needsStatusRedraw = false;
        painted = true;
    }
    if (needsGridRedraw) {
        { uint32_t t0=millis(); ui.drawGrid();        DBG("[TIMING] drawGrid     %lums\n", millis()-t0); }
        needsGridRedraw = false;
        painted = true;
    }

    if (painted) {
        { uint32_t t0=millis(); display.paintLater(); DBG("[TIMING] paintLater   %lums\n", millis()-t0); }
    }
}

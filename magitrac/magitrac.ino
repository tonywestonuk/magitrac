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
#include "DrumEditorPage.h"
#include "ColumnNoteEditorPage.h"
#include "SongStorage.h"
#include "SongPage.h"
#include "ServerPairing.h"
#include "SongSource.h"
#include "BootMenu.h"
#include "InstrumentsPage.h"
#include "SongConfigPage.h"
#include "ColumnEditor.h"
#include "DrumTrackImportPage.h"
#include "SettingsPage.h"
#include "BackupRestorePage.h"
#include "PerformancePage.h"
#include "PixelPostManualPage.h"
#include "SetlistPage.h"
#include "OrganPage.h"
#include "WiFiSettingsPage.h"
#include "Battery.h"
#include "Autosave.h"
#include <SD.h>
#include <I2C_BM8563.h>



// ── Backlight + boot button ───────────────────────────────────────────────────
#define BOARD_BL_EN   11   // PWM frontlight enable — LilyGo T5 S3
#define BL_ON_LEVEL   128  // half brightness when on (e-paper holds its image
                           // with no light; the frontlight is battery-costly)
#define BOOT_BTN_PIN   0   // GPIO 0 = BOOT button (active LOW, internal pull-up)
static bool gBacklightOn    = false;   // off by default — boot button turns it on
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

// 180° flip (panel mounted upside down).  Set rotation back to ROTATION_0 and
// drop the touch.setRotate180() call below to revert.
EPD_PainterAdafruit display(EPD_PAINTER_PRESET.withRotation(EPD_Painter::Rotation::ROTATION_180));
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
DrumEditorPage     drumEditor   (display, touch, song);
ColumnNoteEditorPage noteEditorPage(display, touch, song);
SongPage           songPage(display, touch, song);
BootMenu           bootMenu(display, touch);
InstrumentsPage    instrumentsPage(display, touch, gInstruments);
SongConfigPage     songConfigPage(display, touch, song);
ColumnEditor       columnEditor(display, touch, song, gInstruments);
DrumTrackImportPage drumTrackImportPage(display, touch, song);
PerformancePage    performancePage(display, touch, song);
PixelPostManualPage pixelpostManualPage(display, touch);
SetlistPage        setlistPage(display, touch, song);
OrganPage          organPage(display, touch);
I2C_BM8563*        rtc = nullptr;
SettingsPage*      settingsPage = nullptr;
WiFiSettingsPage   wifiSettingsPage(display, touch);
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
        uiButton(display, 100, 200, 760, 100, "Put server in pair mode...", COL_WHITE, COL_BLACK, 3);

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

    // Redraw on state change BEFORE the touch early-return — otherwise
    // PIN never repaints on REQUEST→CONFIRM unless the user happens to
    // be touching the screen at that exact moment.
    if (state != gLastPairState) {
        gLastPairState = state;
        drawPairingScreen();
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
static bool     drumTrackImportOpen = false;
static bool     noteEditorPageOpen  = false;
static bool     drumEditorOpen      = false;
static bool     settingsPageOpen     = false;
static bool     wifiSettingsPageOpen = false;
static bool     backupRestorePageOpen = false;
static bool     performancePageOpen   = false;
static bool     pixelpostManualPageOpen = false;
static bool     setlistPageOpen       = false;
static bool     organPageOpen         = false;

// ── Autosave state ───────────────────────────────────────────────────────
// markSongDirty() is called from ServerPairing::sendSongPatch / sendNoteSet,
// which sit on every song-mutation path.  30 s after the last edit, if we're
// idle on the tracker page and there's a loaded SD filename, loop() shows a
// "Saving..." indicator and writes the song to SD.
static bool     gSongDirty = false;
static uint32_t gLastEditMs = 0;
static const uint32_t AUTOSAVE_DELAY_MS = 30000;
// After autosave we delay the tracker repaint by ~500 ms so the "Saving..."
// indicator stays visible (paint() already flushed it).  0 = no pending paint.
static uint32_t gAutosaveRepaintAtMs    = 0;
static const uint32_t AUTOSAVE_BANNER_MS = 500;

void markSongDirty() {
    gSongDirty  = true;
    gLastEditMs = millis();
}

void markSongClean() {
    gSongDirty = false;
}

bool songIsDirty() {
    return gSongDirty;
}

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
    delay(200);

    // Pre-init the WiFi driver BEFORE display.begin() takes LCD_CAM.
    // This claims WiFi's internal resources (netifs, driver structs) so
    // the WiFi.begin() call later — after display init — doesn't fight
    // with LCD_CAM for resources.  Keep AP_STA mode so ESP-NOW (for the
    // pairing ceremony) coexists with the STA association.
    // persistent(false) suppresses WiFi's own NVS writes of SSID/PSK.
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    // Disable STA modem-sleep.  The server's AP runs a 400ms beacon (RF-hum
    // mitigation for its audio path); with default power-save the client only
    // wakes at DTIM beacons, so the AP buffers our downlink UDP playhead
    // packets and flushes them in ~400ms lumps → the row cursor feels laggy.
    // Keeping the radio awake delivers them immediately.  Costs some current,
    // worth it for a live-performance client.
    WiFi.setSleep(false);

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
    display.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
    display.clear();

    // Display is up.  Bring up the pairing transport + (if paired) the
    // WiFi STA association to the server's AP.  Server is the AP at
    // 192.168.0.1; we get the static slot 192.168.0.2 issued at pair time.
    loadWifiChannel();   // populates gWifiChannelIdx for the settings UI
    gServerPairing.begin();

    // ── MagiLink (reliable TCP transport) ───────────────────────────────────
    //
    // Two paths depending on apMode:
    //   SERVER_AP   — server hosts its own AP at fixed MAGI_SERVER_IP and
    //                 stops softAP DHCP.  Client must assign itself a
    //                 static IP via WiFi.config, and the server's address
    //                 is known up-front, so beginConnect can fire here.
    //   EXTERNAL_AP — both devices join a third-party AP and take DHCP.
    //                 Server's address is no longer predictable; we defer
    //                 beginConnect until the first MSG_SERVER_ANNOUNCE
    //                 beacon arrives (handled by tickDiscoveryConnect()
    //                 in loop()).
    {
        char    creds_ssid[33] = {};
        char    creds_psk[64]  = {};
        uint8_t apMode         = 0;
        if (pairNvsLoadCreds("magitrac_cli", creds_ssid, creds_psk, &apMode)) {
            Serial.printf("[SETUP] WiFi creds present — STA→%s (apMode=%u)\n",
                creds_ssid, (unsigned)apMode);
            if (apMode == MAGI_AP_MODE_SERVER) {
                WiFi.config(IPAddress(MAGI_CLIENT_IP_0, MAGI_CLIENT_IP_1,
                                      MAGI_CLIENT_IP_2, MAGI_CLIENT_IP_3),
                            IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                                      MAGI_SERVER_IP_2, MAGI_SERVER_IP_3),
                            IPAddress(255, 255, 255, 0));
                WiFi.begin(creds_ssid, creds_psk);
                gMagiLink.beginConnect(MAGI_PORT,
                    IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                              MAGI_SERVER_IP_2, MAGI_SERVER_IP_3));
            } else {
                // EXTERNAL_AP — DHCP everywhere; server IP comes via
                // MSG_SERVER_ANNOUNCE discovery beacon.
                WiFi.begin(creds_ssid, creds_psk);
                Serial.println("[SETUP] EXTERNAL_AP — waiting for server discovery beacon");
            }
        } else {
            Serial.println("[SETUP] no WiFi creds — open WiFi page + pair to connect");
        }
    }

    // Backlight — only available on LilyGo T5 boards (BOARD_BL_EN conflicts
    // with display pins on M5PaperS3)
    if (display.getPreset() != &EPD_M5PAPER_S3_PRESET) {
        pinMode(BOARD_BL_EN,  OUTPUT);
        // Off by default (gBacklightOn=false); boot button toggles it on at
        // half brightness.  Respect the flag rather than forcing it on.
        analogWrite(BOARD_BL_EN, gBacklightOn ? BL_ON_LEVEL : 0);
    }

    TwoWire* wire = display.getConfig().i2c.wire;
    touch.begin(wire, 20);
    touch.setRotate180(true, 540, 960);   // match the 180° display flip (touch res 540×960)

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

    // gServerPairing.begin() was moved up — must run before display.begin()
    // (EPD_Painter Core-0 + GDMA + LCD_CAM grab breaks WiFi softAP attach
    // under m5stack:esp32 if it runs first).  See the note next to its
    // current call site at the top of setup().
    loadMidiLimits();
    loadPixelPostFlashCtrl();
    // loadWifiChannel() moved up — now runs before gServerPairing.begin()
    // so the SoftAP starts on the right channel.  Don't call WiFi.setChannel()
    // here: with the AP already up, changing channel from STA would either
    // be ignored or tear down the AP.

    // Battery — LilyGo T5 S3 has BQ25896 PMIC on I2C with ADC fallback;
    // M5PaperS3 has neither, so leave battery backend as NONE.
    if (!m5paper) {
        battery_begin_adc(4, 2.0f);
        if (display.getConfig().i2c.wire)
            battery_begin_bq25896(display.getConfig().i2c.wire);
    }

    initSong(&song);

    // Instruments are server-owned: start from defaults, then the server pushes
    // the real bank on connect (requestInstruments / copyInstruments in loop()).
    initInstruments(gInstruments);

    ui.drawAll();
    display.paintLater();

    Serial.println("MagiTrac ready.");
}

// ── loop() ────────────────────────────────────────────────────────────────────

void loop() {
    uint32_t now = millis();

    // ── Debug tick (2 s) — paired/link/STA snapshot, drop when connection is fine
    {
        static uint32_t sDbgMs = 0;
        if (now - sDbgMs >= 2000) {
            sDbgMs = now;
            const char* st;
            switch (WiFi.status()) {
                case WL_IDLE_STATUS:     st = "IDLE";       break;
                case WL_NO_SSID_AVAIL:   st = "NO_SSID";    break;
                case WL_SCAN_COMPLETED:  st = "SCAN_DONE";  break;
                case WL_CONNECTED:       st = "CONNECTED";  break;
                case WL_CONNECT_FAILED:  st = "FAILED";     break;
                case WL_CONNECTION_LOST: st = "LOST";       break;
                case WL_DISCONNECTED:    st = "DISCONNECT"; break;
                default:                 st = "?";          break;
            }
            Serial.printf("[DBG] STA=%s SSID='%s' RSSI=%ld IP=%s GW=%s | paired=%d link=%d\n",
                          st,
                          WiFi.SSID().c_str(),
                          WiFi.RSSI(),
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str(),
                          (int)gServerPairing.isPaired(),
                          (int)gMagiLink.isConnected());
        }
    }

    // ── Discovery-driven late connect (EXTERNAL_AP only) ──────────────────────
    //
    // In SERVER_AP mode beginConnect was fired at setup().  In EXTERNAL_AP
    // mode we defer until ServerPairing has seen a MSG_SERVER_ANNOUNCE
    // beacon; this kicks gMagiLink the moment that lands.  One-shot per
    // address — guarded by sIssuedForIp so we don't repeatedly call
    // beginConnect against the same target.
    {
        static IPAddress sIssuedForIp((uint32_t)0);
        if (gServerPairing.hasDiscoveredServerIP()) {
            IPAddress disc = gServerPairing.discoveredServerIP();
            uint16_t  port = gServerPairing.discoveredServerPort();
            if (disc != sIssuedForIp) {
                Serial.printf("[MAGI] discovery → beginConnect %s:%u\n",
                              disc.toString().c_str(), (unsigned)port);
                gMagiLink.beginConnect(port ? port : (uint16_t)MAGI_PORT, disc);
                sIssuedForIp = disc;
            }
        }
    }

    // ── Boot button — toggle backlight ────────────────────────────────────────
    {
        bool down = (digitalRead(BOOT_BTN_PIN) == LOW);
        if (down && !gBootBtnWasDown) {
            gBacklightOn = !gBacklightOn;
            analogWrite(BOARD_BL_EN, gBacklightOn ? BL_ON_LEVEL : 0);
        }
        gBootBtnWasDown = down;
    }

    // ── Connection status text in header ─────────────────────────────────────
    {
        static char sStatusBuf[32]     = {};
        static char sLastStatusBuf[32] = {};

        PairClientState ps = gServerPairing.pairState();
        BrowseState     bs = gServerPairing.browseState();

        // Status reflects the MagiLink socket state.  The legacy pairState
        // is still computed for the pairing-ceremony UI (PAIRING_REQUEST /
        // PAIRING_CONFIRM); browse state isn't surfaced here.
        (void)ps; (void)bs;
        if (gMagiLink.isConnected()) {
            strncpy(sStatusBuf, "Connected", 31);
        } else {
            strncpy(sStatusBuf, "Connecting...", 31);
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

    // ── Deferred post-autosave repaint ───────────────────────────────────────
    // Fires AUTOSAVE_BANNER_MS after the autosave finished, replacing the
    // "Saving..." indicator with the freshly-drawn tracker UI.
    if (gAutosaveRepaintAtMs != 0 && now >= gAutosaveRepaintAtMs) {
        gAutosaveRepaintAtMs = 0;
        display.paintLater();
    }

    // ── Autosave tick ────────────────────────────────────────────────────────
    // Fires only when sitting idle on the tracker page (no overlay open) with
    // a known server filename, paired, and 30 s have passed since the last
    // edit.  Edits mark dirty via markSongDirty() called from
    // ServerPairing::sendSongPatch / sendNoteSet — they sit on every
    // song-mutation path.  The save streams the song to the server, which
    // writes its own SD copy (the source of truth).
    if (gSongDirty && (now - gLastEditMs >= AUTOSAVE_DELAY_MS) &&
        gServerPairing.isPaired() && gMagiLink.isConnected() &&
        !bootMenu.isOpen() && !gPairingUiOpen &&
        !songPageOpen && !instrumentsPageOpen && !songConfigPageOpen &&
        !columnEditorOpen && !noteEditorPageOpen && !settingsPageOpen &&
        !wifiSettingsPageOpen && !backupRestorePageOpen &&
        !drumTrackImportOpen && !drumEditorOpen &&
        !performancePageOpen && !pixelpostManualPageOpen && !setlistPageOpen &&
        !organPageOpen &&
        !noteEditor.isOpen() &&
        pageManager.currentPage() == Page::TRACKER) {
        const char* srvName = songPage.srvLoadedName();
        if (srvName && srvName[0]) {
            // Small "Saving..." indicator overlaid on the bottom-left corner.
            const int bx = 8, by = 506, bw = 130, bh = 28;
            display.fillRect(bx, by, bw, bh, COL_WHITE);
            display.drawRect(bx, by, bw, bh, COL_BLACK);
            display.setTextSize(2);
            display.setTextColor(COL_BLACK);
            display.setCursor(bx + 14, by + 7);
            display.print("Saving...");
            display.paint();   // synchronous — make sure the indicator is visible

            // Tiny "save your in-memory song to SD under this name" — the
            // server already has every edit via the patch / note-set stream,
            // so we don't need to ship the song bytes again.  isAutosave=true
            // routes the server write to /autosave/<name>.mgt — the canonical
            // /songs/<name>.mgt is only touched by explicit Save.
            bool ok = gServerPairing.sendSaveActive(srvName, /*isAutosave=*/true);
            if (ok) gSongDirty = false;
            // If !ok (link dropped between the isConnected() gate and here)
            // push the retry out a full AUTOSAVE_DELAY_MS instead of leaving
            // it dirty — otherwise we'd re-attempt and re-paint the
            // synchronous "Saving..." banner on every loop iteration until
            // the link returns (an e-paper repaint storm).
            else    gLastEditMs = now;

            // Stage the tracker UI back into the framebuffer, but don't
            // flush yet — the display still shows the "Saving..." indicator
            // from the synchronous paint() above.  We let it sit for
            // AUTOSAVE_BANNER_MS, then a later loop tick fires paintLater
            // to push the framebuffer (which clears the indicator).
            ui.drawAll();
            gAutosaveRepaintAtMs = millis() + AUTOSAVE_BANNER_MS;
        }
        // No server filename: leave dirty; user can Save As to give it one.
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
            case BootMenuResult::PIXELPOST:
                pixelpostManualPageOpen = true;
                pixelpostManualPage.open(touch.isTouched);
                display.clear();
                pixelpostManualPage.draw();
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
            case BootMenuResult::DRAWBAR_ORGAN:
                organPageOpen = true;
                organPage.open();   // draws + tells the server to enter organ mode
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
        bool wasOff = (gSongSource == SongSource::NONE);
        engine.stop();
        initSong(&song);
        song.numPatterns = 0;            // OFF = no blocks → perform pads all grey
        ui.setSelected(0, 0);
        ui.setNoSong(true);
        gSongSource = SongSource::NONE;
        // If the server went OFF on its own (its own "-- OFF --"), reflect it
        // now.  When we already drove OFF locally (setlist no-file entry), this
        // is the echo and we skip the redundant redraw.
        if (!wasOff) {
            if (performancePageOpen) {
                performancePage.open(engine.currentPattern(), /*stopped=*/true);
                display.clear();
                performancePage.draw();
                display.paint();
            } else {
                needsFullRedraw = true;
            }
        }
    }

    // ── Unsolicited song push (server pushed song on connect or song change) ──
    // Skip when SongPage or SetlistPage is open — those pages own the SONG_READY
    // handling themselves and will copy the song into place.
    if (!songPageOpen && !setlistPageOpen &&
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

    // ── Setlist page (overlay on PerformancePage) ────────────────────────────
    if (setlistPageOpen) {
        SetlistResult sres = setlistPage.poll();
        if (sres == SetlistResult::BACK) {
            setlistPageOpen = false;
            display.clear();
            performancePage.draw();
            display.paint();
        } else if (sres == SetlistResult::SONG_LOADED) {
            setlistPageOpen = false;
            engine.stop();
            ui.setSelected(0, 0);
            ui.setNoSong(false);
            if (gServerPairing.isPaired()) {
                gSongSource = SongSource::SERVER;
                // Identify the loaded song by the setlist's FILENAME — it's the
                // exact server file we loaded, so it always matches the Songs
                // browser list and the autosave target.  (Relying on the song's
                // internal name field fails when it's blank or out of sync.)
                char bare[STORAGE_FILENAME_MAX];
                strncpy(bare, setlistPage.loadedFilename(), sizeof(bare) - 1);
                bare[sizeof(bare) - 1] = '\0';
                int bl = (int)strlen(bare);
                if (bl > 4 && bare[bl - 4] == '.' &&
                    (bare[bl - 3] == 'm' || bare[bl - 3] == 'M')) {
                    bare[bl - 4] = '\0';
                }
                songPage.setServerLoadedName(bare);
                // Make the tracker status bar (which shows song.name) reflect
                // the setlist filename.  App convention keeps name == filename
                // on save, so this is a no-op for well-formed songs and a fix
                // for any whose internal name is blank or out of sync.
                strncpy(song.name, bare, sizeof(song.name) - 1);
                song.name[sizeof(song.name) - 1] = '\0';
            } else {
                gSongSource = SongSource::SD;
                songPage.clearLoadedFile();
            }
            // Stay on PerformancePage with the freshly-loaded song.  Server-
            // side song load stops the sequencer; we leave it stopped with no
            // pad lit — the performer chooses which pad starts playback.
            performancePage.open(engine.currentPattern(), /*stopped=*/true);
            performancePage.setTitleOverride(setlistPage.loadedDisplayName());
            display.clear();
            performancePage.draw();
            display.paint();
        } else if (sres == SetlistResult::TITLE_ONLY) {
            // No song file attached → drop to OFF / No Song.  Command the server
            // to OFF (it stops the sequencer + clears its active song) and clear
            // locally so the PerformancePage shows no blocks — all pads grey.
            // The server's MsgNoSong reply re-runs noSongPending() but no-ops
            // since we're already OFF.
            setlistPageOpen = false;
            gServerPairing.sendSetOff();
            engine.stop();
            initSong(&song);
            song.numPatterns = 0;            // no blocks → all perform pads grey
            ui.setSelected(0, 0);
            ui.setNoSong(true);
            gSongSource = SongSource::NONE;
            performancePage.open(engine.currentPattern(), /*stopped=*/true);
            performancePage.setTitleOverride(setlistPage.loadedDisplayName());
            display.clear();
            performancePage.draw();
            display.paint();
        }
        return;
    }

    // ── PixelPost manual control page ─────────────────────────────────────────
    if (pixelpostManualPageOpen) {
        if (pixelpostManualPage.poll()) {
            pixelpostManualPageOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Drawbar organ page ────────────────────────────────────────────────────
    if (organPageOpen) {
        if (organPage.poll() == OrganPage::Result::HOME) {
            organPageOpen = false;   // page already sent ORGAN_OP_EXIT to the server
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── Performance page ──────────────────────────────────────────────────────
    if (performancePageOpen) {
        // Forward server position updates to the performance page.  Only
        // light a pad when the server is actually playing — sequencerReset()
        // on the server fires a (pat=0,row=0) notify after song load, and
        // we don't want that to light pad 1 on a freshly-loaded setlist.
        {
            uint8_t rPat, rRow;
            if (gServerPairing.remoteSeqPos(&rPat, &rRow)) {
                if (gServerPairing.serverPlaying()) {
                    performancePage.setPlayingPattern(rPat);
                }
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
        PerformancePage::Result pres = performancePage.poll();
        if (pres == PerformancePage::Result::HOME) {
            performancePageOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        } else if (pres == PerformancePage::Result::SETLIST) {
            setlistPageOpen = true;
            setlistPage.open();
            display.clear();
            setlistPage.draw();
            display.paint();
        }
        return;
    }

    // ── Song config page ──────────────────────────────────────────────────────
    if (songConfigPageOpen) {
        if (gServerPairing.isPaired() && songConfigPage.patchPending()) {
            // Send all editable song-level fields in one patch (bpm..transposeChMask)
            const uint8_t* start = (const uint8_t*)&song.bpm;
            const uint8_t* end   = (const uint8_t*)&song.transposeChMask + sizeof(song.transposeChMask);
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
                // Instruments are server-owned — push edits up.  When unpaired
                // there's nowhere to persist; edits stay in memory and the
                // server bank reloads on the next connect.
                if (gServerPairing.isPaired()) {
                    for (int i = 0; i < MAX_INSTRUMENTS; i++)
                        gServerPairing.sendInstrumentPatch(gInstruments, i);
                }
            } else {
                // User chose NO — discard edits by reloading the server bank.
                if (gServerPairing.isPaired()) {
                    gServerPairing.requestInstruments();  // async reload from server
                }
            }
            display.clear();
            ui.drawAll();
            display.paintLater();
        }
        return;
    }

    // ── WiFi settings page ────────────────────────────────────────────────────
    if (wifiSettingsPageOpen) {
        if (wifiSettingsPage.poll()) {
            wifiSettingsPageOpen = false;
            if (settingsPageOpen && settingsPage) {
                // Return to the parent SettingsPage.
                display.clear();
                settingsPage->draw();
                display.paintLater();
            } else {
                display.clear();
                ui.drawAll();
                display.paintLater();
            }
        }
        return;
    }

    // ── Settings page ─────────────────────────────────────────────────────────
    if (settingsPageOpen && settingsPage) {
        int rc = settingsPage->poll();
        if (rc == 1) {
            settingsPageOpen = false;
            display.clear();
            ui.drawAll();
            display.paintLater();
        } else if (rc == 2) {
            wifiSettingsPageOpen = true;
            wifiSettingsPage.open();
            display.clear();
            wifiSettingsPage.draw();
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
            uint32_t mask = columnEditor.resyncMask();
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
            if (columnEditor.importRequested()) {
                columnEditor.clearImportRequest();
                drumTrackImportOpen = true;
                drumTrackImportPage.open(columnEditor.editPattern(),
                                          columnEditor.editCol());
            } else {
                display.clear();
                ui.drawAll();
                display.paintLater();
            }
        }
        return;
    }

    // ── Drum-track import page ─────────────────────────────────────────────
    if (drumTrackImportOpen) {
        if (drumTrackImportPage.poll()) {
            const bool drumEditorMode =
                drumTrackImportPage.mode() == DrumTrackImportPage::Mode::DRUM_EDITOR;

            if (drumEditorMode) {
                // Drum-editor mode: hand the picked block to the drum editor.
                // importBlock() sends per-note and per-column patches itself.
                if (drumTrackImportPage.importDidComplete()) {
                    drumEditor.importBlock(drumTrackImportPage.pickedFile(),
                                           drumTrackImportPage.pickedBlock(),
                                           drumTrackImportPage.pickedKit());
                    markSongDirty();
                }
                drumTrackImportPage.clearResync();
                drumEditor.clearResync();
                drumTrackImportOpen = false;
                drumEditor.resumeTouch();
                drumEditor.draw();
                display.clear();
                display.paint();
                return;
            }

            // Column-editor mode (original behaviour).
            if (gServerPairing.isPaired() && drumTrackImportPage.importDidComplete()) {
                uint32_t mask = drumTrackImportPage.resyncMask();
                uint8_t pat = columnEditor.editPattern();
                uint8_t len = song.patterns[pat].length;
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
                    for (uint8_t r = 0; r < len; r++) {
                        gServerPairing.sendNoteSet(song, pat, r, c);
                    }
                }
                markSongDirty();
            }
            drumTrackImportPage.clearResync();
            drumTrackImportOpen = false;
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

    // ── Drum editor page ──────────────────────────────────────────────────────
    if (drumEditorOpen) {
        if (drumEditor.poll()) {
            // IMPORT button tapped — hand off to the drum-track import overlay
            // in drum-editor mode.  Drum editor stays open underneath.
            if (drumEditor.importRequested()) {
                drumEditor.clearImportRequest();
                drumTrackImportOpen = true;
                drumTrackImportPage.openForDrumEditor(drumEditor.patIdx());
                return;
            }
            // BACK tapped — return to Block Settings on the same pattern
            drumEditorOpen = false;
            blockSettings.open(drumEditor.patIdx());
            blockSettings.draw();
            display.clear();
            display.paint();
            lastPage = Page::BLOCKS;
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
            // Drum-Editor button: hand off to that page, don't go HOME.
            if (blockSettings.drumEditorRequested()) {
                blockSettings.clearDrumEditor();
                drumEditor.open(blockSettings.patIdx());
                drumEditor.draw();
                display.clear();
                display.paint();
                drumEditorOpen = true;
                return;
            }
            if (gServerPairing.isPaired() && blockSettings.didStructuralChange()) {
                // Structural changes (new, delete, duplicate, length,
                // inputNote edits) affect noteHead links / note pool topology /
                // playback — send full song so the server has a consistent copy.
                // GATE: browse-and-exit must not push, because the push routes
                // through MSG_SAVE_SONG_HEADER and stops the server's sequencer
                // mid-song.
                gServerPairing.sendSongToServer("", &song);
                blockSettings.clearStructuralChange();
            }
            // A delete may have removed the pattern the engine was sitting on.
            // Clamp before any TrackerPage code touches song.patterns[idx].
            if (engine.currentPattern() >= song.numPatterns) {
                engine.setPattern(song.numPatterns - 1);
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
            songPage.open();
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
                        if (gServerPairing.isPaired()) {
                            gServerPairing.sendNoteSet(song, pat, row, col);
                            // Sound the entered note on the column's channel so
                            // the person stepping it in can hear what they typed.
                            gServerPairing.sendAuditionNote(pat, row, col);
                        }
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

    // Yield so the idle task runs and the core drops into low-power WAITI
    // between passes instead of busy-spinning at 240 MHz.  Nothing time-critical
    // lives at the tail of loop(); UDP/playhead draining is event-driven above.
    vTaskDelay(1);
}

// MagiTrac Server — M5Stack CoreS3 + Module Audio v2.2 + Module MIDI (Port A)
// Requires: M5Unified (display + touch buttons), M5Module-Audio (ES8388),
//           arduinoFFT, magitrac_lib, pixelpost_proto.

// SD must precede M5Unified — M5GFX rebinds File I/O if it appears first.
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <M5Unified.h>
#include <magitrac_lib.h>
#include "midi_player.h"
#include "debug_log.h"
#include "SamplePlayer.h"
#include "SampleManifest.h"
#include "mic_spectrum.h"
#include "audio_codec.h"
#include "sd_mutex.h"
#include "hal/uart_ll.h"
#include "esp_wifi.h"

// ── Communications ───────────────────────────────────────────────────────────
// Same architecture as the Core Basic server: MagiLink AP, MagiUdpLink for
// loss-tolerant updates, ESP-NOW for pairing.  See magitrac_server.ino for
// the full prose.
MagiCommsEspNow gTransportEspNow;
static MagiUdpLink gUdpLink;

bool needsFullRedraw = false;
extern bool srvHasActive;

// ── Display ───────────────────────────────────────────────────────────────────
// M5.Display is the auto-detected CoreS3 LCD (320×240 ILI9342C, internal
// SPI bus, AXP-managed backlight).  Aliased as `lcd` to keep the existing
// rendering code (drawHeader/Row/List/Footer) unchanged.
auto& lcd = M5.Display;

// ── Colours ───────────────────────────────────────────────────────────────────
const uint16_t COL_BG          = lgfx::color565( 0,   0, 139);
const uint16_t COL_HDR         = lgfx::color565( 0,   0,  80);
const uint16_t COL_SEL_BG      = TFT_WHITE;
const uint16_t COL_SEL_BG_PLAY = lgfx::color565(144, 238, 144);
const uint16_t COL_SEL_TEXT    = lgfx::color565( 0,   0, 139);
const uint16_t COL_TEXT        = TFT_WHITE;
const uint16_t COL_FOOTER      = lgfx::color565( 0,   0,  80);

// ── Buttons (M5Unified touch zones on CoreS3) ─────────────────────────────────
// Thin wrapper preserves the Button.wasPressed() / .isDown() / ._pressedMs /
// ._held / ._longFired surface the existing loop() handlers use, while M5
// does the actual touch + edge detection underneath.  M5.update() must run
// each loop tick.
struct Button {
    m5::Button_Class& btn;
    uint32_t _pressedMs = 0;
    bool     _held      = false;
    bool     _longFired = false;

    bool wasPressed() {
        if (btn.wasPressed()) {
            _pressedMs = millis();
            return true;
        }
        return false;
    }
    bool isDown() const { return btn.isPressed(); }
};

static Button BtnA{M5.BtnA};
static Button BtnB{M5.BtnB};
static Button BtnC{M5.BtnC};

// ── State ─────────────────────────────────────────────────────────────────────
int      cursor          = 0;
int      scrollOffset    = 0;
uint16_t gLastBPM        = 0;

// ── Sample browser ────────────────────────────────────────────────────────────
#define SRV_SAMPLES_DIR   "/samples"

static int  numSamples        = 0;
static int  sampleCursor      = 0;
static int  sampleScrollOff   = 0;
static bool sampleBrowserOpen = false;

static const uint32_t LONG_PRESS_MS = 500;

// ── Song list (loaded from SD) ────────────────────────────────────────────────
static char songNames[SRV_MAX_FILES][SRV_FNAME_MAX];
static int  numSongs = 0;

void loadSongList() {
    char raw[SRV_MAX_FILES][SRV_FNAME_MAX];
    numSongs = srvListSongs(raw, SRV_MAX_FILES);
    for (int i = 0; i < numSongs; i++) {
        strncpy(songNames[i], raw[i], SRV_FNAME_MAX - 1);
        songNames[i][SRV_FNAME_MAX - 1] = '\0';
        int len = (int)strlen(songNames[i]);
        if (len > 4 && songNames[i][len - 4] == '.')
            songNames[i][len - 4] = '\0';
    }
    if (cursor >= numSongs) cursor = numSongs > 0 ? numSongs - 1 : 0;
    scrollOffset = 0;
    Serial.printf("[UI] loaded %d songs from SD\n", numSongs);
}

// ── Layout (unchanged from Core Basic — UI refit is a follow-up) ──────────────
const int UI_HEADER_H     = 40;
const int UI_FOOTER_H     = 40;
const int UI_ROW_H        = 30;
const int UI_LIST_Y       = UI_HEADER_H;
const int UI_SCR_H        = 320;
const int UI_LIST_H       = UI_SCR_H - UI_HEADER_H - UI_FOOTER_H;
const int UI_VISIBLE_ROWS = UI_LIST_H / UI_ROW_H;
const int UI_TEXT_SIZE    = 3;
const int UI_CHAR_W       = 6 * UI_TEXT_SIZE;
const int UI_CHAR_H       = 8 * UI_TEXT_SIZE;
const int UI_MAX_CHARS    = 240 / UI_CHAR_W;

// ── Sample browser functions ──────────────────────────────────────────────────
static bool nextWavFile(File& dir, char* buf, int bufLen) {
    while (true) {
        File f = dir.openNextFile();
        if (!f) return false;
        if (!f.isDirectory()) {
            const char* n = f.name();
            if (n[0] != '.') {
                int len = (int)strlen(n);
                if (len > 4 && strcasecmp(n + len - 4, ".wav") == 0) {
                    if (buf) {
                        strncpy(buf, n, bufLen - 1);
                        buf[bufLen - 1] = '\0';
                    }
                    f.close();
                    return true;
                }
            }
        }
        f.close();
    }
}

static void loadSampleList() {
    SdLock _;
    numSamples = 0;
    File dir = SD.open(SRV_SAMPLES_DIR);
    if (!dir || !dir.isDirectory()) { dir.close(); return; }
    while (nextWavFile(dir, nullptr, 0)) numSamples++;
    dir.close();
    if (sampleCursor >= numSamples) sampleCursor = numSamples > 0 ? numSamples - 1 : 0;
}

static void drawSampleHeader() {
    lcd.fillRect(0, 0, 240, UI_HEADER_H, COL_HDR);
    lcd.setTextColor(TFT_WHITE, COL_HDR);
    lcd.setTextSize(2);
    lcd.setCursor(8, (UI_HEADER_H - 16) / 2);
    lcd.print("Samples");
    char buf[10];
    snprintf(buf, sizeof(buf), "%d/%d",
             numSamples > 0 ? sampleCursor + 1 : 0, numSamples);
    int bw = (int)strlen(buf) * 12;
    lcd.setCursor(240 - bw - 6, (UI_HEADER_H - 16) / 2);
    lcd.print(buf);
}

static void drawSampleFooter() {
    int y = UI_SCR_H - UI_FOOTER_H;
    lcd.fillRect(0, y, 240, UI_FOOTER_H, COL_FOOTER);
    lcd.setTextColor(TFT_WHITE, COL_FOOTER);
    lcd.setTextSize(2);
    lcd.setCursor(4, y + (UI_FOOTER_H - 16) / 2);
    if (samplePlayerIsPlaying())
        lcd.print("B:Up C:Dn  Playing");
    else
        lcd.print("B:Up C:Dn  A:Play");
}

static void drawSampleList() {
    SdLock _;
    File dir = SD.open(SRV_SAMPLES_DIR);
    bool dirOk = dir && dir.isDirectory();

    for (int i = 0; i < sampleScrollOff && dirOk; i++)
        if (!nextWavFile(dir, nullptr, 0)) { dirOk = false; break; }

    for (int i = 0; i < UI_VISIBLE_ROWS; i++) {
        int listIdx = sampleScrollOff + i;
        int y = UI_LIST_Y + i * UI_ROW_H;
        bool sel = (listIdx == sampleCursor);
        uint16_t bg = sel ? (sequencerIsRunning() ? COL_SEL_BG_PLAY : COL_SEL_BG) : COL_BG;
        uint16_t fg = sel ? COL_SEL_TEXT : COL_TEXT;
        lcd.fillRect(0, y, 240, UI_ROW_H, bg);

        char fname[UI_MAX_CHARS + 1];
        if (dirOk && nextWavFile(dir, fname, sizeof(fname))) {
            int flen = (int)strlen(fname);
            if (flen > 4 && fname[flen - 4] == '.') fname[flen - 4] = '\0';
            lcd.setTextColor(fg, bg);
            lcd.setTextSize(UI_TEXT_SIZE);
            lcd.setCursor(4, y + (UI_ROW_H - UI_CHAR_H) / 2);
            lcd.print(fname);
        }
    }
    if (dir) dir.close();
}

static void drawSampleBrowser() {
    lcd.fillScreen(COL_BG);
    drawSampleHeader();
    drawSampleList();
    drawSampleFooter();
}

static void sampleMoveCursor(int delta) {
    if (numSamples == 0) return;
    int prev = sampleCursor;

    sampleCursor += delta;
    if (sampleCursor < 0)           sampleCursor = 0;
    if (sampleCursor >= numSamples) sampleCursor = numSamples - 1;
    if (sampleCursor == prev) return;

    if (sampleCursor >= sampleScrollOff + UI_VISIBLE_ROWS)
        sampleScrollOff = sampleCursor - UI_VISIBLE_ROWS + 1;
    if (sampleCursor < sampleScrollOff)
        sampleScrollOff = sampleCursor;

    drawSampleList();
    drawSampleHeader();
    drawSampleFooter();
}

// ── Drawing ───────────────────────────────────────────────────────────────────
void drawHeader() {
    lcd.fillRect(0, 0, 240, UI_HEADER_H, COL_HDR);
    lcd.setTextColor(TFT_WHITE, COL_HDR);
    lcd.setTextSize(2);
    lcd.setCursor(8, (UI_HEADER_H - 16) / 2);
    lcd.print("MagiTrac");

    if (pairingIsConnected()) {
        const int bx = 110, by = (UI_HEADER_H - 18) / 2;
        lcd.fillRoundRect(bx, by, 42, 18, 4, lgfx::color565(0, 180, 0));
        lcd.setTextColor(TFT_WHITE, lgfx::color565(0, 180, 0));
        lcd.setCursor(bx + 3, by + 1);
        lcd.print("CLT");
        lcd.setTextColor(TFT_WHITE, COL_HDR);
    }

    char bpmBuf[10];
    snprintf(bpmBuf, sizeof(bpmBuf), "%u BPM", (unsigned)sequencerCurrentBPM());
    int bw = (int)strlen(bpmBuf) * 12;
    lcd.setCursor(240 - bw - 6, (UI_HEADER_H - 16) / 2);
    lcd.print(bpmBuf);
}

#define SONG_IDX_OFFSET 1

void drawRow(int visIdx) {
    int listIdx = scrollOffset + visIdx;
    int y       = UI_LIST_Y + visIdx * UI_ROW_H;
    bool sel    = (listIdx == cursor);

    uint16_t bg = sel ? (sequencerIsRunning() ? COL_SEL_BG_PLAY : COL_SEL_BG) : COL_BG;
    uint16_t fg = sel ? COL_SEL_TEXT : COL_TEXT;

    lcd.fillRect(0, y, 240, UI_ROW_H, bg);
    lcd.setTextColor(fg, bg);
    lcd.setTextSize(UI_TEXT_SIZE);
    lcd.setCursor(4, y + (UI_ROW_H - UI_CHAR_H) / 2);

    if (listIdx == 0) {
        lcd.print("-- OFF --");
    } else {
        char buf[14];
        strncpy(buf, songNames[listIdx - SONG_IDX_OFFSET], UI_MAX_CHARS);
        buf[UI_MAX_CHARS] = '\0';
        lcd.print(buf);
    }
}

void drawList() {
    int total = numSongs + SONG_IDX_OFFSET;
    for (int i = 0; i < UI_VISIBLE_ROWS; i++) {
        int listIdx = scrollOffset + i;
        if (listIdx < total)
            drawRow(i);
        else
            lcd.fillRect(0, UI_LIST_Y + i * UI_ROW_H, 240, UI_ROW_H, COL_BG);
    }
}

void drawFooter() {
    int y = UI_SCR_H - UI_FOOTER_H;
    lcd.fillRect(0, y, 240, UI_FOOTER_H, COL_FOOTER);
    lcd.setTextColor(TFT_WHITE, COL_FOOTER);
    lcd.setTextSize(2);
    lcd.setCursor(4, y + (UI_FOOTER_H - 16) / 2);
    lcd.printf("B:Up  C:Dn  %d/%d", cursor + 1, numSongs + SONG_IDX_OFFSET);
}

void drawAll() {
    lcd.fillScreen(COL_BG);
    drawHeader();
    drawList();
    drawFooter();
}

// ── Song loading ──────────────────────────────────────────────────────────────
void loadSong(int idx) {
    sequencerStop();

    if (idx == 0) {
        srvHasActive = false;
        if (pairingIsConnected()) {
            MsgNoSong msg;
            gMagiLink.acquireMutex();
            gMagiLink.send(&msg, sizeof(msg));
            gMagiLink.releaseMutex();
        }
        return;
    }

    int listIdx = idx - SONG_IDX_OFFSET;
    if (srvLoadSongLocal(listIdx)) {
        sequencerReset();
        extern bool sSongPushPending;
        if (pairingIsConnected()) sSongPushPending = true;
    }
}

// ── Navigation ────────────────────────────────────────────────────────────────
void moveCursor(int delta) {
    int prev    = cursor;
    int prevOff = scrollOffset;
    int total   = numSongs + SONG_IDX_OFFSET;

    cursor += delta;
    if (cursor < 0)       cursor = 0;
    if (cursor >= total)  cursor = total - 1;
    if (cursor == prev)      return;

    if (cursor >= scrollOffset + UI_VISIBLE_ROWS)
        scrollOffset = cursor - UI_VISIBLE_ROWS + 1;
    if (cursor < scrollOffset)
        scrollOffset = cursor;

    if (scrollOffset != prevOff) {
        drawList();
    } else {
        drawRow(prev   - prevOff);
        drawRow(cursor - scrollOffset);
    }

    drawFooter();
    loadSong(cursor);
}

static void switchToSamples() {
    loadSampleList();
    sampleCursor      = 0;
    sampleScrollOff   = 0;
    sampleBrowserOpen = true;
    drawSampleBrowser();
}

static void switchToSongs() {
    samplePlayerStop();
    sampleBrowserOpen = false;
    needsFullRedraw   = true;
}

// ── MIDI ──────────────────────────────────────────────────────────────────────
// M5MIDI Module on Grove Port A.  CoreS3 Port A = G1 / G2.  The M5MIDI's
// Grove cable convention has G1 = the optoisolator OUT to host (host RX,
// MIDI IN side) and G2 = the optoisolator IN from host (host TX, MIDI OUT
// side) — confirmed by direct UART1 loopback test.  Use UART1, not UART2
// — Serial2 was dropped from Arduino-ESP32 v3.x defaults on the S3 and
// UART2 misbehaves on remapped pins.  midi_player.cpp now also targets
// UART_NUM_1 for its direct-FIFO writes.
#define MIDI_TX_PIN 2
#define MIDI_RX_PIN 1

HardwareSerial midi(1);

// ── Inter-task notification (MIDI task → main loop) ───────────────────────────
static volatile struct {
    uint8_t pattern;
    uint8_t row;
    bool    dirty;
} sPosNotify = {};

static volatile struct {
    bool playing;
    bool dirty;
} sStateNotify = {};

static volatile struct {
    uint8_t midiNote;
    uint8_t velocity;
    bool    dirty;
} sMidiNoteInNotify = {};

static volatile struct {
    uint8_t row;
    bool    dirty;
} sPreviewRowNotify = {};

// ── MIDI task — Core 1, high priority ─────────────────────────────────────────
static void midiTaskFn(void* /*pv*/) {
    for (;;) {
        sequencerPollMidiIn();
        sequencerTick();
        sequencerRawAuditionTick();
        vTaskDelay(1);
    }
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    M5.begin(cfg);
    delay(200);

    // BtnA/B/C touch-zone strip.  Default _touch_button_height=0 makes the
    // strip sit at raw.y >= 240 — *outside* the CoreS3 touch panel coverage
    // (touch only covers the visible 240px tall area).  Carve 40px out of
    // the bottom of the physical landscape — in our rotated portrait that
    // becomes a vertical strip along one edge.
    M5.setTouchButtonHeight(40);
    debugLogInit();
    Serial.println("[SETUP] start (CoreS3)");

    // ── Display ──────────────────────────────────────────────────────────────
    // Portrait (240×320) to match the existing UI layout.  The CoreS3 panel
    // has offset_rotation=3 baked in with default _rotation=1, so the values
    // that yield portrait are 0 or 2 (not 1 / 3 like a raw panel).  If this
    // comes out flipped 180°, swap to setRotation(2).  Touch zones use raw
    // pre-rotation coords so they stay on the same physical edge regardless.
    lcd.setRotation(0);
    lcd.setBrightness(200);
    lcd.fillScreen(COL_BG);

    // ── Audio codec (Module Audio v2.2 on M-Bus, ES8388, full-duplex 32 kHz) ──
    // Must come before samplePlayerInit / spectrumInit so the codec is ready
    // when those tasks start.  Wire is initialised by M5.begin (internal I²C
    // on G12/G11 shared with AXP, AW9523, touch).
    if (!audioCodecInit()) {
        Serial.println("[SETUP] WARNING: Module Audio not detected — audio disabled");
    }

    // ── MIDI ─────────────────────────────────────────────────────────────────
    midi.begin(31250, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);
    uart_ll_disable_intr_mask(UART_LL_GET_HW(1),
                              UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
    Serial.println("[SETUP] MIDI serial ready (direct FIFO) on G1/G2 (Port A, UART1)");

    // SAM2695 on the M5MIDI module has no NVS, so its part-routing is reset
    // every power cycle.  Reconfigure on each boot to suppress its synthesis
    // on every channel except 10 (drums) — keeps the chip from doubling up
    // melodic instruments with the downstream keyboard.
    sam2695MuteAllExcept10();

    // ── SD on the CoreS3 base ───────────────────────────────────────────────
    // CoreS3 SD is on SPI: SCK=G36, MISO=G35, MOSI=G37, CS=G4 (shared with
    // the LCD on a different CS).  Must call SPI.begin with those explicit
    // pins because the default constructor would pick the wrong bus.
    sdMutexInit();
    SPI.begin(GPIO_NUM_36, GPIO_NUM_35, GPIO_NUM_37, GPIO_NUM_4);

    // ── WiFi ─────────────────────────────────────────────────────────────────
    Serial.println("[SETUP] comms init");
    extern void pairingHandleMessage(const uint8_t* data, int len);
    extern uint8_t wifiChannelLoad();

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);

    char    creds_ssid[33] = {};
    char    creds_psk[64]  = {};
    uint8_t apMode         = 0;
    bool hasCreds = pairNvsLoadCreds("magitrac_srv", creds_ssid, creds_psk, &apMode);

    if (hasCreds && apMode == MAGI_AP_MODE_SERVER) {
        const uint8_t apChannel = magiWifiChannelFromIdx(wifiChannelLoad());
        Serial.printf("[SETUP] branch=SERVER_AP ch=%u ssid='%s'\n",
                      (unsigned)apChannel, creds_ssid);
        bool apOk = WiFi.softAP(creds_ssid, creds_psk, apChannel);
        if (!apOk) {
            Serial.println("[SETUP] softAP FAILED — common causes: PSK < 8 chars, SSID empty, channel out of range");
        }
        WiFi.softAPConfig(IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                                    MAGI_SERVER_IP_2, MAGI_SERVER_IP_3),
                          IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                                    MAGI_SERVER_IP_2, MAGI_SERVER_IP_3),
                          IPAddress(255, 255, 255, 0));
        if (esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
            esp_netif_dhcps_stop(ap_netif);
        }
        Serial.printf("[SETUP] AP IP: %s  SSID broadcast: %s\n",
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPSSID().c_str());
    } else if (hasCreds && apMode == MAGI_AP_MODE_EXTERNAL) {
        Serial.printf("[SETUP] branch=EXTERNAL_AP ssid='%s' — STA join\n", creds_ssid);
        WiFi.config(IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                              MAGI_SERVER_IP_2, MAGI_SERVER_IP_3),
                    IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                              MAGI_SERVER_IP_2, MAGI_SERVER_IP_3),
                    IPAddress(255, 255, 255, 0));
        WiFi.begin(creds_ssid, creds_psk);
        WiFi.mode(WIFI_STA);
    } else {
        Serial.printf("[SETUP] branch=NONE (hasCreds=%d apMode=%u) — pair via BtnC long-press\n",
                      hasCreds ? 1 : 0, (unsigned)apMode);
    }

    static const uint8_t kClientIp[4] = {
        MAGI_CLIENT_IP_0, MAGI_CLIENT_IP_1, MAGI_CLIENT_IP_2, MAGI_CLIENT_IP_3 };
    gUdpLink.beginSender(kClientIp, MAGI_PORT);

    gMagiLink.beginAccept(MAGI_PORT);

    gTransportEspNow.setCoexistMode(true);
    gTransportEspNow.setOnReceive([](const uint8_t* data, int len) {
        pairingHandleMessage(data, len);
    });
    gTransportEspNow.begin();

    extern void sendBackupToClient();
    gMagiLink.registerCallback(MSG_START_BACKUP,
        [](const uint8_t* /*msg*/, size_t /*len*/, void* /*ctx*/) {
            Serial.println("[BK-SRV] MSG_START_BACKUP received");
            sendBackupToClient();
        },
        nullptr);

    extern void handleCommand(MagiMsgType type, const uint8_t* data, int len);
    auto controlCb = [](const uint8_t* msg, size_t len, void* /*ctx*/) {
        handleCommand((MagiMsgType)msg[0], msg, (int)len);
    };
    gMagiLink.registerCallback(MSG_PLAY,            controlCb, nullptr);
    gMagiLink.registerCallback(MSG_STOP,            controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PAUSE,           controlCb, nullptr);
    gMagiLink.registerCallback(MSG_UNPAUSE,         controlCb, nullptr);
    gMagiLink.registerCallback(MSG_SEEK,            controlCb, nullptr);
    gMagiLink.registerCallback(MSG_GOTO,            controlCb, nullptr);
    gMagiLink.registerCallback(MSG_NOTE_SET,        controlCb, nullptr);
    gMagiLink.registerCallback(MSG_NOTE_AUDITION,   controlCb, nullptr);
    gMagiLink.registerCallback(MSG_SONG_LOAD_NAME,  controlCb, nullptr);
    gMagiLink.registerCallback(MSG_SONG_LIST_REQ,    controlCb, nullptr);
    gMagiLink.registerCallback(MSG_SONG_LOAD_REQ,    controlCb, nullptr);
    gMagiLink.registerCallback(MSG_SONG_DELETE,      controlCb, nullptr);
    gMagiLink.registerCallback(MSG_SET_SONG_DATA,    controlCb, nullptr);
    gMagiLink.registerCallback(MSG_INSTRUMENTS_REQ,  controlCb, nullptr);
    gMagiLink.registerCallback(MSG_INSTRUMENTS_PATCH, controlCb, nullptr);
    gMagiLink.registerCallback(MSG_QUEUE_BLOCK,      controlCb, nullptr);
    gMagiLink.registerCallback(MSG_CANCEL_QUEUE,     controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PREVIEW_START,    controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PREVIEW_STOP,     controlCb, nullptr);
    gMagiLink.registerCallback(MSG_SET_WIFI_CHANNEL, controlCb, nullptr);
    gMagiLink.registerCallback(MSG_SAMPLE_LIST_REQ,  controlCb, nullptr);
    gMagiLink.registerCallback(MSG_MIDI_DATA,        controlCb, nullptr);
    gMagiLink.registerCallback(MSG_FILE_LIST_REQ, controlCb, nullptr);
    gMagiLink.registerCallback(MSG_FILE_LOAD_REQ, controlCb, nullptr);
    gMagiLink.registerCallback(MSG_AUDITION_RAW_NOTE, controlCb, nullptr);

    extern void onMagiLinkSaveHeader(const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkSaveBody  (const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkSaveActive(const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkNewSong   (const uint8_t* msg, size_t len, void* ctx);
    gMagiLink.registerCallback(MSG_SAVE_SONG_HEADER, onMagiLinkSaveHeader, nullptr);
    gMagiLink.registerCallback(MSG_SAVE_SONG_BODY,   onMagiLinkSaveBody,   nullptr);
    gMagiLink.registerCallback(MSG_SAVE_ACTIVE,      onMagiLinkSaveActive, nullptr);
    gMagiLink.registerCallback(MSG_NEW_SONG,         onMagiLinkNewSong,    nullptr);

    extern void onMagiLinkRestoreHeader(const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkRestoreBody  (const uint8_t* msg, size_t len, void* ctx);
    gMagiLink.registerCallback(MSG_RESTORE_HEADER, onMagiLinkRestoreHeader, nullptr);
    gMagiLink.registerCallback(MSG_RESTORE_BODY,   onMagiLinkRestoreBody,   nullptr);

    gMagiLink.registerCallback(MSG_DISCONNECT,
        [](const uint8_t* /*msg*/, size_t /*len*/, void* /*ctx*/) {
            Serial.println("[LINK] MSG_DISCONNECT from client");
            pairingOnMagiLinkDisconnected();
        },
        nullptr);

    extern void pairingOnMagiLinkConnected();
    extern void pairingOnMagiLinkDisconnected();
    xTaskCreate(
        [](void* /*pv*/) {
            bool wasConnected = false;
            uint32_t lastGen  = gMagiLink.generation();
            for (;;) {
                bool nowConnected = gMagiLink.isConnected();
                uint32_t gen = gMagiLink.generation();
                // A generation bump means a *new* peer connected — even if the
                // preempt-swap's brief isConnected() false blip was too short
                // for this 200 ms poll to observe.  Force a teardown so the
                // MSG_CONNECT/ACK handshake re-runs on the new socket.
                if (wasConnected && gen != lastGen) {
                    pairingOnMagiLinkDisconnected();
                    wasConnected = false;
                }
                lastGen = gen;
                if (nowConnected && !wasConnected) {
                    MsgConnect req;
                    gMagiLink.acquireMutex();
                    bool sentOk = gMagiLink.send(&req, sizeof(req));
                    Serial.printf("[LINK-SES] sent MSG_CONNECT (%s)\n",
                                  sentOk ? "OK" : "FAIL");
                    const uint8_t* resp = sentOk ? gMagiLink.read() : nullptr;
                    if (resp && resp[0] == MSG_CONNECT_ACK) {
                        Serial.println("[LINK-SES] got MSG_CONNECT_ACK");
                        pairingOnMagiLinkConnected();
                        wasConnected = true;
                    } else {
                        Serial.println("[LINK-SES] handshake failed");
                    }
                    gMagiLink.releaseMutex();
                } else if (!nowConnected && wasConnected) {
                    pairingOnMagiLinkDisconnected();
                    wasConnected = false;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        },
        "link_ses",
        4096,
        nullptr,
        3,
        nullptr);

    {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        Serial.printf("[SETUP] my MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    {
        uint8_t prim = 0;
        wifi_second_chan_t sec;
        esp_wifi_get_channel(&prim, &sec);
        Serial.printf("[SETUP-DBG] WiFi channel after setup = %u\n", (unsigned)prim);
    }
    extern void pixelpostInit();
    pixelpostInit();
    pairingInit();
    Serial.println("[SETUP] commandsInit");
    commandsInit();
    samplePlayerInit();
    sampleManifestSync();
    spectrumInit();
    loadSongList();

    seqSetRowCallback([](uint8_t pattern, uint8_t row) {
        sPosNotify.pattern = pattern;
        sPosNotify.row     = row;
        sPosNotify.dirty   = true;
    });

    seqSetStateCallback([](bool playing) {
        sStateNotify.playing = playing;
        sStateNotify.dirty   = true;
    });

    seqSetMidiNoteInCallback([](uint8_t midiNote, uint8_t velocity) {
        sMidiNoteInNotify.midiNote  = midiNote;
        sMidiNoteInNotify.velocity  = velocity;
        sMidiNoteInNotify.dirty     = true;
    });

    seqSetPreviewRowCallback([](uint8_t row) {
        sPreviewRowNotify.row   = row;
        sPreviewRowNotify.dirty = true;
    });

    xTaskCreatePinnedToCore(
        midiTaskFn,
        "MIDI",
        8192,
        nullptr,
        10,
        nullptr,
        1
    );

    Serial.println("[SETUP] done");
    drawAll();
    loadSong(cursor);
}

void loop() {
    M5.update();   // drives BtnA/B/C touch-zone state machines

    pairingLoop();
    commandsTick();

    if (sPosNotify.dirty && pairingIsConnected()) {
        sPosNotify.dirty = false;
        MsgSeqPos msg;
        msg.type    = MSG_SEQ_POS;
        msg.pattern = sPosNotify.pattern;
        msg.row     = sPosNotify.row;
        gUdpLink.send(&msg, sizeof(msg));
    }

    if (sStateNotify.dirty) {
        sStateNotify.dirty = false;
        if (pairingIsConnected()) {
            MsgPlay msg;
            msg.id = sStateNotify.playing ? MSG_PLAY : MSG_STOP;
            gMagiLink.acquireMutex();
            gMagiLink.send(&msg, sizeof(msg));
            gMagiLink.releaseMutex();
        }
        if (!sampleBrowserOpen && cursor >= scrollOffset &&
            cursor < scrollOffset + UI_VISIBLE_ROWS) {
            drawRow(cursor - scrollOffset);
        }
    }

    if (sMidiNoteInNotify.dirty && pairingIsConnected()) {
        sMidiNoteInNotify.dirty = false;
        MsgMidiNoteIn msg;
        msg.type     = MSG_MIDI_NOTE_IN;
        msg.midiNote = sMidiNoteInNotify.midiNote;
        msg.velocity = sMidiNoteInNotify.velocity;
        gUdpLink.send(&msg, sizeof(msg));
    }

    if (sPreviewRowNotify.dirty && pairingIsConnected()) {
        sPreviewRowNotify.dirty = false;
        MsgPreviewRow msg;
        msg.type = MSG_PREVIEW_ROW;
        msg.row  = sPreviewRowNotify.row;
        gUdpLink.send(&msg, sizeof(msg));
    }

    if (needsFullRedraw) {
        needsFullRedraw = false;
        gLastBPM = 0;
        drawAll();
    }

    {
        static uint32_t lastBpmDrawMs = 0;
        uint16_t bpm = sequencerCurrentBPM();
        uint32_t now = millis();
        if (bpm != gLastBPM && (now - lastBpmDrawMs >= 500)) {
            gLastBPM      = bpm;
            lastBpmDrawMs = now;
            drawHeader();
        }
    }

    if (!pairingIsActive() || pairingIsConnected()) {
        // ── BTN_B: short press = navigate up, long press = toggle test mode ─
        static bool sBtnBTestActive = false;
        if (BtnB.wasPressed()) {
            BtnB._held      = true;
            BtnB._longFired = false;
        }
        if (BtnB._held) {
            if (!BtnB.isDown()) {
                BtnB._held = false;
                if (!BtnB._longFired) {
                    if (sampleBrowserOpen) sampleMoveCursor(-1);
                    else                  moveCursor(-1);
                }
            } else if (!BtnB._longFired && (millis() - BtnB._pressedMs >= LONG_PRESS_MS)) {
                BtnB._longFired = true;
                sBtnBTestActive = !sBtnBTestActive;
                sequencerSetTestMode(sBtnBTestActive ? 500 : 0);
            }
        }

        // ── BTN_A: short press = activate, long press = switch screen ────────
        if (BtnA.wasPressed()) {
            BtnA._held      = true;
            BtnA._longFired = false;
        }
        if (BtnA._held) {
            if (!BtnA.isDown()) {
                BtnA._held = false;
                if (!BtnA._longFired) {
                    if (sampleBrowserOpen && numSamples > 0) {
                        char fname[32] = {};
                        {
                            SdLock _;
                            File dir = SD.open(SRV_SAMPLES_DIR);
                            if (dir && dir.isDirectory()) {
                                for (int i = 0; i <= sampleCursor; i++)
                                    if (!nextWavFile(dir, fname, sizeof(fname))) { fname[0] = '\0'; break; }
                                dir.close();
                            }
                        }
                        if (fname[0]) {
                            if (samplePlayerIsPlaying()) {
                                samplePlayerStop();
                            } else {
                                char path[64];
                                snprintf(path, sizeof(path), "%s/%s", SRV_SAMPLES_DIR, fname);
                                samplePlayerPlay(path);
                            }
                            drawSampleFooter();
                        }
                    }
                    if (!sampleBrowserOpen && srvHasActive) {
                        if (sequencerIsRunning()) sequencerStop();
                        else                      sequencerStart();
                    }
                }
            } else if (!BtnA._longFired && (millis() - BtnA._pressedMs >= LONG_PRESS_MS)) {
                BtnA._longFired = true;
                if (sampleBrowserOpen) switchToSongs();
                else                   switchToSamples();
            }
        }

        // ── BTN_C: short press = navigate down, long press = pair / re-pair ──
        if (BtnC.wasPressed()) {
            BtnC._held      = true;
            BtnC._longFired = false;
        }
        if (BtnC._held) {
            if (!BtnC.isDown()) {
                BtnC._held = false;
                if (!BtnC._longFired) {
                    if (sampleBrowserOpen) sampleMoveCursor(+1);
                    else                  moveCursor(+1);
                }
            } else if (!BtnC._longFired && (millis() - BtnC._pressedMs >= 2000)) {
                BtnC._longFired = true;
                enterPairingMode();
            }
        }

        if (sampleBrowserOpen) {
            static bool sLastPlaying = false;
            bool nowPlaying = samplePlayerIsPlaying();
            if (nowPlaying != sLastPlaying) {
                sLastPlaying = nowPlaying;
                drawSampleFooter();
            }
        }
    } else {
        BtnA._held = false;
        BtnC._held = false;
    }
}

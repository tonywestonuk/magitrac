// MagiTrac Server — M5Stack CoreS3 + Module Audio v2.2 + Module MIDI (Port A)
// Requires: M5Unified (display + touch buttons), M5Module-Audio (ES8388),
//           arduinoFFT, magitrac_lib, pixelpost_proto.

// SD must precede M5Unified — M5GFX rebinds File I/O if it appears first.
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include <M5Unified.h>
#include <magitrac_lib.h>
#include "midi_player.h"
#include "debug_log.h"
#include "SamplePlayer.h"
#include "SampleManifest.h"
#include "mic_spectrum.h"
#include "audio_codec.h"
#include "drawbar_organ.h"
#include "sample_organ.h"
#include "sd_mutex.h"
#include "Battery.h"
#include "crash_log.h"
#include "mic_echo_test.h"

// Diagnostic: when defined, setup() runs a record / play loop on the codec
// forever instead of booting normally.  Confirms the mic is hearing what we
// think it's hearing.  Uncomment for diagnostics; leave commented for live.
// #define MIC_ECHO_TEST
#include "hal/uart_ll.h"
#include "esp_wifi.h"

// ── Communications ───────────────────────────────────────────────────────────
// Same architecture as the Core Basic server: MagiLink AP, MagiUdpLink for
// loss-tolerant updates, ESP-NOW for pairing.  See magitrac_server.ino for
// the full prose.
MagiCommsEspNow gTransportEspNow;
static MagiUdpLink gUdpLink;

// Discovery broadcast — periodic UDP MSG_SERVER_ANNOUNCE so clients
// (iOS app, future C-client EXTERNAL_AP support) can find this server's
// IP without a fixed-address convention.  Bound lazily on first send so
// WiFi has time to come up.
static WiFiUDP   gAnnounceUdp;
static bool      gAnnounceUdpBound = false;
static uint32_t  gAnnounceNextMs   = 0;
static const uint32_t ANNOUNCE_INTERVAL_MS = 2000;

// UDP broadcast destination — populated in setup() based on apMode.
// SERVER_AP   → softAP subnet broadcast (192.168.0.255).  Using the
//               limited 255.255.255.255 address doesn't reliably route
//               out the softAP interface when AP+STA are both active.
// EXTERNAL_AP → 255.255.255.255.  Works because lwIP sends it out the
//               STA interface, where the upstream router treats it as
//               a normal subnet broadcast.
static IPAddress gBroadcastIp((uint32_t)0);

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
// Thin wasPressed()/isDown() wrapper over M5's bezel touch zones.  The main
// loop now aggregates the strip into one logical control (see Bezel below);
// these per-zone wrappers survive for pairing.ino's cancel/flush use.
// M5.update() must run each loop tick.
struct Button {
    m5::Button_Class& btn;
    bool wasPressed() { return btn.wasPressed(); }
    bool isDown() const { return btn.isPressed(); }
};

static Button BtnA{M5.BtnA};
static Button BtnB{M5.BtnB};
static Button BtnC{M5.BtnC};

// ── State ─────────────────────────────────────────────────────────────────────
int      cursor          = 0;   // SONGS: index of the loaded song (highlighted)
int      scrollOffset    = 0;   // SONGS: top visible row
uint16_t gLastBPM        = 0;

// ── Screens ───────────────────────────────────────────────────────────────────
// The bezel strip cycles through these (short tap); a 2 s hold enters pairing
// (drawn separately by pairing.ino as a serverMode overlay).  Within a screen,
// the glass area is a drag-scroll list — see pollListTouch().
enum Screen { SCR_SONGS = 0, SCR_SAMPLES = 1, SCR_CHORD = 2, SCR_SCOPE = 3, SCR_USB = 4, SCR_ORGAN = 5, SCR_COUNT = 6 };
static Screen gScreen = SCR_SONGS;

// Remote drawbar-organ control (MSG_ORGAN, from the client over MagiLink).  The
// MagiLink callback runs on the link worker task, so it only sets these; the
// LCD work (setScreen / drawOrganBar) is applied on the loop task.  Drawbar
// values themselves are written straight through organSetDrawbar (volatile,
// task-safe).  Non-static so commands_server.ino's handler can reach them.
volatile int      gOrganScreenReq = 0;   // 0 none, 1 enter, 2 exit
volatile uint16_t gOrganBarDirty  = 0;   // bit i → repaint drawbar i

// Live softAP creds, captured when the AP is brought up in setup().  The
// field-flash OTA hands these to the posts so they ALWAYS match the running AP
// (the separate magitrac_srv_self NVS can drift out of sync — see field_flash).
char gApSsid[33] = {};
char gApPsk[64]  = {};

// ── Drag-scroll (shared — only one list is visible at a time) ─────────────────
static const int LIST_DRAG_THRESH = 8;   // px before a press counts as a drag
static bool _touchDown    = false;
static bool _dragMoved    = false;
static int  _dragStartY   = 0;
static int  _dragStartOff = 0;

// ── Sample browser ────────────────────────────────────────────────────────────
#define SRV_SAMPLES_DIR   "/samples"
#define SRV_MAX_SAMPLES   64

static char sampleNames[SRV_MAX_SAMPLES][SRV_FNAME_MAX];
static int  numSamples       = 0;
static int  sampleScrollOff  = 0;
static int  samplePlayingIdx = -1;   // row currently playing (green highlight)

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
    numSamples       = 0;
    sampleScrollOff  = 0;
    samplePlayingIdx = -1;
    File dir = SD.open(SRV_SAMPLES_DIR);
    if (!dir || !dir.isDirectory()) { dir.close(); return; }
    char fname[SRV_FNAME_MAX];
    while (numSamples < SRV_MAX_SAMPLES && nextWavFile(dir, fname, sizeof(fname))) {
        strncpy(sampleNames[numSamples], fname, SRV_FNAME_MAX - 1);
        sampleNames[numSamples][SRV_FNAME_MAX - 1] = '\0';
        numSamples++;
    }
    dir.close();
}

static void drawSampleHeader() {
    lcd.fillRect(0, 0, 240, UI_HEADER_H, COL_HDR);
    lcd.setTextColor(TFT_WHITE, COL_HDR);
    lcd.setTextSize(2);
    lcd.setCursor(8, (UI_HEADER_H - 16) / 2);
    lcd.print("Samples");
    char buf[10];
    snprintf(buf, sizeof(buf), "%d", numSamples);
    int bw = (int)strlen(buf) * 12;
    int rightCursor = 240 - BATT_ICON_W - 4 - bw - 6;
    lcd.setCursor(rightCursor, (UI_HEADER_H - 16) / 2);
    lcd.print(buf);
    batteryDrawIcon(240 - BATT_ICON_W - 4, (UI_HEADER_H - BATT_ICON_H) / 2, COL_HDR);
}

static void drawSampleFooter() {
    int y = UI_SCR_H - UI_FOOTER_H;
    lcd.fillRect(0, y, 240, UI_FOOTER_H, COL_FOOTER);
    lcd.setTextColor(TFT_WHITE, COL_FOOTER);
    lcd.setTextSize(2);
    lcd.setCursor(4, y + (UI_FOOTER_H - 16) / 2);
    lcd.print(samplePlayerIsPlaying() ? "Tap row: stop" : "Tap row: play");
}

// Render a single sample row from the RAM-cached name list (no SD access —
// avoids SdLock contention with SamplePlayer during a drag).
static void drawSampleRow(int visIdx) {
    int listIdx = sampleScrollOff + visIdx;
    int y       = UI_LIST_Y + visIdx * UI_ROW_H;
    bool playing = (listIdx == samplePlayingIdx) && samplePlayerIsPlaying();
    uint16_t bg = playing ? COL_SEL_BG_PLAY : COL_BG;
    uint16_t fg = playing ? COL_SEL_TEXT    : COL_TEXT;
    lcd.fillRect(0, y, 240, UI_ROW_H, bg);
    if (listIdx < 0 || listIdx >= numSamples) return;

    char fname[UI_MAX_CHARS + 1];
    strncpy(fname, sampleNames[listIdx], UI_MAX_CHARS);
    fname[UI_MAX_CHARS] = '\0';
    int flen = (int)strlen(fname);
    if (flen > 4 && fname[flen - 4] == '.') fname[flen - 4] = '\0';
    lcd.setTextColor(fg, bg);
    lcd.setTextSize(UI_TEXT_SIZE);
    lcd.setCursor(4, y + (UI_ROW_H - UI_CHAR_H) / 2);
    lcd.print(fname);
}

static void drawSampleList() {
    for (int i = 0; i < UI_VISIBLE_ROWS; i++)
        drawSampleRow(i);
}

static void drawSampleScreen() {
    lcd.fillScreen(COL_BG);
    drawSampleHeader();
    drawSampleList();
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
        lcd.fillCircle(112, UI_HEADER_H / 2, 5, lgfx::color565(0, 220, 0));
    }

    char bpmBuf[10];
    snprintf(bpmBuf, sizeof(bpmBuf), "%u BPM", (unsigned)sequencerCurrentBPM());
    int bw = (int)strlen(bpmBuf) * 12;
    int rightCursor = 240 - BATT_ICON_W - 4 - bw - 6;
    lcd.setCursor(rightCursor, (UI_HEADER_H - 16) / 2);
    lcd.print(bpmBuf);
    batteryDrawIcon(240 - BATT_ICON_W - 4, (UI_HEADER_H - BATT_ICON_H) / 2, COL_HDR);
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
    lcd.printf("Songs  %d/%d", cursor + 1, numSongs + SONG_IDX_OFFSET);
}

void drawSongScreen() {
    lcd.fillScreen(COL_BG);
    drawHeader();
    drawList();
    drawFooter();
}

// ── Song loading ──────────────────────────────────────────────────────────────
void loadSong(int idx) {
    Serial.printf("[STOPSRC] local loadSong idx=%d t=%lu\n", idx, millis());
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

// ── USB file-transfer screen ──────────────────────────────────────────────────
// ENTER sets a one-shot NVS flag and reboots.  On the next boot — before any
// SD/audio/WiFi init — setup() calls runUsbMsc() (in usb_msc.ino) which owns
// the device exclusively and never returns.
extern void runUsbMsc();

#define USB_NVS_NS    "magiboot"
#define USB_NVS_KEY   "usbmsc"

void requestUsbMscReboot() {
    Preferences p;
    p.begin(USB_NVS_NS, false);
    p.putBool(USB_NVS_KEY, true);
    p.end();
    lcd.fillScreen(COL_BG);
    lcd.setTextColor(TFT_WHITE, COL_BG);
    lcd.setTextSize(2);
    lcd.setCursor(10, 130); lcd.print("Rebooting to");
    lcd.setCursor(10, 158); lcd.print("USB mode...");
    delay(400);
    ESP.restart();
}

static const int USB_BTN_X = 20, USB_BTN_Y = 196, USB_BTN_W = 200, USB_BTN_H = 56;

void drawUsbScreen() {
    lcd.fillScreen(COL_BG);

    lcd.fillRect(0, 0, 240, UI_HEADER_H, COL_HDR);
    lcd.setTextColor(TFT_WHITE, COL_HDR);
    lcd.setTextSize(2);
    lcd.setCursor(8, (UI_HEADER_H - 16) / 2);
    lcd.print("USB Transfer");
    batteryDrawIcon(240 - BATT_ICON_W - 4, (UI_HEADER_H - BATT_ICON_H) / 2, COL_HDR);

    lcd.setTextColor(TFT_WHITE, COL_BG);
    lcd.setTextSize(1);
    lcd.setCursor(10, 80);  lcd.print("ENTER reboots and mounts");
    lcd.setCursor(10, 94);  lcd.print("the SD card on a computer.");
    lcd.setCursor(10, 116); lcd.print("PixelPost FW: drop the .bin in");
    lcd.setCursor(10, 130); lcd.print("/firmware/, eject -> tap FLASH.");
    lcd.setCursor(10, 152); lcd.print("Note: FAT32 cards only.");

    uint16_t btnCol = lgfx::color565(0, 130, 0);
    lcd.fillRoundRect(USB_BTN_X, USB_BTN_Y, USB_BTN_W, USB_BTN_H, 8, btnCol);
    lcd.setTextColor(TFT_WHITE, btnCol);
    lcd.setTextSize(3);
    const char* lbl = "ENTER";
    int lw = (int)strlen(lbl) * 18;
    lcd.setCursor(USB_BTN_X + (USB_BTN_W - lw) / 2, USB_BTN_Y + (USB_BTN_H - 24) / 2);
    lcd.print(lbl);

    int fy = UI_SCR_H - UI_FOOTER_H;
    lcd.fillRect(0, fy, 240, UI_FOOTER_H, COL_FOOTER);
    lcd.setTextColor(TFT_WHITE, COL_FOOTER);
    lcd.setTextSize(1);
    lcd.setCursor(8, fy + (UI_FOOTER_H - 8) / 2);
    lcd.print("Bezel: next   hold 2s: pair");
}

// ── PixelPost firmware flash screen ───────────────────────────────────────────
// Called once at the end of setup().  If a fresh pixel_post.ino.bin is sitting
// on the SD card (copied via USB-MSC), this dedicated screen owns the device:
//   FLASH  — (re)broadcast the OTA packet; press again to catch stragglers.
//   SKIP   — rename the firmware to *.done and REBOOT (clean teardown → music).
// The HTTP server + DHCP run ONLY while this screen is up, so they never touch
// the music path.  We serve posts in this foreground loop.  See field_flash.ino.
void fieldFlashScreen() {
    extern uint32_t fieldFlashFirmwareSize();
    extern void     fieldFlashServeBegin();
    extern int      fieldFlashServePoll();
    extern bool     fieldFlashBroadcast();
    extern void     fieldFlashMarkDone();

    uint32_t fsize = fieldFlashFirmwareSize();
    if (fsize == 0) return;

    const int FB_X = 20, FB_W = 200, FB_H = 56;
    const int FLASH_Y = 196, SKIP_Y = 256;
    const bool canFlash = (WiFi.softAPIP() != IPAddress(0, 0, 0, 0));

    int  served = 0;            // full pulls since the last FLASH press
    char status[28] = "";

    auto draw = [&]() {
        lcd.fillScreen(COL_BG);
        lcd.fillRect(0, 0, 240, UI_HEADER_H, COL_HDR);
        lcd.setTextColor(TFT_WHITE, COL_HDR);
        lcd.setTextSize(2);
        lcd.setCursor(8, (UI_HEADER_H - 16) / 2);
        lcd.print("PixelPost FW");

        lcd.setTextColor(TFT_WHITE, COL_BG);
        lcd.setTextSize(2);
        lcd.setCursor(10, 56);
        lcd.printf("Firmware: %lu KB", (unsigned long)(fsize / 1024));
        lcd.setTextSize(3);
        lcd.setCursor(10, 96);
        lcd.printf("Posts: %d", served);
        lcd.setTextSize(1);
        lcd.setCursor(10, 140);
        if (status[0])      lcd.print(status);
        else if (!canFlash) lcd.print("Server not in AP mode");
        else                lcd.print("FLASH = (re)send. Watch posts.");

        uint16_t g = canFlash ? lgfx::color565(0, 130, 0) : lgfx::color565(60, 60, 60);
        lcd.fillRoundRect(FB_X, FLASH_Y, FB_W, FB_H, 8, g);
        lcd.setTextColor(TFT_WHITE, g);
        lcd.setTextSize(3);
        lcd.setCursor(FB_X + 22, FLASH_Y + (FB_H - 24) / 2);
        lcd.print("FLASH");

        uint16_t gr = lgfx::color565(110, 60, 0);
        lcd.fillRoundRect(FB_X, SKIP_Y, FB_W, FB_H, 8, gr);
        lcd.setTextColor(TFT_WHITE, gr);
        lcd.setTextSize(3);
        lcd.setCursor(FB_X + 28, SKIP_Y + (FB_H - 24) / 2);
        lcd.print("DONE");
    };

    fieldFlashServeBegin();     // HTTP + DHCP up for the life of this screen
    draw();

    bool released = false;      // ignore the boot-held / eject touch until lifted
    int  pressX = -1, pressY = -1;
    for (;;) {
        M5.update();
        crashMarkAlive();

        // Serve every post that's pulling — non-blocking, concurrent.
        int done = fieldFlashServePoll();
        if (done) { served += done; draw(); }

        auto& t   = M5.Touch.getDetail();
        bool  down = t.isPressed();
        if (!down) released = true;
        if (down && released && pressX < 0) { pressX = t.x; pressY = t.y; }
        if (!down && pressX >= 0) {
            bool inFlash = (pressX >= FB_X && pressX < FB_X + FB_W &&
                            pressY >= FLASH_Y && pressY < FLASH_Y + FB_H);
            bool inSkip  = (pressX >= FB_X && pressX < FB_X + FB_W &&
                            pressY >= SKIP_Y && pressY < SKIP_Y + FB_H);
            pressX = pressY = -1;
            if (inFlash && canFlash) {
                served = 0;
                strncpy(status, fieldFlashBroadcast() ? "Sent - posts pulling..."
                                                      : "Broadcast failed",
                        sizeof(status));
                status[sizeof(status) - 1] = 0;
                draw();
            } else if (inSkip) {
                // Tidy up by rebooting — kills the HTTP server + DHCP cleanly
                // and brings the server back up making music.
                fieldFlashMarkDone();
                lcd.fillScreen(COL_BG);
                lcd.setTextColor(TFT_WHITE, COL_BG);
                lcd.setTextSize(2);
                lcd.setCursor(10, 130); lcd.print("Done - rebooting");
                delay(500);
                ESP.restart();
            }
        }
        delay(1);   // tiny yield; the blocking writes pace the loop at radio rate
    }
}

// ── Chord recogniser screen ───────────────────────────────────────────────────
// Reads the mic via the shared spectrum task (chord path), folds the FFT into a
// 12-bin chroma vector and shows the best-fit chord.  Tap the glass to freeze
// the readout (HOLD) so you can read a chord off the screen mid-song.
static bool     gChordFrozen  = false;
static uint32_t gChordLastSeq = 0xFFFFFFFF;

static const int CHORD_BAR_BASE = 268;            // chroma-bar baseline y
static const int CHORD_BAR_MAXH = 56;             // tallest bar
static const int CHORD_BAR_W    = 240 / 12;       // one bar per pitch class

static void drawChordHeader() {
    lcd.fillRect(0, 0, 240, UI_HEADER_H, COL_HDR);
    lcd.setTextColor(TFT_WHITE, COL_HDR);
    lcd.setTextSize(2);
    lcd.setCursor(8, (UI_HEADER_H - 16) / 2);
    lcd.print("Chord");
    if (gChordFrozen) {
        lcd.setTextColor(lgfx::color565(255, 200, 0), COL_HDR);
        lcd.setCursor(96, (UI_HEADER_H - 16) / 2);
        lcd.print("HOLD");
    }
    batteryDrawIcon(240 - BATT_ICON_W - 4, (UI_HEADER_H - BATT_ICON_H) / 2, COL_HDR);
}

static void drawChordFooter() {
    int y = UI_SCR_H - UI_FOOTER_H;
    lcd.fillRect(0, y, 240, UI_FOOTER_H, COL_FOOTER);
    lcd.setTextColor(TFT_WHITE, COL_FOOTER);
    lcd.setTextSize(1);
    lcd.setCursor(8, y + (UI_FOOTER_H - 8) / 2);
    lcd.print("Tap: hold   Bezel: next");
}

// Repaint just the dynamic middle (chord name, notes, chroma bars).
static void drawChordDisplay() {
    ChordResult r = chordGetResult();
    uint16_t    mask = chordToneMask(r);

    lcd.fillRect(0, UI_HEADER_H, 240, (UI_SCR_H - UI_FOOTER_H) - UI_HEADER_H, COL_BG);

    if (!r.valid) {
        lcd.setTextColor(lgfx::color565(120, 120, 160), COL_BG);
        lcd.setTextSize(2);
        const char* msg = "listening...";
        int w = (int)strlen(msg) * 12;
        lcd.setCursor((240 - w) / 2, 110);
        lcd.print(msg);
    } else {
        char name[8];
        chordFullName(r, name, sizeof(name));
        int len  = (int)strlen(name);
        int size = 232 / (len * 6);          // fit width with a small margin
        if (size > 10) size = 10;
        if (size < 3)  size = 3;
        int w = len * 6 * size;
        int h = 8 * size;
        lcd.setTextColor(TFT_WHITE, COL_BG);
        lcd.setTextSize(size);
        lcd.setCursor((240 - w) / 2, 100 - h / 2);
        lcd.print(name);

        // Spell the chord tones, root-first.
        char notes[40]; notes[0] = '\0';
        for (int i = 0; i < 12; i++) {
            int pc = (r.root + i) % 12;
            if (mask & (1u << pc)) {
                if (notes[0]) strncat(notes, " ", sizeof(notes) - strlen(notes) - 1);
                strncat(notes, chordRootName((int8_t)pc), sizeof(notes) - strlen(notes) - 1);
            }
        }
        lcd.setTextColor(lgfx::color565(180, 220, 180), COL_BG);
        lcd.setTextSize(2);
        int nw = (int)strlen(notes) * 12;
        lcd.setCursor((240 - nw) / 2, 178);
        lcd.print(notes);
    }

    // Chroma bars — 12 pitch classes, chord tones in green.
    lcd.setTextSize(1);
    for (int pc = 0; pc < 12; pc++) {
        int  x    = pc * CHORD_BAR_W;
        int  bh   = (int)(r.chroma[pc] * CHORD_BAR_MAXH);
        if (bh < 0) bh = 0;
        if (bh > CHORD_BAR_MAXH) bh = CHORD_BAR_MAXH;
        bool tone = (mask & (1u << pc)) != 0;
        uint16_t col = tone ? lgfx::color565(0, 220, 0)
                            : lgfx::color565(70, 70, 130);
        if (bh > 0) lcd.fillRect(x + 2, CHORD_BAR_BASE - bh, CHORD_BAR_W - 4, bh, col);
        const char* nm = chordRootName((int8_t)pc);
        int lw = (int)strlen(nm) * 6;
        lcd.setTextColor(tone ? TFT_WHITE : lgfx::color565(140, 140, 170), COL_BG);
        lcd.setCursor(x + (CHORD_BAR_W - lw) / 2, CHORD_BAR_BASE + 2);
        lcd.print(nm);
    }
}

static void drawChordScreen() {
    lcd.fillScreen(COL_BG);
    drawChordHeader();
    drawChordDisplay();
    drawChordFooter();
}

// Tap toggles the freeze/HOLD latch.
static void pollChordTouch() {
    auto& t    = M5.Touch.getDetail();
    bool  down = t.isPressed();
    if (down && !_touchDown) {
        _touchDown = true;
    } else if (!down && _touchDown) {
        _touchDown   = false;
        gChordFrozen = !gChordFrozen;
        drawChordHeader();                  // refresh HOLD indicator
        if (!gChordFrozen) gChordLastSeq = 0xFFFFFFFF;  // force a redraw
    }
}

// ── Oscilloscope screen ─────────────────────────────────────────────────────────
// Free-running time-domain view of the onboard (right) mic — the same source
// the chord recogniser and pixelpost bands read.  Each captured frame (~32 ms)
// is reduced by the mic task to one DC-blocked sample per column; we draw a
// connected line.  Redraw is per-column erase-then-draw (no full clear) so the
// trace stays flicker-free.  Tap freezes / unfreezes the trace.
static uint32_t gScopeLastSeq  = 0xFFFFFFFF;
static int16_t  gScopePrevTop[SCOPE_COLS];
static int16_t  gScopePrevBot[SCOPE_COLS];
static bool     gScopeHavePrev = false;
static uint32_t gScopePkPk     = 0;       // peak-to-peak of the last trace
static bool     gScopeFrozen   = false;   // tap to hold the trace

static const int      SCOPE_Y0   = UI_HEADER_H;                 // 40
static const int      SCOPE_Y1   = UI_SCR_H - UI_FOOTER_H;      // 280
static const int      SCOPE_CY   = (SCOPE_Y0 + SCOPE_Y1) / 2;   // 160 (zero line)
static const int      SCOPE_HALF = (SCOPE_Y1 - SCOPE_Y0) / 2 - 4;
static const uint16_t COL_SCOPE_LINE = lgfx::color565(0, 230, 0);
static const uint16_t COL_SCOPE_AXIS = lgfx::color565(70, 70, 130);

static inline int scopeY(int16_t v) {
    int y = SCOPE_CY - (int)((long)v * SCOPE_HALF / 32768);
    if (y < SCOPE_Y0)     y = SCOPE_Y0;
    if (y > SCOPE_Y1 - 1) y = SCOPE_Y1 - 1;
    return y;
}

static void drawScopeHeader() {
    lcd.fillRect(0, 0, 240, UI_HEADER_H, COL_HDR);
    lcd.setTextColor(TFT_WHITE, COL_HDR);
    lcd.setTextSize(2);
    lcd.setCursor(8, (UI_HEADER_H - 16) / 2);
    lcd.print("Scope");
    if (gScopeFrozen) {
        lcd.setTextColor(lgfx::color565(255, 200, 0), COL_HDR);
        lcd.setCursor(80, (UI_HEADER_H - 16) / 2);
        lcd.print("FROZEN");
    }
    batteryDrawIcon(240 - BATT_ICON_W - 4, (UI_HEADER_H - BATT_ICON_H) / 2, COL_HDR);
}

static void drawScopeFooter() {
    int y = UI_SCR_H - UI_FOOTER_H;
    lcd.fillRect(0, y, 240, UI_FOOTER_H, COL_FOOTER);
    lcd.setTextColor(TFT_WHITE, COL_FOOTER);
    lcd.setTextSize(1);
    lcd.setCursor(8, y + (UI_FOOTER_H - 8) / 2);
    lcd.printf("pp:%-5u  tap:%s", (unsigned)gScopePkPk,
               gScopeFrozen ? "run" : "freeze");
}

static void drawScopeDisplay() {
    static int16_t tr[SCOPE_COLS];
    scopeGetTrace(tr, SCOPE_COLS);
    // Peak-to-peak of the trace (DC-removed) as a signal-level gauge.
    int16_t lo = 32767, hi = -32768;
    for (int c = 0; c < SCOPE_COLS; c++) {
        if (tr[c] < lo) lo = tr[c];
        if (tr[c] > hi) hi = tr[c];
    }
    gScopePkPk = (uint32_t)(hi - lo);
    // Draw a continuous line: each column fills the vertical span between this
    // sample and the previous one, so adjacent points connect into a trace.
    for (int c = 0; c < SCOPE_COLS; c++) {
        int y     = scopeY(tr[c]);
        int yPrev = (c == 0) ? y : scopeY(tr[c - 1]);
        int yTop  = (y < yPrev) ? y : yPrev;
        int yBot  = (y < yPrev) ? yPrev : y;
        if (gScopeHavePrev) {
            int pT = gScopePrevTop[c], pB = gScopePrevBot[c];
            lcd.drawFastVLine(c, pT, pB - pT + 1, COL_BG);   // erase last trace
        }
        lcd.drawPixel(c, SCOPE_CY, COL_SCOPE_AXIS);          // keep the zero line
        lcd.drawFastVLine(c, yTop, yBot - yTop + 1, COL_SCOPE_LINE);
        gScopePrevTop[c] = yTop;
        gScopePrevBot[c] = yBot;
    }
    gScopeHavePrev = true;
}

static void drawScopeScreen() {
    lcd.fillScreen(COL_BG);
    drawScopeHeader();
    lcd.drawFastHLine(0, SCOPE_CY, 240, COL_SCOPE_AXIS);
    gScopeHavePrev = false;
    drawScopeDisplay();   // computes gScopePkPk
    drawScopeFooter();    // shows the diagnostic readout
}

static void pollScopeTouch() {
    auto& t    = M5.Touch.getDetail();
    bool  down = t.isPressed();
    if (down && !_touchDown) {
        _touchDown    = true;
        gScopeFrozen  = !gScopeFrozen;       // tap toggles freeze
        gScopeLastSeq = 0xFFFFFFFF;          // force a redraw on unfreeze
        drawScopeHeader();
        drawScopeFooter();
    } else if (!down && _touchDown) {
        _touchDown = false;
    }
}

// ── Drawbar organ screen ──────────────────────────────────────────────────────
// Nine vertical draggable drawbars (0..8) in Hammond colours; live MIDI plays
// the additive synth in drawbar_organ.cpp.  Bars only repaint on touch.
static const int ORG_BAR_N  = ORGAN_DRAWBARS;                  // 9
static const int ORG_BAR_W  = 26;
static const int ORG_X0     = (240 - ORG_BAR_W * ORG_BAR_N) / 2;
static const int ORG_TOP    = UI_HEADER_H + 22;
static const int ORG_BOT    = UI_SCR_H - UI_FOOTER_H - 18;
static const int ORG_KNOB_H = 20;
static int       _organCol  = -1;

static uint16_t organBarColor(int i) {
    switch (i) {
        case 0: case 1:         return lgfx::color565(150, 45, 35);   // 16',5 1/3' brown
        case 4: case 6: case 7: return lgfx::color565(40, 40, 48);    // mutations  black
        default:                return lgfx::color565(235, 235, 235); // foundation white
    }
}

static void drawOrganBar(int i) {
    int x  = ORG_X0 + i * ORG_BAR_W;
    int cx = x + ORG_BAR_W / 2;
    int v  = organGetDrawbar(i);
    uint16_t col = organBarColor(i);

    lcd.fillRect(x, UI_HEADER_H, ORG_BAR_W, (UI_SCR_H - UI_FOOTER_H) - UI_HEADER_H, COL_BG);

    lcd.setTextSize(2);
    lcd.setTextColor(col, COL_BG);
    char buf[4]; snprintf(buf, sizeof(buf), "%d", v);
    lcd.setCursor(cx - 6, UI_HEADER_H + 2);
    lcd.print(buf);

    lcd.drawFastVLine(cx, ORG_TOP, ORG_BOT - ORG_TOP, lgfx::color565(0, 0, 90));
    int tabY = ORG_TOP + v * (ORG_BOT - ORG_TOP) / 8;
    if (tabY > ORG_TOP) lcd.fillRect(cx - 1, ORG_TOP, 3, tabY - ORG_TOP, col);

    int ky = tabY - ORG_KNOB_H / 2;
    if (ky < ORG_TOP) ky = ORG_TOP;
    if (ky + ORG_KNOB_H > ORG_BOT) ky = ORG_BOT - ORG_KNOB_H;
    lcd.fillRoundRect(x + 3, ky, ORG_BAR_W - 6, ORG_KNOB_H, 3, col);
    lcd.drawRoundRect(x + 3, ky, ORG_BAR_W - 6, ORG_KNOB_H, 3, lgfx::color565(0, 0, 40));

    lcd.setTextSize(1);
    lcd.setTextColor(TFT_WHITE, COL_BG);
    const char* lab = ORGAN_FOOTAGE[i];
    int lw = (int)strlen(lab) * 6;
    lcd.setCursor(cx - lw / 2, ORG_BOT + 4);
    lcd.print(lab);
}

static void drawOrganHeader() {
    lcd.fillRect(0, 0, 240, UI_HEADER_H, COL_HDR);
    lcd.setTextColor(TFT_WHITE, COL_HDR);
    lcd.setTextSize(2);
    lcd.setCursor(8, (UI_HEADER_H - 16) / 2);
    lcd.print("Organ");
    int nv = organVoiceCount();
    if (nv > 0) {
        lcd.setTextColor(lgfx::color565(144, 238, 144), COL_HDR);
        char b[8]; snprintf(b, sizeof(b), "%d", nv);
        lcd.setCursor(150, (UI_HEADER_H - 16) / 2);
        lcd.print(b);
    }
    batteryDrawIcon(240 - BATT_ICON_W - 4, (UI_HEADER_H - BATT_ICON_H) / 2, COL_HDR);
}

static void drawOrganFooter() {
    int y = UI_SCR_H - UI_FOOTER_H;
    lcd.fillRect(0, y, 240, UI_FOOTER_H, COL_FOOTER);
    lcd.setTextColor(TFT_WHITE, COL_FOOTER);
    lcd.setTextSize(1);
    lcd.setCursor(8, y + (UI_FOOTER_H - 8) / 2);
    lcd.print("Drag drawbars   Bezel: next");
}

static void drawOrganScreen() {
    lcd.fillScreen(COL_BG);
    drawOrganHeader();
    for (int i = 0; i < ORG_BAR_N; i++) drawOrganBar(i);
    drawOrganFooter();
}

// Lock to the column the press started in, then slide vertically to set 0..8.
static void pollOrganTouch() {
    auto& t   = M5.Touch.getDetail();
    bool down = t.isPressed();
    if (down) {
        if (!_touchDown) {
            _touchDown = true;
            int x = t.x, y = t.y;
            _organCol = (y >= UI_HEADER_H && y < UI_SCR_H - UI_FOOTER_H &&
                         x >= ORG_X0 && x < ORG_X0 + ORG_BAR_W * ORG_BAR_N)
                        ? (x - ORG_X0) / ORG_BAR_W : -1;
        }
        if (_organCol >= 0 && _organCol < ORG_BAR_N) {
            int span = ORG_BOT - ORG_TOP;
            int v = ((t.y - ORG_TOP) * 8 + span / 2) / span;
            if (v < 0) v = 0;
            if (v > 8) v = 8;
            if (v != organGetDrawbar(_organCol)) {
                organSetDrawbar(_organCol, v);
                drawOrganBar(_organCol);
            }
        }
    } else if (_touchDown) {
        _touchDown = false;
        _organCol  = -1;
    }
}

// ── Screen dispatch ───────────────────────────────────────────────────────────
void drawScreen() {
    switch (gScreen) {
        case SCR_SAMPLES: drawSampleScreen(); break;
        case SCR_CHORD:   drawChordScreen();  break;
        case SCR_SCOPE:   drawScopeScreen();  break;
        case SCR_USB:     drawUsbScreen();    break;
        case SCR_ORGAN:   drawOrganScreen();  break;
        case SCR_SONGS:
        default:          drawSongScreen();   break;
    }
}

static void setScreen(Screen s) {
    if (s == gScreen) return;
    if (gScreen == SCR_SAMPLES && s != SCR_SAMPLES) samplePlayerStop();
    if (gScreen == SCR_CHORD   && s != SCR_CHORD)   chordSetActive(false);
    if (gScreen == SCR_SCOPE   && s != SCR_SCOPE) scopeSetActive(false);
    if (gScreen == SCR_ORGAN   && s != SCR_ORGAN) organSetActive(false);
    gScreen     = s;
    _touchDown  = false;
    _dragMoved  = false;
    if (s == SCR_SAMPLES) loadSampleList();
    if (s == SCR_CHORD) {
        gChordFrozen  = false;
        gChordLastSeq = 0xFFFFFFFF;
        chordSetActive(true);
    }
    if (s == SCR_SCOPE) {
        gScopeFrozen   = false;
        gScopeLastSeq  = 0xFFFFFFFF;
        gScopeHavePrev = false;
        scopeSetActive(true);
    }
    if (s == SCR_ORGAN) {
        samplePlayerStop();      // free the codec before the organ task writes
        organSetActive(true);
    }
    drawScreen();
}

static void cycleScreen() {
    setScreen((Screen)((gScreen + 1) % SCR_COUNT));
}

// ── Tap selection ─────────────────────────────────────────────────────────────
static void selectSong(int idx) {
    int total = numSongs + SONG_IDX_OFFSET;
    if (idx < 0 || idx >= total) return;

    if (idx == cursor) {
        // Re-tapping the already-loaded song toggles transport.
        if (idx != 0 && srvHasActive) {
            if (sequencerIsRunning()) {
                Serial.printf("[STOPSRC] local screen toggle idx=%d t=%lu\n",
                              idx, millis());
                sequencerStop();
            }
            else                      sequencerStart();
        }
        return;
    }

    int prev = cursor;
    cursor   = idx;
    bool prevVis = (prev   >= scrollOffset && prev   < scrollOffset + UI_VISIBLE_ROWS);
    bool curVis  = (cursor >= scrollOffset && cursor < scrollOffset + UI_VISIBLE_ROWS);
    if (prevVis && curVis) {
        drawRow(prev   - scrollOffset);
        drawRow(cursor - scrollOffset);
    } else {
        drawList();
    }
    drawFooter();
    loadSong(cursor);
}

static void selectSample(int idx) {
    if (idx < 0 || idx >= numSamples) return;

    if (idx == samplePlayingIdx && samplePlayerIsPlaying()) {
        samplePlayerStop();
        int v = idx - sampleScrollOff;
        if (v >= 0 && v < UI_VISIBLE_ROWS) drawSampleRow(v);
        drawSampleFooter();
        return;
    }

    int prev = samplePlayingIdx;
    samplePlayingIdx = idx;
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", SRV_SAMPLES_DIR, sampleNames[idx]);
    samplePlayerPlay(path);

    int pv = prev - sampleScrollOff;
    if (pv >= 0 && pv < UI_VISIBLE_ROWS) drawSampleRow(pv);
    int nv = idx - sampleScrollOff;
    if (nv >= 0 && nv < UI_VISIBLE_ROWS) drawSampleRow(nv);
    drawSampleFooter();
}

// ── Glass drag-scroll + tap (songs / samples) ─────────────────────────────────
static int listTotalRows() {
    return (gScreen == SCR_SAMPLES) ? numSamples
                                    : numSongs + SONG_IDX_OFFSET;
}

static void listSetScrollOff(int off) {
    int maxOff = listTotalRows() - UI_VISIBLE_ROWS;
    if (maxOff < 0) maxOff = 0;
    if (off < 0)      off = 0;
    if (off > maxOff) off = maxOff;
    if (gScreen == SCR_SAMPLES) {
        if (off == sampleScrollOff) return;
        sampleScrollOff = off;
        drawSampleList();
    } else {
        if (off == scrollOffset) return;
        scrollOffset = off;
        drawList();
    }
}

// Call only when no bezel button is pressed.
static void pollListTouch() {
    auto& t   = M5.Touch.getDetail();
    bool  down = t.isPressed();
    int   y    = t.y;

    if (down && !_touchDown) {
        _touchDown    = true;
        _dragMoved    = false;
        _dragStartY   = y;
        _dragStartOff = (gScreen == SCR_SAMPLES) ? sampleScrollOff : scrollOffset;
    } else if (down && _touchDown) {
        int dy = y - _dragStartY;
        if (!_dragMoved && abs(dy) >= LIST_DRAG_THRESH) _dragMoved = true;
        if (_dragMoved) listSetScrollOff(_dragStartOff - dy / UI_ROW_H);
    } else if (!down && _touchDown) {
        _touchDown = false;
        if (!_dragMoved &&
            _dragStartY >= UI_LIST_Y &&
            _dragStartY <  UI_LIST_Y + UI_VISIBLE_ROWS * UI_ROW_H) {
            int visIdx = (_dragStartY - UI_LIST_Y) / UI_ROW_H;
            int absRow = ((gScreen == SCR_SAMPLES) ? sampleScrollOff : scrollOffset) + visIdx;
            if (gScreen == SCR_SAMPLES) selectSample(absRow);
            else                        selectSong(absRow);
        }
    }
}

// Call only when no bezel button is pressed.
static void pollUsbTouch() {
    auto& t   = M5.Touch.getDetail();
    bool  down = t.isPressed();

    if (down && !_touchDown) {
        _touchDown   = true;
        _dragStartY  = t.y;
        _dragStartOff = t.x;          // reuse to remember the press x
    } else if (!down && _touchDown) {
        _touchDown = false;
        bool inBtn = (_dragStartOff >= USB_BTN_X && _dragStartOff < USB_BTN_X + USB_BTN_W &&
                      _dragStartY  >= USB_BTN_Y && _dragStartY  < USB_BTN_Y + USB_BTN_H);
        if (inBtn) {
            requestUsbMscReboot();   // sets the NVS flag and resets into USB mode
        }
    }
}

// ── Bezel strip — short tap cycles screens, 2 s hold enters pairing ───────────
static const uint32_t BEZEL_HOLD_MS = 2000;
struct Bezel {
    bool     _down      = false;
    uint32_t _downMs    = 0;
    bool     _longFired = false;
    bool isDown() const {
        return M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed();
    }
    void reset() { _down = false; _longFired = false; }
    // 0 = nothing, 1 = completed short tap, 2 = hold threshold just crossed.
    int poll() {
        bool d = isDown();
        if (d && !_down) { _down = true; _downMs = millis(); _longFired = false; }
        if (d && _down && !_longFired && (millis() - _downMs >= BEZEL_HOLD_MS)) {
            _longFired = true;
            return 2;
        }
        if (!d && _down) {
            _down = false;
            if (!_longFired) return 1;
        }
        return 0;
    }
};
static Bezel gBezel;

// ── Idle → Tempo screen ──────────────────────────────────────────────────────
// 10 s of no touch (glass or bezel) flips into a big-BPM readout to help the
// performer hold tempo.  Average of the last 4 distinct BPM measurements is
// shown smaller below.  Any touch returns to the previously-visible screen.
// Suppressed while a pairing overlay is up.
static const uint32_t TEMPO_IDLE_MS = 10000;
static uint32_t gLastInputMs       = 0;
static bool     gTempoMode         = false;
static bool     gTempoTouchLatched = false;   // require fresh press to exit
static bool     gTempoExitLatched  = false;   // swallow the exit touch's release
static uint16_t gTempoLastDrawn    = 0;

static uint16_t gBpmHist[4]   = {0, 0, 0, 0};
static int      gBpmHistCount = 0;
static int      gBpmHistHead  = 0;
static uint16_t gBpmLastSeen  = 0;

static void bpmHistPush(uint16_t bpm) {
    if (bpm == 0) return;
    gBpmHist[gBpmHistHead] = bpm;
    gBpmHistHead = (gBpmHistHead + 1) & 3;
    if (gBpmHistCount < 4) gBpmHistCount++;
}

static uint16_t bpmHistAverage() {
    if (gBpmHistCount == 0) return 0;
    uint32_t sum = 0;
    for (int i = 0; i < gBpmHistCount; i++) sum += gBpmHist[i];
    return (uint16_t)((sum + gBpmHistCount / 2) / gBpmHistCount);
}

static void drawTempoBPMArea(uint16_t bpm, uint16_t avg) {
    lcd.fillRect(0, 50, 240, 200, TFT_BLACK);
    char buf[8];
    if (bpm == 0) snprintf(buf, sizeof(buf), "--");
    else          snprintf(buf, sizeof(buf), "%u", (unsigned)bpm);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(10);
    int cw = 6 * 10, chh = 8 * 10;
    int tw = (int)strlen(buf) * cw;
    int tx = (240 - tw) / 2;
    int ty = 50 + (140 - chh) / 2;
    lcd.setCursor(tx, ty);
    lcd.print(buf);

    lcd.setTextSize(3);
    char abuf[24];
    if (avg == 0) snprintf(abuf, sizeof(abuf), "avg --");
    else          snprintf(abuf, sizeof(abuf), "avg %u", (unsigned)avg);
    int aw = (int)strlen(abuf) * 6 * 3;
    lcd.setCursor((240 - aw) / 2, 210);
    lcd.print(abuf);
}

static void drawTempoChrome() {
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(2);
    const char* lbl = "TEMPO";
    lcd.setCursor((240 - (int)strlen(lbl) * 12) / 2, 14);
    lcd.print(lbl);
    const char* hint = "tap to return";
    lcd.setCursor((240 - (int)strlen(hint) * 12) / 2, 290);
    lcd.print(hint);
    batteryDrawIcon(240 - BATT_ICON_W - 4, 14, TFT_BLACK);
}

static void enterTempoMode() {
    gTempoMode         = true;
    gTempoTouchLatched = false;
    drawTempoChrome();
    uint16_t bpm = sequencerCurrentBPM();
    drawTempoBPMArea(bpm, bpmHistAverage());
    gTempoLastDrawn = bpm;
}

static void exitTempoMode() {
    gTempoMode = false;
    _touchDown = false;
    _dragMoved = false;
    gLastBPM   = 0;             // force header BPM redraw on restored screen
    drawScreen();
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

// Read + clear the one-shot USB boot flag in NVS (set by the USB page's ENTER
// button before it resets the device).  Cleared in the same call so the next
// boot is normal regardless of how this one ends.
static bool consumeUsbMscBootFlag() {
    Preferences p;
    if (!p.begin(USB_NVS_NS, false)) return false;
    bool want = p.getBool(USB_NVS_KEY, false);
    if (want) p.putBool(USB_NVS_KEY, false);
    p.end();
    return want;
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    M5.begin(cfg);
    delay(200);

    batteryInit();

    // BtnA/B/C touch-zone strip.  Default _touch_button_height=0 makes the
    // strip sit at raw.y >= 240 — *outside* the CoreS3 touch panel coverage
    // (touch only covers the visible 240px tall area).  Carve 40px out of
    // the bottom of the physical landscape — in our rotated portrait that
    // becomes a vertical strip along one edge.
    M5.setTouchButtonHeight(40);
    debugLogInit();
    crashLogInit();   // read reset reason + breadcrumb before anything else runs
    Serial.println("[SETUP] start (CoreS3)");

    // ── Display ──────────────────────────────────────────────────────────────
    // Portrait (240×320) to match the existing UI layout.  The CoreS3 panel
    // has offset_rotation=3 baked in with default _rotation=1, so the values
    // that yield portrait are 0 or 2 (not 1 / 3 like a raw panel).  If this
    // comes out flipped 180°, swap to setRotation(2).  Touch zones use raw
    // pre-rotation coords so they stay on the same physical edge regardless.
    lcd.setRotation(0);
    lcd.setBrightness(120);   // was 200 — backlight is a continuous heat/draw source
    lcd.fillScreen(COL_BG);

    // Headless crash readout: if the *last* boot ended in a fault, hold a
    // banner long enough to read on-device (no serial monitor at a gig).
    // Clean power-ons skip the hold so normal boots stay fast.
    if (crashLogLastWasFault()) {
        lcd.fillScreen(TFT_RED);
        lcd.setTextColor(TFT_WHITE, TFT_RED);
        lcd.setTextSize(2);
        lcd.setCursor(6, 10);
        lcd.println("LAST BOOT CRASHED");
        lcd.setTextSize(1);
        lcd.setCursor(6, 50);
        // Wrap the summary across the narrow portrait panel.
        const char* s = crashLogLastSummary();
        int y = 50;
        char line[34];
        for (size_t i = 0; s[i]; ) {
            size_t n = 0;
            while (s[i] && n < sizeof(line) - 1) line[n++] = s[i++];
            line[n] = '\0';
            lcd.setCursor(6, y);
            lcd.println(line);
            y += 12;
        }
        // If the panic left a core dump, paint the decoded backtrace below the
        // summary — readable headless at a gig; serial has the full per-frame list.
        if (crashLogHasCoreDump()) {
            y += 8;
            lcd.setCursor(6, y); lcd.println("coredump:"); y += 12;
            const char* b = crashLogCoreSummary();
            for (size_t i = 0; b[i]; ) {
                size_t n = 0;
                while (b[i] && n < sizeof(line) - 1) line[n++] = b[i++];
                line[n] = '\0';
                lcd.setCursor(6, y);
                lcd.println(line);
                y += 12;
            }
        }
        delay(crashLogHasCoreDump() ? 9000 : 5000);
        lcd.fillScreen(COL_BG);
    }

    // ── Boot-time USB mass-storage ───────────────────────────────────────────
    // Set by the USB page's ENTER button (one-shot NVS flag).  Branch before
    // anything claims the SD card.  runUsbMsc() never returns.
    if (consumeUsbMscBootFlag()) {
        runUsbMsc();
    }

    // ── Audio codec (Module Audio v2.2 on M-Bus, ES8388, full-duplex 32 kHz) ──
    // Must come before samplePlayerInit / spectrumInit so the codec is ready
    // when those tasks start.  Wire is initialised by M5.begin (internal I²C
    // on G12/G11 shared with AXP, AW9523, touch).
    crashSetBootStep(BS_AUDIO);
    if (!audioCodecInit()) {
        Serial.println("[SETUP] WARNING: Module Audio not detected — audio disabled");
    }

#ifdef MIC_ECHO_TEST
    // Diagnostic — never returns.
    micEchoLoop();
#endif

    // ── MIDI ─────────────────────────────────────────────────────────────────
    crashSetBootStep(BS_MIDI);
    sequencerMidiBegin(MIDI_RX_PIN, MIDI_TX_PIN);
    Serial.println("[SETUP] MIDI serial ready (direct FIFO) on G1/G2 (Port A, UART1)");

    // SAM2695 on the M5MIDI module has no NVS, so its part-routing is reset
    // every power cycle.  Reconfigure on each boot to suppress its synthesis
    // on every channel except 10 (drums) — keeps the chip from doubling up
    // melodic instruments with the downstream keyboard.
    crashSetBootStep(BS_SAM2695);
    sam2695MuteAllExcept10();

    // ── SD on the CoreS3 base ───────────────────────────────────────────────
    // CoreS3 SD is on SPI: SCK=G36, MISO=G35, MOSI=G37, CS=G4 (shared with
    // the LCD on a different CS).  Must call SPI.begin with those explicit
    // pins because the default constructor would pick the wrong bus.
    crashSetBootStep(BS_SPI);
    sdMutexInit();
    SPI.begin(GPIO_NUM_36, GPIO_NUM_35, GPIO_NUM_37, GPIO_NUM_4);

    // ── WiFi ─────────────────────────────────────────────────────────────────
    crashSetBootStep(BS_WIFI);
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
        // max_connection 8 (default 4): the client takes one slot and several
        // PixelPost units pile on at once during a field OTA pull.
        bool apOk = WiFi.softAP(creds_ssid, creds_psk, apChannel,
                                /*hidden=*/0, /*max_connection=*/8);
        if (!apOk) {
            Serial.println("[SETUP] softAP FAILED — common causes: PSK < 8 chars, SSID empty, channel out of range");
        }
        // Remember the LIVE AP creds — the field-flash OTA must hand the posts
        // exactly these, not the magitrac_srv_self NVS (which can drift).
        strncpy(gApSsid, creds_ssid, sizeof(gApSsid)); gApSsid[sizeof(gApSsid) - 1] = 0;
        strncpy(gApPsk,  creds_psk,  sizeof(gApPsk));  gApPsk [sizeof(gApPsk)  - 1] = 0;
        WiFi.softAPConfig(IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                                    MAGI_SERVER_IP_2, MAGI_SERVER_IP_3),
                          IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                                    MAGI_SERVER_IP_2, MAGI_SERVER_IP_3),
                          IPAddress(255, 255, 255, 0));
        if (esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
            esp_netif_dhcps_stop(ap_netif);
        }

        // RF-interference mitigation for the audio path (chord/scope/monitor):
        // each WiFi TX burst couples ~10 Hz hum into the ES8388 analog input.
        //  1) Drop 802.11b — its lowest basic rate (1 Mbps DSSS) makes the
        //     beacon sit on-air ~1 ms; 11g/n forces 6 Mbps OFDM, ~6× shorter
        //     burst → far less coupled energy.  No reconnect-timing change.
        //  2) Beacon interval 100 → 400 ms (10 → 2.5 Hz): fewer bursts/sec.
        //     Modest, so STA keepalive/discovery stays comfortable.
        esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        {
            wifi_config_t apCfg = {};
            if (esp_wifi_get_config(WIFI_IF_AP, &apCfg) == ESP_OK) {
                apCfg.ap.beacon_interval = 400;          // ms (valid 100..60000)
                esp_wifi_set_config(WIFI_IF_AP, &apCfg);
            }
        }

        Serial.printf("[SETUP] AP IP: %s  SSID broadcast: %s  (11g/n, beacon 400ms)\n",
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPSSID().c_str());
    } else if (hasCreds && apMode == MAGI_AP_MODE_EXTERNAL) {
        Serial.printf("[SETUP] branch=EXTERNAL_AP ssid='%s' — STA join (DHCP)\n", creds_ssid);
        // No WiFi.config(): let the external AP's DHCP server assign an IP.
        // Clients find us via the periodic MSG_SERVER_ANNOUNCE broadcast
        // (see sBroadcastNextMs in loop()), so the address doesn't need to
        // be predictable from the client's side any more.
        WiFi.begin(creds_ssid, creds_psk);
        WiFi.mode(WIFI_STA);
        // Match the AP path: drop 11b so our own TX bursts use shorter 11g/n
        // OFDM frames (less RF coupling into the audio path).  The external
        // AP owns the beacon interval here, so we can't change that.
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    } else {
        Serial.printf("[SETUP] branch=NONE (hasCreds=%d apMode=%u) — pair via BtnC long-press\n",
                      hasCreds ? 1 : 0, (unsigned)apMode);
    }

    // Pick the broadcast destination once we know which mode we're in.
    // softAP-hosted networks need the subnet broadcast (192.168.0.255)
    // because the limited 255.255.255.255 doesn't reliably route out the
    // softAP interface in WIFI_AP_STA mode.  On EXTERNAL_AP the device
    // is a STA and 255.255.255.255 works.
    if (hasCreds && apMode == MAGI_AP_MODE_SERVER) {
        gBroadcastIp = IPAddress(MAGI_SERVER_IP_0, MAGI_SERVER_IP_1,
                                 MAGI_SERVER_IP_2, 255);
    } else {
        gBroadcastIp = IPAddress(255, 255, 255, 255);
    }
    Serial.printf("[UDP] broadcast destination: %s\n",
                  gBroadcastIp.toString().c_str());
    crashSetBootStep(BS_NET);
    {
        uint8_t octets[4] = { gBroadcastIp[0], gBroadcastIp[1],
                              gBroadcastIp[2], gBroadcastIp[3] };
        gUdpLink.beginSender(octets, MAGI_PORT);
    }

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
    gMagiLink.registerCallback(MSG_AUDITION_PROGRAM,  controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PIXELPOST_SET_EFFECT,     controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PIXELPOST_SET_SLIDER,     controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PIXELPOST_SET_TOUCHPAD,   controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PIXELPOST_POWER_OFF,      controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PIXELPOST_SET_POST_COUNT,  controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PIXELPOST_FIRMWARE_UPDATE, controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PIXELPOST_SET_FLASH_CTRL,  controlCb, nullptr);
    gMagiLink.registerCallback(MSG_PIXELPOST_OVERRIDE,        controlCb, nullptr);
    gMagiLink.registerCallback(MSG_ORGAN,                     controlCb, nullptr);

    extern void onMagiLinkSaveHeader(const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkSaveBody  (const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkSaveActive(const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkNewSong   (const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkSetNoSong (const uint8_t* msg, size_t len, void* ctx);
    gMagiLink.registerCallback(MSG_SAVE_SONG_HEADER, onMagiLinkSaveHeader, nullptr);
    gMagiLink.registerCallback(MSG_SAVE_SONG_BODY,   onMagiLinkSaveBody,   nullptr);
    gMagiLink.registerCallback(MSG_SAVE_ACTIVE,      onMagiLinkSaveActive, nullptr);
    gMagiLink.registerCallback(MSG_NEW_SONG,         onMagiLinkNewSong,    nullptr);
    gMagiLink.registerCallback(MSG_SET_NO_SONG,      onMagiLinkSetNoSong,  nullptr);

    extern void onMagiLinkRestoreHeader(const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkRestoreBody  (const uint8_t* msg, size_t len, void* ctx);
    gMagiLink.registerCallback(MSG_RESTORE_HEADER, onMagiLinkRestoreHeader, nullptr);
    gMagiLink.registerCallback(MSG_RESTORE_BODY,   onMagiLinkRestoreBody,   nullptr);

    extern void onMagiLinkFileSaveHeader(const uint8_t* msg, size_t len, void* ctx);
    extern void onMagiLinkFileSaveBody  (const uint8_t* msg, size_t len, void* ctx);
    gMagiLink.registerCallback(MSG_FILE_SAVE_HEADER, onMagiLinkFileSaveHeader, nullptr);
    gMagiLink.registerCallback(MSG_FILE_SAVE_BODY,   onMagiLinkFileSaveBody,   nullptr);

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
    crashSetBootStep(BS_PIXELPOST);
    extern void pixelpostInit();
    pixelpostInit();
    crashSetBootStep(BS_PAIRING);
    pairingInit();
    crashSetBootStep(BS_COMMANDS);
    Serial.println("[SETUP] commandsInit");
    commandsInit();
    crashSetBootStep(BS_SAMPLE);
    samplePlayerInit();
    sampleManifestSync();
    crashSetBootStep(BS_SPECTRUM);
    spectrumInit();
    organInit();
    sampleOrganInit();
    crashSetBootStep(BS_SONGLIST);
    loadSongList();
    loadSampleList();   // so the SAMPLE organ can pick samples without visiting the Samples screen

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

    // Live MIDI → drawbar organ (only while the organ screen owns the codec).
    seqSetMidiRawNoteCallback([](bool on, uint8_t note, uint8_t vel) {
        if (!organActive()) return;
        if (on) organNoteOn(note, vel);
        else    organNoteOff(note);
    });

    seqSetPreviewRowCallback([](uint8_t row) {
        sPreviewRowNotify.row   = row;
        sPreviewRowNotify.dirty = true;
    });

    crashSetBootStep(BS_TASKS);
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
    crashSetBootStep(BS_UI);
    gLastInputMs = millis();
    fieldFlashScreen();   // dedicated FLASH screen if /firmware/ has a fresh bin
    drawScreen();
    loadSong(cursor);
    crashSetBootStep(BS_DONE);
}

void loop() {
    M5.update();   // drives BtnA/B/C touch-zone state machines
    crashMarkAlive();   // heartbeat → approximate uptime-before-crash

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
        if (gScreen == SCR_SONGS && cursor >= scrollOffset &&
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

    // ── Discovery broadcast ───────────────────────────────────────────
    // Fire MSG_SERVER_ANNOUNCE to 255.255.255.255:MAGI_PORT every
    // ANNOUNCE_INTERVAL_MS so clients on the same L2 can discover us
    // without a fixed-IP convention.  Only when WiFi link is up.
    if (millis() >= gAnnounceNextMs) {
        gAnnounceNextMs = millis() + ANNOUNCE_INTERVAL_MS;
        bool linkUp = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA)
                    ? (WiFi.softAPIP() != IPAddress(0,0,0,0))
                    : (WiFi.status() == WL_CONNECTED);
        if (linkUp) {
            if (!gAnnounceUdpBound) {
                gAnnounceUdpBound = gAnnounceUdp.begin(0);  // ephemeral local port
            }
            if (gAnnounceUdpBound) {
                MsgServerAnnounce ann{};
                ann.type    = MSG_SERVER_ANNOUNCE;
                ann.tcpPort = MAGI_PORT;
                // Friendly name = the SSID we're on (AP-side SSID if hosting,
                // STA-side SSID otherwise). Truncated to fit MAGI_ANNOUNCE_NAME_LEN.
                String ssid;
                wifi_mode_t mode = WiFi.getMode();
                if (mode == WIFI_AP || mode == WIFI_AP_STA) {
                    ssid = WiFi.softAPSSID();
                }
                if (ssid.isEmpty()) ssid = WiFi.SSID();
                strncpy(ann.name, ssid.c_str(), MAGI_ANNOUNCE_NAME_LEN - 1);
                ann.name[MAGI_ANNOUNCE_NAME_LEN - 1] = '\0';
                gAnnounceUdp.beginPacket(gBroadcastIp, MAGI_PORT);
                gAnnounceUdp.write((const uint8_t*)&ann, sizeof(ann));
                gAnnounceUdp.endPacket();
            }
        }
    }

    // Idle-watcher input tracking + BPM-history sampling.  Touching anything
    // on the panel (glass or bezel zones) — or being inside a pairing flow
    // — resets the idle countdown.  BPM history captures each distinct
    // non-zero reading, so the "avg" reflects real performer snaps.
    bool anyTouch = M5.Touch.getDetail().isPressed();
    if (anyTouch || pairingIsActive()) gLastInputMs = millis();
    {
        uint16_t bpmNow = sequencerCurrentBPM();
        if (bpmNow != gBpmLastSeen) {
            bpmHistPush(bpmNow);
            gBpmLastSeen = bpmNow;
        }
    }

    if (needsFullRedraw) {
        needsFullRedraw = false;
        gLastBPM = 0;
        if (gTempoMode) {
            drawTempoChrome();
            uint16_t bpm = sequencerCurrentBPM();
            drawTempoBPMArea(bpm, bpmHistAverage());
            gTempoLastDrawn = bpm;
        } else {
            drawScreen();
        }
    }

    if (gTempoMode) {
        if (pairingIsActive()) {
            exitTempoMode();
            gLastInputMs = millis();
            return;
        }
        if (anyTouch && !gTempoTouchLatched) {
            gTempoTouchLatched = true;
            exitTempoMode();
            gTempoExitLatched = true;
            gLastInputMs      = millis();
            return;
        }
        if (!anyTouch) gTempoTouchLatched = false;

        static uint32_t lastTempoDrawMs = 0;
        uint32_t now    = millis();
        uint16_t bpmNow = sequencerCurrentBPM();
        if (bpmNow != gTempoLastDrawn && (now - lastTempoDrawMs >= 250)) {
            gTempoLastDrawn = bpmNow;
            lastTempoDrawMs = now;
            drawTempoBPMArea(bpmNow, bpmHistAverage());
        }
        return;
    }

    // Absorb the release of the touch that just exited tempo mode so it
    // doesn't fire a stale tap on the restored screen.
    if (gTempoExitLatched) {
        if (!anyTouch) gTempoExitLatched = false;
        return;
    }

    if (!pairingIsActive() && gScreen != SCR_CHORD && gScreen != SCR_SCOPE &&
        gScreen != SCR_ORGAN &&   // organ is driven remotely → never idle-timeout it
        (millis() - gLastInputMs >= TEMPO_IDLE_MS)) {
        enterTempoMode();
        return;
    }

    if (gScreen == SCR_SONGS) {
        static uint32_t lastBpmDrawMs = 0;
        uint16_t bpm = sequencerCurrentBPM();
        uint32_t now = millis();
        if (bpm != gLastBPM && (now - lastBpmDrawMs >= 500)) {
            gLastBPM      = bpm;
            lastBpmDrawMs = now;
            drawHeader();
        }
    }

    if (batteryPoll()) {
        switch (gScreen) {
            case SCR_SONGS:   drawHeader();         break;
            case SCR_SAMPLES: drawSampleHeader();   break;
            case SCR_CHORD:   drawChordHeader();     break;
            case SCR_SCOPE:   drawScopeHeader();     break;
            case SCR_USB:     /* whole-screen draw is heavy; just patch icon */ {
                batteryDrawIcon(240 - BATT_ICON_W - 4,
                                (UI_HEADER_H - BATT_ICON_H) / 2, COL_HDR);
                break;
            }
            default: break;
        }
        if (gTempoMode) {
            batteryDrawIcon(240 - BATT_ICON_W - 4, 14, TFT_BLACK);
        }
    }

    // ── Input ──────────────────────────────────────────────────────────────────
    // While a pairing screen is up, pairing.ino owns the bezel (cancel on tap);
    // otherwise a short bezel tap cycles screens and a 2 s hold enters pairing.
    // A touch on the glass (no bezel button pressed) drives the active screen.
    if (pairingIsActive()) {
        gBezel.reset();
    } else {
        int bz = gBezel.poll();
        if (bz == 1) {
            cycleScreen();
        } else if (bz == 2) {
            enterPairingMode();
        } else if (!gBezel.isDown()) {
            if      (gScreen == SCR_USB)   pollUsbTouch();
            else if (gScreen == SCR_CHORD) pollChordTouch();
            else if (gScreen == SCR_SCOPE) pollScopeTouch();
            else if (gScreen == SCR_ORGAN) pollOrganTouch();
            else                           pollListTouch();
        }
    }

    // Chord screen: redraw when the recogniser posts a new result (throttled).
    if (gScreen == SCR_CHORD && !gChordFrozen) {
        static uint32_t lastDrawMs = 0;
        uint32_t seq = chordResultSeq();
        uint32_t now = millis();
        if (seq != gChordLastSeq && now - lastDrawMs >= 120) {
            gChordLastSeq = seq;
            lastDrawMs    = now;
            drawChordDisplay();
        }
    }

    // Scope screen: redraw each new captured frame (throttled to ~30 fps),
    // and refresh the pk-pk / dropped-frame readout a few times a second.
    if (gScreen == SCR_SCOPE) {
        static uint32_t lastDrawMs = 0, lastFootMs = 0;
        uint32_t seq = scopeResultSeq();
        uint32_t now = millis();
        if (!gScopeFrozen && seq != gScopeLastSeq && now - lastDrawMs >= 33) {
            gScopeLastSeq = seq;
            lastDrawMs    = now;
            drawScopeDisplay();
        }
        if (now - lastFootMs >= 300) {
            lastFootMs = now;
            drawScopeFooter();
        }
    }

    // SAMPLE organ: a sample was selected → load + analyse it here (SD-safe,
    // ~0.2-0.4 s).  The synth plays silence until the frames are ready.
    {
        extern volatile int gSampleLoadReq;
        if (gSampleLoadReq >= 0) {
            int idx = gSampleLoadReq;
            gSampleLoadReq = -1;
            if (numSamples == 0) loadSampleList();   // lazy scan if not done yet
            Serial.printf("[SAMPORG] select idx=%d of %d samples\n", idx, numSamples);
            if (idx >= 0 && idx < numSamples) {
                char path[80];
                snprintf(path, sizeof(path), "%s/%s", SRV_SAMPLES_DIR, sampleNames[idx]);
                sampleOrganLoad(path);
            }
        }
    }

    // Remote organ control from the client (applied here, on the loop task).
    if (gOrganScreenReq) {
        int req = gOrganScreenReq; gOrganScreenReq = 0;
        if (req == 1 && gScreen != SCR_ORGAN)      setScreen(SCR_ORGAN);
        else if (req == 2 && gScreen == SCR_ORGAN) setScreen(SCR_SONGS);
    }
    if (gScreen == SCR_ORGAN && gOrganBarDirty) {
        uint16_t d = gOrganBarDirty; gOrganBarDirty = 0;
        for (int i = 0; i < ORG_BAR_N; i++) if (d & (1u << i)) drawOrganBar(i);
    }

    // Organ screen: refresh the header voice-count readout when it changes.
    if (gScreen == SCR_ORGAN) {
        static uint32_t lastMs = 0; static int lastV = -1;
        uint32_t now = millis();
        int v = organVoiceCount();
        if (v != lastV && now - lastMs >= 60) { lastV = v; lastMs = now; drawOrganHeader(); }
    }

    // Keep the samples row/footer in sync when playback ends on its own.
    if (gScreen == SCR_SAMPLES) {
        static bool sLastPlaying = false;
        bool nowPlaying = samplePlayerIsPlaying();
        if (nowPlaying != sLastPlaying) {
            sLastPlaying = nowPlaying;
            int v = samplePlayingIdx - sampleScrollOff;
            if (v >= 0 && v < UI_VISIBLE_ROWS) drawSampleRow(v);
            drawSampleFooter();
        }
    }

    // Yield so the idle task can run and the core drops into low-power WAITI
    // between passes.  loop() is purely event-driven (dirty-flag sends) and
    // nothing time-critical lives here — the sequencer/MIDI runs in midiTaskFn.
    // Without this the loopTask busy-spins and pins the core at 240 MHz / 100%.
    vTaskDelay(1);
}

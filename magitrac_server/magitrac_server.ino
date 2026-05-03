// MagiTrac Server — M5Stack Core Basic
// Requires: LovyanGFX, ESP-NOW (built-in)

#define MAGICOMMS_ESPNOW_ARDUINO3X
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <SD.h>
#include "MagiMsg.h"
#include "MagiCommsEspNow.h"
#include "midi_player.h"
#include "debug_log.h"
#include "SamplePlayer.h"
#include "hal/uart_ll.h"

// ── Communications ───────────────────────────────────────────────────────────
static MagiCommsEspNow gTransport;
MagiComms gComms(gTransport);

bool needsFullRedraw = false;   // set by pairing.ino to trigger song list redraw
extern bool srvHasActive;       // defined in commands_server.ino

// ── Display (ILI9341 on M5Stack Core Basic) ───────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9342 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI3_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.pin_sclk    = 18;
            cfg.pin_mosi    = 23;
            cfg.pin_miso    = 19;
            cfg.pin_dc      = 27;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs          = 14;
            cfg.pin_rst         = 33;
            cfg.panel_width     = 320;   // ILI9342C native landscape width
            cfg.panel_height    = 240;   // ILI9342C native landscape height
            cfg.offset_rotation =   0;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = 32;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

LGFX lcd;

// ── Colours (RGB565 via lgfx::color565 — avoids raw hex misinterpretation) ────
const uint16_t COL_BG       = lgfx::color565( 0,   0, 139);  // dark blue
const uint16_t COL_HDR      = lgfx::color565( 0,   0,  80);  // darker blue
const uint16_t COL_SEL_BG       = TFT_WHITE;
const uint16_t COL_SEL_BG_PLAY  = lgfx::color565(144, 238, 144);  // light green when playing
const uint16_t COL_SEL_TEXT     = lgfx::color565( 0,   0, 139);   // dark blue on white/green
const uint16_t COL_TEXT     = TFT_WHITE;
const uint16_t COL_FOOTER   = lgfx::color565( 0,   0,  80);

// ── Buttons (GPIO, active LOW, external pull-ups) ─────────────────────────────
#define BTN_A 39
#define BTN_B 38
#define BTN_C 37

// Interrupt-driven buttons with debounce.
// GPIO 36/39 on ESP32 have a hardware errata that can generate spurious
// edges when WiFi transmits; the 30ms debounce window filters those out.
struct Button {
    uint8_t           pin;
    volatile bool     _isrFlag   = false;   // set by ISR on falling edge
    uint32_t          _lastAccept = 0;      // last accepted press (debounce)
    uint32_t          _pressedMs = 0;       // millis() when press accepted
    bool              _held      = false;   // true while tracking press→release
    bool              _longFired = false;   // true once long-press action fires

    void begin(void (*isr)()) {
        pinMode(pin, INPUT);
        attachInterrupt(digitalPinToInterrupt(pin), isr, FALLING);
    }

    // Call from loop — returns true once per debounced press
    bool wasPressed() {
        if (!_isrFlag) return false;
        _isrFlag = false;
        uint32_t now = millis();
        if (now - _lastAccept < 30) return false;  // bounce filter
        _lastAccept = now;
        _pressedMs  = now;
        return true;
    }

    bool isDown() const { return digitalRead(pin) == LOW; }

    void IRAM_ATTR isr() { _isrFlag = true; }
};

static Button BtnA {BTN_A};
static Button BtnB {BTN_B};
static Button BtnC {BTN_C};

static void IRAM_ATTR isrA() { BtnA.isr(); }
static void IRAM_ATTR isrB() { BtnB.isr(); }
static void IRAM_ATTR isrC() { BtnC.isr(); }

// ── State ─────────────────────────────────────────────────────────────────────
int      cursor          = 0;
int      scrollOffset    = 0;
uint16_t gLastBPM        = 0;   // last BPM drawn in header; 0 = not yet drawn

// ── Sample browser ────────────────────────────────────────────────────────────
#define SRV_SAMPLES_DIR   "/samples"

static int  numSamples        = 0;   // count only — no filename array
static int  sampleCursor      = 0;
static int  sampleScrollOff   = 0;
static bool sampleBrowserOpen = false;

static const uint32_t LONG_PRESS_MS = 500;   // threshold for long-press actions

// ── Song list (loaded from SD) ────────────────────────────────────────────────
static char songNames[SRV_MAX_FILES][SRV_FNAME_MAX];
static int  numSongs = 0;

void loadSongList() {
    char raw[SRV_MAX_FILES][SRV_FNAME_MAX];
    numSongs = srvListSongs(raw, SRV_MAX_FILES);
    for (int i = 0; i < numSongs; i++) {
        strncpy(songNames[i], raw[i], SRV_FNAME_MAX - 1);
        songNames[i][SRV_FNAME_MAX - 1] = '\0';
        // Strip .mgt extension for display
        int len = (int)strlen(songNames[i]);
        if (len > 4 && songNames[i][len - 4] == '.')
            songNames[i][len - 4] = '\0';
    }
    if (cursor >= numSongs) cursor = numSongs > 0 ? numSongs - 1 : 0;
    scrollOffset = 0;
    Serial.printf("[UI] loaded %d songs from SD\n", numSongs);
}

// ── Layout ────────────────────────────────────────────────────────────────────
// textSize(3): 6*3=18px/char wide, 8*3=24px tall → 13 chars across 240px
const int UI_HEADER_H     = 40;
const int UI_FOOTER_H     = 40;
const int UI_ROW_H        = 30;
const int UI_LIST_Y       = UI_HEADER_H;
const int UI_SCR_H        = 320;                               // physical draw height (>lcd.height() but renders correctly in this rotation)
const int UI_LIST_H       = UI_SCR_H - UI_HEADER_H - UI_FOOTER_H;
const int UI_VISIBLE_ROWS = UI_LIST_H / UI_ROW_H;              // 8
const int UI_TEXT_SIZE    = 3;
const int UI_CHAR_W       = 6 * UI_TEXT_SIZE;                  // 18
const int UI_CHAR_H       = 8 * UI_TEXT_SIZE;                  // 24
const int UI_MAX_CHARS    = 240 / UI_CHAR_W;                   // 13

// ── Sample browser functions ──────────────────────────────────────────────────

// Advances dir to the next .wav file (skips dot-files and directories).
// Copies filename to buf if non-null. Returns true on success.
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

// Count .wav files in samples dir and update numSamples
static void loadSampleList() {
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

// Open dir, skip to sampleScrollOff, then stream filenames directly to screen
static void drawSampleList() {
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

    // Client connected badge — green pill after title
    if (pairingIsConnected()) {
        const int bx = 110, by = (UI_HEADER_H - 18) / 2;
        lcd.fillRoundRect(bx, by, 42, 18, 4, lgfx::color565(0, 180, 0));
        lcd.setTextColor(TFT_WHITE, lgfx::color565(0, 180, 0));
        lcd.setCursor(bx + 3, by + 1);
        lcd.print("CLT");
        lcd.setTextColor(TFT_WHITE, COL_HDR);
    }

    // BPM — right-aligned
    char bpmBuf[10];
    snprintf(bpmBuf, sizeof(bpmBuf), "%u BPM", (unsigned)sequencerCurrentBPM());
    int bw = (int)strlen(bpmBuf) * 12;   // textSize(2) = 12px/char
    lcd.setCursor(240 - bw - 6, (UI_HEADER_H - 16) / 2);
    lcd.print(bpmBuf);
}

// Index 0 is always "-- OFF --"; real songs are at indices 1..numSongs
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
// Songs load in STOP mode — user presses BTN_A to toggle play/stop.
void loadSong(int idx) {
    sequencerStop();

    if (idx == 0) {
        // OFF — deactivate song and notify connected client
        srvHasActive = false;
        if (pairingIsConnected()) {
            uint8_t msg = (uint8_t)MSG_NO_SONG;
            gComms.send(&msg, 1);
        }
        return;
    }

    int listIdx = idx - SONG_IDX_OFFSET;  // 0-based index into songNames[]
    if (srvLoadSongLocal(listIdx)) {
        sequencerReset();   // snap to row 0, stay stopped
        // Push new song to client if connected
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

// ── Switch between song list and sample browser ──────────────────────────────
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
#define MIDI_RX_PIN 16
#define MIDI_TX_PIN 17

HardwareSerial midi(2);

// ── Inter-task notification (MIDI task → main loop) ───────────────────────────
// MIDI task writes these; main loop reads them and sends ESP-NOW messages.
// Using simple volatile structs — no mutex needed: worst case is one stale read,
// which is harmless for position/state notifications.
static volatile struct {
    uint8_t pattern;
    uint8_t row;
    bool    dirty;
} sPosNotify = {};

static volatile struct {
    bool playing;
    bool dirty;
} sStateNotify = {};

// MIDI note-on received while sequencer is stopped — forward to client
static volatile struct {
    uint8_t midiNote;
    uint8_t velocity;
    bool    dirty;
} sMidiNoteInNotify = {};

// ── MIDI task — Core 1, high priority ─────────────────────────────────────────
// Handles all MIDI input and sequencer timing independently of the display loop.
static void midiTaskFn(void* /*pv*/) {
    for (;;) {
        sequencerPollMidiIn();
        sequencerTick();
        vTaskDelay(1);  // 1 tick (1 ms) — caps latency, lets main loop run
    }
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    debugLogInit();
    Serial.println("[SETUP] start");

    midi.begin(31250, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);
    // Disable UART2 RX interrupts — we poll the hardware FIFO directly in
    // sequencerPollMidiIn() for lower latency.  Without this, Arduino's ISR
    // would drain the FIFO into its software ring buffer before we see it.
    uart_ll_disable_intr_mask(UART_LL_GET_HW(2),
                              UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
    Serial.println("[SETUP] MIDI serial ready (direct FIFO)");

    BtnA.begin(isrA);
    BtnB.begin(isrB);
    BtnC.begin(isrC);

    Serial.println("[SETUP] lcd init");
    lcd.init();
    lcd.invertDisplay(true);
    lcd.setRotation(3);
    lcd.setBrightness(200);

    Serial.println("[SETUP] comms init");
    // Forward declarations (defined in pairing.ino)
    extern void pairingHandleMessage(const uint8_t* data, int len);
    gComms.setOnReceive([](const uint8_t* data, int len) {
        pairingHandleMessage(data, len);
    });
    gComms.begin();
    pairingInit();   // load stored pairing from NVS
    Serial.println("[SETUP] commandsInit");
    commandsInit();
    samplePlayerInit();  // SD init after WiFi/ESP-NOW is fully up
    loadSongList();  // populate list from SD

    // Row advance — set dirty flag; main loop sends the ESP-NOW message
    // (ESP-NOW send must not be called from the MIDI task)
    seqSetRowCallback([](uint8_t pattern, uint8_t row) {
        sPosNotify.pattern = pattern;
        sPosNotify.row     = row;
        sPosNotify.dirty   = true;
    });

    // Play/stop state change — same deferred-send approach
    seqSetStateCallback([](bool playing) {
        sStateNotify.playing = playing;
        sStateNotify.dirty   = true;
    });

    // MIDI note-on while stopped — forward to client for step-entry
    seqSetMidiNoteInCallback([](uint8_t midiNote, uint8_t velocity) {
        sMidiNoteInNotify.midiNote  = midiNote;
        sMidiNoteInNotify.velocity  = velocity;
        sMidiNoteInNotify.dirty     = true;
    });

    // Launch MIDI task on Core 1 at priority 10 (above loop()'s priority 1)
    xTaskCreatePinnedToCore(
        midiTaskFn,
        "MIDI",
        8192,     // stack: Serial.printf needs headroom
        nullptr,
        10,       // priority — preempts main loop for tight MIDI timing
        nullptr,
        1         // Core 1 (same core as loop, but higher priority)
    );

    Serial.println("[SETUP] done");
    drawAll();
    loadSong(cursor);
}

void loop() {
    // Buttons are interrupt-driven — no update() polling needed

    pairingLoop();
    commandsTick();
    // sequencerPollMidiIn() and sequencerTick() now run in midiTaskFn()

    // Forward row-position notifications queued by the MIDI task
    if (sPosNotify.dirty) {
        sPosNotify.dirty = false;
        MsgSeqPos msg;
        msg.type    = MSG_SEQ_POS;
        msg.pattern = sPosNotify.pattern;
        msg.row     = sPosNotify.row;
        pairingSendToClient(&msg, sizeof(msg));
    }

    // Forward play/stop notifications queued by the MIDI task
    if (sStateNotify.dirty) {
        sStateNotify.dirty = false;
        uint8_t msg = sStateNotify.playing ? (uint8_t)MSG_PLAY : (uint8_t)MSG_STOP;
        pairingSendToClient(&msg, 1);
        // Redraw selected row so selector bar colour reflects play/stop state
        if (!sampleBrowserOpen && cursor >= scrollOffset &&
            cursor < scrollOffset + UI_VISIBLE_ROWS) {
            drawRow(cursor - scrollOffset);
        }
    }

    // Forward MIDI note-on (received while stopped) to client for step-entry
    if (sMidiNoteInNotify.dirty) {
        sMidiNoteInNotify.dirty = false;
        MsgMidiNoteIn msg;
        msg.type     = MSG_MIDI_NOTE_IN;
        msg.midiNote = sMidiNoteInNotify.midiNote;
        msg.velocity = sMidiNoteInNotify.velocity;
        pairingSendToClient(&msg, sizeof(msg));
    }

    if (needsFullRedraw) {
        needsFullRedraw = false;
        gLastBPM = 0;   // force header redraw
        drawAll();
    }

    // Refresh BPM in header — throttled to once per 500 ms to avoid constant SPI writes
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

    // Allow navigation in STANDALONE and CONNECTED; only block during active pairing ceremony
    if (!pairingIsActive() || pairingIsConnected()) {
        uint32_t now = millis();

        // ── BTN_B: short press = navigate up, long press = toggle test mode ─
        static bool sBtnBTestActive = false;
        if (BtnB.wasPressed()) {
            BtnB._held      = true;
            BtnB._longFired = false;
        }
        if (BtnB._held) {
            if (!BtnB._longFired && (now - BtnB._pressedMs >= LONG_PRESS_MS)) {
                BtnB._longFired = true;
                sBtnBTestActive = !sBtnBTestActive;
                sequencerSetTestMode(sBtnBTestActive ? 500 : 0);
            }
            if (!BtnB.isDown()) {
                BtnB._held = false;
                if (!BtnB._longFired) {
                    // Short press released — navigate up
                    if (sampleBrowserOpen) sampleMoveCursor(-1);
                    else                  moveCursor(-1);
                }
            }
        }

        // ── BTN_A: short press = activate, long press = switch screen ────────
        if (BtnA.wasPressed()) {
            BtnA._held      = true;
            BtnA._longFired = false;
        }
        if (BtnA._held) {
            if (!BtnA._longFired && (now - BtnA._pressedMs >= LONG_PRESS_MS)) {
                BtnA._longFired = true;
                if (sampleBrowserOpen) switchToSongs();
                else                   switchToSamples();
            }
            if (!BtnA.isDown()) {
                BtnA._held = false;
                if (!BtnA._longFired) {
                    // Short press released — activate current item
                    if (sampleBrowserOpen && numSamples > 0) {
                        char fname[32] = {};
                        File dir = SD.open(SRV_SAMPLES_DIR);
                        if (dir && dir.isDirectory()) {
                            for (int i = 0; i <= sampleCursor; i++)
                                if (!nextWavFile(dir, fname, sizeof(fname))) { fname[0] = '\0'; break; }
                            dir.close();
                        }
                        if (fname[0]) {
                            char path[64];
                            snprintf(path, sizeof(path), "%s/%s", SRV_SAMPLES_DIR, fname);
                            samplePlayerPlay(path);
                            drawSampleFooter();
                        }
                    }
                    if (!sampleBrowserOpen && srvHasActive) {
                        if (sequencerIsRunning()) sequencerStop();
                        else                      sequencerStart();
                    }
                }
            }
        }

        // ── BTN_C: short press = navigate down, long press = pairing mode ────
        if (BtnC.wasPressed()) {
            BtnC._held      = true;
            BtnC._longFired = false;
        }
        if (BtnC._held) {
            if (!BtnC._longFired && (now - BtnC._pressedMs >= 2000)) {
                BtnC._longFired = true;
                enterPairingMode();
            }
            if (!BtnC.isDown()) {
                BtnC._held = false;
                if (!BtnC._longFired) {
                    // Short press released — navigate down
                    if (sampleBrowserOpen) sampleMoveCursor(+1);
                    else                  moveCursor(+1);
                }
            }
        }

        // Refresh sample footer while playing
        if (sampleBrowserOpen) {
            static bool sLastPlaying = false;
            bool nowPlaying = samplePlayerIsPlaying();
            if (nowPlaying != sLastPlaying) {
                sLastPlaying = nowPlaying;
                drawSampleFooter();
            }
        }
    } else {
        // Reset held state when buttons are handled by pairing code
        BtnA._held = false;
        BtnC._held = false;
    }
}

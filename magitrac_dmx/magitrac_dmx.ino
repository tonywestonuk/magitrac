// magitrac_dmx — PPOST-locked DMX wash node
// ------------------------------------------
// Target: original M5Stick (ESP32-PICO-D4)  https://docs.m5stack.com/en/core/m5stick
// Wiring: M5Stick Grove port -> DMX interface (RS-485 -> XLR) -> 7-ch RGBW wash.
//
// This is a PixelPost receiver that drives a DMX light instead of a LED strip.
// It speaks the exact same wire protocol as `pixel_post.ino` (see
// pixelpost_proto.h): it listens for "PPOST" state broadcasts over ESP-NOW,
// locks its effect clock to the sender's tick, and re-renders every 20 ms — but
// the render writes DMX channel codes and ships a frame, rather than painting
// pixels.  So the wash follows the show in lock-step with the PixelPosts.
//
// Three pieces, matching the brief:
//   1. PAIRING  — hold the M5Stick button (GPIO35).  The node scans WiFi
//                 channels {1,6,11} and the FIRST PPOST it hears adopts that
//                 packet's channelId + locks the radio channel, then saves to
//                 NVS.  (Same model as the posts — channelId scopes which show
//                 we follow.)  While pairing, the wash strobes white.
//   2. CONTROL  — PPOST (and PPOSB beat) packets set the live effect, slider,
//                 tap, power-off and beat data.
//   3. SEQUENCE — every 20 ms tick, dmxRender() turns the current effect + tick
//                 (+ beat) into RGBW codes and sends a DMX frame.

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>
#include <SPI.h>
#include <U8x8lib.h>            // original M5Stick OLED (see binx example sketch)
#include <pixelpost_proto.h>
#include "driver/uart.h"
#include <string.h>
#include "dmx_effects.h"   // EffectCtx, WashColour, washRenderEffect — one eff_*.cpp per effect

// ════════════════════════════════════════════════════════════════════════════
//  DMX-512 transport  (proven bring-up: GPIO25, 250k 8N2, line-invert BREAK)
// ════════════════════════════════════════════════════════════════════════════
static const int      DMX_TX_PIN = 25;        // Grove UART TX (G13 = RX, unused)
static const uint32_t DMX_BAUD   = 250000;    // 4 us/bit
static const int      DMX_UART   = 1;         // UART1 == HardwareSerial(1) == UART_NUM_1
#define DMX_CHANNELS 512

// 7-channel RGBW wash, fixture DMX start address = D001.
//   CH1 master dimmer · CH2 R · CH3 G · CH4 B · CH5 W · CH6 strobe · CH7 mode.
// Hold CH6 (0 = no strobe) and CH7 (0 = manual, CH1-CH6 valid) at 0 so we keep
// full RGBW control instead of the fixture's built-in auto programs.
enum {
  CH_MASTER = 1, CH_RED = 2, CH_GREEN = 3, CH_BLUE = 4,
  CH_WHITE  = 5, CH_STROBE = 6, CH_MODE = 7,
};

static uint8_t      dmxData[DMX_CHANNELS];
HardwareSerial      DmxSerial(DMX_UART);

static inline void dmxSet(int ch, uint8_t v) {
  if (ch >= 1 && ch <= DMX_CHANNELS) dmxData[ch - 1] = v;
}

// One place to set the wash: R/G/B/W + master, manual mode, no fixture strobe.
static void washWrite(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t master) {
  dmxSet(CH_MASTER, master);
  dmxSet(CH_RED,    r);
  dmxSet(CH_GREEN,  g);
  dmxSet(CH_BLUE,   b);
  dmxSet(CH_WHITE,  w);
  dmxSet(CH_STROBE, 0);
  dmxSet(CH_MODE,   0);
}

static void dmxBreak() {
  DmxSerial.flush();
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV);      // TX low  — BREAK
  delayMicroseconds(120);
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE);  // TX high — MAB
  delayMicroseconds(12);
}

static void dmxSendFrame() {
  dmxBreak();
  DmxSerial.write((uint8_t)0x00);            // start code
  DmxSerial.write(dmxData, DMX_CHANNELS);    // 512 slots
  DmxSerial.flush();
}

// ════════════════════════════════════════════════════════════════════════════
//  PixelPost receive / tick-lock / pairing  (mirrors pixel_post.ino)
// ════════════════════════════════════════════════════════════════════════════
#define BUTTON_PIN 35    // original M5Stick BtnA (input-only; board pulls it high)

Preferences preferences;

static const uint16_t PP_CHANNEL_ID_UNPAIRED = 0xFFFF;
static uint16_t pp_channel_id = PP_CHANNEL_ID_UNPAIRED;

// WiFi radio channel — same {1,6,11} non-overlapping set the posts use.
static uint8_t  channels[]       = { 1, 6, 11 };
static uint8_t  wifi_channel_idx = 0;

// Effect-tick clock — locked to the sender's millis() on every valid PPOST and
// free-run from the local crystal between frames, so this node sees the same
// tick value as every PixelPost on the same frame (phase stays in sync).
static portMUX_TYPE tickMutex  = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     lockedTick  = 0;
static uint32_t     lockedAtMs  = 0;

static uint32_t currentTickMs() {
  portENTER_CRITICAL(&tickMutex);
  uint32_t t = lockedTick + (millis() - lockedAtMs);
  portEXIT_CRITICAL(&tickMutex);
  return t;
}

// Live control state, written by the ESP-NOW recv callback (WiFi-task context),
// read by the render loop.  Guarded by a spinlock.
static portMUX_TYPE ctrlMutex   = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  ctrlEffect   = 0;
static volatile uint8_t  ctrlSlider   = 0;
static volatile uint8_t  ctrlX        = 0;       // touchpad X (latched)
static volatile uint8_t  ctrlY        = 0;       // touchpad Y (latched)
static volatile bool     ctrlPressed  = false;   // finger on the pad right now
static volatile bool     ctrlTapped   = false;   // rising-edge of TOUCHED
static volatile bool     ctrlPowerOff = false;
static volatile uint32_t ctrlBeatSeq  = 0;
static volatile uint8_t  ctrlBeatStr  = 0;
static uint8_t           ctrlBands[PP_AUDIO_BANDS] = {0};  // 5 FFT magnitudes (under ctrlMutex)
static bool              last_touched = false;

static portMUX_TYPE      rxMutex             = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t last_valid_packet_ms = 0;
static volatile uint32_t ppPacketCount        = 0;     // valid PPOSTs for our show (OLED RX indicator)
// Hop to the next channel after this long with no valid PPOST at all — so the
// node follows the sender if it genuinely moves WiFi channel.  Any valid PPOST
// resets the timer (see recv callback), so a packet flood — which keeps packets
// flowing — can't trip it.  Matches the PixelPosts' own 6 s hop.
static const uint32_t    CHANNEL_HOP_MS       = 6000;

// ── Pairing state ───────────────────────────────────────────────────────────
static bool     pairing_mode        = false;
static bool     pairing_adopted     = false;
static uint32_t pairing_scan_next_ms = 0;
static uint32_t pairing_adopted_ms   = 0;
static const uint32_t PAIRING_SCAN_INTERVAL_MS = 3000;
static const uint32_t PAIRING_CONFIRM_MS       = 1500;  // green hold after adopt, then save+exit

// ── Button: hold to enter pairing ───────────────────────────────────────────
static bool     btn_down_prev   = false;
static uint32_t btn_pressed_at  = 0;
static const uint32_t PAIRING_ENTRY_HOLD_MS = 1500;

static void set_radio_channel(uint8_t idx) {
  if (idx >= sizeof(channels) / sizeof(channels[0])) idx = 0;
  wifi_channel_idx = idx;
  esp_wifi_set_channel(channels[idx], WIFI_SECOND_CHAN_NONE);
  Serial.printf("[CH] radio -> ch=%u (idx=%u) t=%lu\n",
                (unsigned)channels[idx], (unsigned)idx, (unsigned long)millis());
}

static void enter_pairing_mode() {
  Serial.println("[PAIR] enter");
  pairing_mode         = true;
  pairing_adopted      = false;
  pairing_scan_next_ms = millis() + PAIRING_SCAN_INTERVAL_MS;
}

static void exit_pairing_save() {
  Serial.printf("[PAIR] exit, saved channelId=0x%04X radio_ch=%u\n",
                (unsigned)pp_channel_id, (unsigned)channels[wifi_channel_idx]);
  preferences.putUShort("channel-id", pp_channel_id);
  preferences.putUChar ("radio-chan", wifi_channel_idx);
  pairing_mode    = false;
  pairing_adopted = false;
}

// GPIO35 is input-only with NO internal pull-up, so under WiFi/DMX EMI (a packet
// flood) it phantom-reads LOW — the same class of bug as the server's GPIO39 BtnA
// under load.  A flickery phantom was faking the 1.5 s pairing-hold, which kicked
// the node into channel-scanning.  Reject it: a real hold sits solidly LOW, so
// require a burst of re-samples (~1 ms) to ALL read LOW; any HIGH in the burst
// means it's noise.
static bool button_confirmed_down(void) {
  for (int i = 0; i < 5; i++) {
    if (digitalRead(BUTTON_PIN) != LOW) return false;
    delayMicroseconds(200);
  }
  return true;
}

static void button_tick(uint32_t now) {
  bool down = button_confirmed_down();

  // Only acts in running mode: a 1.5 s hold drops into pairing.  In pairing
  // mode the button is ignored (adoption is driven by the incoming packet).
  if (!pairing_mode) {
    if (down && !btn_down_prev) {
      btn_pressed_at = now;
    } else if (down && (now - btn_pressed_at >= PAIRING_ENTRY_HOLD_MS)) {
      enter_pairing_mode();
    }
  }
  btn_down_prev = down;
}

// ── ESP-NOW receive ─────────────────────────────────────────────────────────
static void msg_recv_cb(const esp_now_recv_info * /*info*/,
                        const uint8_t *data, int len) {
  // PPOSB — beat / spectrum.  Just latch the beat so WASH_BEAT can react.
  if (len >= (int)sizeof(PpAudioPacket) &&
      memcmp(data, PP_AUDIO_MAGIC, PP_MAGIC_LEN) == 0) {
    const PpAudioPacket *aud = (const PpAudioPacket *)data;
    if (aud->channelId != pp_channel_id) return;
    portENTER_CRITICAL(&ctrlMutex);
    ctrlBeatSeq = aud->beatSeq;
    ctrlBeatStr = aud->beatStrength;
    memcpy(ctrlBands, aud->bands, PP_AUDIO_BANDS);
    portEXIT_CRITICAL(&ctrlMutex);
    return;
  }

  // PPOST — the state broadcast.
  if (len < (int)sizeof(PpStatePacket)) return;
  if (memcmp(data, PP_MAGIC, PP_MAGIC_LEN) != 0) return;
  const PpStatePacket *pkt = (const PpStatePacket *)data;

  // Any valid PPOST proves a sender is broadcasting on this channel, so reset the
  // hop timer NOW — before the pairing/channelId filtering — so a packet flood
  // keeps us anchored here instead of timing out and hopping away.
  portENTER_CRITICAL(&rxMutex);
  last_valid_packet_ms = millis();
  ppPacketCount++;
  portEXIT_CRITICAL(&rxMutex);

  // Pairing: first packet on any scanned channel adopts the show.
  if (pairing_mode && !pairing_adopted) {
    pp_channel_id      = pkt->channelId;
    pairing_adopted    = true;
    pairing_adopted_ms = millis();
    preferences.putUShort("channel-id", pp_channel_id);
    preferences.putUChar ("radio-chan", wifi_channel_idx);
    Serial.printf("[PAIR] adopted channelId=0x%04X on ch=%u\n",
                  (unsigned)pp_channel_id, (unsigned)channels[wifi_channel_idx]);
  }

  if (pkt->channelId != pp_channel_id) return;   // not our show (still anchors us)

  // Lock the effect clock to the sender's tick.
  portENTER_CRITICAL(&tickMutex);
  lockedTick = pkt->tick;
  lockedAtMs = millis();
  portEXIT_CRITICAL(&tickMutex);

  bool touched = (pkt->flags & PP_FLAG_TOUCHED) != 0;
  portENTER_CRITICAL(&ctrlMutex);
  ctrlEffect   = pkt->effectId;
  ctrlSlider   = pkt->slider;
  ctrlX        = pkt->touchX;
  ctrlY        = pkt->touchY;
  ctrlPressed  = touched;
  ctrlPowerOff = (pkt->flags & PP_FLAG_POWER_OFF) != 0;
  if (touched && !last_touched) ctrlTapped = true;   // consumed by the renderer
  last_touched = touched;
  portEXIT_CRITICAL(&ctrlMutex);
}

static void network_setup() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  // An un-associated STA defaults to modem-sleep, which makes ESP-NOW reception
  // drop packets in bursts under heavy traffic (a slider flood) — long enough to
  // trip the channel-hop.  Keep the radio awake.
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_LR | WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  if (esp_now_init() != 0) { Serial.println("esp_now_init FAILED"); return; }
  // Receive-only — no peer needed for broadcast RX.
  if (esp_now_register_recv_cb(msg_recv_cb) != ESP_OK) {
    Serial.println("register_recv_cb FAILED");
    return;
  }
  Serial.println("[NET] esp-now RX ready");
}

// ════════════════════════════════════════════════════════════════════════════
//  Render one frame
// ════════════════════════════════════════════════════════════════════════════
// Snapshot the control state, derive the beat/tap envelopes, hand it to the live
// effect (effectId -> renderer in dmx_effects.cpp; each effect in its own
// eff_*.cpp), then ship the RGBW.  Master brightness + power-off live here so
// the effects stay pure functions of one frame's control state.
static void dmxRender(uint32_t tick) {
  EffectCtx c;
  c.tick = tick;
  uint8_t slider;
  bool    powerOff;
  portENTER_CRITICAL(&ctrlMutex);
  c.effectId = ctrlEffect;
  slider     = ctrlSlider;
  c.x        = ctrlX;
  c.y        = ctrlY;
  c.pressed  = ctrlPressed;
  powerOff   = ctrlPowerOff;
  c.tapped   = ctrlTapped;  ctrlTapped = false;
  c.beatSeq  = ctrlBeatSeq;
  c.beatStr  = ctrlBeatStr;
  memcpy(c.bands, ctrlBands, PP_AUDIO_BANDS);
  portEXIT_CRITICAL(&ctrlMutex);

  if (powerOff) { washWrite(0, 0, 0, 0, 0); return; }

  // Slider drives master brightness directly: top = full, bottom = off.  The
  // server defaults the slider to 255, so an untouched controller is full bright
  // (don't special-case 0 here — that made sliding to the bottom jump to full).
  uint8_t master = slider;

  // Beat envelope — decays ~250 ms after each new beat_seq.  Tap envelope —
  // a one-shot pop that decays ~200 ms after the finger lands.  Kept here (not
  // in the effects) because they carry state across frames.
  static uint32_t lastBeatSeq = 0, beatStartMs = 0, tapStartMs = 0;
  if (c.beatSeq != lastBeatSeq) { lastBeatSeq = c.beatSeq; beatStartMs = millis(); }
  if (c.tapped)                 { tapStartMs  = millis(); }
  uint32_t bdt = millis() - beatStartMs;
  c.beatEnv = (bdt >= 250) ? 0 : (uint8_t)(255 - bdt * 255 / 250);
  uint32_t tdt = millis() - tapStartMs;
  c.tapEnv  = (tdt >= 200) ? 0 : (uint8_t)(255 - tdt * 255 / 200);

  WashColour col = washRenderEffect(c);
  washWrite(col.r, col.g, col.b, col.w, master);
}

// Pairing feedback on the wash itself: white strobe (200 ms on / 800 ms off)
// while scanning; solid green once a show has been adopted.
static void renderPairing(uint32_t tick) {
  if (pairing_adopted) { washWrite(0, 255, 0, 0, 255); return; }
  bool on = (tick % 50) < 10;
  uint8_t v = on ? 255 : 0;
  washWrite(v, v, v, v, 255);
}

// ════════════════════════════════════════════════════════════════════════════
//  OLED status display  (original M5Stick SH1107, U8x8 text — see binx sketch)
// ════════════════════════════════════════════════════════════════════════════
// 64x128 panel, 8x8 font => an 8-column x 16-row character grid.  Only the loop
// touches the OLED (SPI); the recv callback just bumps a counter, so no locking
// of the bus is needed.
U8X8_SH1107_64X128_4W_HW_SPI oled(/*cs=*/14, /*dc=*/27, /*reset=*/33);

// Draw one 8-char row, space-padded so stale characters get cleared.
static void oledRow(uint8_t row, const char *s) {
  char buf[9];
  uint8_t i = 0;
  for (; i < 8 && s[i]; i++) buf[i] = s[i];
  for (; i < 8;          i++) buf[i] = ' ';
  buf[8] = 0;
  oled.drawString(0, row, buf);
}

// Draw a longer string across two rows (chars 0-7 then 8-15).
static void oledRow2(uint8_t row, const char *s) {
  oledRow(row, s);
  size_t len = strlen(s);
  oledRow(row + 1, len > 8 ? s + 8 : "");
}

static void oledInit() {
  // Original M5Stick OLED is on the ESP32 default VSPI: SCK=18, MOSI=23 (write-
  // only so MISO=-1), with CS=14, DC=27, RST=33.  Confirmed by the user's
  // aquafficent sketch, which uses G13 (serial RX) and G25 (heater out) for other
  // things — so the OLED can't be on those.  The factory sketches rely on the
  // M5Stick board's default SPI; we set it explicitly so it works under any board.
  SPI.begin(/*sck=*/18, /*miso=*/-1, /*mosi=*/23, /*ss=*/14);
  oled.begin();
  oled.setFont(u8x8_font_chroma48medium8_r);
  oled.clear();
  oledRow(0, "MAGITRAC");
  oledRow(1, "  DMX");
}

// Refresh the status screen ~5 Hz: current effect (number + name) and a
// packet-received indicator (RX blinks + running count).
static void oledTick() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 200) return;
  lastMs = millis();

  uint8_t effect;
  portENTER_CRITICAL(&ctrlMutex);
  effect = ctrlEffect;
  portEXIT_CRITICAL(&ctrlMutex);

  uint32_t pkts;
  portENTER_CRITICAL(&rxMutex);
  pkts = ppPacketCount;
  portEXIT_CRITICAL(&rxMutex);

  // Mode / pairing status.
  const char *status = pairing_mode ? (pairing_adopted ? "ADOPTED" : "PAIRING")
                                     : "RUN";
  oledRow(3, status);

  if (pairing_mode && !pairing_adopted) {
    // Show which channel we're scanning while waiting for a packet.
    char cbuf[9];
    snprintf(cbuf, sizeof(cbuf), "SCAN ch%u", (unsigned)channels[wifi_channel_idx]);
    oledRow(4, cbuf);
    oledRow(5, "HOLD BTN");
    oledRow(6, "for show");
  } else {
    // Effect number + name.
    char ebuf[9];
    snprintf(ebuf, sizeof(ebuf), "EFF  %3u", (unsigned)effect);
    oledRow(4, ebuf);
    oledRow2(5, pixelPostEffectName(effect));
  }

  // Packet-received notification: a '*' that blinks each time a new PPOST
  // landed since the last refresh, plus a rolling count.
  static uint32_t lastPkts = 0;
  bool rx = (pkts != lastPkts);
  lastPkts = pkts;
  char rbuf[9];
  snprintf(rbuf, sizeof(rbuf), "RX%c%5lu", rx ? '*' : ' ',
           (unsigned long)(pkts % 100000));
  oledRow(8, rbuf);

  // WiFi channel currently being listened on — watch this: if it jumps off the
  // sender's channel during a packet burst, the channel-hop is the culprit.
  // Split over two rows so channel 11 (2 digits) isn't truncated by the 8-wide
  // display.
  char wbuf[9];
  oledRow(10, "WIFI CH:");
  snprintf(wbuf, sizeof(wbuf), "   %u", (unsigned)channels[wifi_channel_idx]);
  oledRow(11, wbuf);
}

// ════════════════════════════════════════════════════════════════════════════
//  Arduino entry points
// ════════════════════════════════════════════════════════════════════════════
static const uint32_t DMX_FRAME_MS = 20;   // 50 Hz refresh, == post tick rate
static uint32_t lastFrameMs = 0;

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(BUTTON_PIN, INPUT);
  oledInit();
  memset(dmxData, 0, sizeof(dmxData));
  DmxSerial.begin(DMX_BAUD, SERIAL_8N2, /*rxPin=*/-1, /*txPin=*/DMX_TX_PIN);

  network_setup();

  preferences.begin("magitrac-dmx", false);
  pp_channel_id    = preferences.getUShort("channel-id", PP_CHANNEL_ID_UNPAIRED);
  wifi_channel_idx = preferences.getUChar ("radio-chan", 0);
  set_radio_channel(wifi_channel_idx);

  Serial.printf("[BOOT] channelId=0x%04X radio_ch=%u  DMX TX=GPIO%d\n",
                (unsigned)pp_channel_id, (unsigned)channels[wifi_channel_idx],
                DMX_TX_PIN);

  if (pp_channel_id == PP_CHANNEL_ID_UNPAIRED) enter_pairing_mode();

  last_valid_packet_ms = millis();
}

void loop() {
  uint32_t now = millis();
  button_tick(now);
  oledTick();

  // DEBUG: 1 Hz status — watch sinceRx (timer staleness), pair, and ch.
  {
    static uint32_t lastDbg = 0;
    if (now - lastDbg >= 1000) {
      lastDbg = now;
      uint32_t last, pkts;
      portENTER_CRITICAL(&rxMutex);
      last = last_valid_packet_ms;
      pkts = ppPacketCount;
      portEXIT_CRITICAL(&rxMutex);
      Serial.printf("[DBG] t=%lu pair=%d ch=%u pkts=%lu sinceRx=%ldms\n",
                    (unsigned long)now, (int)pairing_mode,
                    (unsigned)channels[wifi_channel_idx], (unsigned long)pkts,
                    (long)(int32_t)(millis() - last));
    }
  }

  if (pairing_mode) {
    // Scan {1,6,11} until a PPOST is heard; hold green briefly after adopting,
    // then save + return to running mode.
    if (!pairing_adopted && (long)(now - pairing_scan_next_ms) >= 0) {
      set_radio_channel((wifi_channel_idx + 1) % (sizeof(channels) / sizeof(channels[0])));
      pairing_scan_next_ms = now + PAIRING_SCAN_INTERVAL_MS;
      Serial.printf("[PAIR] scan ch=%u\n", (unsigned)channels[wifi_channel_idx]);
    }
    if (pairing_adopted && (now - pairing_adopted_ms) >= PAIRING_CONFIRM_MS) {
      exit_pairing_save();
    }
  } else {
    // Running mode: hop to the next channel after a genuine silence so we follow
    // the sender if it moves channel.  Any valid PPOST resets last_valid_packet_ms.
    uint32_t last;
    portENTER_CRITICAL(&rxMutex);
    last = last_valid_packet_ms;
    portEXIT_CRITICAL(&rxMutex);
    // SIGNED + fresh millis(): during a flood the recv callback (other core) can
    // set last_valid_packet_ms to a time AFTER the loop-top `now`, so a plain
    // unsigned (now - last) underflows to ~4e9 and false-triggers a hop.  This was
    // the "flood makes it cycle channels" bug.
    if ((int32_t)(millis() - last) >= (int32_t)CHANNEL_HOP_MS) {
      set_radio_channel((wifi_channel_idx + 1) % (sizeof(channels) / sizeof(channels[0])));
      preferences.putUChar("radio-chan", wifi_channel_idx);
      Serial.printf("[HOP] now on WiFi ch=%u\n", (unsigned)channels[wifi_channel_idx]);
      portENTER_CRITICAL(&rxMutex);
      last_valid_packet_ms = millis();
      portEXIT_CRITICAL(&rxMutex);
    }
  }

  // Render + emit one DMX frame every 20 ms.
  if (now - lastFrameMs >= DMX_FRAME_MS) {
    lastFrameMs = now;
    uint32_t tick = currentTickMs() / 20;
    if (pairing_mode) renderPairing(tick);
    else              dmxRender(tick);
    dmxSendFrame();
  }
}

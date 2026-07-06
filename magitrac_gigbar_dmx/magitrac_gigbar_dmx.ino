// magitrac_gigbar_dmx — PPOST-locked DMX node for a GigBar 2
// ----------------------------------------------------------
// Target: original M5Stick (ESP32-PICO-D4)  https://docs.m5stack.com/en/core/m5stick
// Wiring: M5Stick Grove port -> DMX interface (RS-485 -> XLR) -> GigBar 2 (23-ch).
//
// Sibling of magitrac_dmx (the single RGBW wash node): identical PixelPost
// receiver / tick-lock / pairing / channel-hop / OLED plumbing, but the render
// drives a GigBar 2 — a 4-in-1 bar with two RGB+UV Pars, two Derbies, a Laser and
// a dedicated Strobe panel — instead of one wash.  It speaks the exact same
// pixelpost_proto.h wire protocol, locks its effect clock to the sender's tick,
// and re-emits a DMX frame every 20 ms, so the bar follows the show in lock-step
// with the PixelPosts (and the wash node).
//
// Three pieces, matching the brief:
//   1. PAIRING  — hold the M5Stick button (GPIO35).  The node scans WiFi
//                 channels {1,6,11} and the FIRST PPOST it hears adopts that
//                 packet's channelId + locks the radio channel, then saves to
//                 NVS.  While pairing, the pars strobe white.
//   2. CONTROL  — PPOST (and PPOSB beat) packets set the live effect, slider,
//                 tap, power-off and beat data.
//   3. SEQUENCE — every 20 ms tick, gigRender() turns the current effect + tick
//                 (+ beat) into a full GigFrame and sends a DMX frame.
//
// Fixture DMX start address = D001.  Both Pars run in MANUAL mode (control chans
// CH5/CH10 = 0 => "RGB based on channels"), so CH1-4 / CH6-9 are direct R/G/B/UV.

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
#include "gig_effects.h"   // EffectCtx, GigFrame, gigRenderEffect — one eff_*.cpp per effect

// ════════════════════════════════════════════════════════════════════════════
//  DMX-512 transport  (proven bring-up: GPIO25, 250k 8N2, line-invert BREAK)
// ════════════════════════════════════════════════════════════════════════════
static const int      DMX_TX_PIN = 25;        // Grove UART TX (G13 = RX, unused)
static const uint32_t DMX_BAUD   = 250000;    // 4 us/bit
static const int      DMX_UART   = 1;         // UART1 == HardwareSerial(1) == UART_NUM_1
#define DMX_CHANNELS 512

// GigBar 2 — 23-channel mode, fixture DMX start address = D001.
//   Par 1 : CH1 R  CH2 G  CH3 B  CH4 UV  CH5 control (0=manual RGB)
//   Par 2 : CH6 R  CH7 G  CH8 B  CH9 UV  CH10 control (0=manual RGB)
//   Derby1: CH11 colour  CH12 strobe-rate  CH13 rotation
//   Derby2: CH14 colour  CH15 strobe-rate  CH16 rotation
//   Laser : CH17 colour  CH18 strobe  CH19 pattern
//   Strobe: CH20 pattern  CH21 white-dimmer  CH22 uv-dimmer  CH23 speed
enum {
  CH_P1_R = 1,  CH_P1_G = 2,  CH_P1_B = 3,  CH_P1_UV = 4,  CH_P1_CTRL = 5,
  CH_P2_R = 6,  CH_P2_G = 7,  CH_P2_B = 8,  CH_P2_UV = 9,  CH_P2_CTRL = 10,
  CH_D1_COLOR = 11, CH_D1_STROBE = 12, CH_D1_ROT = 13,
  CH_D2_COLOR = 14, CH_D2_STROBE = 15, CH_D2_ROT = 16,
  CH_LAS_COLOR = 17, CH_LAS_STROBE = 18, CH_LAS_PATTERN = 19,
  CH_STR_PATTERN = 20, CH_STR_WHITE = 21, CH_STR_UV = 22, CH_STR_SPEED = 23,
};

// Per-par "control" channel (CH5 / CH10).  The chart's 0..127 band is "RGB based
// on channels 1,2,3" (i.e. manual RGB) — but value 0 tends to leave the par
// inactive/blacked-out on these bars, so we sit mid-band to actually enable the
// manual RGB output.  If the pars still don't light, try the "RGB 100%" band
// (250..255) instead.
static const uint8_t PAR_CTRL_MANUAL = 64;

static uint8_t      dmxData[DMX_CHANNELS];
HardwareSerial      DmxSerial(DMX_UART);

static inline void dmxSet(int ch, uint8_t v) {
  if (ch >= 1 && ch <= DMX_CHANNELS) dmxData[ch - 1] = v;
}

static inline uint8_t scale8(uint8_t v, uint8_t m) {   // v * m / 255
  return (uint8_t)(((uint16_t)v * m) / 255);
}

// The second derby counter-spins the first: turn a CW code into the matching CCW
// code (and vice-versa) so the two derbies sweep in opposite directions.
static uint8_t mirrorRot(uint8_t rot) {
  if (rot == 0) return 0;
  if (rot >= 5 && rot <= 127) {                    // CW -> CCW
    uint16_t ccw = 134 + (rot - 5);
    return (uint8_t)(ccw > 255 ? 255 : ccw);
  }
  if (rot >= 134) return (uint8_t)(5 + (rot - 134)); // CCW -> CW
  return rot;                                        // stop bands pass through
}

// One place to lay a GigFrame onto the 23 channels.  master scales the dimmable
// sections (pars + strobe LEDs); the derby/laser have no dimmer, so master==0
// blacks them out via their colour code instead.
static void gigWrite(const GigFrame &f, uint8_t master) {
  // Resolve each side's par RGB+UV and derby colour.  Normally both sides show
  // the shared colour (white folded into RGB so plain-white looks still light the
  // pars); a split effect (Circles) drives the two sides independently.
  uint8_t p1r, p1g, p1b, p1uv, p2r, p2g, p2b, p2uv, d1c, d2c;
  if (f.splitSides) {
    p1r = f.p1r; p1g = f.p1g; p1b = f.p1b; p1uv = f.p1uv; d1c = f.d1Color;
    p2r = f.p2r; p2g = f.p2g; p2b = f.p2b; p2uv = f.p2uv; d2c = f.d2Color;
  } else {
    uint8_t pr = f.r > f.w ? f.r : f.w;
    uint8_t pg = f.g > f.w ? f.g : f.w;
    uint8_t pb = f.b > f.w ? f.b : f.w;
    p1r = p2r = pr; p1g = p2g = pg; p1b = p2b = pb; p1uv = p2uv = f.uv;
    d1c = d2c = f.derbyColor;
  }

  // Par 1 (manual RGB mode: control channel = PAR_CTRL_MANUAL).
  dmxSet(CH_P1_R,  scale8(p1r,  master));
  dmxSet(CH_P1_G,  scale8(p1g,  master));
  dmxSet(CH_P1_B,  scale8(p1b,  master));
  dmxSet(CH_P1_UV, scale8(p1uv, master));
  dmxSet(CH_P1_CTRL, PAR_CTRL_MANUAL);
  // Par 2.
  dmxSet(CH_P2_R,  scale8(p2r,  master));
  dmxSet(CH_P2_G,  scale8(p2g,  master));
  dmxSet(CH_P2_B,  scale8(p2b,  master));
  dmxSet(CH_P2_UV, scale8(p2uv, master));
  dmxSet(CH_P2_CTRL, PAR_CTRL_MANUAL);

  // Derbies — no dimmer, so master==0 blacks them out by colour code.
  dmxSet(CH_D1_COLOR,  master ? d1c : 0);
  dmxSet(CH_D1_STROBE, f.derbyStrobe);
  dmxSet(CH_D1_ROT,    f.derbyRotate);
  dmxSet(CH_D2_COLOR,  master ? d2c : 0);
  dmxSet(CH_D2_STROBE, f.derbyStrobe);
  dmxSet(CH_D2_ROT,    mirrorRot(f.derbyRotate));

  // Laser — likewise gated by master via its colour code.
  dmxSet(CH_LAS_COLOR,   master ? f.laserColor : 0);
  dmxSet(CH_LAS_STROBE,  f.laserStrobe);
  dmxSet(CH_LAS_PATTERN, f.laserPattern);

  // Dedicated strobe panel — its white/UV LEDs dim with master.
  dmxSet(CH_STR_PATTERN, f.strobePattern);
  dmxSet(CH_STR_WHITE,   scale8(f.strobeWhite, master));
  dmxSet(CH_STR_UV,      scale8(f.strobeUv,    master));
  dmxSet(CH_STR_SPEED,   f.strobeSpeed);
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
static volatile uint8_t  ctrlPostCount = 1;      // posts in the show (spatial effects)
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
  // PPOSB — beat / spectrum.  Just latch the beat so the beat effects can react.
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
  ctrlPostCount = pkt->postCount ? pkt->postCount : 1;
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
// effect (effectId -> renderer in gig_effects.cpp; each effect in its own
// eff_*.cpp), then ship the GigFrame.  Master brightness + power-off live here so
// the effects stay pure functions of one frame's control state.
static void gigRender(uint32_t tick) {
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
  c.postCount = ctrlPostCount;
  memcpy(c.bands, ctrlBands, PP_AUDIO_BANDS);
  portEXIT_CRITICAL(&ctrlMutex);
  c.slider = slider;

  if (powerOff) { GigFrame off; gigWrite(off, 0); return; }

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

  GigFrame f = gigRenderEffect(c);
  gigWrite(f, master);
}

// Pairing feedback on the bar itself: white par strobe (200 ms on / 800 ms off)
// while scanning; solid green pars once a show has been adopted.
static void renderPairing(uint32_t tick) {
  GigFrame f;
  if (pairing_adopted) { f.g = 255; gigWrite(f, 255); return; }
  bool on = (tick % 50) < 10;
  uint8_t v = on ? 255 : 0;
  f.r = f.g = f.b = f.w = v;
  gigWrite(f, 255);
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
  oledRow(1, " GIGBAR");
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

  preferences.begin("magitrac-gig", false);
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
    else              gigRender(tick);
    dmxSendFrame();
  }
}

// magitrac_gigbar_test — GigBar 2 DMX channel walk-through
// --------------------------------------------------------
// Target: original M5Stick (ESP32-PICO-D4).  Same DMX transport as
// magitrac_gigbar_dmx (GPIO25, 250k 8N2, line-invert BREAK) but NO PixelPost /
// pairing / OLED — it just steps through every section of the bar, blinks it
// three times, and prints to Serial what it is driving so you can tell which
// physical light corresponds to which channel.
//
// Set the fixture to 23-channel mode, DMX start address = 1.  Open the Serial
// Monitor at 115200.  The sequence loops forever.
//
// Reading the output:  each step prints e.g.
//     [TEST] Par1 RED        ch1=255  ctrl(ch5)=64
// then that light blinks 3x.  Note any step where nothing happens.

#include <Arduino.h>
#include "driver/uart.h"
#include <string.h>

// ── DMX-512 transport (identical to the main node) ──────────────────────────
static const int      DMX_TX_PIN = 25;
static const uint32_t DMX_BAUD   = 250000;
static const int      DMX_UART   = 1;
#define DMX_CHANNELS 512

// GigBar 2, 23-ch mode, start address 1.
enum {
  CH_P1_R = 1,  CH_P1_G = 2,  CH_P1_B = 3,  CH_P1_UV = 4,  CH_P1_CTRL = 5,
  CH_P2_R = 6,  CH_P2_G = 7,  CH_P2_B = 8,  CH_P2_UV = 9,  CH_P2_CTRL = 10,
  CH_D1_COLOR = 11, CH_D1_STROBE = 12, CH_D1_ROT = 13,
  CH_D2_COLOR = 14, CH_D2_STROBE = 15, CH_D2_ROT = 16,
  CH_LAS_COLOR = 17, CH_LAS_STROBE = 18, CH_LAS_PATTERN = 19,
  CH_STR_PATTERN = 20, CH_STR_WHITE = 21, CH_STR_UV = 22, CH_STR_SPEED = 23,
};

static uint8_t dmxData[DMX_CHANNELS];
HardwareSerial DmxSerial(DMX_UART);

static inline void dmxSet(int ch, uint8_t v) {
  if (ch >= 1 && ch <= DMX_CHANNELS) dmxData[ch - 1] = v;
}
static void allOff() { memset(dmxData, 0, sizeof(dmxData)); }

static void dmxBreak() {
  DmxSerial.flush();
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV);      // TX low  — BREAK
  delayMicroseconds(120);
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE);  // TX high — MAB
  delayMicroseconds(12);
}
static void dmxSendFrame() {
  dmxBreak();
  DmxSerial.write((uint8_t)0x00);
  DmxSerial.write(dmxData, DMX_CHANNELS);
  DmxSerial.flush();
}

// Emit DMX continuously for `ms` (the line must keep refreshing ~50 Hz).
static void emit(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { dmxSendFrame(); delay(20); }
}

// Print the label, then blink whatever is currently in dmxData 3 times so it is
// unmistakable which fixture is under test (on 350 ms / off 200 ms).
static void blinkCurrent(const char *label) {
  Serial.printf("[TEST] %s\n", label);
  uint8_t saved[DMX_CHANNELS];
  memcpy(saved, dmxData, sizeof(saved));
  for (int i = 0; i < 3; i++) {
    memcpy(dmxData, saved, sizeof(saved)); emit(350);
    allOff();                              emit(200);
  }
  allOff();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  memset(dmxData, 0, sizeof(dmxData));
  DmxSerial.begin(DMX_BAUD, SERIAL_8N2, /*rxPin=*/-1, /*txPin=*/DMX_TX_PIN);
  Serial.println("\n[GIGBAR TEST] DMX on GPIO25, 23-ch mode, start addr 1");
  Serial.println("[GIGBAR TEST] watch which light blinks for each step.\n");
}

void loop() {
  Serial.println("==== sweep start ====");

  // ── Par 1: resolve the control-channel value first (which one lights it?) ──
  allOff(); dmxSet(CH_P1_R,255); dmxSet(CH_P1_G,255); dmxSet(CH_P1_B,255);
  dmxSet(CH_P1_CTRL,0);   blinkCurrent("Par1 WHITE  ctrl(ch5)=0    (RGB=255)");
  allOff(); dmxSet(CH_P1_R,255); dmxSet(CH_P1_G,255); dmxSet(CH_P1_B,255);
  dmxSet(CH_P1_CTRL,64);  blinkCurrent("Par1 WHITE  ctrl(ch5)=64   (RGB=255)");
  allOff(); dmxSet(CH_P1_R,255); dmxSet(CH_P1_G,255); dmxSet(CH_P1_B,255);
  dmxSet(CH_P1_CTRL,250); blinkCurrent("Par1 WHITE  ctrl(ch5)=250  (RGB=255)");

  // ── Par 1: each colour (using ctrl=64; if 250 won above, that's the fix) ──
  allOff(); dmxSet(CH_P1_CTRL,64); dmxSet(CH_P1_R,255);  blinkCurrent("Par1 RED    ch1=255  ctrl=64");
  allOff(); dmxSet(CH_P1_CTRL,64); dmxSet(CH_P1_G,255);  blinkCurrent("Par1 GREEN  ch2=255  ctrl=64");
  allOff(); dmxSet(CH_P1_CTRL,64); dmxSet(CH_P1_B,255);  blinkCurrent("Par1 BLUE   ch3=255  ctrl=64");
  allOff(); dmxSet(CH_P1_CTRL,64); dmxSet(CH_P1_UV,255); blinkCurrent("Par1 UV     ch4=255  ctrl=64");

  // ── Par 2 ──────────────────────────────────────────────────────────────────
  allOff(); dmxSet(CH_P2_R,255); dmxSet(CH_P2_G,255); dmxSet(CH_P2_B,255);
  dmxSet(CH_P2_CTRL,0);   blinkCurrent("Par2 WHITE  ctrl(ch10)=0   (RGB=255)");
  allOff(); dmxSet(CH_P2_R,255); dmxSet(CH_P2_G,255); dmxSet(CH_P2_B,255);
  dmxSet(CH_P2_CTRL,64);  blinkCurrent("Par2 WHITE  ctrl(ch10)=64  (RGB=255)");
  allOff(); dmxSet(CH_P2_R,255); dmxSet(CH_P2_G,255); dmxSet(CH_P2_B,255);
  dmxSet(CH_P2_CTRL,250); blinkCurrent("Par2 WHITE  ctrl(ch10)=250 (RGB=255)");
  allOff(); dmxSet(CH_P2_CTRL,64); dmxSet(CH_P2_R,255);  blinkCurrent("Par2 RED    ch6=255  ctrl=64");
  allOff(); dmxSet(CH_P2_CTRL,64); dmxSet(CH_P2_G,255);  blinkCurrent("Par2 GREEN  ch7=255  ctrl=64");
  allOff(); dmxSet(CH_P2_CTRL,64); dmxSet(CH_P2_B,255);  blinkCurrent("Par2 BLUE   ch8=255  ctrl=64");
  allOff(); dmxSet(CH_P2_CTRL,64); dmxSet(CH_P2_UV,255); blinkCurrent("Par2 UV     ch9=255  ctrl=64");

  // ── Derby 1 (ch11 colour / ch12 strobe / ch13 rotation) ────────────────────
  allOff(); dmxSet(CH_D1_COLOR,37);  blinkCurrent("Derby1 RED   ch11=37");
  allOff(); dmxSet(CH_D1_COLOR,62);  blinkCurrent("Derby1 GREEN ch11=62");
  allOff(); dmxSet(CH_D1_COLOR,87);  blinkCurrent("Derby1 BLUE  ch11=87");
  allOff(); dmxSet(CH_D1_COLOR,187); blinkCurrent("Derby1 RGB   ch11=187");
  allOff(); dmxSet(CH_D1_COLOR,187); dmxSet(CH_D1_ROT,70);
            blinkCurrent("Derby1 SPIN  ch11=187 ch13=70 (CW)");

  // ── Derby 2 (ch14 / ch15 / ch16) ───────────────────────────────────────────
  allOff(); dmxSet(CH_D2_COLOR,37);  blinkCurrent("Derby2 RED   ch14=37");
  allOff(); dmxSet(CH_D2_COLOR,62);  blinkCurrent("Derby2 GREEN ch14=62");
  allOff(); dmxSet(CH_D2_COLOR,87);  blinkCurrent("Derby2 BLUE  ch14=87");
  allOff(); dmxSet(CH_D2_COLOR,187); dmxSet(CH_D2_ROT,200);
            blinkCurrent("Derby2 SPIN  ch14=187 ch16=200 (CCW)");

  // ── Laser (ch17 colour / ch18 strobe / ch19 pattern) ───────────────────────
  allOff(); dmxSet(CH_LAS_COLOR,60);  blinkCurrent("Laser RED    ch17=60");
  allOff(); dmxSet(CH_LAS_COLOR,100); blinkCurrent("Laser GREEN  ch17=100");
  allOff(); dmxSet(CH_LAS_COLOR,120); blinkCurrent("Laser R+G    ch17=120");
  allOff(); dmxSet(CH_LAS_COLOR,120); dmxSet(CH_LAS_PATTERN,70);
            blinkCurrent("Laser SPIN   ch17=120 ch19=70 (CW)");

  // ── Strobe panel (ch20 pattern / ch21 white / ch22 uv / ch23 speed) ────────
  allOff(); dmxSet(CH_STR_PATTERN,0);   dmxSet(CH_STR_WHITE,255);
            blinkCurrent("Strobe WHITE static  ch20=0 ch21=255");
  allOff(); dmxSet(CH_STR_PATTERN,105); dmxSet(CH_STR_WHITE,255); dmxSet(CH_STR_SPEED,200);
            blinkCurrent("Strobe WHITE strobe  ch20=105 ch21=255 ch23=200");
  allOff(); dmxSet(CH_STR_PATTERN,0);   dmxSet(CH_STR_UV,255);
            blinkCurrent("Strobe UV static     ch20=0 ch22=255");

  Serial.println("==== sweep done, looping ====\n");
  emit(1000);   // brief blackout between sweeps
}

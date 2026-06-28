// gt911_touch_test — measure the GT911's *fresh-frame* behaviour on the LilyGo
// T5 S3, to verify (before we trust it) whether a held-still finger keeps
// producing touch frames or whether the controller's config filters/throttles
// them.  This is the load-bearing assumption behind the proposed release-
// watchdog fix for the phantom-STOP bug.
//
// WHAT IT MEASURES (per touch, start → lift):
//   dur     — how long the finger was down (ms)
//   frames  — number of *fresh frames* the controller produced (0x814E bit7)
//   avg     — dur/frames (ms) average inter-frame interval
//   MAXGAP  — the LONGEST silence between fresh frames during the touch (ms)
//   x[..] y[..] — coordinate range seen (reveals whether jitter is filtered)
//
// HOW TO READ IT:
//   Hold a finger STILL for several seconds, then lift.  Then:
//     MAXGAP small (~10-20ms)              → controller reports continuously
//                                            even when still  → watchdog SAFE.
//     MAXGAP large (hundreds–thousands ms) → controller goes quiet on a static
//                                            touch → watchdog would WRONGLY
//                                            release a real hold → UNSAFE.
//   x/y range near zero with small MAXGAP  → jitter IS filtered, but frames
//                                            still flow → watchdog still SAFE
//                                            (it keys on frames, not coords).
//
// NOTE on method: we poll register 0x814E directly rather than via
// gt911_lite::read(), because read() returns "coordinates changed" and would
// mask the very throttling we're testing for.  We still call gt911_lite::begin()
// first so the controller gets the *same* config (sensitivity 20) as the real
// client — we want representative behaviour.
//
// Output goes to Serial AND to the e-paper (one line per touch, drawn only on
// finger-UP so there is no EPD refresh *during* a measured touch — that keeps
// display activity from injecting the very phantoms we're studying, and lets
// the test run on battery with no USB).

#include "EPD_Painter_presets.h"
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include <Wire.h>

#define BOARD_BL_EN 11          // LilyGo T5 S3 frontlight
#define COL_WHITE   0xFF
#define COL_BLACK   0x00

// GT911 registers (same as gt911_lite.h)
static const uint16_t GT_POINT_INFO = 0x814E;
static const uint16_t GT_POINT_1    = 0x814F;

EPD_PainterAdafruit display(EPD_PAINTER_PRESET);
GT911_Lite          touch;      // used only to apply the standard config

static TwoWire* gWire = nullptr;
static uint8_t  gAddr = 0;

// ── Raw GT911 register access on the shared display I²C bus ──────────────────
static uint8_t gtRead8(uint16_t reg) {
    gWire->beginTransmission(gAddr);
    gWire->write((uint8_t)(reg >> 8));
    gWire->write((uint8_t)(reg & 0xFF));
    gWire->endTransmission(false);                 // repeated start
    if (gWire->requestFrom(gAddr, (uint8_t)1) != 1) return 0;
    return gWire->read();
}

static void gtWrite8(uint16_t reg, uint8_t val) {
    gWire->beginTransmission(gAddr);
    gWire->write((uint8_t)(reg >> 8));
    gWire->write((uint8_t)(reg & 0xFF));
    gWire->write(val);
    gWire->endTransmission();
}

static void gtReadBlock(uint16_t reg, uint8_t* buf, uint8_t n) {
    gWire->beginTransmission(gAddr);
    gWire->write((uint8_t)(reg >> 8));
    gWire->write((uint8_t)(reg & 0xFF));
    gWire->endTransmission(false);
    if (gWire->requestFrom(gAddr, n) != n) return;
    for (uint8_t i = 0; i < n && gWire->available(); i++) buf[i] = gWire->read();
}

static bool gtDetectAddr() {
    const uint8_t cands[2] = { 0x5D, 0x14 };
    for (uint8_t i = 0; i < 2; i++) {
        gWire->beginTransmission(cands[i]);
        if (gWire->endTransmission() == 0) { gAddr = cands[i]; return true; }
    }
    return false;
}

// ── On-screen output (drawn on finger-up only) ──────────────────────────────
static int sLineY = 40;
static void epdLine(const char* s) {
    if (sLineY > 520) {                 // wrapped — clear and restart
        display.fillScreen(COL_WHITE);
        sLineY = 40;
    }
    display.setTextColor(COL_BLACK);
    display.setTextSize(2);
    display.setCursor(8, sLineY);
    display.print(s);
    sLineY += 22;
    display.paintLater();               // single refresh, between touches only
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[GT911-TEST] boot");

    if (!display.begin()) {
        Serial.println("[GT911-TEST] display.begin FAILED");
    }
    display.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
    pinMode(BOARD_BL_EN, OUTPUT);
    analogWrite(BOARD_BL_EN, 255);
    display.fillScreen(COL_WHITE);
    display.setTextColor(COL_BLACK);
    display.setTextSize(2);
    display.setCursor(8, 8);
    display.print("GT911 frame test - hold STILL, then lift");
    display.paint();

    // Apply the same controller config the real client uses (sensitivity 20),
    // then grab the shared I²C bus for raw polling.
    gWire = display.getConfig().i2c.wire;
    touch.begin(gWire, 20);

    if (!gtDetectAddr()) {
        Serial.println("[GT911-TEST] GT911 not found on I2C!");
        epdLine("GT911 NOT FOUND");
    } else {
        Serial.printf("[GT911-TEST] GT911 at 0x%02X. Poll running.\n", gAddr);
        Serial.println("[GT911-TEST] Hold a finger STILL several seconds, then lift.");
        Serial.println("[GT911-TEST] Watch MAXGAP: small=continuous, large=throttled.");
    }
}

void loop() {
    static bool     active   = false;
    static uint32_t t0       = 0;
    static uint32_t lastFrame = 0;
    static uint32_t frames   = 0;
    static uint32_t maxGap   = 0;
    static uint16_t xMin=0, xMax=0, yMin=0, yMax=0;
    static uint32_t touchNum = 0;

    if (gAddr == 0) { delay(100); return; }

    uint8_t pointInfo = gtRead8(GT_POINT_INFO);
    if (!(pointInfo & 0x80)) return;        // no fresh frame this poll

    gtWrite8(GT_POINT_INFO, 0);             // ack/clear so controller refreshes
    uint32_t now     = millis();
    uint8_t  touches = pointInfo & 0x0F;

    if (touches > 0) {
        uint8_t d[8] = {0};
        gtReadBlock(GT_POINT_1, d, 8);
        uint16_t x = (uint16_t)d[1] | ((uint16_t)d[2] << 8);
        uint16_t y = (uint16_t)d[3] | ((uint16_t)d[4] << 8);

        if (!active) {
            active   = true;
            t0       = now;
            lastFrame = now;
            frames   = 1;
            maxGap   = 0;
            xMin = xMax = x;
            yMin = yMax = y;
        } else {
            uint32_t gap = now - lastFrame;
            if (gap > maxGap) maxGap = gap;
            lastFrame = now;
            frames++;
            if (x < xMin) xMin = x; if (x > xMax) xMax = x;
            if (y < yMin) yMin = y; if (y > yMax) yMax = y;
        }
    } else {
        // touches == 0 — release frame.  Capture the final (possibly long)
        // silent gap and report.
        if (active) {
            uint32_t gap = now - lastFrame;
            if (gap > maxGap) maxGap = gap;
            uint32_t dur = now - t0;
            float    avg = frames ? (float)dur / (float)frames : 0.0f;
            char line[120];
            snprintf(line, sizeof(line),
                     "#%lu dur=%lums frames=%lu avg=%.1fms MAXGAP=%lums x[%u-%u] y[%u-%u]",
                     (unsigned long)++touchNum, (unsigned long)dur,
                     (unsigned long)frames, avg, (unsigned long)maxGap,
                     xMin, xMax, yMin, yMax);
            Serial.printf("[GT911-TEST] %s\n", line);
            epdLine(line);
            active = false;
        }
    }
}

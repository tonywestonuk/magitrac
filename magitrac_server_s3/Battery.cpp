#include "Battery.h"
#include <M5Unified.h>
#include <Arduino.h>

static int8_t   sPct      = -1;     // -1 = unknown
static bool     sCharging = false;
static uint32_t sLastMs   = 0;

static constexpr uint32_t POLL_INTERVAL_MS = 5000;

void batteryInit() {
    sPct      = M5.Power.getBatteryLevel();
    sCharging = (M5.Power.isCharging() == m5::Power_Class::is_charging);
    sLastMs   = millis();
}

bool batteryPoll() {
    uint32_t now = millis();
    if (now - sLastMs < POLL_INTERVAL_MS) return false;
    sLastMs = now;

    int newPct = M5.Power.getBatteryLevel();
    bool newCharging = (M5.Power.isCharging() == m5::Power_Class::is_charging);

    bool changed = false;
    // 2-point hysteresis to stop the digit twitching between 67/68 on stage.
    if (newPct >= 0 && newPct <= 100) {
        if (sPct < 0 || abs(newPct - sPct) >= 2) {
            sPct = (int8_t)newPct;
            changed = true;
        }
    }
    if (newCharging != sCharging) {
        sCharging = newCharging;
        changed = true;
    }
    return changed;
}

int  batteryPercent()  { return sPct; }
bool batteryCharging() { return sCharging; }

void batteryDrawIcon(int x, int y, uint16_t bgColor) {
    auto& d = M5.Display;

    const int W = 18;                // body width (terminal is the +2 outside)
    const int H = BATT_ICON_H;

    // Clear the whole footprint including the 2 px terminal cap.
    d.fillRect(x, y, BATT_ICON_W, H, bgColor);

    int pct = sPct < 0 ? 0 : sPct;
    uint16_t fg;
    if (sPct < 0)          fg = lgfx::color565(120, 120, 120);
    else if (pct >= 30)    fg = lgfx::color565(  0, 200,   0);
    else if (pct >= 10)    fg = lgfx::color565(220, 180,   0);
    else                   fg = lgfx::color565(220,   0,   0);

    d.drawRect(x, y, W, H, TFT_WHITE);
    d.fillRect(x + W, y + 3, 2, H - 6, TFT_WHITE);    // terminal cap

    if (sPct >= 0) {
        int inner = W - 4;
        int fillW = (inner * pct + 50) / 100;
        if (fillW > inner) fillW = inner;
        if (fillW < 0)     fillW = 0;
        if (fillW > 0)     d.fillRect(x + 2, y + 2, fillW, H - 4, fg);
    } else {
        d.setTextColor(TFT_WHITE, bgColor);
        d.setTextSize(1);
        d.setCursor(x + 6, y + 2);
        d.print("?");
    }

    // Small bolt overlay when charging.  Drawn on top of the fill so it
    // remains visible at any battery level.
    if (sCharging) {
        int cx = x + W / 2;
        d.fillRect(cx,     y + 2,         1, H - 4, TFT_WHITE);
        d.fillRect(cx - 2, y + H / 2 - 1, 5, 2,     TFT_WHITE);
    }
}

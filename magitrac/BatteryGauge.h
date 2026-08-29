#pragma once
#include <Arduino.h>

// ── BatteryGauge ─────────────────────────────────────────────────────────────
//
// A voltage-based state-of-charge estimator sitting on top of the raw Battery
// driver.  The BQ25896 is a charger, not a fuel-gauge IC — it gives terminal
// voltage + charge current but does NOT coulomb-count — so SoC is derived from
// an open-circuit-voltage (OCV) discharge curve with load / charge-current
// compensation so the reading means the same thing plugged or unplugged.
//
// Self-calibrating anchors keep the curve honest over time: charge-termination
// pins the top to 100 %, the empty knee pins the bottom to 0 %.  The estimate
// is persisted to NVS (survives reboots) and smoothed so it glides.
//
//   setup():  battery_begin_*();  batteryGaugeBegin();
//   loop  (~30 s):  batteryGaugeUpdate(millis());
//                   ui.setBattery(batteryGaugePct(), batteryGaugeCharging());

// Post-mortem of the previous power-down, decided at boot.
struct BatteryDeath {
    bool     flatBattery;   // last shutdown was (probably) a flat battery
    bool     brownout;      // last reset was a brownout (power sag)
    uint8_t  lastSoc;       // SoC recorded just before it died (0–100)
};

// Call once in setup(), AFTER battery_begin_*.  Loads the persisted SoC and
// runs the flat-battery post-mortem (reset reason + RTC magic + low flag).
void batteryGaugeBegin();

// Call from the battery poll (~30 s).  Reads the driver, updates the smoothed
// SoC, applies anchors, and persists periodically.  nowMs = millis().
void batteryGaugeUpdate(uint32_t nowMs);

// Smoothed state-of-charge 0–100, or -1 until the first update / no battery.
int  batteryGaugePct();
bool batteryGaugeCharging();

// Raw open-circuit-voltage estimate (mV) from the last update — for diagnostics.
int  batteryGaugeVoc();

// Result of the boot post-mortem (valid after batteryGaugeBegin()).
const BatteryDeath& batteryLastDeath();

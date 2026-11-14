#pragma once
#include <Arduino.h>

class TwoWire;

// ── Initialisation (call exactly one, or ADC then BQ to auto-select) ──────────

// ADC voltage-divider backend (M5PaperS3 — BAT_ADC on GPIO 3, 2:1 divider;
// charge status on GPIO 4, low = charging).  chg_stat_pin < 0 = no charge
// detection.  mv_full / mv_empty are no longer used (BatteryGauge maps
// voltage to percent); they remain only for existing callers.
void battery_begin_adc(uint8_t pin, float div_ratio,
                       int mv_full = 4200, int mv_empty = 3000,
                       int chg_stat_pin = -1, bool chg_active_low = true);

// BQ25896 charger-IC backend (LilyGo T5 S3) — overrides ADC if called after.
void battery_begin_bq25896(TwoWire* wire, uint8_t addr = 0x6B);

// ── Runtime ───────────────────────────────────────────────────────────────────

// (State of charge lives in the BatteryGauge layer, built on the readings below.)

// Returns true when USB/charger is present (BQ25896, or ADC chg_stat_pin).
bool battery_is_charging();

// Raw terminal voltage in mV (both backends), or -1 if unavailable.
int  battery_read_mv();

// Coarse charge phase.  ADC backend only distinguishes DISCHARGING /
// FAST_CHARGE via chg_stat_pin; BQ25896 reports all four from REG0B.
enum BatteryChargePhase {
    BATT_DISCHARGING = 0,   // no external power
    BATT_PRECHARGE,         // trickle charge (BQ only)
    BATT_FAST_CHARGE,       // actively charging
    BATT_CHARGE_DONE        // charge terminated / full (BQ only)
};
BatteryChargePhase battery_charge_phase();

// Measured charge current in mA (BQ25896 REG12), or -1 if not measurable.
int  battery_charge_current_ma();

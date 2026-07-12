#pragma once
#include <Arduino.h>

class TwoWire;

// ── Initialisation (call exactly one, or ADC then BQ to auto-select) ──────────

// ADC voltage-divider backend (M5PaperS3 — BAT_ADC on GPIO 3, 2:1 divider;
// charge status on GPIO 4, low = charging).  chg_stat_pin < 0 = no charge
// detection.
void battery_begin_adc(uint8_t pin, float div_ratio,
                       int mv_full = 4200, int mv_empty = 3000,
                       int chg_stat_pin = -1, bool chg_active_low = true);

// BQ25896 charger-IC backend (LilyGo T5 S3) — overrides ADC if called after.
void battery_begin_bq25896(TwoWire* wire, uint8_t addr = 0x6B);

// ── Runtime ───────────────────────────────────────────────────────────────────

// Returns battery charge 0–100, or -1 if uninitialised / unavailable.
int  battery_read_pct();

// Returns true when USB/charger is present (BQ25896, or ADC chg_stat_pin).
bool battery_is_charging();

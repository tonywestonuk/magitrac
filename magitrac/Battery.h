#pragma once
#include <Arduino.h>

class TwoWire;

// ── Initialisation (call exactly one, or ADC then BQ to auto-select) ──────────

// ADC voltage-divider backend (M5PaperS3 — GPIO 4, 100k/100k divider).
void battery_begin_adc(uint8_t pin, float div_ratio,
                       int mv_full = 4200, int mv_empty = 3000);

// BQ25896 charger-IC backend (LilyGo T5 S3) — overrides ADC if called after.
void battery_begin_bq25896(TwoWire* wire, uint8_t addr = 0x6B);

// ── Runtime ───────────────────────────────────────────────────────────────────

// Returns battery charge 0–100, or -1 if uninitialised / unavailable.
int  battery_read_pct();

// Returns true when USB/charger is present (BQ25896 only; always false for ADC).
bool battery_is_charging();

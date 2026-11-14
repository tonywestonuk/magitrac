#include "Battery.h"
#include <Wire.h>

static enum { NONE, ADC_DIV, BQ25896_I2C } _backend = NONE;

// ── ADC backend ───────────────────────────────────────────────────────────────

static uint8_t _adc_pin;
static float   _adc_div;
static int     _adc_chg_pin = -1;
static bool    _adc_chg_low = true;

void battery_begin_adc(uint8_t pin, float div_ratio, int mv_full, int mv_empty,
                       int chg_stat_pin, bool chg_active_low) {
    // mv_full / mv_empty fed the old linear voltage→percent map, which is
    // gone (BatteryGauge's OCV curve does that job).  The parameters stay only
    // so existing callers keep compiling.
    (void)mv_full;
    (void)mv_empty;
    _adc_pin      = pin;
    _adc_div      = div_ratio;
    _adc_chg_pin  = chg_stat_pin;
    _adc_chg_low  = chg_active_low;
    analogSetPinAttenuation(pin, ADC_11db);
    if (_adc_chg_pin >= 0) pinMode(_adc_chg_pin, INPUT);
    _backend = ADC_DIV;
}

static int _read_adc_mv() {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += analogReadMilliVolts(_adc_pin);
    return (int)(sum / 4 * _adc_div);
}

// ── BQ25896 backend ───────────────────────────────────────────────────────────
//
// Reg 0x0E — Battery Voltage (BATV):  Vbat (mV) = 2304 + bits[6:0] × 20
// Reg 0x0B — Charger Status 1:        bits[7:3] non-zero → VBUS present

static TwoWire* _wire    = nullptr;
static uint8_t  _bq_addr = 0x6B;

void battery_begin_bq25896(TwoWire* wire, uint8_t addr) {
    _wire    = wire;
    _bq_addr = addr;
    _backend = BQ25896_I2C;
}

static bool _bq_read_reg(uint8_t reg, uint8_t& out) {
    _wire->beginTransmission(_bq_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) return false;
    if (_wire->requestFrom(_bq_addr, (uint8_t)1) == 0) return false;
    out = _wire->read();
    return true;
}

static bool _bq_write_reg(uint8_t reg, uint8_t val) {
    _wire->beginTransmission(_bq_addr);
    _wire->write(reg);
    _wire->write(val);
    return _wire->endTransmission() == 0;
}

// REG0E (BATV) is only refreshed by an ADC conversion.  In the POR one-shot
// mode CONV_START never fires on its own, so a cold read returns the reset
// value (2304 mV → 0 %) and the meter looks frozen.  Kick a one-shot
// conversion (REG02 bit 7) and wait for it to auto-clear before reading.
//
// One conversion refreshes every ADC register (BATV 0x0E and ICHGR 0x12
// alike), and the wait blocks the caller, so readers called back-to-back —
// the gauge reads voltage then charge current in one update — share a
// conversion instead of each paying for its own.
static const uint32_t BQ_CONV_FRESH_MS = 500;
static bool     _bq_conv_done    = false;   // a conversion has completed
static uint32_t _bq_conv_done_ms = 0;       // millis() when it did

static void _bq_kick_adc() {
    if (_bq_conv_done && millis() - _bq_conv_done_ms < BQ_CONV_FRESH_MS) return;
    _bq_conv_done = false;
    uint8_t reg02;
    if (!_bq_read_reg(0x02, reg02)) return;
    if (!_bq_write_reg(0x02, reg02 | 0x80)) return;   // CONV_START
    for (int i = 0; i < 40; i++) {                    // ≤ ~120 ms guard
        if (!_bq_read_reg(0x02, reg02)) return;
        if (!(reg02 & 0x80)) {                        // conversion complete
            _bq_conv_done    = true;
            _bq_conv_done_ms = millis();
            return;
        }
        delay(3);
    }
}

static int _read_bq25896_mv() {
    if (!_wire) return -1;
    _bq_kick_adc();
    uint8_t reg0e;
    if (!_bq_read_reg(0x0E, reg0e)) return -1;
    return 2304 + (int)(reg0e & 0x7F) * 20;
}

static bool _bq25896_usb_present() {
    if (!_wire) return false;
    uint8_t reg0b;
    if (!_bq_read_reg(0x0B, reg0b)) return false;
    return (reg0b & 0xF8) != 0;  // VBUS_GD or any VBUS_STAT bit
}

// ── Public API ────────────────────────────────────────────────────────────────

bool battery_is_charging() {
    switch (_backend) {
        case BQ25896_I2C: return _bq25896_usb_present();
        case ADC_DIV:     return _adc_chg_pin >= 0 &&
                                 digitalRead(_adc_chg_pin) == (_adc_chg_low ? LOW : HIGH);
        default:          return false;
    }
}

int battery_read_mv() {
    switch (_backend) {
        case ADC_DIV:     return _read_adc_mv();
        case BQ25896_I2C: return _read_bq25896_mv();
        default:          return -1;
    }
}

BatteryChargePhase battery_charge_phase() {
    switch (_backend) {
        case BQ25896_I2C: {
            uint8_t reg0b;
            if (!_wire || !_bq_read_reg(0x0B, reg0b)) return BATT_DISCHARGING;
            switch ((reg0b >> 3) & 0x3) {          // CHRG_STAT
                case 0x1: return BATT_PRECHARGE;
                case 0x2: return BATT_FAST_CHARGE;
                case 0x3: return BATT_CHARGE_DONE;
                default:  return BATT_DISCHARGING;  // 0x0 = not charging
            }
        }
        case ADC_DIV:
            return battery_is_charging() ? BATT_FAST_CHARGE : BATT_DISCHARGING;
        default:
            return BATT_DISCHARGING;
    }
}

int battery_charge_current_ma() {
    if (_backend != BQ25896_I2C || !_wire) return -1;
    _bq_kick_adc();
    uint8_t reg12;
    if (!_bq_read_reg(0x12, reg12)) return -1;
    return (int)(reg12 & 0x7F) * 50;   // ICHGR: 50 mA / LSB
}

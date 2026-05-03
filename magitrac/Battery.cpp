#include "Battery.h"
#include <Wire.h>

static enum { NONE, ADC_DIV, BQ25896_I2C } _backend = NONE;

// ── ADC backend ───────────────────────────────────────────────────────────────

static uint8_t _adc_pin;
static float   _adc_div;
static int     _adc_mv_full;
static int     _adc_mv_empty;

void battery_begin_adc(uint8_t pin, float div_ratio, int mv_full, int mv_empty) {
    _adc_pin      = pin;
    _adc_div      = div_ratio;
    _adc_mv_full  = mv_full;
    _adc_mv_empty = mv_empty;
    analogSetPinAttenuation(pin, ADC_11db);
    _backend = ADC_DIV;
}

static int _read_adc() {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += analogReadMilliVolts(_adc_pin);
    int bat_mv = (int)(sum / 4 * _adc_div);
    return constrain((bat_mv - _adc_mv_empty) * 100 / (_adc_mv_full - _adc_mv_empty), 0, 100);
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

static int _read_bq25896_pct() {
    if (!_wire) return -1;
    uint8_t reg0e;
    if (!_bq_read_reg(0x0E, reg0e)) return -1;
    int bat_mv = 2304 + (int)(reg0e & 0x7F) * 20;
    return constrain((bat_mv - 3000) * 100 / (4200 - 3000), 0, 100);
}

static bool _bq25896_usb_present() {
    if (!_wire) return false;
    uint8_t reg0b;
    if (!_bq_read_reg(0x0B, reg0b)) return false;
    return (reg0b & 0xF8) != 0;  // VBUS_GD or any VBUS_STAT bit
}

// ── Public API ────────────────────────────────────────────────────────────────

int battery_read_pct() {
    switch (_backend) {
        case ADC_DIV:     return _read_adc();
        case BQ25896_I2C: return _read_bq25896_pct();
        default:          return -1;
    }
}

bool battery_is_charging() {
    return (_backend == BQ25896_I2C) && _bq25896_usb_present();
}

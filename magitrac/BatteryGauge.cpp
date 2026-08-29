#include "BatteryGauge.h"
#include "Battery.h"
#include <Preferences.h>
#include <esp_system.h>
#include <math.h>

// ── Tunables ─────────────────────────────────────────────────────────────────
//
// Single-cell LiPo.  R_int and the load estimate turn the terminal voltage
// into an open-circuit voltage; tune R_int if the plugged/unplugged readings
// still disagree (raise it if unplug still drops the number).
static const int   RINT_MOHM       = 200;   // pack internal resistance (mΩ)
static const int   ILOAD_MA        = 120;   // avg draw when unplugged (e-paper idle)
static const int   DEFAULT_CHG_MA  = 400;   // assumed charge current if unmeasured
static const int   EMPTY_MV        = 3300;  // OCV empty knee → force 0 %
static const int   LOW_SOC         = 6;     // ≤ this (or ≤ LOW_MV) sets the low breadcrumb
static const int   LOW_MV          = 3450;
static const int   LOW_CLEAR_SOC   = 12;    // hysteresis: clear low above this
static const float SLEW_PER_UPDATE = 3.0f;  // max % the display SoC moves per poll
static const uint32_t PERSIST_MS   = 300000;// flush SoC to NVS every 5 min

static const char* BATT_NS = "magitrac_bat";
static const uint32_t RTC_MAGIC = 0xB0FFEE01;

// Survives soft reset / deep sleep, but is WIPED on true power loss — so a
// missing magic at boot means the battery (or USB) was fully removed.
RTC_DATA_ATTR static uint32_t s_rtcMagic;

// ── OCV → SoC discharge curve (resting single-cell LiPo) ─────────────────────
// Descending; piecewise-linear interpolated.  The flat 3.7–3.9 V middle is why
// a straight-line map reads so badly — this table captures the real knees.
struct OcvPoint { int mv; int soc; };
static const OcvPoint OCV[] = {
    {4200, 100}, {4160, 95}, {4120, 90}, {4080, 85}, {4020, 80}, {3980, 75},
    {3940,  70}, {3900, 65}, {3870, 60}, {3840, 55}, {3820, 50}, {3800, 45},
    {3790,  40}, {3770, 35}, {3750, 30}, {3730, 25}, {3710, 20}, {3690, 15},
    {3660,  10}, {3600,  5}, {3450,  2}, {3300,  0},
};
static const int OCV_N = sizeof(OCV) / sizeof(OCV[0]);

static int ocvToSoc(int mv) {
    if (mv >= OCV[0].mv)        return 100;
    if (mv <= OCV[OCV_N-1].mv)  return 0;
    for (int i = 1; i < OCV_N; i++) {
        if (mv >= OCV[i].mv) {
            const OcvPoint& hi = OCV[i-1];
            const OcvPoint& lo = OCV[i];
            return lo.soc + (mv - lo.mv) * (hi.soc - lo.soc) / (hi.mv - lo.mv);
        }
    }
    return 0;
}

// ── State ────────────────────────────────────────────────────────────────────
static float        s_soc      = -1.0f;   // smoothed SoC (-1 = unseeded)
static bool         s_charging = false;
static int          s_voc      = -1;
static bool         s_lowFlag  = false;
static uint32_t     s_lastPersistMs = 0;
static BatteryDeath s_death = { false, false, 0 };

static void persistSoc() {
    Preferences p;
    p.begin(BATT_NS, false);
    p.putUChar("soc", (uint8_t)lroundf(s_soc));
    p.putUChar("valid", 1);
    p.putUChar("low", s_lowFlag ? 1 : 0);
    p.end();
}

void batteryGaugeBegin() {
    Preferences p;
    p.begin(BATT_NS, true);
    uint8_t prevSoc  = p.getUChar("soc", 0);
    bool    valid    = p.getUChar("valid", 0) != 0;
    bool    prevLow  = p.getUChar("low", 0) != 0;
    p.end();

    esp_reset_reason_t reason = esp_reset_reason();
    bool coldBoot = (s_rtcMagic != RTC_MAGIC);   // RTC RAM wiped → power was lost
    s_rtcMagic = RTC_MAGIC;

    s_death.brownout    = (reason == ESP_RST_BROWNOUT);
    s_death.lastSoc     = prevSoc;
    // Died flat = we were critically low last run, then power vanished (cold
    // boot) or the rail sagged (brownout).  A clean reflash/soft-reset with a
    // healthy battery is not a death.
    s_death.flatBattery = valid && prevLow && (coldBoot || s_death.brownout);

    if (s_death.flatBattery)
        Serial.printf("[BATT] flat-battery shutdown detected — last SoC %u%%, "
                      "reset=%d coldBoot=%d\n", prevSoc, (int)reason, coldBoot);
    else if (s_death.brownout)
        Serial.printf("[BATT] brownout reset (last SoC %u%%)\n", prevSoc);

    // Consume the low breadcrumb so the death is reported exactly once.
    if (prevLow) {
        Preferences w;
        w.begin(BATT_NS, false);
        w.putUChar("low", 0);
        w.end();
    }
    s_lowFlag = false;

    // Seed the smoothed value from the persisted SoC; the first update refines it.
    if (valid) s_soc = prevSoc;
}

void batteryGaugeUpdate(uint32_t nowMs) {
    int mv = battery_read_mv();
    if (mv < 0) return;

    BatteryChargePhase phase = battery_charge_phase();
    s_charging = (phase != BATT_DISCHARGING);

    // Terminal voltage → open-circuit voltage (undo IR offset).
    int voc;
    if (phase == BATT_FAST_CHARGE || phase == BATT_PRECHARGE) {
        int ichg = battery_charge_current_ma();
        if (ichg < 0) ichg = DEFAULT_CHG_MA;
        voc = mv - ichg * RINT_MOHM / 1000;        // charging pushes V up
    } else if (phase == BATT_CHARGE_DONE) {
        voc = mv;                                  // taper ~complete, Ichg ≈ 0
    } else {
        voc = mv + ILOAD_MA * RINT_MOHM / 1000;    // load sags V down
    }
    s_voc = voc;

    int raw = ocvToSoc(voc);
    if (phase == BATT_CHARGE_DONE) raw = 100;      // anchor: charger says full
    if (voc <= EMPTY_MV)           raw = 0;        // anchor: empty knee

    // Smooth — slew-limit so the number glides instead of jumping.
    if (s_soc < 0.0f) {
        s_soc = raw;
    } else {
        float d = raw - s_soc;
        if (d >  SLEW_PER_UPDATE) d =  SLEW_PER_UPDATE;
        if (d < -SLEW_PER_UPDATE) d = -SLEW_PER_UPDATE;
        s_soc += d;
    }
    if (s_soc < 0)   s_soc = 0;
    if (s_soc > 100) s_soc = 100;

    // Low-battery breadcrumb (with hysteresis) — the flat-death evidence.
    bool wasLow = s_lowFlag;
    if (!s_charging && (s_soc <= LOW_SOC || voc <= LOW_MV)) s_lowFlag = true;
    else if (s_soc >= LOW_CLEAR_SOC)                        s_lowFlag = false;

    if (s_lowFlag != wasLow ||
        nowMs - s_lastPersistMs >= PERSIST_MS ||
        s_lastPersistMs == 0) {
        s_lastPersistMs = nowMs;
        persistSoc();
    }
}

int  batteryGaugePct()      { return s_soc < 0 ? -1 : (int)lroundf(s_soc); }
bool batteryGaugeCharging() { return s_charging; }
int  batteryGaugeVoc()      { return s_voc; }
const BatteryDeath& batteryLastDeath() { return s_death; }

#include "crash_log.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

#define CRASH_MAGIC 0xC0DEBEEFu
#define CRASH_RING  8            // keep the last N faults
#define CRASH_NS    "crashlog"

// RTC slow memory — NOT zeroed on a CPU reset (panic / watchdog) and usually
// retained across a brownout reset, but garbage after a true power-on.  Guarded
// by sMagic so we know whether the breadcrumb is trustworthy.
RTC_NOINIT_ATTR static uint32_t sMagic;
RTC_NOINIT_ATTR static uint8_t  sPhaseMain;
RTC_NOINIT_ATTR static uint8_t  sPhaseAudio;
RTC_NOINIT_ATTR static uint32_t sAliveMs;

struct CrashRecord {
    uint32_t bootNum;
    uint32_t aliveMs;
    uint8_t  reason;
    uint8_t  phaseMain;
    uint8_t  phaseAudio;
    uint8_t  _pad;
};

static char sSummary[160];
static bool sLastWasFault = false;

static const char* reasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWER-ON";
        case ESP_RST_EXT:       return "EXT-RESET";
        case ESP_RST_SW:        return "SW-RESET";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT-WDT";
        case ESP_RST_TASK_WDT:  return "TASK-WDT";
        case ESP_RST_WDT:       return "OTHER-WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

static const char* phaseStr(uint8_t p) {
    switch (p) {
        case CP_BOOT:        return "boot";
        case CP_IDLE:        return "idle";
        case CP_SEQ_TICK:    return "seq-tick";
        case CP_SAMPLE_READ: return "sample-read";
        case CP_SONG_PUSH:   return "song-push";
        case CP_AUTOSAVE:    return "autosave";
        case CP_SD_OP:       return "sd-op";
        case CP_MIDI:        return "midi";
        default:             return "?";
    }
}

void crashSetPhase(uint8_t phase)      { sPhaseMain  = phase; }
void crashSetAudioPhase(uint8_t phase) { sPhaseAudio = phase; }
void crashMarkAlive()                  { sAliveMs    = millis(); }

void crashLogInit() {
    esp_reset_reason_t reason = esp_reset_reason();

    bool     rtcValid = (sMagic == CRASH_MAGIC);
    uint8_t  pMain    = rtcValid ? sPhaseMain  : CP_BOOT;
    uint8_t  pAudio   = rtcValid ? sPhaseAudio : CP_BOOT;
    uint32_t alive    = rtcValid ? sAliveMs    : 0;

    sLastWasFault = (reason == ESP_RST_PANIC   || reason == ESP_RST_INT_WDT ||
                     reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT     ||
                     reason == ESP_RST_BROWNOUT);

    Preferences prefs;
    prefs.begin(CRASH_NS, false);
    uint32_t bootNum = prefs.getUInt("boot", 0) + 1;
    prefs.putUInt("boot", bootNum);

    if (sLastWasFault) {
        uint8_t head = prefs.getUChar("head", 0);
        CrashRecord rec = { bootNum, alive, (uint8_t)reason, pMain, pAudio, 0 };
        char key[6];
        snprintf(key, sizeof(key), "r%u", (unsigned)head);
        prefs.putBytes(key, &rec, sizeof(rec));
        prefs.putUChar("head", (uint8_t)((head + 1) % CRASH_RING));
    }
    prefs.end();

    uint32_t aliveMin = alive / 60000;
    uint32_t aliveSec = (alive / 1000) % 60;
    snprintf(sSummary, sizeof(sSummary),
             "last reset: %s  main=%s audio=%s  alive=%lum%02lus  boot#%lu",
             reasonStr(reason), phaseStr(pMain), phaseStr(pAudio),
             (unsigned long)aliveMin, (unsigned long)aliveSec,
             (unsigned long)bootNum);
    Serial.printf("[CRASH] %s\n", sSummary);

    // Re-arm the breadcrumb for this run.
    sMagic      = CRASH_MAGIC;
    sPhaseMain  = CP_BOOT;
    sPhaseAudio = CP_BOOT;
    sAliveMs    = 0;
}

bool        crashLogLastWasFault() { return sLastWasFault; }
const char* crashLogLastSummary()  { return sSummary; }

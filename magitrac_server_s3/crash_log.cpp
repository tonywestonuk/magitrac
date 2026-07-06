#include "crash_log.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include "esp_core_dump.h"

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
RTC_NOINIT_ATTR static uint8_t  sBootStep;

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

// Decoded core-dump backtrace from the LAST panic (read once at boot, then the
// flash image is erased so it's not re-reported).  Empty if no dump was found.
static char sCoreBt[240];
static bool sHasCore = false;

// Read the ELF core dump the IDF panic handler wrote to the `coredump` flash
// partition.  This is the headless backtrace path: USB-serial is dead on the
// first boot after a USB-MSC reset, so the panic's live backtrace is lost —
// but the dump in flash survives and THIS boot (serial alive) can decode it.
// Decode the printed PCs with:
//   xtensa-esp32s3-elf-addr2line -pfiaC -e <sketch>.ino.elf <pc> <pc> ...
static void crashReadCoreDump() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (esp_core_dump_image_check() != ESP_OK) return;   // no valid dump in flash

    esp_core_dump_summary_t* s =
        (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (!s) return;

    if (esp_core_dump_get_summary(s) == ESP_OK) {
        sHasCore = true;
        Serial.printf("[COREDUMP] task='%s' pc=0x%08x cause=%u vaddr=0x%08x "
                      "depth=%u%s\n",
                      s->exc_task, (unsigned)s->exc_pc,
                      (unsigned)s->ex_info.exc_cause,
                      (unsigned)s->ex_info.exc_vaddr,
                      (unsigned)s->exc_bt_info.depth,
                      s->exc_bt_info.corrupted ? " (BT CORRUPT)" : "");

        int n = snprintf(sCoreBt, sizeof(sCoreBt),
                         "%s pc=%08x cz=%u va=%08x bt:",
                         s->exc_task, (unsigned)s->exc_pc,
                         (unsigned)s->ex_info.exc_cause,
                         (unsigned)s->ex_info.exc_vaddr);
        for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16; i++) {
            uint32_t pc = s->exc_bt_info.bt[i];
            Serial.printf("[COREDUMP]   #%u  0x%08x\n", (unsigned)i, (unsigned)pc);
            if (n > 0 && n < (int)sizeof(sCoreBt) - 10)
                n += snprintf(sCoreBt + n, sizeof(sCoreBt) - n, " %08x",
                              (unsigned)pc);
        }
        Serial.print("[COREDUMP] decode: xtensa-esp32s3-elf-addr2line -pfiaC -e "
                     "<sketch>.ino.elf");
        for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16; i++)
            Serial.printf(" 0x%08x", (unsigned)s->exc_bt_info.bt[i]);
        Serial.println();
    }
    free(s);

    // One-shot: wipe so the next clean boot doesn't re-report a stale crash.
    esp_core_dump_image_erase();
#endif
}

bool        crashLogHasCoreDump()  { return sHasCore; }
const char* crashLogCoreSummary()  { return sCoreBt; }

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

static const char* bootStepStr(uint8_t s) {
    switch (s) {
        case BS_START:     return "start";
        case BS_AUDIO:     return "audio";
        case BS_MIDI:      return "midi";
        case BS_SAM2695:   return "sam2695";
        case BS_SPI:       return "spi";
        case BS_WIFI:      return "wifi";
        case BS_NET:       return "net";
        case BS_PIXELPOST: return "pixelpost";
        case BS_PAIRING:   return "pairing";
        case BS_COMMANDS:  return "commands(SD)";
        case BS_SAMPLE:    return "sample";
        case BS_SPECTRUM:  return "spectrum";
        case BS_SONGLIST:  return "songlist";
        case BS_TASKS:     return "tasks";
        case BS_UI:        return "ui";
        case BS_DONE:      return "done";
        default:           return "?";
    }
}

void crashSetPhase(uint8_t phase)      { sPhaseMain  = phase; }
void crashSetAudioPhase(uint8_t phase) { sPhaseAudio = phase; }
void crashSetBootStep(uint8_t step)    { sBootStep   = step;  }
void crashMarkAlive()                  { sAliveMs    = millis(); }

void crashLogInit() {
    esp_reset_reason_t reason = esp_reset_reason();

    bool     rtcValid = (sMagic == CRASH_MAGIC);
    uint8_t  pMain    = rtcValid ? sPhaseMain  : CP_BOOT;
    uint8_t  pAudio   = rtcValid ? sPhaseAudio : CP_BOOT;
    uint32_t alive    = rtcValid ? sAliveMs    : 0;
    uint8_t  bStep    = rtcValid ? sBootStep   : BS_DONE;

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
             "last reset: %s  step=%s main=%s audio=%s  alive=%lum%02lus  boot#%lu",
             reasonStr(reason), bootStepStr(bStep), phaseStr(pMain), phaseStr(pAudio),
             (unsigned long)aliveMin, (unsigned long)aliveSec,
             (unsigned long)bootNum);
    Serial.printf("[CRASH] %s\n", sSummary);

    // Decode any ELF core dump the panic handler left in flash (full backtrace,
    // survives the dead-USB-serial first boot after a USB-MSC reset).
    crashReadCoreDump();

    // Re-arm the breadcrumb for this run.
    sMagic      = CRASH_MAGIC;
    sPhaseMain  = CP_BOOT;
    sPhaseAudio = CP_BOOT;
    sAliveMs    = 0;
    sBootStep   = BS_START;
}

bool        crashLogLastWasFault() { return sLastWasFault; }
const char* crashLogLastSummary()  { return sSummary; }

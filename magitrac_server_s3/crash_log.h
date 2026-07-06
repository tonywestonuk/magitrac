#pragma once
#include <stdint.h>

// Persistent crash diagnostics for the headless gig case (no serial monitor
// attached).  On boot we read esp_reset_reason() + a breadcrumb stashed in
// RTC memory (survives panic / watchdog / brownout resets), append a record to
// NVS (ring of the last few faults), and expose a one-line summary that the
// sketch paints on the LCD so the cause is readable on-device after a reboot.
//
// Two breadcrumbs because the suspects run on different cores concurrently:
//   - "main"  : main loop + MagiLink/command handlers
//   - "audio" : the WAV task on Core 0 (sample read + codec)
// A single breadcrumb would be ambiguous about which context died.

enum CrashPhase : uint8_t {
    CP_BOOT = 0,
    CP_IDLE,
    CP_SEQ_TICK,
    CP_SAMPLE_READ,   // WAV task: sample playback active (SD read + resample + codec)
    CP_SONG_PUSH,     // receiving a full song into srvActiveBuf
    CP_AUTOSAVE,      // writing the active song to SD
    CP_SD_OP,         // other SD access (load / list / backup)
    CP_MIDI,
    CP_COUNT
};

// Boot-step breadcrumb — finer-grained than CrashPhase, scoped to setup().
// Set before each init call so that if a boot panics (common after the
// USB-MSC reboot, when the USB-CDC console is dead and the backtrace is lost),
// the NEXT boot can name the exact init that was running.  Kept in its own RTC
// byte so it never fights the main/audio runtime phases.
enum CrashBootStep : uint8_t {
    BS_START = 0,     // re-armed value: panicked before the first marker below
    BS_AUDIO,         // audioCodecInit (ES8388 over I²C)
    BS_MIDI,          // sequencerMidiBegin (UART1 FIFO)
    BS_SAM2695,       // sam2695MuteAllExcept10
    BS_SPI,           // sdMutexInit + SPI.begin
    BS_WIFI,          // WiFi mode/softAP/softAPConfig/protocol
    BS_NET,           // UDP sender + MagiLink accept + ESP-NOW begin
    BS_PIXELPOST,     // pixelpostInit
    BS_PAIRING,       // pairingInit
    BS_COMMANDS,      // commandsInit (SD mount + recovery)
    BS_SAMPLE,        // samplePlayerInit + manifest sync
    BS_SPECTRUM,      // spectrumInit
    BS_SONGLIST,      // loadSongList
    BS_TASKS,         // xTaskCreate (MIDI / link session)
    BS_UI,            // fieldFlashScreen + drawScreen + loadSong
    BS_DONE,          // reached the main loop
    BS_STEP_COUNT
};

// Call once, early in setup(), after Serial is up.  Reads the reset reason +
// breadcrumb, logs to NVS + Serial, then re-arms the breadcrumb for this run.
void crashLogInit();

// Boot-step breadcrumb setter — one byte to RTC RAM, call before each init.
void crashSetBootStep(uint8_t step);

// Breadcrumbs — set the current phase so a fault leaves a trail.  Cheap
// (one byte to RTC RAM); safe to call from any task/core.
void crashSetPhase(uint8_t phase);        // main-context phase
void crashSetAudioPhase(uint8_t phase);   // WAV-task phase

// Heartbeat — call from the main loop so the stored uptime approximates how
// long the device ran before it died.
void crashMarkAlive();

// True if the *last* reset was a fault (panic / watchdog / brownout) rather
// than a clean power-on / external reset / reflash.  Used to decide whether to
// hold the on-screen banner at boot.
bool crashLogLastWasFault();

// One-line human-readable summary of the last reset, e.g.
//   "last reset: BROWNOUT  main=idle audio=sample-read  alive=41m  boot#7"
const char* crashLogLastSummary();

// Core-dump backtrace from the last panic (decoded from the `coredump` flash
// partition at boot, then erased).  crashLogHasCoreDump() is true when a dump
// was found; crashLogCoreSummary() is a compact "task pc=… cz=… va=… bt: …"
// string of PCs.  Decode the PCs with xtensa-esp32s3-elf-addr2line against the
// matching .ino.elf.  Full per-frame backtrace is also printed to Serial.
bool        crashLogHasCoreDump();
const char* crashLogCoreSummary();

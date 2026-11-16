#pragma once
#include <Arduino.h>

// ── Sample-load diagnostics ───────────────────────────────────────────────────
// Timing lines on the serial port, tagged [SMPDBG], on both the client and the
// server: when a pad is pressed, when each sample message arrives, how long the
// server's waveform scan and SD reads take, when the editor draws.  Each line
// starts with the device's millis() so the two logs can be read side by side.
//
// Added in Nov 2026 to find out why a long sample took 7–9 s to open in the
// sample editor (answer: the SD card was mounted at the library's default
// 4 MHz — see SRV_SD_HZ in the server's commands_server.ino).  OFF by default:
// one of the lines is on the audio task, at the start of every streamed
// sample.  Set SAMPLE_DEBUG to 1 on both devices to get them back.
#define SAMPLE_DEBUG 0

#if SAMPLE_DEBUG
#define SMPDBG(fmt, ...) \
    Serial.printf("[SMPDBG] t=%lu " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)
#else
// Never runs, but still type-checks the arguments and counts as a use of them.
#define SMPDBG(fmt, ...) \
    do { if (0) Serial.printf(fmt "\n", ##__VA_ARGS__); } while (0)
#endif

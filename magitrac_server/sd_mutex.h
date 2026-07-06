// sd_mutex.h — global lock around the shared SD/SPI bus.
//
// SD on ESP32 Arduino is NOT thread-safe.  We have three tasks that may
// touch SD simultaneously:
//   • TCP reader task (commands_server.ino: load/save/restore handlers)
//   • Main loop / commandsTick (deferred SD work)
//   • MIDI task (SamplePlayer streams sample bytes during playback)
//
// Without serialisation, two concurrent SD.open/read/write calls corrupt
// the VFS internals and crash with a LoadProhibited inside esp_vfs_stat.
// This mutex serialises every SD operation so callers can ignore the
// concurrency concern.
//
// Usage:  { SdLock _; SD.open(...); }  — RAII releases on scope exit.

#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t sSdMutex;

// Initialise the mutex.  Call once from setup() before any task that
// touches SD is spawned.
void sdMutexInit();

class SdLock {
    bool _taken = false;
public:
    SdLock();
    ~SdLock();
    SdLock(const SdLock&)            = delete;
    SdLock& operator=(const SdLock&) = delete;
};

#pragma once

#include <stdarg.h>

// Async debug logging — formats into a buffer and enqueues for a low-priority
// background task to print.  xQueueSend is ~microseconds vs ~milliseconds for
// Serial.printf, so the calling thread is barely blocked.

void debugLogInit();   // call once in setup() — creates queue + task
void debugPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

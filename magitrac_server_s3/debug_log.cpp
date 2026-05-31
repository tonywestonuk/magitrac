#include "debug_log.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>

static const size_t   kMsgLen   = 128;   // max chars per message (truncated if longer)
static const uint16_t kQueueLen = 64;    // number of messages buffered

static QueueHandle_t sQueue = nullptr;

static void debugLogTask(void*) {
    char buf[kMsgLen];
    for (;;) {
        if (xQueueReceive(sQueue, buf, portMAX_DELAY) == pdTRUE) {
            Serial.print(buf);
        }
    }
}

void debugLogInit() {
    sQueue = xQueueCreate(kQueueLen, kMsgLen);
    xTaskCreatePinnedToCore(debugLogTask, "dbgLog", 2048, nullptr,
                            1,        // priority 1 — lowest practical
                            nullptr,
                            0);       // core 0 — keep off the MIDI core
}

void debugPrintf(const char* fmt, ...) {
    if (!sQueue) return;
    char buf[kMsgLen];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, kMsgLen, fmt, ap);
    va_end(ap);
    // Non-blocking — drop message if queue is full
    xQueueSend(sQueue, buf, 0);
}

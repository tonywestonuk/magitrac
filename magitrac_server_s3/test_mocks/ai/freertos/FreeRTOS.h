#pragma once
// Host stub for test_claude_ai.cpp only: the AI "task" runs synchronously
// inside xTaskCreatePinnedToCore, so a test sees the finished request as soon
// as aiBegin() returns.
#include <stdint.h>
typedef void*    SemaphoreHandle_t;
typedef void*    TaskHandle_t;
typedef int      BaseType_t;
#define pdPASS 1
static inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char*, uint32_t,
                                                 void* arg, int, TaskHandle_t*, int) {
    fn(arg);
    return pdPASS;
}
static inline void vTaskDelete(TaskHandle_t) {}

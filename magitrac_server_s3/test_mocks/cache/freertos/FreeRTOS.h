#pragma once
// Host stub of the little FreeRTOS that SamplePlayer.cpp uses — for
// test_sample_cache.cpp only (kept out of test_mocks/ proper so it can't
// shadow anything for the other host builds).
#include <stdint.h>
typedef void*    SemaphoreHandle_t;
typedef void*    TaskHandle_t;
typedef uint32_t TickType_t;
typedef int      BaseType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (ms)

// The critical section: the test checks it is never left held and never nested.
struct portMUX_TYPE { int held; };
#define portMUX_INITIALIZER_UNLOCKED { 0 }
extern int gMockCriticalDepth, gMockCriticalErrors;
static inline void portENTER_CRITICAL(portMUX_TYPE* m) { if (m->held || gMockCriticalDepth) gMockCriticalErrors++; m->held = 1; gMockCriticalDepth++; }
static inline void portEXIT_CRITICAL(portMUX_TYPE* m)  { if (!m->held) gMockCriticalErrors++; m->held = 0; gMockCriticalDepth--; }

static inline void vTaskDelay(TickType_t) {}
static inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*, int, TaskHandle_t*, int) { return 1; }

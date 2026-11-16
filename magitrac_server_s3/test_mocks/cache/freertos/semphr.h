#pragma once
#include "FreeRTOS.h"
extern int gMockSemGives;                       // "the WAV task was woken"
static inline SemaphoreHandle_t xSemaphoreCreateBinary() { return (SemaphoreHandle_t)1; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { gMockSemGives++; return 1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return 1; }

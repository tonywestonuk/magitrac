#pragma once
// Host stub: PSRAM is just the heap.
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1 << 10)
static inline void* heap_caps_calloc(size_t n, size_t size, uint32_t) { return calloc(n, size); }

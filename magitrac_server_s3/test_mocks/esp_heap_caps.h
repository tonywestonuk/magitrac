#pragma once
// Host-test mock: PSRAM-capable allocs degrade to plain malloc.
#include <stdlib.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM  (1 << 10)
#define MALLOC_CAP_8BIT    (1 << 2)
#define MALLOC_CAP_32BIT   (1 << 1)

static inline void* heap_caps_malloc(size_t size, uint32_t /*caps*/) {
    return malloc(size);
}
static inline void* heap_caps_aligned_alloc(size_t align, size_t size, uint32_t /*caps*/) {
    void* p = nullptr;
    if (posix_memalign(&p, align, size) != 0) return nullptr;
    return p;
}

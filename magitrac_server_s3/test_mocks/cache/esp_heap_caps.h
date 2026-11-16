#pragma once
// Host stub with bookkeeping: the cache test checks every buffer is freed and
// how much "PSRAM" is held.  Blocks carry their size in a header.
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define MALLOC_CAP_SPIRAM  (1 << 10)
#define MALLOC_CAP_8BIT    (1 << 2)
extern long gMockHeapBytes, gMockHeapBlocks;
extern bool gMockHeapFail;                      // make the next allocations fail
static inline void* mockAlloc(size_t size, bool zero) {
    if (gMockHeapFail) return nullptr;
    size_t* p = (size_t*)malloc(size + 16);
    if (!p) return nullptr;
    p[0] = size; gMockHeapBytes += (long)size; gMockHeapBlocks++;
    void* u = (char*)p + 16;
    if (zero) memset(u, 0, size);
    return u;
}
static inline void mockFree(void* u) {
    if (!u) return;
    size_t* p = (size_t*)((char*)u - 16);
    gMockHeapBytes -= (long)p[0]; gMockHeapBlocks--;
    free(p);
}
static inline void* heap_caps_malloc(size_t size, uint32_t) { return mockAlloc(size, false); }
static inline void* heap_caps_calloc(size_t n, size_t size, uint32_t) { return mockAlloc(n * size, true); }
// SamplePlayer.cpp frees with plain free(): route it (and calloc) through the bookkeeping.
#define free(p)       mockFree(p)
#define calloc(n, s)  mockAlloc((n) * (s), true)

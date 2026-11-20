#pragma once
#include <stdlib.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT     (1 << 2)
extern size_t gMockInternalFree;
static inline void*  heap_caps_malloc(size_t n, uint32_t)            { return malloc(n); }
static inline void*  heap_caps_realloc(void* p, size_t n, uint32_t)  { return realloc(p, n); }
static inline void   heap_caps_free(void* p)                         { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t)               { return gMockInternalFree; }
static inline size_t heap_caps_get_largest_free_block(uint32_t)      { return gMockInternalFree / 2; }
static inline void*  heap_caps_calloc_prefer(size_t n, size_t size, size_t, ...) { return calloc(n, size); }
static inline void*  heap_caps_calloc(size_t n, size_t size, uint32_t)  { return calloc(n, size); }

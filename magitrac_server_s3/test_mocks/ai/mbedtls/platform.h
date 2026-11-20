#pragma once
#include <stddef.h>
extern int gMockTlsAllocSwaps;
static inline int mbedtls_platform_set_calloc_free(void* (*)(size_t, size_t), void (*)(void*)) {
    gMockTlsAllocSwaps++;
    return 0;
}

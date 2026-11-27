/* Host-only allocator shim: no PSRAM on the host, so malloc stands in. */
#ifndef VIDEO_GATE_P4HEAP_H
#define VIDEO_GATE_P4HEAP_H
#include <stdlib.h>
static inline void *p4heap_alloc_psram(size_t n) { return malloc(n); }
static inline void *p4heap_realloc_psram(void *p, size_t n) { return realloc(p, n); }
#endif

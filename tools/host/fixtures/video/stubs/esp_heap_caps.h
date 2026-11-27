/* Host-only allocator shim: the gate maps the PSRAM allocator to malloc. */
#ifndef VIDEO_GATE_ESP_HEAP_CAPS_H
#define VIDEO_GATE_ESP_HEAP_CAPS_H
#include <stdlib.h>
static inline void host_video_free(void *p) { free(p); }
#define heap_caps_free(p) host_video_free(p)
#endif

/* Host-only strlcpy declaration for the real EPUB nav parser (glibc shim). */
#ifndef VIDEO_GATE_STRLCPY_H
#define VIDEO_GATE_STRLCPY_H
#include <stddef.h>
size_t strlcpy(char *dst, const char *src, size_t size);
#endif

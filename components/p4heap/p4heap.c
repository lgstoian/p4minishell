/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file p4heap.c
 * @brief Central RAM/PSRAM allocation policy (see `p4heap.h`).
 */

#include "p4heap.h"

#include <stdlib.h>
#include <string.h>

void *p4heap_alloc_psram(size_t size)
{
    if (size == 0) {
        size = 1;
    }
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *p4heap_calloc_psram(size_t n, size_t size)
{
    if (n == 0 || size == 0) {
        n = (n == 0) ? 1 : n;
        size = (size == 0) ? 1 : size;
    }
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *p4heap_alloc_any(size_t size)
{
    void *p;

    if (size == 0) {
        size = 1;
    }
    p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        /* Plain malloc is PSRAM-first under
         * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0; this only helps when the
         * SPIRAM-explicit request raced another task. */
        p = malloc(size);
    }
    return p;
}

void *p4heap_calloc_any(size_t n, size_t size)
{
    void *p;

    if (n == 0 || size == 0) {
        n = (n == 0) ? 1 : n;
        size = (size == 0) ? 1 : size;
    }
    p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = calloc(n, size);
    }
    return p;
}

void *p4heap_alloc_dma(size_t size)
{
    if (size == 0) {
        size = 1;
    }
    return heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

void *p4heap_calloc_dma(size_t n, size_t size)
{
    if (n == 0 || size == 0) {
        n = (n == 0) ? 1 : n;
        size = (size == 0) ? 1 : size;
    }
    return heap_caps_calloc(n, size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

void *p4heap_alloc_dma_aligned(size_t align, size_t size)
{
    if (size == 0) {
        size = 1;
    }
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    return heap_caps_aligned_alloc(align, size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

void *p4heap_realloc_psram(void *ptr, size_t size)
{
    if (size == 0) {
        size = 1;
    }
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *p4heap_realloc_any(void *ptr, size_t size)
{
    void *out;

    if (size == 0) {
        size = 1;
    }
    out = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL && ptr != NULL) {
        /* heap_caps_realloc leaves @p ptr valid on failure; fall back to a
         * plain realloc (PSRAM-first under the project malloc policy). */
        out = realloc(ptr, size);
    } else if (out == NULL) {
        out = realloc(NULL, size);
    }
    return out;
}

char *p4heap_strdup_psram(const char *s)
{
    size_t len;
    char *out;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s) + 1;
    out = p4heap_alloc_psram(len);
    if (out != NULL) {
        memcpy(out, s, len);
    }
    return out;
}

void p4heap_free(void *ptr)
{
    heap_caps_free(ptr);
}

size_t p4heap_dma_free(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_DMA);
}

size_t p4heap_dma_largest(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
}

bool p4heap_dma_ready(size_t need)
{
    return p4heap_dma_largest() >= need;
}

size_t p4heap_internal_largest(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

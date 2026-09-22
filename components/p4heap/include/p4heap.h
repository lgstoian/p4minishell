/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_heap_caps.h"

/**
 * @file p4heap.h
 * @brief Central RAM/PSRAM allocation policy for P4MiniShell.
 *
 * One policy, enforced everywhere (see bugs.md F23):
 *
 * - Bulk data (files, documents, buffers, stacks) goes to PSRAM first
 *   (`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`). Large allocations must NEVER
 *   silently spill into the internal DMA-capable heap: on the M5Stack Tab5
 *   that pool is small and fragments over a busy session, and once it is
 *   gone hardware consumers (esp-aes GDMA descriptors, SDMMC bounce) fail
 *   even though `mem.internal` still reports free bytes (non-DMA internal
 *   RAM is included in that figure and is not usable for DMA).
 * - DMA consumers (AES chunks, SDMMC scratch, SD bounce buffers) request
 *   `MALLOC_CAP_DMA | MALLOC_CAP_8BIT` explicitly and handle NULL — they
 *   must never be handed a PSRAM pointer (PSRAM is not DMA-capable here:
 *   this P4 build has `dma_spi=0`, so a PSRAM buffer forces an internal
 *   bounce buffer plus a descriptor array and makes pressure worse).
 * - Small control blocks that historically used plain `malloc()` keep doing
 *   so through `p4heap_alloc_any()`; with
 *   `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` plain malloc is PSRAM-first
 *   anyway, with the 32 KB `RESERVE_INTERNAL` pool kept for DMA/internal
 *   needs.
 *
 * All `p4heap_free()` calls route to `heap_caps_free()` (NULL-safe).
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Bulk allocation from PSRAM. Returns NULL on failure (never spills to
 *  internal RAM, so a large request cannot starve the DMA pool). */
void *p4heap_alloc_psram(size_t size);

/** Zeroed bulk allocation from PSRAM. NULL on failure, no internal spill. */
void *p4heap_calloc_psram(size_t n, size_t size);

/** General small allocation: PSRAM first, plain-malloc fallback (which is
 *  itself PSRAM-first under the project malloc policy). For control blocks
 *  and short strings, not for bulk or DMA. */
void *p4heap_alloc_any(size_t size);

/** Zeroed general small allocation (see `p4heap_alloc_any()`). */
void *p4heap_calloc_any(size_t n, size_t size);

/** DMA-capable allocation (`MALLOC_CAP_DMA | MALLOC_CAP_8BIT`). Returns NULL
 *  on failure — callers must fall back (software path, PSRAM staging, or a
 *  clear error), never silently use PSRAM for DMA. */
void *p4heap_alloc_dma(size_t size);

/** Zeroed DMA-capable allocation. NULL on failure. */
void *p4heap_calloc_dma(size_t n, size_t size);

/** DMA-capable allocation with @p align byte alignment (power of two).
 *  Crypto/DMA fast paths want cache-line alignment so the peripheral needs
 *  no internal alignment bounce buffer (each bounce buffer is another
 *  internal allocation that can fail on a fragmented Tab5 heap). */
void *p4heap_alloc_dma_aligned(size_t align, size_t size);

/** Grow/shrink a PSRAM block (NULL on failure, old block untouched). */
void *p4heap_realloc_psram(void *ptr, size_t size);

/** Grow/shrink a general block (NULL on failure, old block untouched). */
void *p4heap_realloc_any(void *ptr, size_t size);

/** Duplicate a string into PSRAM. NULL on failure (or if @p s is NULL). */
char *p4heap_strdup_psram(const char *s);

/** Free any `p4heap_*` (or `heap_caps_*`/`malloc()`) block. NULL-safe. */
void p4heap_free(void *ptr);

/** Free DMA pool bytes currently available. */
size_t p4heap_dma_free(void);

/** Largest single DMA-capable block available. This is the F23 health
 *  metric: hardware descriptor arrays need one contiguous DMA block. */
size_t p4heap_dma_largest(void);

/** True when the DMA pool can currently satisfy a @p need-byte contiguous
 *  request (largest free DMA block >= @p need). */
bool p4heap_dma_ready(size_t need);

/** Largest single internal-RAM block available (includes non-DMA RAM;
 *  compare with `p4heap_dma_largest()` for the DMA-usable figure). */
size_t p4heap_internal_largest(void);

#ifdef __cplusplus
}
#endif

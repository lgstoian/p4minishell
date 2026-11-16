/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file dom_arena.h
 * @brief Bump arena for DOM nodes (components/markdown).
 *
 * A parsed page builds hundreds of `dom_node_t` records (~3.6 KB each). Calling
 * `calloc` for every one of them churns the heap and fragments it; freeing the
 * tree then walks every node again. The arena hands out nodes from a handful of
 * large PSRAM blocks instead, so a page costs a few allocations and one free.
 *
 * The arena is bounded: once `P4_CONFIG_DOM_ARENA_LIMIT_BYTES` is reached,
 * allocations fail and the caller falls back to plain `calloc` for that node
 * (marked as not arena-owned), so a pathological page still renders - it just
 * pays per-node allocation for the surplus instead of failing.
 */
#ifndef P4MINISHELL_DOM_ARENA_H
#define P4MINISHELL_DOM_ARENA_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dom_arena dom_arena_t;

/** Create an empty arena. @return NULL on OOM. */
dom_arena_t *dom_arena_create(void);

/** Release every block. NULL-safe. */
void dom_arena_destroy(dom_arena_t *arena);

/** Bump-allocate @p size bytes (8-byte aligned). @return NULL when the arena is
 *  exhausted or the request is too large; the caller then falls back to calloc. */
void *dom_arena_alloc(dom_arena_t *arena, size_t size);

/** Bytes handed out so far. */
size_t dom_arena_used(const dom_arena_t *arena);

/** Largest single request the arena will consider (bigger ones go to calloc). */
size_t dom_arena_max_request(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_DOM_ARENA_H */

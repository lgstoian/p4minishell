/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file dom_arena.c
 * @brief Bounded bump arena for DOM nodes (see dom_arena.h).
 *
 * Blocks live in PSRAM because a full page's DOM is hundreds of kilobytes and
 * internal DRAM is the scarce resource on both boards (see ai-context.md).
 */

#include "dom_arena.h"

#include <string.h>

#include "p4heap.h"
#include "p4minishell_config.h"

typedef struct dom_arena_block {
    struct dom_arena_block *next;
    size_t used;
    size_t capacity;
    uint8_t data[];
} dom_arena_block_t;

/** First 8-byte-aligned address inside a block's payload. p4heap returns an
 *  aligned block, but `data` starts after the three header words (12 bytes),
 *  so the payload itself can be off by four; align the payload base here so
 *  `dom_arena_alloc`'s "8-byte aligned" contract is actually true. */
static uint8_t *dom_arena_block_data(dom_arena_block_t *block)
{
    uintptr_t base = (uintptr_t)block->data;

    return (uint8_t *)((base + 7u) & ~(uintptr_t)7u);
}

struct dom_arena {
    dom_arena_block_t *head;
    size_t total;
};

size_t dom_arena_max_request(void)
{
    /* Node-sized pieces only; anything larger goes straight to the heap. */
    return P4_CONFIG_DOM_ARENA_BLOCK_BYTES / 2u;
}

dom_arena_t *dom_arena_create(void)
{
    dom_arena_t *arena = malloc(sizeof(*arena));

    if (arena == NULL) {
        return NULL;
    }
    arena->head = NULL;
    arena->total = 0;
    return arena;
}

void dom_arena_destroy(dom_arena_t *arena)
{
    dom_arena_block_t *block;

    if (arena == NULL) {
        return;
    }
    block = arena->head;
    while (block != NULL) {
        dom_arena_block_t *next = block->next;

        p4heap_free(block);
        block = next;
    }
    free(arena);
}

void *dom_arena_alloc(dom_arena_t *arena, size_t size)
{
    dom_arena_block_t *block;
    size_t offset;

    if (arena == NULL || size == 0 || size > dom_arena_max_request()) {
        return NULL;
    }
    if (arena->total >= P4_CONFIG_DOM_ARENA_LIMIT_BYTES ||
        arena->total + P4_CONFIG_DOM_ARENA_BLOCK_BYTES > P4_CONFIG_DOM_ARENA_LIMIT_BYTES) {
        return NULL;                       /* bounded: let the caller fall back */
    }

    block = arena->head;
    if (block == NULL || block->used + size > block->capacity) {
        uint8_t *data;
        size_t pad;

        block = p4heap_alloc_psram(sizeof(*block) + P4_CONFIG_DOM_ARENA_BLOCK_BYTES);
        if (block == NULL) {
            return NULL;
        }
        memset(block, 0, sizeof(*block));
        data = dom_arena_block_data(block);
        pad = (size_t)(data - block->data);
        block->capacity = P4_CONFIG_DOM_ARENA_BLOCK_BYTES > pad
                              ? P4_CONFIG_DOM_ARENA_BLOCK_BYTES - pad
                              : 0;
        block->next = arena->head;
        arena->head = block;
        arena->total += P4_CONFIG_DOM_ARENA_BLOCK_BYTES;
    }

    offset = (block->used + 7u) & ~(size_t)7u;
    if (offset + size > block->capacity) {
        return NULL;
    }
    block->used = offset + size;
    return dom_arena_block_data(block) + offset;
}

size_t dom_arena_used(const dom_arena_t *arena)
{
    size_t used = 0;
    const dom_arena_block_t *block;

    if (arena == NULL) {
        return 0;
    }
    for (block = arena->head; block != NULL; block = block->next) {
        used += block->used;
    }
    return used;
}

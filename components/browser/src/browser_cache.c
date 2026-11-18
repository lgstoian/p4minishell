/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file browser_cache.c
 * @brief Bounded PSRAM LRU caches for decoded images and rendered pages.
 *
 * See browser.h for the rationale and the budget. Both caches are
 * fixed-capacity arrays with a monotonically increasing tick as the LRU
 * timestamp: no per-access heap work, no fragmentation, and eviction is a
 * linear scan of a few hundred entries at worst.
 */

#include "browser.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "p4heap.h"
#include "p4minishell_config.h"
#include "tui.h"

#define BROWSER_CACHE_TAG           "browser"

typedef struct {
    char url[P4_CONFIG_BROWSER_URL_BYTES];
    int width;
    int height;
    uint8_t *pixels;            /**< PSRAM, width*height bytes */
    uint32_t last_used;
} browser_image_entry_t;

typedef struct {
    char url[P4_CONFIG_BROWSER_URL_BYTES];
    void *shot;                 /**< tui_page_shot_t, PSRAM */
    size_t bytes;
    uint32_t last_used;
} browser_page_entry_t;

/* The entry tables themselves are bulk data too (IMAGE_CACHE_MAX x ~532 bytes
 * for the URL alone would be ~136 KB of .bss), so they are PSRAM-allocated on
 * first use like every other large table in this project. A failed allocation
 * disables that cache rather than faulting. */
static browser_image_entry_t *s_images;
static int s_image_count;
static size_t s_image_bytes;

static browser_page_entry_t *s_pages;
static int s_page_count;
static size_t s_page_bytes;

static SemaphoreHandle_t s_lock;
static uint32_t s_clock;

void browser_cache_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_images == NULL) {
        s_images = p4heap_calloc_psram(P4_CONFIG_BROWSER_IMAGE_CACHE_MAX,
                                       sizeof(*s_images));
    }
    if (s_pages == NULL) {
        s_pages = p4heap_calloc_psram(P4_CONFIG_BROWSER_PAGE_CACHE_MAX,
                                      sizeof(*s_pages));
    }
}

static void cache_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void cache_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static uint32_t cache_tick(void)
{
    return ++s_clock;
}

/* ========================================================================
 * IMAGE CACHE
 * ======================================================================== */

bool browser_image_cache_get(const char *url, dom_image_t *out)
{
    browser_image_entry_t *hit = NULL;
    uint8_t *copy;
    size_t bytes;

    if (url == NULL || url[0] == '\0' || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (s_images == NULL) {
        return false;
    }

    cache_lock();
    for (int i = 0; i < s_image_count; i++) {
        if (strcmp(s_images[i].url, url) == 0) {
            hit = &s_images[i];
            break;
        }
    }
    if (hit == NULL) {
        cache_unlock();
        return false;
    }
    bytes = (size_t)hit->width * (size_t)hit->height;
    if (hit->pixels == NULL || bytes == 0) {
        cache_unlock();
        return false;
    }
    copy = malloc(bytes);
    if (copy == NULL) {
        cache_unlock();
        return false;
    }
    memcpy(copy, hit->pixels, bytes);
    hit->last_used = cache_tick();
    out->width = hit->width;
    out->height = hit->height;
    out->pixels = copy;
    cache_unlock();
    return true;
}

bool browser_image_cache_put(const char *url, const dom_image_t *img)
{
    browser_image_entry_t *slot = NULL;
    browser_image_entry_t *victim = NULL;
    uint8_t *copy;
    size_t bytes;
    bool fresh = false;

    if (url == NULL || url[0] == '\0' || img == NULL ||
        img->pixels == NULL || img->width <= 0 || img->height <= 0) {
        return false;
    }
    if (strlen(url) >= P4_CONFIG_BROWSER_URL_BYTES) {
        return false;
    }
    bytes = (size_t)img->width * (size_t)img->height;
    if (s_images == NULL) {
        return false;
    }

    cache_lock();
    for (int i = 0; i < s_image_count; i++) {
        if (strcmp(s_images[i].url, url) == 0) {
            slot = &s_images[i];
            break;
        }
    }
    if (slot == NULL) {
        if (s_image_count < P4_CONFIG_BROWSER_IMAGE_CACHE_MAX) {
            slot = &s_images[s_image_count];
            fresh = true;
        } else {
            /* Evict the least recently used entry. */
            for (int i = 0; i < s_image_count; i++) {
                if (victim == NULL || s_images[i].last_used < victim->last_used) {
                    victim = &s_images[i];
                }
            }
            slot = victim;
        }
    }
    if (slot == NULL) {
        cache_unlock();
        return false;
    }

    copy = p4heap_alloc_psram(bytes);
    if (copy == NULL) {
        /* Nothing is committed: a failed store must not consume a slot. */
        cache_unlock();
        return false;
    }

    if (slot->pixels != NULL) {
        s_image_bytes -= (size_t)slot->width * (size_t)slot->height;
        p4heap_free(slot->pixels);
    }
    if (fresh) {
        s_image_count++;
    }
    memcpy(copy, img->pixels, bytes);
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    slot->width = img->width;
    slot->height = img->height;
    slot->pixels = copy;
    slot->last_used = cache_tick();
    s_image_bytes += bytes;
    cache_unlock();
    return true;
}

/* ---- negative cache: URLs whose decode failed ---------------------------- */

#define BROWSER_IMAGE_BAD_MAX 48

typedef struct {
    char url[P4_CONFIG_BROWSER_URL_BYTES];
} browser_bad_entry_t;

/* 48 x 512 bytes of URL text is 24 KB of DIRAM, which the size-budget gate
 * charges to the firmware on every boot even on pages with no broken image.
 * It follows the other cache tables into PSRAM; a failed allocation simply
 * disables the negative cache (one refetch per reference, not a fault). */
static browser_bad_entry_t *s_bad;
static int s_bad_count;

/** The bad-URL table, allocated on first use. Caller holds the cache lock. */
static browser_bad_entry_t *bad_table_locked(void)
{
    if (s_bad == NULL) {
        s_bad = p4heap_calloc_psram(BROWSER_IMAGE_BAD_MAX, sizeof(*s_bad));
    }
    return s_bad;
}

void browser_image_cache_mark_bad(const char *url)
{
    browser_bad_entry_t *bad;

    if (url == NULL || url[0] == '\0' ||
        strlen(url) >= P4_CONFIG_BROWSER_URL_BYTES) {
        return;
    }
    if (browser_image_cache_is_bad(url)) {
        return;
    }
    cache_lock();
    bad = bad_table_locked();
    if (bad != NULL) {
        if (s_bad_count >= BROWSER_IMAGE_BAD_MAX) {
            /* Oldest first: a page re-renders in the same order, so dropping
             * the oldest entry costs at most one extra fetch. */
            memmove(&bad[0], &bad[1],
                    sizeof(bad[0]) * (BROWSER_IMAGE_BAD_MAX - 1));
            s_bad_count = BROWSER_IMAGE_BAD_MAX - 1;
        }
        snprintf(bad[s_bad_count].url, sizeof(bad[0].url), "%s", url);
        s_bad_count++;
    }
    cache_unlock();
}

bool browser_image_cache_is_bad(const char *url)
{
    bool found = false;

    if (url == NULL || url[0] == '\0') {
        return false;
    }
    cache_lock();
    for (int i = 0; s_bad != NULL && i < s_bad_count; i++) {
        if (strcmp(s_bad[i].url, url) == 0) {
            found = true;
            break;
        }
    }
    cache_unlock();
    return found;
}

void browser_image_cache_clear(void)
{
    s_bad_count = 0;

    if (s_images == NULL) {
        return;
    }
    cache_lock();
    for (int i = 0; i < s_image_count; i++) {
        p4heap_free(s_images[i].pixels);
        memset(&s_images[i], 0, sizeof(s_images[i]));
    }
    s_image_count = 0;
    s_image_bytes = 0;
    cache_unlock();
}

int browser_image_cache_count(void)
{
    int count;

    if (s_images == NULL) {
        return 0;
    }
    cache_lock();
    count = s_image_count;
    cache_unlock();
    return count;
}

size_t browser_image_cache_bytes(void)
{
    size_t bytes;

    if (s_images == NULL) {
        return 0;
    }
    cache_lock();
    bytes = s_image_bytes;
    cache_unlock();
    return bytes;
}

/* ========================================================================
 * PAGE CACHE
 * ======================================================================== */

bool browser_page_cache_get(const char *url)
{
    browser_page_entry_t *hit = NULL;
    bool restored;

    if (url == NULL || url[0] == '\0') {
        return false;
    }
    if (s_pages == NULL) {
        return false;
    }

    cache_lock();
    for (int i = 0; i < s_page_count; i++) {
        if (strcmp(s_pages[i].url, url) == 0 && s_pages[i].shot != NULL) {
            hit = &s_pages[i];
            break;
        }
    }
    if (hit == NULL) {
        cache_unlock();
        return false;
    }
    hit->last_used = cache_tick();
    restored = tui_page_shot_restore(hit->shot);
    cache_unlock();
    return restored;
}

bool browser_page_cache_put(const char *url)
{
    browser_page_entry_t *slot = NULL;
    browser_page_entry_t *victim = NULL;
    const tui_page_shot_t *hdr;
    size_t bytes;
    void *shot;
    bool fresh = false;

    if (url == NULL || url[0] == '\0' || strlen(url) >= P4_CONFIG_BROWSER_URL_BYTES) {
        return false;
    }
    if (s_pages == NULL) {
        return false;
    }

    cache_lock();
    for (int i = 0; i < s_page_count; i++) {
        if (strcmp(s_pages[i].url, url) == 0) {
            slot = &s_pages[i];
            break;
        }
    }
    if (slot == NULL) {
        if (s_page_count < P4_CONFIG_BROWSER_PAGE_CACHE_MAX) {
            slot = &s_pages[s_page_count];
            fresh = true;
        } else {
            for (int i = 0; i < s_page_count; i++) {
                if (victim == NULL || s_pages[i].last_used < victim->last_used) {
                    victim = &s_pages[i];
                }
            }
            slot = victim;
        }
    }
    if (slot == NULL) {
        cache_unlock();
        return false;
    }

    shot = tui_page_shot_capture();
    if (shot == NULL) {
        cache_unlock();
        return false;
    }
    hdr = (const tui_page_shot_t *)shot;
    bytes = (size_t)hdr->total_bytes;
    if (bytes < sizeof(*hdr) ||
        bytes != (size_t)hdr->link_offset +
                 (size_t)hdr->link_count * sizeof(tui_link_t)) {
        /* A capture that does not report its own size consistently is not a
         * page snapshot we can account for. */
        tui_page_shot_release(shot);
        cache_unlock();
        return false;
    }

    /* Drop the previous blob only after the new one exists. */
    if (slot->shot != NULL) {
        s_page_bytes -= slot->bytes;
        tui_page_shot_release(slot->shot);
        slot->shot = NULL;
    }
    if (fresh) {
        s_page_count++;
    }
    slot->bytes = bytes;
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    slot->shot = shot;
    slot->last_used = cache_tick();
    s_page_bytes += bytes;
    cache_unlock();
    return true;
}

void browser_page_cache_clear(void)
{
    if (s_pages == NULL) {
        return;
    }
    cache_lock();
    for (int i = 0; i < s_page_count; i++) {
        tui_page_shot_release(s_pages[i].shot);
        memset(&s_pages[i], 0, sizeof(s_pages[i]));
    }
    s_page_count = 0;
    s_page_bytes = 0;
    cache_unlock();
}

int browser_page_cache_count(void)
{
    int count;

    if (s_pages == NULL) {
        return 0;
    }
    cache_lock();
    count = s_page_count;
    cache_unlock();
    return count;
}

size_t browser_page_cache_bytes(void)
{
    size_t bytes;

    if (s_pages == NULL) {
        return 0;
    }
    cache_lock();
    bytes = s_page_bytes;
    cache_unlock();
    return bytes;
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_browser_cache.c
 * @brief Unit tests for the browser PSRAM caches (components/browser).
 *
 * The image cache is covered end to end (round-trip, miss, NULL handling,
 * refresh-in-place, LRU eviction and clear). The page cache is covered for its
 * no-page behaviour: capturing requires an initialised TUI viewport, which the
 * unit app deliberately does not bring up, so the tests assert the safe
 * degradation paths rather than a successful snapshot.
 */

#include "unity.h"
#include "browser.h"
#include "p4minishell_config.h"

#include <stdlib.h>
#include <string.h>

static dom_image_t make_image(int w, int h, uint8_t fill)
{
    dom_image_t img;

    img.width = w;
    img.height = h;
    img.pixels = malloc((size_t)w * (size_t)h);
    TEST_ASSERT_NOT_NULL(img.pixels);
    memset(img.pixels, fill, (size_t)w * (size_t)h);
    return img;
}

void test_browser_image_cache_roundtrip(void)
{
    dom_image_t stored;
    dom_image_t fetched;

    browser_cache_init();
    browser_image_cache_clear();

    stored = make_image(4, 3, 0x5A);
    TEST_ASSERT_TRUE(browser_image_cache_put("https://example.com/a.png", &stored));
    free(stored.pixels);

    TEST_ASSERT_TRUE(browser_image_cache_get("https://example.com/a.png", &fetched));
    TEST_ASSERT_EQUAL_INT(4, fetched.width);
    TEST_ASSERT_EQUAL_INT(3, fetched.height);
    TEST_ASSERT_NOT_NULL(fetched.pixels);
    TEST_ASSERT_EQUAL_UINT8(0x5A, fetched.pixels[0]);
    TEST_ASSERT_EQUAL_UINT8(0x5A, fetched.pixels[4 * 3 - 1]);
    free(fetched.pixels);

    TEST_ASSERT_EQUAL_INT(1, browser_image_cache_count());
}

void test_browser_image_cache_miss_and_null(void)
{
    dom_image_t fetched;

    browser_image_cache_clear();

    TEST_ASSERT_FALSE(browser_image_cache_get("https://example.com/none.png", &fetched));
    TEST_ASSERT_NULL(fetched.pixels);
    TEST_ASSERT_EQUAL_INT(0, fetched.width);

    TEST_ASSERT_FALSE(browser_image_cache_get(NULL, &fetched));
    TEST_ASSERT_FALSE(browser_image_cache_get("", &fetched));
    TEST_ASSERT_FALSE(browser_image_cache_get("https://example.com/x", NULL));
    TEST_ASSERT_FALSE(browser_image_cache_put(NULL, &fetched));
    TEST_ASSERT_FALSE(browser_image_cache_put("https://example.com/x", NULL));

    /* Oversized and empty payloads are refused, not stored. */
    {
        dom_image_t empty = { 0, 0, NULL };

        TEST_ASSERT_FALSE(browser_image_cache_put("https://example.com/x", &empty));
    }

    /* A URL at the capacity limit is refused rather than truncated. */
    {
        char big[P4_CONFIG_BROWSER_URL_BYTES + 8];
        dom_image_t img = make_image(2, 2, 1);

        memset(big, 'u', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        TEST_ASSERT_FALSE(browser_image_cache_put(big, &img));
        free(img.pixels);
    }

    TEST_ASSERT_EQUAL_INT(0, browser_image_cache_count());
}

void test_browser_image_cache_refresh_in_place(void)
{
    dom_image_t first;
    dom_image_t second;
    dom_image_t fetched;

    browser_image_cache_clear();

    first = make_image(2, 2, 0x11);
    TEST_ASSERT_TRUE(browser_image_cache_put("https://example.com/b.png", &first));
    free(first.pixels);

    second = make_image(3, 2, 0x22);
    TEST_ASSERT_TRUE(browser_image_cache_put("https://example.com/b.png", &second));
    free(second.pixels);

    /* Same URL replaces the entry instead of adding a second one. */
    TEST_ASSERT_EQUAL_INT(1, browser_image_cache_count());
    TEST_ASSERT_TRUE(browser_image_cache_get("https://example.com/b.png", &fetched));
    TEST_ASSERT_EQUAL_INT(3, fetched.width);
    TEST_ASSERT_EQUAL_UINT8(0x22, fetched.pixels[0]);
    free(fetched.pixels);
}

void test_browser_image_cache_lru_eviction(void)
{
    dom_image_t fetched;
    char url[64];
    int cap = P4_CONFIG_BROWSER_IMAGE_CACHE_MAX;

    browser_image_cache_clear();
    if (cap < 2) {
        TEST_IGNORE_MESSAGE("image cache capacity too small to test eviction");
    }

    /* Fill the cache, then touch entry 0 so the LAST entry becomes the LRU. */
    for (int i = 0; i < cap; i++) {
        dom_image_t img = make_image(2, 2, (uint8_t)i);

        snprintf(url, sizeof(url), "https://example.com/%d.png", i);
        TEST_ASSERT_TRUE(browser_image_cache_put(url, &img));
        free(img.pixels);
    }
    TEST_ASSERT_EQUAL_INT(cap, browser_image_cache_count());

    snprintf(url, sizeof(url), "https://example.com/0.png");
    TEST_ASSERT_TRUE(browser_image_cache_get(url, &fetched));
    free(fetched.pixels);

    /* One more insert must evict the least recently used entry. Entry 0 was
     * touched last, so the oldest untouched entry (1) goes first. */
    {
        dom_image_t img = make_image(2, 2, 0x7F);

        TEST_ASSERT_TRUE(browser_image_cache_put("https://example.com/new.png", &img));
        free(img.pixels);
    }
    TEST_ASSERT_EQUAL_INT(cap, browser_image_cache_count());

    snprintf(url, sizeof(url), "https://example.com/%d.png", 1);
    TEST_ASSERT_FALSE(browser_image_cache_get(url, &fetched));
    snprintf(url, sizeof(url), "https://example.com/0.png");
    TEST_ASSERT_TRUE(browser_image_cache_get(url, &fetched));
    free(fetched.pixels);
    snprintf(url, sizeof(url), "https://example.com/new.png");
    TEST_ASSERT_TRUE(browser_image_cache_get(url, &fetched));
    free(fetched.pixels);
}

void test_browser_image_cache_clear(void)
{
    dom_image_t img = make_image(2, 2, 0x33);

    browser_image_cache_clear();
    TEST_ASSERT_TRUE(browser_image_cache_put("https://example.com/c.png", &img));
    free(img.pixels);
    TEST_ASSERT_EQUAL_INT(1, browser_image_cache_count());
    TEST_ASSERT_TRUE(browser_image_cache_bytes() > 0);

    browser_image_cache_clear();
    TEST_ASSERT_EQUAL_INT(0, browser_image_cache_count());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)browser_image_cache_bytes());

    /* Clearing twice, and using a cache that never came up, must be safe. */
    browser_image_cache_clear();
    TEST_ASSERT_FALSE(browser_image_cache_get("https://example.com/c.png", NULL));
}

void test_browser_page_cache_degrades_without_a_page(void)
{
    /* The unit app drives no display, so there is no TUI page to snapshot. The
     * cache must report a miss rather than fault, and stay usable. */
    browser_page_cache_clear();
    TEST_ASSERT_EQUAL_INT(0, browser_page_cache_count());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)browser_page_cache_bytes());

    TEST_ASSERT_FALSE(browser_page_cache_put("https://example.com/"));
    TEST_ASSERT_FALSE(browser_page_cache_get("https://example.com/"));
    TEST_ASSERT_FALSE(browser_page_cache_get(NULL));
    TEST_ASSERT_FALSE(browser_page_cache_get(""));
    TEST_ASSERT_EQUAL_INT(0, browser_page_cache_count());

    browser_page_cache_clear();
}

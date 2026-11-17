/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_emoji.c
 * @brief Emoji core: detection, the retro DOS fallback map, UTF-8.
 *
 * The same core drives the TUI/HTML text path and the LVGL fallback font, so
 * these cases pin the shared contract: what counts as emoji, that every emoji
 * maps deterministically to a CP437 glyph, and that the fallback survives a
 * UTF-8 round trip.
 */

#include "unity.h"
#include "emoji_core.h"

#include <string.h>

void test_emoji_detects_ranges(void)
{
    TEST_ASSERT_TRUE(emoji_is_codepoint(0x1F600));   /* 😀 */
    TEST_ASSERT_TRUE(emoji_is_codepoint(0x1F44D));   /* 👍 */
    TEST_ASSERT_TRUE(emoji_is_codepoint(0x2600));    /* ☀ */
    TEST_ASSERT_TRUE(emoji_is_codepoint(0x2B50));    /* ⭐ */
    TEST_ASSERT_TRUE(emoji_is_codepoint(0x1F1E6));   /* regional indicator */
    TEST_ASSERT_FALSE(emoji_is_codepoint('A'));
    TEST_ASSERT_FALSE(emoji_is_codepoint(0x00E9));   /* é */
    TEST_ASSERT_FALSE(emoji_is_codepoint(0x2500));   /* box drawing */
    TEST_ASSERT_FALSE(emoji_is_codepoint(0x4E2D));   /* CJK */
}

void test_emoji_detects_modifiers(void)
{
    TEST_ASSERT_TRUE(emoji_is_modifier(0x200D));     /* ZWJ */
    TEST_ASSERT_TRUE(emoji_is_modifier(0xFE0F));     /* VS16 */
    TEST_ASSERT_TRUE(emoji_is_modifier(0x1F3FB));    /* skin tone 1 */
    TEST_ASSERT_TRUE(emoji_is_modifier(0x1F3FF));    /* skin tone 6 */
    TEST_ASSERT_FALSE(emoji_is_modifier(0x1F600));
    TEST_ASSERT_FALSE(emoji_is_modifier('a'));
}

void test_emoji_fallback_map(void)
{
    /* The curated faces and marks are stable; a page must render identically
     * every time it is opened. */
    TEST_ASSERT_EQUAL_UINT32(0x263A, emoji_dos_glyph(0x1F600));  /* ☺ */
    TEST_ASSERT_EQUAL_UINT32(0x2665, emoji_dos_glyph(0x2764));   /* ♥ */
    TEST_ASSERT_EQUAL_UINT32(0x221A, emoji_dos_glyph(0x1F44D));  /* √ */
    TEST_ASSERT_EQUAL_UINT32(0x2605, emoji_dos_glyph(0x2B50));   /* ★ */
    TEST_ASSERT_EQUAL_UINT32(0x263C, emoji_dos_glyph(0x2600));   /* ☼ */

    /* Unlisted emoji fall back by range, never to zero (no tofu). */
    TEST_ASSERT_EQUAL_UINT32(0x25CB, emoji_dos_glyph(0x1F9A5));  /* sloth */
    TEST_ASSERT_EQUAL_UINT32(0x25BA, emoji_dos_glyph(0x1F6F0));  /* satellite */
    TEST_ASSERT_EQUAL_UINT32(0x25A0, emoji_dos_glyph(0x1F1E9));  /* flag */

    /* Non-emoji text has no fallback: it renders as itself. */
    TEST_ASSERT_EQUAL_UINT32(0, emoji_dos_glyph('A'));
    TEST_ASSERT_EQUAL_UINT32(0, emoji_dos_glyph(0x2500));
}

void test_emoji_utf8_round_trip(void)
{
    static const uint32_t samples[] = {
        'A', 0x00E9, 0x2500, 0x263A, 0x1F600, 0x10FFFF,
    };

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        char buf[5] = { 0 };
        uint32_t back = 0;
        size_t len = emoji_utf8_encode(samples[i], buf);

        TEST_ASSERT_TRUE(len >= 1 && len <= 4);
        TEST_ASSERT_EQUAL_size_t(len, emoji_utf8_decode(buf, &back));
        TEST_ASSERT_EQUAL_UINT32(samples[i], back);
    }
    /* Malformed input is U+FFFD and one byte consumed: the caller always makes
     * progress, so a corrupt page cannot stall the renderer. */
    {
        uint32_t cp = 0;

        TEST_ASSERT_EQUAL_size_t(1, emoji_utf8_decode("\xFF", &cp));
        TEST_ASSERT_EQUAL_UINT32(0xFFFD, cp);
    }
}

void test_emoji_fallback_is_valid_utf8(void)
{
    char buf[5] = { 0 };
    size_t len = emoji_fallback_utf8(0x1F600, buf);
    uint32_t cp = 0;

    TEST_ASSERT_EQUAL_size_t(3, len);          /* ☺ is 3 bytes */
    TEST_ASSERT_EQUAL_size_t(3, emoji_utf8_decode(buf, &cp));
    TEST_ASSERT_EQUAL_UINT32(0x263A, cp);

    TEST_ASSERT_EQUAL_size_t(0, emoji_fallback_utf8('A', buf));
}

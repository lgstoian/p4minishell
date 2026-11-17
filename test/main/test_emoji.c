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
    TEST_ASSERT_EQUAL_UINT32(0x2611, emoji_dos_glyph(0x1F44D));  /* ☑ */
    TEST_ASSERT_EQUAL_UINT32(0x2605, emoji_dos_glyph(0x2B50));   /* ★ */
    TEST_ASSERT_EQUAL_UINT32(0x263C, emoji_dos_glyph(0x2600));   /* ☼ */

    /* Unlisted emoji fall back by range, never to zero (no tofu). */
    TEST_ASSERT_EQUAL_UINT32(0x2610, emoji_dos_glyph(0x1F9A5));  /* sloth */
    TEST_ASSERT_EQUAL_UINT32(0x2605, emoji_dos_glyph(0x1F6F0));  /* satellite */
    TEST_ASSERT_EQUAL_UINT32(0x2610, emoji_dos_glyph(0x1F1E9));  /* flag */

    /* Non-emoji text has no fallback: it renders as itself. */
    TEST_ASSERT_EQUAL_UINT32(0, emoji_dos_glyph('A'));
    TEST_ASSERT_EQUAL_UINT32(0, emoji_dos_glyph(0x2500));
}

/** Every fallback must be drawable by the shipped cell font.
 *
 * `lv_font_unscii_16` covers ASCII, U+2500-U+257F and U+2600-U+26FF only; a
 * fallback outside those ranges renders as the font's placeholder, which is
 * exactly the tofu this feature exists to prevent. This walks every codepoint
 * the core recognises and pins the invariant, so adding a pretty-looking
 * symbol from another block fails here instead of on the panel.
 */
void test_emoji_glyphs_are_in_font(void)
{
    static const uint32_t ranges[][2] = {
        { 0x1F000, 0x1FAFF },
        { 0x2300, 0x2BFF },
        { 0x2000, 0x206F },
    };
    int checked = 0;

    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
        for (uint32_t cp = ranges[r][0]; cp <= ranges[r][1]; cp++) {
            uint32_t glyph;

            if (!emoji_is_codepoint(cp)) {
                continue;
            }
            glyph = emoji_dos_glyph(cp);
            if (glyph == 0) {
                continue;   /* no substitute: renders as itself */
            }
            checked++;
            TEST_ASSERT_TRUE_MESSAGE(
                (glyph >= 32 && glyph <= 126) ||
                (glyph >= 0x2500 && glyph <= 0x257F) ||
                (glyph >= 0x2600 && glyph <= 0x26FF),
                "fallback outside the shipped font's cmap");
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(100, checked);
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

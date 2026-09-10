/**
 * @file test_tui.c
 * @brief Unit tests for headless-safe TUI state.
 *
 * Covers the LVGL-free accessors in components/tui/tui.c: the default
 * colour round-trip (DOS COLOR parity), cursor save/restore/get, and the
 * inactive/fullscreen defaults. Grid drawing and flushing need the LVGL
 * label and stay hardware-verified (see bugs.md).
 */

#include "unity.h"
#include "tui.h"
#include "command.h"
#include <stdint.h>
#include <stdbool.h>

void test_tui_default_color_roundtrip(void)
{
    uint8_t fg = 0;
    uint8_t bg = 0;

    tui_set_default_color(2, 4);
    tui_get_default_color(&fg, &bg);
    TEST_ASSERT_EQUAL_UINT8(2, fg);
    TEST_ASSERT_EQUAL_UINT8(4, bg);

    tui_set_default_color(16, 16);
    tui_get_default_color(&fg, &bg);
    TEST_ASSERT_EQUAL_UINT8(16, fg);
    TEST_ASSERT_EQUAL_UINT8(16, bg);
}

void test_tui_default_color_null_safe(void)
{
    /* NULL outputs are ignored, never dereferenced. */
    tui_set_default_color(7, 0);
    tui_get_default_color(NULL, NULL);
    tui_get_default_color(NULL, &(uint8_t){0});
}

void test_tui_cursor_save_restore(void)
{
    int row = 0;
    int col = 0;

    tui_save_cursor();
    tui_restore_cursor();
    tui_get_cursor(&row, &col);
    TEST_ASSERT_EQUAL_INT(1, row);
    TEST_ASSERT_EQUAL_INT(1, col);
}

void test_tui_inactive_by_default(void)
{
    TEST_ASSERT_FALSE(tui_is_active());
    TEST_ASSERT_FALSE(tui_is_fullscreen());
}

void test_tui_rgb_to_dos_exact(void)
{
    /* Primary CGA values map to themselves. */
    TEST_ASSERT_EQUAL_UINT8(0, tui_rgb_to_dos(0x000000));
    TEST_ASSERT_EQUAL_UINT8(4, tui_rgb_to_dos(0xAA0000));
    TEST_ASSERT_EQUAL_UINT8(2, tui_rgb_to_dos(0x00AA00));
    TEST_ASSERT_EQUAL_UINT8(1, tui_rgb_to_dos(0x0000AA));
    TEST_ASSERT_EQUAL_UINT8(7, tui_rgb_to_dos(0xAAAAAA));
    TEST_ASSERT_EQUAL_UINT8(15, tui_rgb_to_dos(0xFFFFFF));
    TEST_ASSERT_EQUAL_UINT8(12, tui_rgb_to_dos(0xFF5555));
    TEST_ASSERT_EQUAL_UINT8(14, tui_rgb_to_dos(0xFFFF55));
}

void test_tui_rgb_to_dos_nearest(void)
{
    /* Near-black / near-white snap to the right end of the palette. */
    TEST_ASSERT_EQUAL_UINT8(0, tui_rgb_to_dos(0x010101));
    TEST_ASSERT_EQUAL_UINT8(15, tui_rgb_to_dos(0xFEFEFE));
    /* Pure green is closer to CGA green (0x00AA00) than bright green. */
    TEST_ASSERT_EQUAL_UINT8(2, tui_rgb_to_dos(0x00FF00));
    /* Golden 0xFFCC00 snaps to bright yellow, not brown. */
    TEST_ASSERT_EQUAL_UINT8(14, tui_rgb_to_dos(0xFFCC00));
}

void test_tui_table_total_width(void)
{
    int widths2[] = {4, 6};

    /* sum + 3 per column + leading border: 4+6 + 6 + 1 = 17. */
    TEST_ASSERT_EQUAL_INT(17, tui_table_total_width(2, widths2));
    TEST_ASSERT_EQUAL_INT(0, tui_table_total_width(0, widths2));
    TEST_ASSERT_EQUAL_INT(0, tui_table_total_width(2, NULL));
}

void test_draw_hold_default_off(void)
{
    /* Verb behavior (on/off/usage/close-reset) is hardware-verified;
     * headless units only pin the default + linkage. */
    TEST_ASSERT_FALSE(draw_hold_active());
}

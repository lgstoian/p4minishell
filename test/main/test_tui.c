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

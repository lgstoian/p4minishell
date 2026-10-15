/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
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
#include "tui_pane.h"
#include "tui_modal.h"
#include "command.h"
#include "p4minishell_config.h"
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

void test_tui_table_parse_cursor(void)
{
    /* 1-based data rows; 0/unset/malformed/out-of-range all read as none. */
    TEST_ASSERT_EQUAL_INT(3, tui_table_parse_cursor("3", 6));
    TEST_ASSERT_EQUAL_INT(1, tui_table_parse_cursor(" 1 ", 6));
    TEST_ASSERT_EQUAL_INT(0, tui_table_parse_cursor("0", 6));
    TEST_ASSERT_EQUAL_INT(0, tui_table_parse_cursor("7", 6));
    TEST_ASSERT_EQUAL_INT(0, tui_table_parse_cursor("", 6));
    TEST_ASSERT_EQUAL_INT(0, tui_table_parse_cursor("x", 6));
    TEST_ASSERT_EQUAL_INT(0, tui_table_parse_cursor("2x", 6));
    TEST_ASSERT_EQUAL_INT(0, tui_table_parse_cursor("-1", 6));
    TEST_ASSERT_EQUAL_INT(0, tui_table_parse_cursor("1", 0));
    TEST_ASSERT_EQUAL_INT(0, tui_table_parse_cursor(NULL, 6));
}

void test_tui_table_parse_sel(void)
{
    /* Bit i = data row i+1; bad tokens ignored, valid ones clipped. */
    TEST_ASSERT_EQUAL_UINT32(0x05u, tui_table_parse_sel("1,3", 6));
    TEST_ASSERT_EQUAL_UINT32(0x01u, tui_table_parse_sel("1", 6));
    TEST_ASSERT_EQUAL_UINT32(0x00u, tui_table_parse_sel("", 6));
    TEST_ASSERT_EQUAL_UINT32(0x00u, tui_table_parse_sel("x", 6));
    TEST_ASSERT_EQUAL_UINT32(0x02u, tui_table_parse_sel("2,x,9", 6));
    TEST_ASSERT_EQUAL_UINT32(0x20u, tui_table_parse_sel("6", 6));
    TEST_ASSERT_EQUAL_UINT32(0x00u, tui_table_parse_sel("7", 6));
    TEST_ASSERT_EQUAL_UINT32(0x00u, tui_table_parse_sel("1", 0));
    TEST_ASSERT_EQUAL_UINT32(0x00u, tui_table_parse_sel(NULL, 6));
}

void test_tui_scroll_up_inactive_safe(void)
{
    int row = 0;
    int col = 0;

    /* Scrolling with no allocated grid (VT100 pump not running) must be a
     * no-op: no crash, cursor and active state untouched. Live scrolling is
     * hardware-verified via `usb userial term`. */
    TEST_ASSERT_FALSE(tui_is_active());
    tui_scroll_up();
    TEST_ASSERT_FALSE(tui_is_active());
    tui_get_cursor(&row, &col);
    TEST_ASSERT_EQUAL_INT(1, row);
    TEST_ASSERT_EQUAL_INT(1, col);
}

void test_tui_selftest_palette_clean(void)
{
    const char *fail_name = "unset";

    /* Would have failed before the double-glyph fix (X2 duplicated T2's
     * bytes; T2/B2/L2/R2 decoded off-spec). NULL out-param is safe. */
    TEST_ASSERT_EQUAL_INT(0, tui_selftest_palette(&fail_name));
    TEST_ASSERT_NULL(fail_name);
    TEST_ASSERT_EQUAL_INT(0, tui_selftest_palette(NULL));
}

void test_tui_selftest_coverage_clean(void)
{
    const char *fail_name = "unset";

    /* Every box codepoint present in every committed cell font with matching
     * advance (no fallback substitution). Pure table reads, headless-safe. */
    TEST_ASSERT_EQUAL_INT(0, tui_selftest_coverage(&fail_name));
    TEST_ASSERT_NULL(fail_name);
    TEST_ASSERT_EQUAL_INT(0, tui_selftest_coverage(NULL));
}

void test_tui_selftest_metrics_clean(void)
{
    const char *fail_name = "unset";

    TEST_ASSERT_EQUAL_INT(0, tui_selftest_metrics(&fail_name));
    TEST_ASSERT_NULL(fail_name);
    TEST_ASSERT_EQUAL_INT(0, tui_selftest_metrics(NULL));
}

void test_tui_selftest_check_clean(void)
{
    const char *fail_name = "unset";

    TEST_ASSERT_EQUAL_INT(0, tui_selftest_check(&fail_name));
    TEST_ASSERT_NULL(fail_name);
    TEST_ASSERT_EQUAL_INT(0, tui_selftest_check(NULL));
}

void test_tui_bound_cell_inactive(void)
{
    int w = -1;
    int h = -1;
    int index = -2;

    /* Test app never enters TUI mode (black screen): unbound reads zeros. */
    TEST_ASSERT_FALSE(tui_is_active());
    tui_bound_cell_info(&w, &h, &index);
    TEST_ASSERT_EQUAL_INT(0, w);
    TEST_ASSERT_EQUAL_INT(0, h);
    TEST_ASSERT_EQUAL_INT(-1, index);
    tui_bound_cell_info(NULL, NULL, NULL);
}

/* ========================================================================
 * WORKSPACE (tui_pane.c) headless tests
 * ================================================================ */

void test_ws_pane_add_close(void)
{
    tui_workspace_reset();
    TEST_ASSERT_EQUAL_INT(0, tui_pane_count());
    TEST_ASSERT_TRUE(tui_pane_add("LOG", 1, 2, 20, 10, "Log"));
    TEST_ASSERT_EQUAL_INT(1, tui_pane_count());
    TEST_ASSERT_TRUE(tui_pane_add("IN", 1, 13, 20, 10, "Input"));
    TEST_ASSERT_EQUAL_INT(2, tui_pane_count());
    TEST_ASSERT_EQUAL_INT(0, tui_pane_get_focus());
    TEST_ASSERT_TRUE(tui_pane_close("LOG"));
    TEST_ASSERT_EQUAL_INT(1, tui_pane_count());
    TEST_ASSERT_TRUE(tui_pane_close("IN"));
    TEST_ASSERT_EQUAL_INT(0, tui_pane_count());
}

void test_ws_pane_duplicate_rejected(void)
{
    tui_workspace_reset();
    TEST_ASSERT_TRUE(tui_pane_add("A", 1, 1, 10, 5, NULL));
    TEST_ASSERT_FALSE(tui_pane_add("A", 20, 1, 10, 5, NULL));
    TEST_ASSERT_EQUAL_INT(1, tui_pane_count());
}

void test_ws_pane_focus_cycle(void)
{
    tui_workspace_reset();
    tui_pane_add("A", 1, 1, 10, 5, NULL);
    tui_pane_add("B", 20, 1, 10, 5, NULL);
    TEST_ASSERT_EQUAL_INT(0, tui_pane_get_focus());
    TEST_ASSERT_TRUE(tui_pane_focus("B"));
    TEST_ASSERT_EQUAL_INT(1, tui_pane_get_focus());
    TEST_ASSERT_TRUE(tui_pane_cycle());
    TEST_ASSERT_EQUAL_INT(0, tui_pane_get_focus());
    TEST_ASSERT_TRUE(tui_pane_cycle());
    TEST_ASSERT_EQUAL_INT(1, tui_pane_get_focus());
}

void test_ws_pane_hide_skips_focus(void)
{
    tui_workspace_reset();
    tui_pane_add("A", 1, 1, 10, 5, NULL);
    tui_pane_add("B", 20, 1, 10, 5, NULL);
    tui_pane_focus("A");
    TEST_ASSERT_TRUE(tui_pane_hide("A", true));
    TEST_ASSERT_EQUAL_INT(1, tui_pane_get_focus());
    TEST_ASSERT_FALSE(tui_pane_focus("A"));
    TEST_ASSERT_TRUE(tui_pane_hide("A", false));
    TEST_ASSERT_TRUE(tui_pane_focus("A"));
}

void test_ws_pane_split(void)
{
    tui_workspace_reset();
    tui_pane_add("A", 1, 1, 20, 10, NULL);
    TEST_ASSERT_TRUE(tui_pane_split("A", true));
    TEST_ASSERT_EQUAL_INT(2, tui_pane_count());
    TEST_ASSERT_TRUE(tui_pane_split("A", false));
    TEST_ASSERT_EQUAL_INT(3, tui_pane_count());
}

void test_ws_pane_append_clear(void)
{
    tui_workspace_reset();
    tui_pane_add("LOG", 1, 1, 20, 5, NULL);
    TEST_ASSERT_TRUE(tui_pane_append("LOG", "hello"));
    TEST_ASSERT_TRUE(tui_pane_append("LOG", "world"));
    TEST_ASSERT_TRUE(tui_pane_clear("LOG"));
    TEST_ASSERT_FALSE(tui_pane_append("NOPE", "x"));
}

void test_ws_widget_add_remove(void)
{
    tui_workspace_reset();
    TEST_ASSERT_FALSE(tui_widget_exists("W1"));
    TEST_ASSERT_TRUE(tui_widget_add("W1", "text", 1, 1, 10, 1, "Label"));
    TEST_ASSERT_TRUE(tui_widget_exists("W1"));
    TEST_ASSERT_TRUE(tui_widget_set_text("W1", "hello"));
    TEST_ASSERT_TRUE(tui_widget_remove("W1"));
    TEST_ASSERT_FALSE(tui_widget_exists("W1"));
}

void test_ws_widget_types(void)
{
    tui_workspace_reset();
    TEST_ASSERT_TRUE(tui_widget_add("T", "text", 1, 1, 10, 1, NULL));
    TEST_ASSERT_TRUE(tui_widget_add("G", "gauge", 1, 2, 10, 1, NULL));
    TEST_ASSERT_TRUE(tui_widget_set_value("G", 50));
    TEST_ASSERT_TRUE(tui_widget_set_max("G", 200));
    TEST_ASSERT_TRUE(tui_widget_add("B", "button", 1, 3, 10, 1, "Run"));
    TEST_ASSERT_TRUE(tui_widget_bind_label("B", "run_click"));
    TEST_ASSERT_TRUE(tui_widget_add("L", "list", 1, 4, 10, 2, NULL));
    TEST_ASSERT_TRUE(tui_widget_set_value("L", 2));
    TEST_ASSERT_TRUE(tui_widget_add("P", "panel", 1, 7, 20, 5, "Box"));
}

void test_ws_hud_add_remove(void)
{
    tui_workspace_reset();
    TEST_ASSERT_TRUE(tui_hud_add("batt", 0));
    TEST_ASSERT_TRUE(tui_hud_set_text("batt", "BAT 85%"));
    TEST_ASSERT_TRUE(tui_hud_add("clock", 1));
    TEST_ASSERT_TRUE(tui_hud_set_text("clock", "12:30"));
    TEST_ASSERT_TRUE(tui_hud_flash("batt", 3));
    TEST_ASSERT_TRUE(tui_hud_remove("batt"));
    TEST_ASSERT_FALSE(tui_hud_remove("batt"));
}

void test_ws_hud_refresh_cadence(void)
{
    tui_workspace_reset();
    TEST_ASSERT_EQUAL_UINT32(P4_CONFIG_WS_HUD_REFRESH_MS, tui_hud_get_refresh());
    tui_hud_set_refresh(1000);
    TEST_ASSERT_EQUAL_UINT32(1000, tui_hud_get_refresh());
    tui_hud_set_refresh(0);
    TEST_ASSERT_EQUAL_UINT32(0, tui_hud_get_refresh());
}

void test_ws_event_post_get_wait(void)
{
    tui_workspace_reset();
    TEST_ASSERT_EQUAL_INT(0, tui_event_count());
    TEST_ASSERT_TRUE(tui_event_post(TUI_EVENT_KEY, "F1"));
    TEST_ASSERT_TRUE(tui_event_post(TUI_EVENT_TAP, "run_click"));
    TEST_ASSERT_EQUAL_INT(2, tui_event_count());

    tui_event_type_t type;
    char payload[64];
    TEST_ASSERT_TRUE(tui_event_get(0, &type, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(TUI_EVENT_KEY, type);
    TEST_ASSERT_EQUAL_STRING("F1", payload);
    TEST_ASSERT_TRUE(tui_event_get(1, &type, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(TUI_EVENT_TAP, type);
    TEST_ASSERT_EQUAL_STRING("run_click", payload);

    TEST_ASSERT_EQUAL_INT(0, tui_event_wait(0));
    tui_event_clear();
    TEST_ASSERT_EQUAL_INT(0, tui_event_count());
    TEST_ASSERT_EQUAL_INT(-1, tui_event_wait(10));
}

void test_ws_key_bind_unbind(void)
{
    tui_workspace_reset();
    TEST_ASSERT_EQUAL_INT(0, tui_key_count());
    TEST_ASSERT_TRUE(tui_key_bind("F1", "help"));
    TEST_ASSERT_TRUE(tui_key_bind("CTRL+R", "reload"));
    TEST_ASSERT_EQUAL_INT(2, tui_key_count());
    TEST_ASSERT_EQUAL_STRING("help", tui_key_lookup("F1"));
    TEST_ASSERT_EQUAL_STRING("reload", tui_key_lookup("CTRL+R"));
    TEST_ASSERT_NULL(tui_key_lookup("F2"));
    TEST_ASSERT_TRUE(tui_key_unbind("F1"));
    TEST_ASSERT_EQUAL_INT(1, tui_key_count());
    TEST_ASSERT_NULL(tui_key_lookup("F1"));
}

void test_ws_key_mode(void)
{
    tui_workspace_reset();
    TEST_ASSERT_EQUAL_INT(0, tui_key_get_mode());
    tui_key_set_mode(1);
    TEST_ASSERT_EQUAL_INT(1, tui_key_get_mode());
    tui_key_set_mode(2);
    TEST_ASSERT_EQUAL_INT(2, tui_key_get_mode());
    tui_key_set_mode(0);
    TEST_ASSERT_EQUAL_INT(0, tui_key_get_mode());
}

void test_ws_tap_add_hit(void)
{
    tui_workspace_reset();
    TEST_ASSERT_EQUAL_INT(0, tui_tap_count());
    TEST_ASSERT_TRUE(tui_tap_add(2, 4, 6, 2, "run_click"));
    TEST_ASSERT_TRUE(tui_tap_add(10, 10, 5, 3, "other"));
    TEST_ASSERT_EQUAL_INT(2, tui_tap_count());
    TEST_ASSERT_EQUAL_STRING("run_click", tui_tap_hit(3, 5));
    TEST_ASSERT_EQUAL_STRING("other", tui_tap_hit(12, 11));
    TEST_ASSERT_NULL(tui_tap_hit(1, 1));
    TEST_ASSERT_EQUAL_INT(1, tui_tap_remove(2, 4));
    TEST_ASSERT_EQUAL_INT(1, tui_tap_count());
    tui_tap_clear();
    TEST_ASSERT_EQUAL_INT(0, tui_tap_count());
}

void test_ws_workspace_reset(void)
{
    tui_workspace_reset();
    tui_pane_add("A", 1, 1, 10, 5, NULL);
    tui_widget_add("W", "text", 1, 1, 10, 1, NULL);
    tui_hud_add("h", 0);
    tui_key_bind("F1", "help");
    tui_tap_add(1, 1, 5, 5, "x");
    tui_event_post(TUI_EVENT_KEY, "F1");
    TEST_ASSERT_EQUAL_INT(1, tui_pane_count());
    TEST_ASSERT_TRUE(tui_widget_exists("W"));
    TEST_ASSERT_EQUAL_INT(1, tui_key_count());
    TEST_ASSERT_EQUAL_INT(1, tui_tap_count());
    TEST_ASSERT_EQUAL_INT(1, tui_event_count());

    tui_workspace_reset();
    TEST_ASSERT_EQUAL_INT(0, tui_pane_count());
    TEST_ASSERT_FALSE(tui_widget_exists("W"));
    TEST_ASSERT_EQUAL_INT(0, tui_key_count());
    TEST_ASSERT_EQUAL_INT(0, tui_tap_count());
    TEST_ASSERT_EQUAL_INT(0, tui_event_count());
}

/* ========================================================================
 * MODAL LAYER (tui_modal.c) headless tests
 * ================================================================ */

static int test_exec_count = 0;
static int test_exec_label_cb(const char *label, void *context)
{
    (void)context;
    test_exec_count++;
    return 0;
}

static const char *test_env_get_cb(const char *name)
{
    (void)name;
    return NULL;
}

static int test_env_set_cb(const char *name, const char *value)
{
    (void)name;
    (void)value;
    return 0;
}

void test_modal_init_reset(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    TEST_ASSERT_EQUAL_INT(0, modal_mode_count());
    TEST_ASSERT_EQUAL_INT(0, modal_cmd_count());
    TEST_ASSERT_EQUAL_INT(0, modal_macro_count());
}

void test_modal_mode_push_pop(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    TEST_ASSERT_TRUE(modal_mode_push("normal", MODAL_FRAME_NORMAL, "BASE"));
    TEST_ASSERT_EQUAL_INT(1, modal_mode_count());
    TEST_ASSERT_TRUE(modal_mode_push("insert", MODAL_FRAME_INSERT, "INS"));
    TEST_ASSERT_EQUAL_INT(2, modal_mode_count());
    TEST_ASSERT_EQUAL_INT(1, modal_mode_top());
    TEST_ASSERT_EQUAL_INT(MODAL_FRAME_INSERT, modal_mode_get_type(0));
    TEST_ASSERT_TRUE(modal_mode_pop());
    TEST_ASSERT_EQUAL_INT(1, modal_mode_count());
    TEST_ASSERT_TRUE(modal_mode_pop());
    TEST_ASSERT_EQUAL_INT(0, modal_mode_count());
}

void test_modal_mode_switch(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    modal_mode_push("a", MODAL_FRAME_NORMAL, NULL);
    modal_mode_push("b", MODAL_FRAME_NORMAL, NULL);
    modal_mode_push("c", MODAL_FRAME_NORMAL, NULL);
    TEST_ASSERT_EQUAL_INT(3, modal_mode_count());
    modal_mode_switch("a");
    TEST_ASSERT_EQUAL_INT(1, modal_mode_count());
}

void test_modal_bind_unbind(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    TEST_ASSERT_TRUE(modal_bind("BASE", "j", "mv_down"));
    TEST_ASSERT_TRUE(modal_bind("BASE", "k", "mv_up"));
    TEST_ASSERT_TRUE(modal_bind("BASE", "gg", "goto_top"));
    TEST_ASSERT_EQUAL_INT(3, modal_bind_count("BASE"));
    TEST_ASSERT_TRUE(modal_unbind("BASE", "k"));
    TEST_ASSERT_EQUAL_INT(2, modal_bind_count("BASE"));
}

void test_modal_chord_pending(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    modal_map_create("TEST");
    modal_bind("TEST", "j", "down");
    modal_bind("TEST", "g g", "top");

    char label[32];
    /* Single-stroke match. */
    TEST_ASSERT_TRUE(modal_chord_push("j", label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("down", label);

    /* Partial match (prefix). */
    modal_chord_push("g", label, sizeof(label));
    char pending[32];
    TEST_ASSERT_TRUE(modal_chord_pending(pending, sizeof(pending)));
    TEST_ASSERT_EQUAL_STRING("g", pending);

    /* Complete the chord. */
    TEST_ASSERT_TRUE(modal_chord_push("g", label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("top", label);
}

void test_modal_cmd_def_run(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    TEST_ASSERT_TRUE(modal_cmd_def("save", "do_save", "write file"));
    TEST_ASSERT_TRUE(modal_cmd_def("quit", "do_quit", "exit"));
    TEST_ASSERT_EQUAL_INT(2, modal_cmd_count());
    TEST_ASSERT_EQUAL_STRING("do_save", modal_cmd_find("save"));
    TEST_ASSERT_EQUAL_STRING("write file", modal_cmd_get_doc("save"));
    test_exec_count = 0;
    modal_cmd_run("save");
    TEST_ASSERT_EQUAL_INT(1, test_exec_count);
    TEST_ASSERT_TRUE(modal_cmd_undef("quit"));
    TEST_ASSERT_EQUAL_INT(1, modal_cmd_count());
}

void test_modal_scope_save_restore(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    const char *vars[] = {"CURSOR", "SEL"};
    TEST_ASSERT_TRUE(modal_scope_save("insert", 2, vars));
    TEST_ASSERT_TRUE(modal_scope_restore("insert"));
    TEST_ASSERT_TRUE(modal_scope_clear("insert"));
    TEST_ASSERT_FALSE(modal_scope_restore("insert"));
}

void test_modal_guard_set_get(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    TEST_ASSERT_TRUE(modal_guard_set(MODAL_GUARD_LEAVE, MODAL_FRAME_INSERT, "check_dirty"));
    TEST_ASSERT_EQUAL_STRING("check_dirty", modal_guard_get(MODAL_GUARD_LEAVE, MODAL_FRAME_INSERT));
    TEST_ASSERT_TRUE(modal_guard_clear(MODAL_GUARD_LEAVE, MODAL_FRAME_INSERT));
    TEST_ASSERT_NULL(modal_guard_get(MODAL_GUARD_LEAVE, MODAL_FRAME_INSERT));
}

void test_modal_dialog_push_pop(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    TEST_ASSERT_TRUE(modal_dialog_push("confirm", "Discard?"));
    TEST_ASSERT_EQUAL_INT(1, modal_dialog_count());
    modal_dialog_set_result("1");
    TEST_ASSERT_EQUAL_STRING("1", modal_dialog_get_result());
    modal_dialog_pop();
    TEST_ASSERT_EQUAL_INT(0, modal_dialog_count());
}

void test_modal_macro_record_replay(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    TEST_ASSERT_TRUE(modal_macro_record("M1"));
    modal_macro_capture("down");
    modal_macro_capture("down");
    modal_macro_capture("top");
    TEST_ASSERT_EQUAL_INT(3, modal_macro_stop());
    test_exec_count = 0;
    modal_macro_replay("M1", 2);
    TEST_ASSERT_EQUAL_INT(6, test_exec_count);
}

void test_modal_repeat(void)
{
    modal_init(test_exec_label_cb, test_env_get_cb, test_env_set_cb, NULL);
    modal_reset();
    TEST_ASSERT_EQUAL_INT(1, modal_get_repeat());
    modal_set_repeat(10);
    TEST_ASSERT_EQUAL_INT(10, modal_get_repeat());
    TEST_ASSERT_EQUAL_INT(1, modal_get_repeat()); /* consumed */
    modal_clear_repeat();
    TEST_ASSERT_EQUAL_INT(1, modal_get_repeat());
}

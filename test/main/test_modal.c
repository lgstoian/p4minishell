/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_modal.c
 * @brief Unit tests for the shared modal option parsers.
 *
 * Covers modal_parse_timeout_arg() / modal_parse_var_arg() from
 * components/modal/modal_surf.c, the single implementation behind the
 * `/t:secs` / `/v:NAME` options of dialog/list/ask/browse/view/hexview
 * (extracted in v0.35.5; previously copy-pasted at six call sites), plus
 * view_parse_args() from components/command/tui_commands.c, the shared
 * `view`/`open` option parser.
 */

#include "unity.h"
#include "modal_surf.h"
#include "command.h"
#include "batch.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void test_modal_timeout_basic(void)
{
    uint32_t ms = 0;

    TEST_ASSERT_TRUE(modal_parse_timeout_arg("/t:5", &ms));
    TEST_ASSERT_EQUAL_UINT32(5000, ms);
}

void test_modal_timeout_case_and_zero(void)
{
    uint32_t ms = 999;

    /* Case-insensitive prefix. */
    TEST_ASSERT_TRUE(modal_parse_timeout_arg("/T:2", &ms));
    TEST_ASSERT_EQUAL_UINT32(2000, ms);

    /* Zero / non-numeric yield 0 (wait forever), still consumed. */
    TEST_ASSERT_TRUE(modal_parse_timeout_arg("/t:0", &ms));
    TEST_ASSERT_EQUAL_UINT32(0, ms);
    TEST_ASSERT_TRUE(modal_parse_timeout_arg("/t:abc", &ms));
    TEST_ASSERT_EQUAL_UINT32(0, ms);
    TEST_ASSERT_TRUE(modal_parse_timeout_arg("/t:-3", &ms));
    TEST_ASSERT_EQUAL_UINT32(0, ms);
}

void test_modal_timeout_rejects(void)
{
    uint32_t ms = 4242;

    /* Bare prefix (no value) is NOT an option: historical parsers left it
     * as a positional argument, so the helper must decline it. */
    TEST_ASSERT_FALSE(modal_parse_timeout_arg("/t:", &ms));
    TEST_ASSERT_EQUAL_UINT32(4242, ms);
    TEST_ASSERT_FALSE(modal_parse_timeout_arg("/v:x", &ms));
    TEST_ASSERT_FALSE(modal_parse_timeout_arg("hello", &ms));
    TEST_ASSERT_FALSE(modal_parse_timeout_arg("", &ms));
    TEST_ASSERT_FALSE(modal_parse_timeout_arg(NULL, &ms));
    TEST_ASSERT_FALSE(modal_parse_timeout_arg("/t:5", NULL));
}

void test_modal_var_basic(void)
{
    const char *name = NULL;

    TEST_ASSERT_TRUE(modal_parse_var_arg("/v:MYVAR", &name));
    TEST_ASSERT_EQUAL_STRING("MYVAR", name);
}

void test_modal_var_rejects(void)
{
    const char *name = "keep";

    TEST_ASSERT_FALSE(modal_parse_var_arg("/v:", &name));
    TEST_ASSERT_EQUAL_STRING("keep", name);
    TEST_ASSERT_FALSE(modal_parse_var_arg("/t:5", &name));
    TEST_ASSERT_FALSE(modal_parse_var_arg("plain", &name));
    TEST_ASSERT_FALSE(modal_parse_var_arg(NULL, &name));
    TEST_ASSERT_FALSE(modal_parse_var_arg("/v:X", NULL));
}

void test_view_parse_basic(void)
{
    /* argv[0] is the verb name; options ride in any order. */
    char *argv[] = { "view", "/t:5", "--raw", "note.txt" };
    const char *path = NULL;
    uint32_t timeout_ms = 0;
    bool raw = false;

    TEST_ASSERT_TRUE(view_parse_args("view", 4, argv, &path, &timeout_ms, &raw));
    TEST_ASSERT_EQUAL_STRING("note.txt", path);
    TEST_ASSERT_EQUAL_UINT32(5000, timeout_ms);
    TEST_ASSERT_TRUE(raw);
}

void test_view_parse_defaults(void)
{
    char *argv[] = { "open", "a.bmp" };
    const char *path = NULL;
    uint32_t timeout_ms = 4242;
    bool raw = true;

    TEST_ASSERT_TRUE(view_parse_args("open", 2, argv, &path, &timeout_ms, &raw));
    TEST_ASSERT_EQUAL_STRING("a.bmp", path);
    TEST_ASSERT_EQUAL_UINT32(0, timeout_ms);
    TEST_ASSERT_FALSE(raw);
}

void test_view_parse_missing(void)
{
    /* No path: usage path (prints Usage, ERRORLEVEL 2); restore EL after. */
    char *argv[] = { "view", "/t:5" };
    const char *path = "keep";
    uint32_t timeout_ms = 0;
    bool raw = false;

    TEST_ASSERT_FALSE(view_parse_args("view", 2, argv, &path, &timeout_ms, &raw));
    batch_set_errorlevel(0);
}

/* -------------------------------------------------------------------------
 * v1.5 additive modal abilities: pure focus/progress/session state.
 * These call the SAME translation unit the firmware surfaces use
 * (components/modal/modal_args.c), so no host-only copy exists.
 * ---------------------------------------------------------------------- */

void test_modal_focus_clamp(void)
{
    TEST_ASSERT_EQUAL_INT(0, modal_focus_clamp(-5, 4));
    TEST_ASSERT_EQUAL_INT(3, modal_focus_clamp(99, 4));
    TEST_ASSERT_EQUAL_INT(2, modal_focus_clamp(2, 4));
    TEST_ASSERT_EQUAL_INT(0, modal_focus_clamp(7, 0));
    TEST_ASSERT_EQUAL_INT(0, modal_focus_clamp(7, -3));
}

void test_modal_focus_move_basic(void)
{
    int next = 99;

    TEST_ASSERT_TRUE(modal_focus_move(MODAL_KEY_DOWN, 5, 1, 5, &next));
    TEST_ASSERT_EQUAL_INT(2, next);
    TEST_ASSERT_TRUE(modal_focus_move(MODAL_KEY_UP, 5, 1, 5, &next));
    TEST_ASSERT_EQUAL_INT(0, next);
    TEST_ASSERT_TRUE(modal_focus_move(MODAL_KEY_END, 5, 1, 5, &next));
    TEST_ASSERT_EQUAL_INT(4, next);
    TEST_ASSERT_TRUE(modal_focus_move(MODAL_KEY_HOME, 5, 4, 5, &next));
    TEST_ASSERT_EQUAL_INT(0, next);
}

void test_modal_focus_move_bounds_and_page(void)
{
    int next = 77;

    /* Clamped at the ends: no movement means no repaint. */
    TEST_ASSERT_FALSE(modal_focus_move(MODAL_KEY_UP, 5, 0, 5, &next));
    TEST_ASSERT_EQUAL_INT(77, next);
    TEST_ASSERT_FALSE(modal_focus_move(MODAL_KEY_DOWN, 5, 4, 5, &next));
    TEST_ASSERT_EQUAL_INT(77, next);
    TEST_ASSERT_TRUE(modal_focus_move(MODAL_KEY_PAGE_DOWN, 12, 1, 4, &next));
    TEST_ASSERT_EQUAL_INT(5, next);
    TEST_ASSERT_TRUE(modal_focus_move(MODAL_KEY_PAGE_UP, 12, 11, 4, &next));
    TEST_ASSERT_EQUAL_INT(7, next);
    /* Default page size when step <= 0. */
    TEST_ASSERT_TRUE(modal_focus_move(MODAL_KEY_PAGE_UP, 20, 19, 0, &next));
    TEST_ASSERT_EQUAL_INT(14, next);
    TEST_ASSERT_FALSE(modal_focus_move(0x99, 20, 3, 0, &next));
}

void test_modal_progress_percent(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, modal_progress_percent(0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, modal_progress_percent(0, 200));
    TEST_ASSERT_EQUAL_UINT8(50, modal_progress_percent(50, 100));
    TEST_ASSERT_EQUAL_UINT8(33, modal_progress_percent(1, 3));
    TEST_ASSERT_EQUAL_UINT8(99, modal_progress_percent(99, 100));
    TEST_ASSERT_EQUAL_UINT8(100, modal_progress_percent(100, 100));
    TEST_ASSERT_EQUAL_UINT8(100, modal_progress_percent(500, 100));
    /* Large values must not overflow the integer math. */
    TEST_ASSERT_EQUAL_UINT8(50, modal_progress_percent(0x80000000u, 0u - 1u));
}

void test_modal_session_stack_push_pop(void)
{
    modal_session_stack_t st;

    modal_session_stack_init(&st);
    TEST_ASSERT_EQUAL_INT(0, modal_session_stack_depth(&st));
    TEST_ASSERT_TRUE(modal_session_stack_empty(&st));

    TEST_ASSERT_TRUE(modal_session_stack_push(&st, "notes"));
    TEST_ASSERT_EQUAL_INT(1, modal_session_stack_depth(&st));
    TEST_ASSERT_EQUAL_STRING("notes", modal_session_stack_name(&st, 0));
    TEST_ASSERT_EQUAL_INT(0, modal_session_stack_find(&st, "notes"));

    TEST_ASSERT_TRUE(modal_session_stack_push(&st, "calc"));
    TEST_ASSERT_EQUAL_INT(2, modal_session_stack_depth(&st));
    TEST_ASSERT_EQUAL_STRING("calc", modal_session_stack_name(&st, 0));
    TEST_ASSERT_EQUAL_STRING("notes", modal_session_stack_name(&st, 1));
    TEST_ASSERT_EQUAL_INT(0, modal_session_stack_find(&st, "calc"));
    TEST_ASSERT_EQUAL_INT(1, modal_session_stack_find(&st, "notes"));

    TEST_ASSERT_TRUE(modal_session_stack_pop(&st));
    TEST_ASSERT_EQUAL_INT(1, modal_session_stack_depth(&st));
    TEST_ASSERT_EQUAL_STRING("notes", modal_session_stack_name(&st, 0));
    TEST_ASSERT_TRUE(modal_session_stack_pop(&st));
    TEST_ASSERT_TRUE(modal_session_stack_empty(&st));
    TEST_ASSERT_FALSE(modal_session_stack_pop(&st));
}

void test_modal_session_stack_suspend_resume(void)
{
    modal_session_stack_t st;

    modal_session_stack_init(&st);
    TEST_ASSERT_TRUE(modal_session_stack_push(&st, "a"));
    TEST_ASSERT_TRUE(modal_session_stack_push(&st, "b"));

    TEST_ASSERT_TRUE(modal_session_stack_suspend(&st));
    TEST_ASSERT_EQUAL_INT(MODAL_SESSION_BACK, st.slot[st.depth - 1].state);
    TEST_ASSERT_TRUE(modal_session_stack_resume(&st));
    TEST_ASSERT_EQUAL_INT(MODAL_SESSION_FORE, st.slot[st.depth - 1].state);

    modal_session_stack_init(&st);
    TEST_ASSERT_FALSE(modal_session_stack_suspend(&st));
    TEST_ASSERT_FALSE(modal_session_stack_resume(&st));
}

void test_modal_session_stack_switch_find(void)
{
    modal_session_stack_t st;

    modal_session_stack_init(&st);
    TEST_ASSERT_TRUE(modal_session_stack_push(&st, "a"));
    TEST_ASSERT_TRUE(modal_session_stack_push(&st, "b"));
    TEST_ASSERT_TRUE(modal_session_stack_push(&st, "c"));

    TEST_ASSERT_TRUE(modal_session_stack_switch(&st, "a"));
    TEST_ASSERT_EQUAL_INT(0, modal_session_stack_find(&st, "a"));
    TEST_ASSERT_EQUAL_STRING("a", modal_session_stack_name(&st, 0));
    TEST_ASSERT_EQUAL_STRING("c", modal_session_stack_name(&st, 1));
    TEST_ASSERT_EQUAL_STRING("b", modal_session_stack_name(&st, 2));
    TEST_ASSERT_EQUAL_INT(MODAL_SESSION_FORE, st.slot[st.depth - 1].state);
    TEST_ASSERT_EQUAL_INT(MODAL_SESSION_BACK, st.slot[0].state);

    TEST_ASSERT_TRUE(modal_session_stack_switch(&st, "a"));
    TEST_ASSERT_FALSE(modal_session_stack_switch(&st, "missing"));
    TEST_ASSERT_EQUAL_INT(-1, modal_session_stack_find(&st, "missing"));
}

void test_modal_session_stack_bounds_and_focus(void)
{
    modal_session_stack_t st;
    int i;

    modal_session_stack_init(&st);
    TEST_ASSERT_FALSE(modal_session_stack_push(&st, NULL));
    TEST_ASSERT_FALSE(modal_session_stack_push(&st, ""));
    TEST_ASSERT_NULL(modal_session_stack_name(&st, 0));

    for (i = 0; i < MODAL_SESSION_MAX; i++) {
        TEST_ASSERT_TRUE(modal_session_stack_push(&st, "slot"));
    }
    TEST_ASSERT_TRUE(modal_session_stack_full(&st));
    TEST_ASSERT_FALSE(modal_session_stack_push(&st, "overflow"));
    TEST_ASSERT_EQUAL_INT(MODAL_SESSION_MAX, modal_session_stack_depth(&st));

    TEST_ASSERT_TRUE(modal_session_stack_set_focus(&st, 0, 7));
    TEST_ASSERT_EQUAL_INT(7, modal_session_stack_focus(&st, 0));
    TEST_ASSERT_TRUE(modal_session_stack_set_focus(&st, 1, 3));
    TEST_ASSERT_EQUAL_INT(3, modal_session_stack_focus(&st, 1));
    TEST_ASSERT_FALSE(modal_session_stack_set_focus(&st, -1, 1));
    TEST_ASSERT_FALSE(modal_session_stack_set_focus(&st, 0, -1));
    TEST_ASSERT_EQUAL_INT(-1, modal_session_stack_focus(&st, MODAL_SESSION_MAX));
}

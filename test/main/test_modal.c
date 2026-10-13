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

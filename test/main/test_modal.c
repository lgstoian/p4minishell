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
 * (extracted in v0.35.5; previously copy-pasted at six call sites).
 */

#include "unity.h"
#include "modal_surf.h"
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

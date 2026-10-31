/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_breadcrumb.c
 * @brief Pure formatter tests for the `breadcrumb` verb.
 *
 * The live verb reads the cwd, the batch frame stack, the background slot, and
 * the modal state; the formatter is separated so the trail shape is unit-tested
 * without SD, LVGL, or an active batch frame.
 */

#include "unity.h"
#include "command.h"

#include <stddef.h>
#include <string.h>

void test_breadcrumb_format_full(void)
{
    char out[256];

    command_breadcrumb_format("sd:\\APPS", "ROOT.BAT > CHILD.BAT", 2, true,
                              out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "cwd=sd:\\APPS"));
    TEST_ASSERT_NOT_NULL(strstr(out, "batch=ROOT.BAT > CHILD.BAT"));
    TEST_ASSERT_NOT_NULL(strstr(out, "bg=2"));
    TEST_ASSERT_NOT_NULL(strstr(out, "modal=on"));
}

void test_breadcrumb_format_empty(void)
{
    char out[256];

    command_breadcrumb_format(NULL, NULL, 0, false, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "cwd=?"));
    TEST_ASSERT_NOT_NULL(strstr(out, "batch=none"));
    TEST_ASSERT_NOT_NULL(strstr(out, "bg=0"));
    TEST_ASSERT_NOT_NULL(strstr(out, "modal=off"));
}

void test_breadcrumb_format_truncates(void)
{
    char out[8];

    command_breadcrumb_format("sd:\\LONG\\PATH", "A.BAT", 1, false,
                              out, sizeof(out));
    TEST_ASSERT_EQUAL_INT((int)sizeof(out) - 1, (int)strlen(out));
    TEST_ASSERT_EQUAL_INT(0, out[sizeof(out) - 1]);
}

void test_breadcrumb_format_null_out(void)
{
    /* A NULL output buffer must be a silent no-op. */
    command_breadcrumb_format("sd:\\", "", 0, false, NULL, 0);
    TEST_PASS();
}

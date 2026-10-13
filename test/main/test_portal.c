/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_portal.c
 * @brief Unit tests for the pure portal helpers.
 *
 * Covers portal_html_escape() from components/portal/portal_http.c (SSID
 * interpolation into the setup page). Pure (no Wi-Fi access) so it is tested
 * directly.
 */

#include "unity.h"
#include "portal.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

void test_portal_html_escape_plain(void)
{
    char out[64];

    TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)portal_html_escape("nova1_home", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("nova1_home", out);
}

void test_portal_html_escape_entities(void)
{
    char out[64];

    portal_html_escape("a&b<c>d\"e", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a&amp;b&lt;c&gt;d&quot;e", out);
}

void test_portal_html_escape_truncates(void)
{
    char out[8];

    /* "aa&bb" needs 2+5+1+1+1 = 10 bytes; "aa&amp;" + NUL fits exactly. */
    TEST_ASSERT_EQUAL_UINT32(7, (uint32_t)portal_html_escape("aa&bb", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("aa&amp;", out);

    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)portal_html_escape(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)portal_html_escape("x", NULL, 0));
}

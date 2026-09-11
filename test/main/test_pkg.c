/**
 * @file test_pkg.c
 * @brief Unit tests for the `pkg` pure helpers (pkg_commands.c).
 *
 * Only the I/O-free helpers are covered here; the pkg verbs' file operations
 * are hardware-verified (see tools/pkg_test.py).
 */

#include "unity.h"
#include "command.h"
#include <stdbool.h>
#include <string.h>

void test_pkg_app_name_from_appinfo_ok(void)
{
    char out[32];

    TEST_ASSERT_TRUE(pkg_app_name_from_appinfo("BOUNCE.APPINFO", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("BOUNCE", out);

    /* Case is uppercased. */
    TEST_ASSERT_TRUE(pkg_app_name_from_appinfo("Snake.appinfo", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("SNAKE", out);

    /* Underscore/dash are allowed app-name characters. */
    TEST_ASSERT_TRUE(pkg_app_name_from_appinfo("tcmd-1_x.APPINFO", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("TCMD-1_X", out);
}

void test_pkg_app_name_from_appinfo_bad(void)
{
    char out[32];

    /* Not an APPINFO name. */
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("BOUNCE.BAT", out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("APPINFO", out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo(".APPINFO", out, sizeof(out)));
    /* Disallowed characters in the base (space, dot, slash). */
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("A B.APPINFO", out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("A.B.APPINFO", out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("A/B.APPINFO", out, sizeof(out)));
    /* Buffer too small. */
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("BOUNCE.APPINFO", out, 4));
    /* NULL-safe. */
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo(NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("BOUNCE.APPINFO", NULL, 0));
}

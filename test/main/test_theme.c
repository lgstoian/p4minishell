/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_theme.c
 * @brief Unit tests for the theme registry (components/font/theme.c).
 *
 * Pure data + selection: built-in table lookup, case-insensitive match, and
 * active selection. Live application (LVGL) is hardware-verified.
 */

#include "unity.h"
#include "theme.h"
#include <string.h>

void test_theme_registry(void)
{
    int n = theme_builtin_count();

    TEST_ASSERT_GREATER_OR_EQUAL_INT(4, n);
    TEST_ASSERT_NOT_NULL(theme_builtin_at(0));
    TEST_ASSERT_NULL(theme_builtin_at(-1));
    TEST_ASSERT_NULL(theme_builtin_at(n));
    /* Every built-in has a non-empty name and description. */
    for (int i = 0; i < n; i++) {
        const theme_t *t = theme_builtin_at(i);
        TEST_ASSERT_NOT_NULL(t->name);
        TEST_ASSERT_TRUE(t->name[0] != '\0');
        TEST_ASSERT_NOT_NULL(t->description);
    }
}

void test_theme_lookup(void)
{
    const theme_t *d = theme_get("default");

    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("default", d->name);
    /* Case-insensitive. */
    TEST_ASSERT_EQUAL_PTR(d, theme_get("DEFAULT"));
    TEST_ASSERT_EQUAL_PTR(d, theme_get("Default"));
    /* Unknown / NULL. */
    TEST_ASSERT_NULL(theme_get("not-a-theme"));
    TEST_ASSERT_NULL(theme_get(NULL));
}

void test_theme_set_and_active(void)
{
    const theme_t *cur;

    TEST_ASSERT_TRUE(theme_set("default"));
    TEST_ASSERT_EQUAL_INT(0, theme_active_index());
    cur = theme_current();
    TEST_ASSERT_NOT_NULL(cur);
    TEST_ASSERT_EQUAL_STRING("default", cur->name);

    TEST_ASSERT_TRUE(theme_set("amber"));
    TEST_ASSERT_EQUAL_STRING("amber", theme_current()->name);
    TEST_ASSERT_EQUAL_PTR(theme_get("amber"), theme_current());

    /* Unknown name is rejected and leaves the active theme unchanged. */
    TEST_ASSERT_FALSE(theme_set("bogus"));
    TEST_ASSERT_EQUAL_STRING("amber", theme_current()->name);
    TEST_ASSERT_FALSE(theme_set(NULL));

    /* Every built-in is selectable and then resolves as current. */
    for (int i = 0; i < theme_builtin_count(); i++) {
        const theme_t *t = theme_builtin_at(i);

        TEST_ASSERT_TRUE(theme_set(t->name));
        TEST_ASSERT_EQUAL_PTR(t, theme_current());
        TEST_ASSERT_EQUAL_INT(i, theme_active_index());
    }

    /* Restore for other suites. */
    TEST_ASSERT_TRUE(theme_set("default"));
}

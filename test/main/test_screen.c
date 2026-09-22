/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_screen.c
 * @brief Unit tests for the declarative `screen` helpers (screen_commands.c).
 *
 * Covers screen_split_list() (the pure `|`-split behind `screen run` buttons
 * and items) and screen_flow_select() (the pure routing matcher behind
 * `screen flow`). Rendering and graph walking need the modal runtime and are
 * hardware-verified through the batch suite.
 */

#include "unity.h"
#include "command.h"

#include <string.h>

void test_screen_split_basic(void)
{
    char store[64];
    const char *out[8];

    TEST_ASSERT_EQUAL_INT(3, screen_split_list("a|b|c", '|', store, sizeof(store), out, 8));
    TEST_ASSERT_EQUAL_STRING("a", out[0]);
    TEST_ASSERT_EQUAL_STRING("b", out[1]);
    TEST_ASSERT_EQUAL_STRING("c", out[2]);

    /* Single entry, no delimiter. */
    TEST_ASSERT_EQUAL_INT(1, screen_split_list("OK", '|', store, sizeof(store), out, 8));
    TEST_ASSERT_EQUAL_STRING("OK", out[0]);
}

void test_screen_split_trims_and_drops_empties(void)
{
    char store[64];
    const char *out[8];

    TEST_ASSERT_EQUAL_INT(2, screen_split_list("  yes |no  ", '|', store, sizeof(store), out, 8));
    TEST_ASSERT_EQUAL_STRING("yes", out[0]);
    TEST_ASSERT_EQUAL_STRING("no", out[1]);

    TEST_ASSERT_EQUAL_INT(2, screen_split_list("a||b", '|', store, sizeof(store), out, 8));
    TEST_ASSERT_EQUAL_STRING("a", out[0]);
    TEST_ASSERT_EQUAL_STRING("b", out[1]);
}

void test_screen_split_bounds(void)
{
    char store[64];
    const char *out[8];
    char tiny[4];
    const char *tout[8];

    /* NULL/empty inputs yield zero, never a crash. */
    TEST_ASSERT_EQUAL_INT(0, screen_split_list(NULL, '|', store, sizeof(store), out, 8));
    TEST_ASSERT_EQUAL_INT(0, screen_split_list("", '|', store, sizeof(store), out, 8));

    /* A store that cannot hold the split reports -1. */
    TEST_ASSERT_EQUAL_INT(-1, screen_split_list("abcdef", '|', tiny, sizeof(tiny), tout, 8));

    /* Unusable arguments report -1. */
    TEST_ASSERT_EQUAL_INT(-1, screen_split_list("a", '|', NULL, 64, tout, 8));
    TEST_ASSERT_EQUAL_INT(-1, screen_split_list("a", '|', tiny, 0, tout, 8));
    TEST_ASSERT_EQUAL_INT(-1, screen_split_list("a", '|', tiny, sizeof(tiny), NULL, 8));
    TEST_ASSERT_EQUAL_INT(-1, screen_split_list("a", '|', tiny, sizeof(tiny), tout, 0));

    /* Entries past the cap are ignored, not overflowed. */
    TEST_ASSERT_EQUAL_INT(2, screen_split_list("a|b|c|d", '|', store, sizeof(store), out, 2));
    TEST_ASSERT_EQUAL_STRING("a", out[0]);
    TEST_ASSERT_EQUAL_STRING("b", out[1]);
}

/* ========================================================================
 * Declarative flow routing (screen_flow_select).
 * ======================================================================== */

static screen_flow_rule_t rule(const char *step, const char *token, const char *target)
{
    screen_flow_rule_t r;

    memset(&r, 0, sizeof(r));
    snprintf(r.step, sizeof(r.step), "%s", step);
    snprintf(r.token, sizeof(r.token), "%s", token);
    snprintf(r.target, sizeof(r.target), "%s", target);
    return r;
}

void test_screen_flow_select_errorlevel(void)
{
    screen_flow_rule_t rules[3];

    rules[0] = rule("menu", "err:1", "wifi");
    rules[1] = rule("menu", "err:2", "about");
    rules[2] = rule("menu", "*", "end");

    TEST_ASSERT_EQUAL_STRING("wifi", screen_flow_select(rules, 3, "menu", 1, NULL));
    TEST_ASSERT_EQUAL_STRING("about", screen_flow_select(rules, 3, "menu", 2, NULL));
    /* Fallback wins when nothing specific matches. */
    TEST_ASSERT_EQUAL_STRING("end", screen_flow_select(rules, 3, "menu", 255, NULL));
    /* A rule for a different step is ignored. */
    TEST_ASSERT_NULL(screen_flow_select(rules, 3, "other", 1, NULL));
}

void test_screen_flow_select_value(void)
{
    screen_flow_rule_t rules[3];

    rules[0] = rule("menu", "val:Download", "dl");
    rules[1] = rule("menu", "val:Back", "home");
    rules[2] = rule("menu", "*", "end");

    /* Value matching is case-insensitive. */
    TEST_ASSERT_EQUAL_STRING("dl", screen_flow_select(rules, 3, "menu", 0, "download"));
    TEST_ASSERT_EQUAL_STRING("home", screen_flow_select(rules, 3, "menu", 0, "BACK"));
    TEST_ASSERT_EQUAL_STRING("end", screen_flow_select(rules, 3, "menu", 0, "other"));
    /* No var value: val: rules cannot match, the fallback still does. */
    TEST_ASSERT_EQUAL_STRING("end", screen_flow_select(rules, 3, "menu", 0, NULL));
}

void test_screen_flow_select_order_and_bounds(void)
{
    screen_flow_rule_t rules[3];

    /* First match in array order wins, even against a later duplicate. */
    rules[0] = rule("s", "err:0", "first");
    rules[1] = rule("s", "err:0", "second");
    rules[2] = rule("s", "*", "fallback");
    TEST_ASSERT_EQUAL_STRING("first", screen_flow_select(rules, 3, "s", 0, NULL));

    /* Unknown/malformed tokens never match. */
    rules[0] = rule("s", "bogus", "x");
    rules[1] = rule("s", "err:", "y");
    rules[2] = rule("s", "val:", "z");
    TEST_ASSERT_NULL(screen_flow_select(rules, 3, "s", 0, ""));
    /* `val:` with an empty expression does not match an empty value. */
    rules[0] = rule("s", "val:", "z");
    TEST_ASSERT_NULL(screen_flow_select(rules, 1, "s", 0, ""));

    /* NULL-safe. */
    TEST_ASSERT_NULL(screen_flow_select(NULL, 3, "s", 0, NULL));
    TEST_ASSERT_NULL(screen_flow_select(rules, 0, "s", 0, NULL));
    TEST_ASSERT_NULL(screen_flow_select(rules, 3, NULL, 0, NULL));
}

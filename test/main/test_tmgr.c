/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_tmgr.c
 * @brief Unit tests for the task manager core (pool config + nice parsing).
 *
 * Covers the pure pieces behind `suspend`/`resume`/`nice`: the pool sizing
 * contract (BG_TASKS/BG_EAGER_TASKS), the priority bounds, the well-known
 * tmgr_* event names (sync namespace rule), and the `nice` priority parser.
 * Job freeze/thaw/restart behavior needs live tasks and is hardware-verified
 * through the batch suite.
 */

#include "unity.h"
#include "batch.h"
#include "command.h"
#include "p4minishell_config.h"

#include <string.h>

void test_tmgr_pool_config(void)
{
    /* Two resident services plus a spare job slot (task manager plan). */
    TEST_ASSERT_EQUAL_INT(3, (int)P4_CONFIG_BG_TASKS);
    /* Eager tasks must fit the boot heap window story: never more than total. */
    TEST_ASSERT_TRUE((int)P4_CONFIG_BG_EAGER_TASKS <= (int)P4_CONFIG_BG_TASKS);
    TEST_ASSERT_EQUAL_INT(2, (int)P4_CONFIG_BG_EAGER_TASKS);
}

void test_tmgr_priority_bounds(void)
{
    /* Pool workers run at tskIDLE+2; the nice window must contain it. */
    TEST_ASSERT_TRUE((int)P4_CONFIG_TMGR_PRIO_MIN <= 2);
    TEST_ASSERT_TRUE(2 <= (int)P4_CONFIG_TMGR_PRIO_MAX);
    TEST_ASSERT_TRUE((int)P4_CONFIG_TMGR_PRIO_MIN < (int)P4_CONFIG_TMGR_PRIO_MAX);
}

void test_tmgr_event_names(void)
{
    /* Well-known events must pass the sync namespace rule. */
    TEST_ASSERT_TRUE(sync_name_ok(P4_CONFIG_TMGR_EVENT_EXIT));
    TEST_ASSERT_TRUE(sync_name_ok(P4_CONFIG_TMGR_EVENT_SVC_RESTART));
    TEST_ASSERT_TRUE(sync_name_ok(P4_CONFIG_TMGR_EVENT_POOL_FULL));
    TEST_ASSERT_EQUAL_STRING("tmgr_exit", P4_CONFIG_TMGR_EVENT_EXIT);
    TEST_ASSERT_EQUAL_STRING("tmgr_svc_restart", P4_CONFIG_TMGR_EVENT_SVC_RESTART);
    TEST_ASSERT_EQUAL_STRING("tmgr_pool_full", P4_CONFIG_TMGR_EVENT_POOL_FULL);
}

void test_tmgr_clamp_priority(void)
{
    TEST_ASSERT_EQUAL_INT(1, command_tmgr_clamp_priority(0));
    TEST_ASSERT_EQUAL_INT(1, command_tmgr_clamp_priority(-7));
    TEST_ASSERT_EQUAL_INT(1, command_tmgr_clamp_priority(1));
    TEST_ASSERT_EQUAL_INT(2, command_tmgr_clamp_priority(2));
    TEST_ASSERT_EQUAL_INT(3, command_tmgr_clamp_priority(3));
    TEST_ASSERT_EQUAL_INT(3, command_tmgr_clamp_priority(4));
    TEST_ASSERT_EQUAL_INT(3, command_tmgr_clamp_priority(99));
}

void test_tmgr_parse_absolute(void)
{
    int out = -1;

    TEST_ASSERT_TRUE(command_tmgr_parse_priority("1", 2, &out));
    TEST_ASSERT_EQUAL_INT(1, out);
    TEST_ASSERT_TRUE(command_tmgr_parse_priority("3", 2, &out));
    TEST_ASSERT_EQUAL_INT(3, out);
    /* Out of bounds is a usage error, never a silent clamp. */
    out = -1;
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("0", 2, &out));
    TEST_ASSERT_EQUAL_INT(-1, out);
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("4", 2, &out));
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("9", 2, &out));
}

void test_tmgr_parse_relative(void)
{
    int out = -1;

    TEST_ASSERT_TRUE(command_tmgr_parse_priority("+1", 2, &out));
    TEST_ASSERT_EQUAL_INT(3, out);
    TEST_ASSERT_TRUE(command_tmgr_parse_priority("-1", 2, &out));
    TEST_ASSERT_EQUAL_INT(1, out);
    /* Deltas clamp into range instead of failing. */
    TEST_ASSERT_TRUE(command_tmgr_parse_priority("+9", 2, &out));
    TEST_ASSERT_EQUAL_INT(3, out);
    TEST_ASSERT_TRUE(command_tmgr_parse_priority("-9", 2, &out));
    TEST_ASSERT_EQUAL_INT(1, out);
    TEST_ASSERT_TRUE(command_tmgr_parse_priority("+0", 2, &out));
    TEST_ASSERT_EQUAL_INT(2, out);
}

void test_tmgr_parse_rejects(void)
{
    int out = -1;

    TEST_ASSERT_FALSE(command_tmgr_parse_priority(NULL, 2, &out));
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("", 2, &out));
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("x", 2, &out));
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("2x", 2, &out));
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("+", 2, &out));
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("-", 2, &out));
    TEST_ASSERT_FALSE(command_tmgr_parse_priority(" 2", 2, &out));
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("2 ", 2, &out));
    TEST_ASSERT_FALSE(command_tmgr_parse_priority("1", 2, NULL));
    TEST_ASSERT_EQUAL_INT(-1, out);
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_power.c
 * @brief Unit tests for the pure power helpers.
 *
 * Covers shell_power_parse_seconds() and shell_power_wake_cause_string()
 * from components/command/power_commands.c (promoted to command.h in
 * v0.35.5 for testability; previously static).
 */

#include "unity.h"
#include "command.h"
#include "p4minishell_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void test_power_parse_default(void)
{
    char *argv[] = { "sleep" };
    uint32_t secs = 0;

    TEST_ASSERT_TRUE(shell_power_parse_seconds(1, argv, &secs));
    TEST_ASSERT_EQUAL_UINT32(P4_CONFIG_POWER_SLEEP_DEFAULT_SECS, secs);
}

void test_power_parse_valid(void)
{
    char *argv[] = { "sleep", "30" };
    uint32_t secs = 0;

    TEST_ASSERT_TRUE(shell_power_parse_seconds(2, argv, &secs));
    TEST_ASSERT_EQUAL_UINT32(30, secs);
}

void test_power_parse_clamp_and_reject(void)
{
    char *big[] = { "sleep", "999999999" };
    char *neg[] = { "sleep", "-5" };
    char *junk[] = { "sleep", "abc" };
    char *trail[] = { "sleep", "10x" };
    uint32_t secs = 0;

    TEST_ASSERT_TRUE(shell_power_parse_seconds(2, big, &secs));
    TEST_ASSERT_EQUAL_UINT32(P4_CONFIG_POWER_SLEEP_MAX_SECS, secs);
    TEST_ASSERT_FALSE(shell_power_parse_seconds(2, neg, &secs));
    TEST_ASSERT_FALSE(shell_power_parse_seconds(2, junk, &secs));
    TEST_ASSERT_FALSE(shell_power_parse_seconds(2, trail, &secs));
}

void test_power_wake_cause_strings(void)
{
    TEST_ASSERT_EQUAL_STRING("timer", shell_power_wake_cause_string(ESP_SLEEP_WAKEUP_TIMER));
    TEST_ASSERT_EQUAL_STRING("gpio", shell_power_wake_cause_string(ESP_SLEEP_WAKEUP_GPIO));
    TEST_ASSERT_EQUAL_STRING("none", shell_power_wake_cause_string(ESP_SLEEP_WAKEUP_UNDEFINED));
}

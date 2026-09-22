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
#include "board_config.h"
#include "power_monitor.h"
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

/* Pack-presence classifier (bugs.md F24). The full-voltage/rail-band values
 * come from the board profile; the tests use the configured thresholds. */
void test_power_pack_present_below_present_is_absent(void)
{
    /* A real pack never reads below the present floor; the no-pack node does. */
    TEST_ASSERT_FALSE(power_monitor_pack_present(
        BOARD_CFG_BATTERY_PRESENT_MV - 1, 0, NULL, 0, -1, -1));
}

void test_power_pack_present_in_range_is_immediate(void)
{
    /* Clearly inside the pack range: trusted with no history. */
    int mid = (BOARD_CFG_BATTERY_EMPTY_MV + BOARD_CFG_BATTERY_FULL_MV) / 2;

    TEST_ASSERT_TRUE(power_monitor_pack_present(mid, 0, NULL, 0, -1, -1));
}

void test_power_pack_present_rail_band_needs_stable_window(void)
{
    int full = BOARD_CFG_BATTERY_FULL_MV;
    int stable[2] = { full - 50, full - 80 };

    /* No history yet: ambiguous rail reading is rejected. */
    TEST_ASSERT_FALSE(power_monitor_pack_present(full, 0, NULL, 0, -1, -1));
    /* A prior low (no-pack) sample poisons the window. */
    {
        int poisoned[2] = { BOARD_CFG_BATTERY_PRESENT_MV - 500, full };
        TEST_ASSERT_FALSE(power_monitor_pack_present(full, 0, poisoned, 2, -1, -1));
    }
    /* A full, stable, plausible window is accepted. */
    TEST_ASSERT_TRUE(power_monitor_pack_present(full, 0, stable, 2, -1, -1));
}

void test_power_pack_present_rail_band_unstable_is_absent(void)
{
    int full = BOARD_CFG_BATTERY_FULL_MV;
    /* A floating node swings volts (e.g. 8.4 <-> 3.9). */
    int swinging[2] = { BOARD_CFG_BATTERY_PRESENT_MV + 200, full - 40 };

    TEST_ASSERT_FALSE(power_monitor_pack_present(full, 0, swinging, 2, -1, -1));
}

void test_power_pack_present_chg_stat_corroboration(void)
{
    int full = BOARD_CFG_BATTERY_FULL_MV;
    int stable[2] = { full - 50, full - 80 };

    /* Stable voltage, but the charge-status line toggles with ~0 current:
     * charger blinking into no pack -> absent. */
    TEST_ASSERT_FALSE(power_monitor_pack_present(full, 0, stable, 2, 1, 0));
    /* Same toggle while charging (non-zero current) is a present pack. */
    TEST_ASSERT_TRUE(power_monitor_pack_present(full, -500, stable, 2, 1, 0));
    /* Unavailable status line never blocks a stable pack. */
    TEST_ASSERT_TRUE(power_monitor_pack_present(full, 0, stable, 2, -1, -1));
}

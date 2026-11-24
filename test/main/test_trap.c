/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_trap.c
 * @brief Unit tests for the `trap` verb and its engine hook.
 *
 * The trap state is pure C (per-task handler stack + two ordinary environment
 * variables). These cases cover the defects that previously made `trap`
 * inert: an installed handler was never fired, `/raise` wrote read-only
 * virtual slots, and trap state was never reset at batch_init().
 */

#include "unity.h"
#include "batch.h"

static void reset_trap_state(void)
{
    trap_init();
    batch_set_errorlevel(0);
    (void)shell_env_set("TRAP_CODE", "");
    (void)shell_env_set("TRAP_MSG", "");
}

void test_trap_install_and_clear(void)
{
    char *argv[2];

    reset_trap_state();
    TEST_ASSERT_FALSE(trap_handler_installed());

    argv[0] = "trap";
    argv[1] = ":recover";
    shell_command_trap(2, argv);
    TEST_ASSERT_TRUE(trap_handler_installed());
    TEST_ASSERT_EQUAL_STRING(":recover", trap_handler_label());
    TEST_ASSERT_EQUAL_INT(0, batch_get_errorlevel());

    argv[1] = "/clear";
    shell_command_trap(2, argv);
    TEST_ASSERT_FALSE(trap_handler_installed());
}

void test_trap_raise_publishes_ordinary_variables(void)
{
    char *argv[4];
    const char *value;

    reset_trap_state();
    argv[0] = "trap";
    argv[1] = "/raise";
    argv[2] = "3";
    argv[3] = "bad input";
    shell_command_trap(4, argv);

    TEST_ASSERT_EQUAL_INT(3, batch_get_errorlevel());
    value = shell_env_get("TRAP_CODE");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_STRING("3", value);
    value = shell_env_get("TRAP_MSG");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_STRING("bad input", value);
}

void test_trap_fire_is_one_shot(void)
{
    char *argv[2];

    reset_trap_state();
    argv[0] = "trap";
    argv[1] = ":recover";
    shell_command_trap(2, argv);

    batch_set_errorlevel(2);
    TEST_ASSERT_TRUE(batch_trap_maybe_fire());
    TEST_ASSERT_FALSE(trap_handler_installed());
    TEST_ASSERT_EQUAL_INT(0, batch_get_errorlevel());

    /* A second failure must not re-enter a consumed handler. */
    batch_set_errorlevel(2);
    TEST_ASSERT_FALSE(batch_trap_maybe_fire());
}

void test_trap_ignores_success(void)
{
    char *argv[2];

    reset_trap_state();
    argv[0] = "trap";
    argv[1] = ":recover";
    shell_command_trap(2, argv);

    batch_set_errorlevel(0);
    TEST_ASSERT_FALSE(batch_trap_maybe_fire());
    TEST_ASSERT_TRUE(trap_handler_installed());
}

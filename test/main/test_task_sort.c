/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_task_sort.c
 * @brief Unit tests for the pure `ps`/`top` `/O:` row comparator.
 *
 * Covers shell_task_row_compare() from components/shell/shell.c, the sort
 * comparator behind the DOS-style `/O:` ordering switches added in v0.24.28.
 * It is pure (no I/O, no FreeRTOS calls) so it is tested directly.
 */

#include "unity.h"
#include "shell.h"
#include <string.h>

static shell_task_row_t row(const char *name, const char *state,
                            unsigned int prio, uint32_t highwater, int cpu)
{
    shell_task_row_t r;

    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "%s", name);
    snprintf(r.state, sizeof(r.state), "%s", state);
    r.priority = prio;
    r.highwater_bytes = highwater;
    r.cpu_percent = cpu;
    return r;
}

void test_task_sort_by_name(void)
{
    shell_task_row_t a = row("zeta", "RDY", 1, 100, 5);
    shell_task_row_t b = row("alpha", "RUN", 1, 200, 90);

    TEST_ASSERT_TRUE(shell_task_row_compare(&a, &b, SHELL_TASK_SORT_NAME, false) > 0);
    TEST_ASSERT_TRUE(shell_task_row_compare(&b, &a, SHELL_TASK_SORT_NAME, false) < 0);
    /* Reverse flips the order. */
    TEST_ASSERT_TRUE(shell_task_row_compare(&a, &b, SHELL_TASK_SORT_NAME, true) < 0);
}

void test_task_sort_by_cpu(void)
{
    shell_task_row_t low = row("idle", "BLK", 0, 100, 1);
    shell_task_row_t high = row("worker", "RUN", 20, 100, 95);

    TEST_ASSERT_TRUE(shell_task_row_compare(&low, &high, SHELL_TASK_SORT_CPU, false) < 0);
    TEST_ASSERT_TRUE(shell_task_row_compare(&high, &low, SHELL_TASK_SORT_CPU, false) > 0);
    /* `top` uses CPU descending, which is reverse. */
    TEST_ASSERT_TRUE(shell_task_row_compare(&low, &high, SHELL_TASK_SORT_CPU, true) > 0);
}

void test_task_sort_by_stack(void)
{
    shell_task_row_t big = row("tall", "RDY", 1, 4096, 5);
    shell_task_row_t small = row("tiny", "RDY", 1, 512, 5);

    TEST_ASSERT_TRUE(shell_task_row_compare(&big, &small, SHELL_TASK_SORT_STACK, false) > 0);
    TEST_ASSERT_TRUE(shell_task_row_compare(&small, &big, SHELL_TASK_SORT_STACK, true) > 0);
}

void test_task_sort_by_priority(void)
{
    shell_task_row_t low = row("idle", "BLK", 1, 100, 5);
    shell_task_row_t high = row("sys", "RDY", 24, 100, 5);

    TEST_ASSERT_TRUE(shell_task_row_compare(&low, &high, SHELL_TASK_SORT_PRIORITY, false) < 0);
    TEST_ASSERT_TRUE(shell_task_row_compare(&high, &low, SHELL_TASK_SORT_PRIORITY, false) > 0);
}

void test_task_sort_by_state(void)
{
    shell_task_row_t run = row("a", "RUN", 1, 100, 5);
    shell_task_row_t blk = row("b", "BLK", 1, 100, 5);

    /* "BLK" < "RUN" alphabetically. */
    TEST_ASSERT_TRUE(shell_task_row_compare(&blk, &run, SHELL_TASK_SORT_STATE, false) < 0);
    TEST_ASSERT_TRUE(shell_task_row_compare(&run, &blk, SHELL_TASK_SORT_STATE, false) > 0);
}

void test_task_sort_name_tie_break(void)
{
    /* Equal CPU: the name decides the order deterministically. */
    shell_task_row_t a = row("beta", "RDY", 1, 100, 42);
    shell_task_row_t b = row("alpha", "RDY", 1, 100, 42);

    TEST_ASSERT_TRUE(shell_task_row_compare(&a, &b, SHELL_TASK_SORT_CPU, false) > 0);
    TEST_ASSERT_TRUE(shell_task_row_compare(&b, &a, SHELL_TASK_SORT_CPU, false) < 0);

    /* Equal stack and priority too. */
    TEST_ASSERT_TRUE(shell_task_row_compare(&a, &b, SHELL_TASK_SORT_STACK, false) > 0);
    TEST_ASSERT_TRUE(shell_task_row_compare(&a, &b, SHELL_TASK_SORT_PRIORITY, false) > 0);
}

void test_task_sort_null_safe(void)
{
    shell_task_row_t a = row("a", "RDY", 1, 100, 5);

    TEST_ASSERT_EQUAL_INT(0, shell_task_row_compare(NULL, &a, SHELL_TASK_SORT_NAME, false));
    TEST_ASSERT_EQUAL_INT(0, shell_task_row_compare(&a, NULL, SHELL_TASK_SORT_NAME, false));
    TEST_ASSERT_EQUAL_INT(0, shell_task_row_compare(NULL, NULL, SHELL_TASK_SORT_NAME, false));
}

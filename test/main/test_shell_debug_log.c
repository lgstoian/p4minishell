/**
 * @file test_shell_debug_log.c
 * @brief Unit tests for the shell debug log ring buffer and warning counter.
 *
 * Tests shell_debug_log_push(), shell_record_errorf(),
 * shell_record_warningf(), shell_record_infof(), and shell_get_warning_count().
 */

#include "unity.h"
#include "shell.h"
#include <string.h>

/* ========================================================================
 * DEBUG LOG RING BUFFER
 * ======================================================================== */

void test_debug_log_push_and_read(void)
{
    size_t before = shell_get_warning_count();

    /* Push an error entry. */
    shell_debug_log_push("test", "ERROR: something broke");

    /* The warning count should not change (errors are not warnings). */
    TEST_ASSERT_EQUAL(before, shell_get_warning_count());
}

void test_debug_log_warning_count(void)
{
    size_t before = shell_get_warning_count();

    shell_record_warningf("test", "warning message %d", 42);
    TEST_ASSERT_EQUAL(before + 1, shell_get_warning_count());

    shell_record_warningf("test", "another warning");
    TEST_ASSERT_EQUAL(before + 2, shell_get_warning_count());
}

void test_debug_log_error_does_not_increment_warning(void)
{
    size_t before = shell_get_warning_count();

    shell_record_errorf("test", 1, "error message");
    TEST_ASSERT_EQUAL(before, shell_get_warning_count());
}

void test_debug_log_info_does_not_increment_warning(void)
{
    size_t before = shell_get_warning_count();

    shell_record_infof("test", "info message");
    TEST_ASSERT_EQUAL(before, shell_get_warning_count());
}

/* ========================================================================
 * DEBUG LOG RING BEHAVIOR
 * ======================================================================== */

void test_debug_log_ring_overflow(void)
{
    /* Push more entries than the ring depth (SHELL_DEBUG_LOG_DEPTH = 5). */
    size_t before = shell_get_warning_count();

    shell_record_warningf("overflow", "entry %d", 1);
    shell_record_warningf("overflow", "entry %d", 2);
    shell_record_warningf("overflow", "entry %d", 3);
    shell_record_warningf("overflow", "entry %d", 4);
    shell_record_warningf("overflow", "entry %d", 5);
    shell_record_warningf("overflow", "entry %d", 6);
    shell_record_warningf("overflow", "entry %d", 7);

    /* Warning count should be exactly 7 (all warnings). */
    TEST_ASSERT_EQUAL(before + 7, shell_get_warning_count());

    /* The debug command should not crash with overflowed ring. */
    shell_command_debug();
}

/* ========================================================================
 * NULL SAFETY
 * ======================================================================== */

void test_debug_log_push_null(void)
{
    /* Should not crash. */
    shell_debug_log_push(NULL, "message");
    shell_debug_log_push("test", NULL);
    shell_debug_log_push(NULL, NULL);
}

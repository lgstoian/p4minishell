/**
 * @file test_main.c
 * @brief Unity test runner for P4MiniShell unit tests.
 *
 * Runs all registered test suites and reports results via the
 * standard Unity output format. Tests are designed to be
 * deterministic and run without hardware dependencies.
 */

#include "unity.h"
#include <stdio.h>

/* Forward declarations for all test suites */
extern void test_shell_parser_split_args(void);
extern void test_shell_parser_trim(void);
extern void test_shell_parser_text_equals(void);
extern void test_shell_parser_percentage_parse(void);

extern void test_shell_history_store(void);
extern void test_shell_history_recall(void);
extern void test_shell_history_password_mask(void);

extern void test_wifi_state_transitions(void);
extern void test_wifi_mutex(void);

extern void test_ansi_format_basic(void);
extern void test_ansi_format_colors(void);
extern void test_ansi_strip_to_plain(void);

void app_main(void)
{
    printf("\n=== P4MiniShell Unit Tests ===\n\n");

    /* Shell parser tests */
    UNITY_BEGIN();
    RUN_TEST(test_shell_parser_split_args);
    RUN_TEST(test_shell_parser_trim);
    RUN_TEST(test_shell_parser_text_equals);
    RUN_TEST(test_shell_parser_percentage_parse);
    UNITY_END();

    /* Shell history tests */
    UNITY_BEGIN();
    RUN_TEST(test_shell_history_store);
    RUN_TEST(test_shell_history_recall);
    RUN_TEST(test_shell_history_password_mask);
    UNITY_END();

    /* Wi-Fi state machine tests */
    UNITY_BEGIN();
    RUN_TEST(test_wifi_state_transitions);
    RUN_TEST(test_wifi_mutex);
    UNITY_END();

    /* ANSI format tests */
    UNITY_BEGIN();
    RUN_TEST(test_ansi_format_basic);
    RUN_TEST(test_ansi_format_colors);
    RUN_TEST(test_ansi_strip_to_plain);
    UNITY_END();

    printf("\n=== All tests completed ===\n");
}

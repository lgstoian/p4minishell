/**
 * @file test_main.c
 * @brief Unity test runner for P4MiniShell unit tests.
 *
 * Runs all registered test suites and reports results via the
 * standard Unity output format. Tests are designed to be
 * deterministic and run without hardware dependencies.
 */

#include "unity.h"
#include "shell.h"
#include "batch.h"
#include <stdio.h>

/* Forward declarations for all test suites */
extern void test_shell_parser_split_args(void);
extern void test_shell_parser_trim(void);
extern void test_shell_parser_text_equals(void);
extern void test_shell_parser_percentage_parse(void);

extern void test_shell_history_store(void);
extern void test_shell_history_recall(void);
extern void test_shell_history_password_mask(void);
extern void test_shell_format_command_masks_password(void);
extern void test_shell_format_command_passthrough(void);
extern void test_shell_format_command_handles_null(void);

extern void test_shell_prompt_template_roundtrip(void);
extern void test_shell_prompt_metacharacters(void);
extern void test_shell_prompt_path_expansion(void);
extern void test_shell_key_wait_state(void);

extern void test_shell_quoting_find_unquoted(void);
extern void test_shell_quoting_unescape(void);
extern void test_shell_quoting_split_args(void);
extern void test_shell_chain_split(void);

extern void test_pipe_detection_agreement(void);
extern void test_redirection_quote_awareness(void);
extern void test_chain_pipe_vs_chain_under_quotes(void);
extern void test_chain_single_quote_protection(void);
extern void test_chain_truncation_with_pipes(void);
extern void test_chain_empty_and_whitespace(void);
extern void test_find_unquoted_pipe(void);

extern void test_storage_format_size(void);
extern void test_storage_wildcard_match(void);
extern void test_storage_paths_are_same(void);
extern void test_storage_path_helpers(void);

extern void test_batch_expr_literals(void);
extern void test_batch_expr_arithmetic(void);
extern void test_batch_expr_bitwise(void);
extern void test_batch_expr_variables(void);
extern void test_batch_expr_errors(void);
extern void test_batch_expr_comparisons(void);
extern void test_batch_expr_logical(void);

extern void test_variable_expansion_env_var(void);
extern void test_variable_expansion_empty_name(void);
extern void test_variable_expansion_single_quotes(void);
extern void test_variable_expansion_caret_escape(void);
extern void test_variable_expansion_multiple(void);
extern void test_variable_expansion_output_truncated(void);
extern void test_variable_expansion_null_input(void);
extern void test_variable_expansion_null_output(void);
extern void test_variable_expansion_no_batch_frame(void);

extern void test_debug_log_push_and_read(void);
extern void test_debug_log_warning_count(void);
extern void test_debug_log_error_does_not_increment_warning(void);
extern void test_debug_log_info_does_not_increment_warning(void);
extern void test_debug_log_ring_overflow(void);
extern void test_debug_log_push_null(void);

extern void test_wifi_state_transitions(void);
extern void test_wifi_mutex(void);

extern void test_ansi_format_basic(void);
extern void test_ansi_format_colors(void);
extern void test_ansi_strip_to_plain(void);
extern void test_ansi_format_width_flags(void);
extern void test_ansi_to_lvgl_recolor(void);

void app_main(void)
{
    printf("\n=== P4MiniShell Unit Tests ===\n\n");

    /* The prompt renderer and the keypress queue are shell-core state, so
     * the module has to be initialized before those suites run. Transcript
     * writes are safe without LVGL: they land in the RAM buffer and the
     * widget pointer is NULL. */
    shell_init();

    /* The expression evaluator reads the batch module's environment table,
     * so that module must be initialized before those suites run. */
    batch_init();

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
    RUN_TEST(test_shell_format_command_masks_password);
    RUN_TEST(test_shell_format_command_passthrough);
    RUN_TEST(test_shell_format_command_handles_null);
    UNITY_END();

    /* Prompt template and keypress-wait tests */
    UNITY_BEGIN();
    RUN_TEST(test_shell_prompt_template_roundtrip);
    RUN_TEST(test_shell_prompt_metacharacters);
    RUN_TEST(test_shell_prompt_path_expansion);
    RUN_TEST(test_shell_key_wait_state);
    UNITY_END();

    /* Quoting, escaping, and command chaining tests */
    UNITY_BEGIN();
    RUN_TEST(test_shell_quoting_find_unquoted);
    RUN_TEST(test_shell_quoting_unescape);
    RUN_TEST(test_shell_quoting_split_args);
    RUN_TEST(test_shell_chain_split);
    UNITY_END();

    /* Pipeline detection, redirection awareness, and chain edge cases */
    UNITY_BEGIN();
    RUN_TEST(test_pipe_detection_agreement);
    RUN_TEST(test_redirection_quote_awareness);
    RUN_TEST(test_chain_pipe_vs_chain_under_quotes);
    RUN_TEST(test_chain_single_quote_protection);
    RUN_TEST(test_chain_truncation_with_pipes);
    RUN_TEST(test_chain_empty_and_whitespace);
    RUN_TEST(test_find_unquoted_pipe);
    UNITY_END();

    /* Storage formatting and matching tests */
    UNITY_BEGIN();
    RUN_TEST(test_storage_format_size);
    RUN_TEST(test_storage_wildcard_match);
    RUN_TEST(test_storage_paths_are_same);
    RUN_TEST(test_storage_path_helpers);
    UNITY_END();

    /* Batch arithmetic expression tests */
    UNITY_BEGIN();
    RUN_TEST(test_batch_expr_literals);
    RUN_TEST(test_batch_expr_arithmetic);
    RUN_TEST(test_batch_expr_bitwise);
    RUN_TEST(test_batch_expr_variables);
    RUN_TEST(test_batch_expr_comparisons);
    RUN_TEST(test_batch_expr_logical);
    RUN_TEST(test_batch_expr_errors);
    UNITY_END();

    /* Variable expansion tests */
    UNITY_BEGIN();
    RUN_TEST(test_variable_expansion_env_var);
    RUN_TEST(test_variable_expansion_empty_name);
    RUN_TEST(test_variable_expansion_single_quotes);
    RUN_TEST(test_variable_expansion_caret_escape);
    RUN_TEST(test_variable_expansion_multiple);
    RUN_TEST(test_variable_expansion_output_truncated);
    RUN_TEST(test_variable_expansion_null_input);
    RUN_TEST(test_variable_expansion_null_output);
    RUN_TEST(test_variable_expansion_no_batch_frame);
    UNITY_END();

    /* Debug log tests */
    UNITY_BEGIN();
    RUN_TEST(test_debug_log_push_and_read);
    RUN_TEST(test_debug_log_warning_count);
    RUN_TEST(test_debug_log_error_does_not_increment_warning);
    RUN_TEST(test_debug_log_info_does_not_increment_warning);
    RUN_TEST(test_debug_log_ring_overflow);
    RUN_TEST(test_debug_log_push_null);
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
    RUN_TEST(test_ansi_format_width_flags);
    RUN_TEST(test_ansi_to_lvgl_recolor);
    UNITY_END();

    printf("\n=== All tests completed ===\n");
}

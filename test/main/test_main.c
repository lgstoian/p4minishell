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
extern void test_shell_parser_count_args(void);
extern void test_shell_parser_text_equals(void);
extern void test_shell_parser_percentage_parse(void);
extern void test_redirect_capture_nested(void);

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

extern void test_findstr_regex_literal(void);
extern void test_findstr_regex_case(void);
extern void test_findstr_regex_anchors(void);
extern void test_findstr_regex_dot_and_star(void);
extern void test_findstr_regex_class(void);
extern void test_findstr_regex_escapes(void);
extern void test_findstr_match_literal(void);
extern void test_findstr_match_switches(void);
extern void test_findstr_match_regex(void);

extern void test_comp_identical(void);
extern void test_comp_byte_difference(void);
extern void test_comp_length_difference(void);
extern void test_comp_case(void);
extern void test_comp_mid_buffer_difference(void);

extern void test_task_sort_by_name(void);
extern void test_task_sort_by_cpu(void);
extern void test_task_sort_by_stack(void);
extern void test_task_sort_by_priority(void);
extern void test_task_sort_by_state(void);
extern void test_task_sort_name_tie_break(void);
extern void test_task_sort_null_safe(void);

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
extern void test_variable_expansion_pseudo_vars(void);
extern void test_variable_expansion_errorlevel(void);
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

extern void test_config_get_simple(void);
extern void test_config_get_case_and_spacing(void);
extern void test_config_get_comments_and_absent(void);
extern void test_config_get_prefix_does_not_match(void);
extern void test_config_upsert_replaces_existing(void);
extern void test_config_upsert_appends_when_absent(void);
extern void test_config_upsert_dedupes_multiple_lines(void);
extern void test_config_upsert_into_empty(void);
extern void test_config_upsert_overflow_refused(void);
extern void test_config_remove_existing(void);
extern void test_config_remove_absent(void);
extern void test_config_remove_multiple(void);
extern void test_config_remove_last_line_no_newline(void);

extern void test_editor_new_doc(void);
extern void test_editor_insert_and_cursor(void);
extern void test_editor_newline_split_and_join(void);
extern void test_editor_delete_forward(void);
extern void test_editor_overwrite_toggle(void);
extern void test_editor_delete_line_and_eol(void);
extern void test_editor_doc_home_end(void);
extern void test_editor_find_next(void);
extern void test_editor_replace_next(void);
extern void test_editor_undo_redo(void);
extern void test_editor_selection(void);
extern void test_editor_selection_copy_lf(void);
extern void test_editor_selection_copy_crlf(void);
extern void test_editor_set_path(void);
extern void test_editor_paste_multiline(void);
extern void test_editor_lex_batch(void);
extern void test_editor_newline_on_empty_doc(void);
extern void test_editor_selection_delete_empty_start(void);
extern void test_editor_word_nav_empty_line(void);
extern void test_editor_line_cap_bounded(void);
extern void test_editor_undo_redo_empty_last_line(void);
extern void test_editor_find_wrap_boundary(void);
extern void test_editor_undo_ring_wrap_free(void);
extern void test_editor_selection_delete_multirow_tail(void);
extern void test_editor_format_line_number(void);

extern void test_keyboard_osk_dedup(void);

extern void test_calc_arithmetic(void);
extern void test_calc_power_and_mod(void);
extern void test_calc_hex_literals(void);
extern void test_calc_math_functions(void);
extern void test_calc_trig_degrees(void);
extern void test_calc_trig_radians(void);
extern void test_calc_math_errors(void);
extern void test_calc_new_math_functions(void);
extern void test_calc_new_math_errors(void);
extern void test_calc_string_functions(void);
extern void test_calc_string_numbers(void);
extern void test_calc_string_errors(void);
extern void test_calc_env_variables(void);
extern void test_calc_command_assignment(void);
extern void test_calc_pol_rec_side_effects(void);
extern void test_calc_random(void);
extern void test_calc_format_number(void);
extern void test_calc_syntax_errors(void);
extern void test_forf_options_defaults(void);
extern void test_forf_parse_options(void);
extern void test_forf_parse_star(void);
extern void test_forf_parse_errors(void);
extern void test_forf_split_line(void);

extern void test_applib_alloc_free(void);
extern void test_applib_calloc_zeroes(void);
extern void test_applib_realloc(void);
extern void test_applib_strdup(void);
extern void test_applib_strndup(void);
extern void test_applib_free_null(void);
extern void test_applib_time_helpers(void);
extern void test_applib_uptime_formatted(void);
extern void test_applib_sysinfo(void);
extern void test_applib_printf(void);
extern void test_applib_wifi_ops(void);
extern void test_applib_input_timeout(void);
extern void test_applib_state(void);
extern void test_applib_menu_primitives(void);
extern void test_applib_read_password(void);
extern void test_applib_app_mode(void);
extern void test_applib_env_ops(void);
extern void test_applib_app_registry(void);

extern void test_db_name_valid_basic(void);
extern void test_db_name_valid_rejects_bad(void);
extern void test_db_name_valid_length(void);

extern void test_alarm_recur_weekday_matches(void);
extern void test_alarm_advance_daily(void);
extern void test_alarm_advance_none(void);
extern void test_alarm_advance_weekly_all_days(void);
extern void test_alarm_advance_weekly_single_day(void);
extern void test_alarm_parse_valid(void);
extern void test_alarm_parse_invalid(void);

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
    RUN_TEST(test_shell_parser_count_args);
    RUN_TEST(test_shell_parser_text_equals);
    RUN_TEST(test_shell_parser_percentage_parse);
    RUN_TEST(test_redirect_capture_nested);
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

    /* findstr regex / literal matcher tests */
    UNITY_BEGIN();
    RUN_TEST(test_findstr_regex_literal);
    RUN_TEST(test_findstr_regex_case);
    RUN_TEST(test_findstr_regex_anchors);
    RUN_TEST(test_findstr_regex_dot_and_star);
    RUN_TEST(test_findstr_regex_class);
    RUN_TEST(test_findstr_regex_escapes);
    RUN_TEST(test_findstr_match_literal);
    RUN_TEST(test_findstr_match_switches);
    RUN_TEST(test_findstr_match_regex);
    UNITY_END();

    /* comp byte-comparison helper tests */
    UNITY_BEGIN();
    RUN_TEST(test_comp_identical);
    RUN_TEST(test_comp_byte_difference);
    RUN_TEST(test_comp_length_difference);
    RUN_TEST(test_comp_case);
    RUN_TEST(test_comp_mid_buffer_difference);
    UNITY_END();

    /* ps/top /O: row comparator tests */
    UNITY_BEGIN();
    RUN_TEST(test_task_sort_by_name);
    RUN_TEST(test_task_sort_by_cpu);
    RUN_TEST(test_task_sort_by_stack);
    RUN_TEST(test_task_sort_by_priority);
    RUN_TEST(test_task_sort_by_state);
    RUN_TEST(test_task_sort_name_tie_break);
    RUN_TEST(test_task_sort_null_safe);
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
    RUN_TEST(test_variable_expansion_errorlevel);
    RUN_TEST(test_variable_expansion_pseudo_vars);
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

    /* CONFIG.SYS directive line-editing tests */
    UNITY_BEGIN();
    RUN_TEST(test_config_get_simple);
    RUN_TEST(test_config_get_case_and_spacing);
    RUN_TEST(test_config_get_comments_and_absent);
    RUN_TEST(test_config_get_prefix_does_not_match);
    RUN_TEST(test_config_upsert_replaces_existing);
    RUN_TEST(test_config_upsert_appends_when_absent);
    RUN_TEST(test_config_upsert_dedupes_multiple_lines);
    RUN_TEST(test_config_upsert_into_empty);
    RUN_TEST(test_config_upsert_overflow_refused);
    RUN_TEST(test_config_remove_existing);
    RUN_TEST(test_config_remove_absent);
    RUN_TEST(test_config_remove_multiple);
    RUN_TEST(test_config_remove_last_line_no_newline);
    UNITY_END();

    /* Editor document-model tests */
    UNITY_BEGIN();
    RUN_TEST(test_editor_new_doc);
    RUN_TEST(test_editor_insert_and_cursor);
    RUN_TEST(test_editor_newline_split_and_join);
    RUN_TEST(test_editor_delete_forward);
    RUN_TEST(test_editor_overwrite_toggle);
    RUN_TEST(test_editor_delete_line_and_eol);
    RUN_TEST(test_editor_doc_home_end);
    RUN_TEST(test_editor_find_next);
    RUN_TEST(test_editor_replace_next);
    RUN_TEST(test_editor_undo_redo);
    RUN_TEST(test_editor_selection);
    RUN_TEST(test_editor_selection_copy_lf);
    RUN_TEST(test_editor_selection_copy_crlf);
    RUN_TEST(test_editor_set_path);
    RUN_TEST(test_editor_paste_multiline);
    RUN_TEST(test_editor_lex_batch);
    RUN_TEST(test_editor_newline_on_empty_doc);
    RUN_TEST(test_editor_selection_delete_empty_start);
    RUN_TEST(test_editor_word_nav_empty_line);
    RUN_TEST(test_editor_line_cap_bounded);
    RUN_TEST(test_editor_undo_redo_empty_last_line);
    RUN_TEST(test_editor_find_wrap_boundary);
    RUN_TEST(test_editor_undo_ring_wrap_free);
    RUN_TEST(test_editor_selection_delete_multirow_tail);
    RUN_TEST(test_editor_format_line_number);
    UNITY_END();

    /* On-screen keyboard input deduplication tests */
    UNITY_BEGIN();
    RUN_TEST(test_keyboard_osk_dedup);
    UNITY_END();

    /* `calc` float evaluator tests */
    UNITY_BEGIN();
    RUN_TEST(test_calc_arithmetic);
    RUN_TEST(test_calc_power_and_mod);
    RUN_TEST(test_calc_hex_literals);
    RUN_TEST(test_calc_math_functions);
    RUN_TEST(test_calc_trig_degrees);
    RUN_TEST(test_calc_trig_radians);
    RUN_TEST(test_calc_math_errors);
    RUN_TEST(test_calc_new_math_functions);
    RUN_TEST(test_calc_new_math_errors);
    RUN_TEST(test_calc_string_functions);
    RUN_TEST(test_calc_string_numbers);
    RUN_TEST(test_calc_string_errors);
    RUN_TEST(test_calc_env_variables);
    RUN_TEST(test_calc_command_assignment);
    RUN_TEST(test_calc_pol_rec_side_effects);
    RUN_TEST(test_calc_random);
    RUN_TEST(test_calc_format_number);
    RUN_TEST(test_calc_syntax_errors);
    UNITY_END();

    /* `for /f` option parser / line splitter tests */
    UNITY_BEGIN();
    RUN_TEST(test_forf_options_defaults);
    RUN_TEST(test_forf_parse_options);
    RUN_TEST(test_forf_parse_star);
    RUN_TEST(test_forf_parse_errors);
    RUN_TEST(test_forf_split_line);
    UNITY_END();

    /* applib native-app runtime library tests */
    UNITY_BEGIN();
    RUN_TEST(test_applib_alloc_free);
    RUN_TEST(test_applib_calloc_zeroes);
    RUN_TEST(test_applib_realloc);
    RUN_TEST(test_applib_strdup);
    RUN_TEST(test_applib_strndup);
    RUN_TEST(test_applib_free_null);
    RUN_TEST(test_applib_time_helpers);
    RUN_TEST(test_applib_uptime_formatted);
    RUN_TEST(test_applib_sysinfo);
    RUN_TEST(test_applib_printf);
    RUN_TEST(test_applib_wifi_ops);
    RUN_TEST(test_applib_input_timeout);
    RUN_TEST(test_applib_state);
    RUN_TEST(test_applib_menu_primitives);
    RUN_TEST(test_applib_read_password);
    RUN_TEST(test_applib_app_mode);
    RUN_TEST(test_applib_env_ops);
    RUN_TEST(test_applib_app_registry);
    UNITY_END();

    /* Palm-OS-style database (components/db) tests — pure logic only; the
     * SD-backed operations are verified on hardware (see db_test.py). */
    UNITY_BEGIN();
    RUN_TEST(test_db_name_valid_basic);
    RUN_TEST(test_db_name_valid_rejects_bad);
    RUN_TEST(test_db_name_valid_length);
    UNITY_END();

    /* Alarm (components/alarm) pure time/recurrence helpers. */
    UNITY_BEGIN();
    RUN_TEST(test_alarm_recur_weekday_matches);
    RUN_TEST(test_alarm_advance_daily);
    RUN_TEST(test_alarm_advance_none);
    RUN_TEST(test_alarm_advance_weekly_all_days);
    RUN_TEST(test_alarm_advance_weekly_single_day);
    RUN_TEST(test_alarm_parse_valid);
    RUN_TEST(test_alarm_parse_invalid);
    UNITY_END();

    printf("\n=== All tests completed ===\n");
}

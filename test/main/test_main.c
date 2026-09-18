/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
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
extern void test_shell_abort_initially_clear(void);
extern void test_shell_abort_request_and_clear(void);
extern void test_shell_command_busy_tracks(void);
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
extern void test_batch_label_is_line(void);
extern void test_batch_label_extract(void);
extern void test_batch_on_parse(void);
extern void test_batch_on_select(void);
extern void test_batch_on_dispatch_selection(void);

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
extern void test_variable_expansion_tilde_modifiers(void);

extern void test_debug_log_push_and_read(void);
extern void test_debug_log_warning_count(void);
extern void test_debug_log_error_does_not_increment_warning(void);
extern void test_debug_log_info_does_not_increment_warning(void);
extern void test_debug_log_ring_overflow(void);
extern void test_debug_log_push_null(void);
extern void test_debug_get_entry_order_and_range(void);
extern void test_wifi_state_transitions(void);
extern void test_wifi_mutex(void);

extern void test_ansi_format_basic(void);
extern void test_ansi_format_colors(void);
extern void test_ansi_strip_to_plain(void);
extern void test_ansi_format_width_flags(void);
extern void test_ansi_to_lvgl_recolor(void);
extern void test_ansi_csi_trailing_complete(void);
extern void test_ansi_csi_trailing_split(void);

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

extern void test_editor_osk_key_from_label(void);
extern void test_editor_word_count(void);
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
extern void test_editor_lex_markdown(void);
extern void test_editor_lex_json(void);
extern void test_editor_replace_all(void);
extern void test_editor_comment_toggle(void);
extern void test_editor_match_jump(void);
extern void test_editor_undo_restores_clean(void);
extern void test_editor_newline_auto_indent(void);
extern void test_editor_newline_on_empty_doc(void);
extern void test_editor_selection_delete_empty_start(void);
extern void test_editor_word_nav_empty_line(void);
extern void test_editor_line_cap_bounded(void);
extern void test_editor_undo_redo_empty_last_line(void);
extern void test_editor_find_wrap_boundary(void);
extern void test_editor_undo_ring_wrap_free(void);
extern void test_editor_selection_delete_multirow_tail(void);
extern void test_editor_format_line_number(void);

extern void test_keyboard_mode_names(void);
extern void test_keyboard_mode_parse(void);
extern void test_keyboard_capability_mapping(void);
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
extern void test_calc_base_and_units(void);
extern void test_calc_env_variables(void);
extern void test_calc_command_assignment(void);
extern void test_calc_pol_rec_side_effects(void);
extern void test_calc_random(void);
extern void test_calc_format_number(void);
extern void test_calc_syntax_errors(void);
extern void test_calc_financial_tvm(void);
extern void test_calc_financial_npv_irr(void);
extern void test_calc_financial_depreciation(void);
extern void test_calc_financial_errors(void);
extern void test_calc_dates(void);
extern void test_calc_date_today_roundtrip(void);
extern void test_calc_date_errors(void);
extern void test_import_vcf_prop_split_plain(void);
extern void test_import_vcf_prop_split_params(void);
extern void test_import_vcf_prop_split_group(void);
extern void test_import_vcf_prop_split_first_colon(void);
extern void test_import_vcf_prop_split_rejects(void);
extern void test_import_json_unescape_basic(void);
extern void test_import_json_unescape_unicode(void);
extern void test_import_json_unescape_safety(void);
extern void test_import_ics_datetime_full(void);
extern void test_import_ics_datetime_forms(void);
extern void test_import_ics_datetime_rejects(void);
extern void test_forf_options_defaults(void);
extern void test_forf_parse_options(void);
extern void test_forf_parse_star(void);
extern void test_forf_parse_errors(void);
extern void test_forf_split_line(void);
extern void test_forf_command_set(void);
extern void test_arg_apply_modifiers(void);

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
extern void test_db_field_get_basic(void);
extern void test_db_field_get_edges(void);

extern void test_alarm_recur_weekday_matches(void);
extern void test_alarm_advance_daily(void);
extern void test_alarm_advance_none(void);
extern void test_alarm_advance_weekly_all_days(void);
extern void test_alarm_advance_weekly_single_day(void);
extern void test_alarm_parse_valid(void);
extern void test_alarm_parse_invalid(void);
extern void test_alarm_advance_monthly_by_day(void);
extern void test_alarm_advance_monthly_nth(void);
extern void test_alarm_advance_yearly(void);
extern void test_alarm_advance_event_dispatch(void);

extern void test_modal_timeout_basic(void);
extern void test_modal_timeout_case_and_zero(void);
extern void test_modal_timeout_rejects(void);
extern void test_modal_var_basic(void);
extern void test_modal_var_rejects(void);

extern void test_power_parse_default(void);
extern void test_power_parse_valid(void);
extern void test_power_parse_clamp_and_reject(void);
extern void test_power_wake_cause_strings(void);

extern void test_serial_bmp_headers(void);
extern void test_serial_bmp_headers_small(void);

extern void test_tui_default_color_roundtrip(void);
extern void test_tui_default_color_null_safe(void);
extern void test_tui_cursor_save_restore(void);
extern void test_tui_inactive_by_default(void);
extern void test_tui_rgb_to_dos_exact(void);
extern void test_tui_rgb_to_dos_nearest(void);
extern void test_tui_table_total_width(void);
extern void test_draw_hold_default_off(void);
extern void test_tui_table_parse_cursor(void);
extern void test_tui_table_parse_sel(void);
extern void test_tui_scroll_up_inactive_safe(void);

extern void test_clipboard_set_get(void);
extern void test_clipboard_empty_and_null(void);
extern void test_clipboard_file_flag(void);
extern void test_clipboard_copy_transcript(void);

extern void test_history_file_roundtrip(void);
extern void test_history_file_skips_blanks(void);
extern void test_history_file_null_stream(void);

extern void test_completion_empty_line(void);
extern void test_completion_first_token_includes_help_names(void);
extern void test_completion_match_index_bounds(void);
extern void test_ghost_first_token_prefix(void);
extern void test_ghost_usage_flag(void);

extern void test_history_search_matches_basic(void);
extern void test_history_search_matches_null(void);
extern void test_history_generation_advances(void);
extern void test_markdown_plain_passthrough(void);
extern void test_markdown_emphasis(void);
extern void test_markdown_links(void);
extern void test_markdown_blocks(void);
extern void test_markdown_doc_tables(void);
extern void test_markdown_doc_fences(void);
extern void test_markdown_display_width(void);
extern void test_markdown_render_html(void);
extern void test_markdown_render_print(void);extern void test_filetype_batch(void);
extern void test_filetype_markdown(void);
extern void test_filetype_json_text(void);
extern void test_filetype_unknown(void);
extern void test_filetype_image(void);
extern void test_filetype_has_extension(void);
extern void test_json_validate_ok(void);
extern void test_json_validate_bad(void);
extern void test_json_pretty(void);
extern void test_gfx_rgb_to_565(void);
extern void test_gfx_alloc_bounds(void);
extern void test_gfx_pixel_clip(void);
extern void test_gfx_line_endpoints(void);
extern void test_gfx_rect_fill_and_border(void);
extern void test_gfx_circle_outline_and_fill(void);
extern void test_gfx_bmp_parse_ok(void);
extern void test_gfx_bmp_parse_rejects(void);
extern void test_gfx_bmp_decode_565(void);
extern void test_gfx_bmp_ex_32bit_top_down(void);
extern void test_gfx_bmp_ex_large_and_scaled(void);
extern void test_gfx_bmp_fit(void);
extern void test_gfx_blit_scaled(void);
extern void test_gfx_blit_clip_transparent(void);
extern void test_gfx_565_to_888_row(void);
extern void test_gfx_hline_vline_clip(void);
extern void test_gfx_triangle_fill_outline(void);
extern void test_gfx_polygon_fill_outline(void);
extern void test_gfx_ellipse(void);
extern void test_gfx_flood_fill(void);
extern void test_gfx_font_table(void);
extern void test_gfx_text_render(void);
extern void test_gfx_view_map(void);
extern void test_gfx_view_clip_line(void);
extern void test_gfx_view_line_draws(void);
extern void test_gfx_view_nice_step(void);
extern void test_gfx_frame_stats_intervals(void);
extern void test_gfx_frame_stats_dropped(void);
extern void test_asset_crc32_vectors(void);
extern void test_csv_split_simple(void);
extern void test_csv_split_quoted(void);
extern void test_csv_split_empty(void);
extern void test_csv_split_over_max(void);
extern void test_csv_split_exhaustion(void);
extern void test_csv_substitute_refs(void);
extern void test_csv_substitute_ranges(void);
extern void test_csv_format_field(void);
extern void test_bind_set_and_lookup(void);
extern void test_bind_rejects_non_fkey(void);
extern void test_bind_get_by_index(void);
extern void test_bind_chord_set_and_lookup(void);
extern void test_bind_chord_rejects(void);
extern void test_macro_idle_paths(void);
extern void test_macro_record_and_exclude(void);
extern void test_macro_record_overflow(void);
extern void test_macro_double_record(void);
extern void test_crypt_derive_deterministic(void);
extern void test_crypt_mem_roundtrip(void);
extern void test_crypt_mem_rejects(void);
extern void test_tcp_parse_target_ok(void);
extern void test_tcp_parse_target_rejects(void);
extern void test_tcp_unescape(void);
extern void test_tcp_sanitize(void);
extern void test_userial_parse_id_ok(void);
extern void test_userial_parse_id_rejects(void);
extern void test_userial_parse_coding_defaults(void);
extern void test_userial_parse_coding_values(void);
extern void test_userial_parse_coding_rejects(void);
extern void test_asset_parse_ok(void);
extern void test_asset_parse_skip(void);
extern void test_asset_parse_bad(void);
extern void test_pkg_app_name_from_appinfo_ok(void);
extern void test_pkg_app_name_from_appinfo_bad(void);
extern void test_theme_registry(void);
extern void test_theme_lookup(void);
extern void test_theme_set_and_active(void);
extern void test_header_layout_all_full(void);
extern void test_header_layout_compacts_sides(void);
extern void test_header_layout_center_yields_first(void);
extern void test_header_layout_requests_smaller_font(void);
extern void test_header_layout_degenerate(void);
extern void test_header_status_glyphs(void);
extern void test_header_status_wifi(void);
extern void test_header_status_bt_usb(void);
extern void test_header_status_sd(void);
extern void test_header_status_system_tone(void);
extern void test_header_notify_queue_fifo(void);
extern void test_header_notify_queue_overflow_drops_oldest(void);
extern void test_header_notify_queue_clear_and_blank(void);
extern void test_header_refresh_priority(void);
extern void test_header_refresh_clock_and_idle_off(void);
extern void test_clock_format_hm_snapshot(void);
extern void test_clock_timer_start_stop(void);
extern void test_clock_timer_lap_and_status(void);
extern void test_clock_timer_usage_and_restart(void);
extern void test_clock_rtc_restore_math(void);
extern void test_clock_rtc_bcd(void);
extern void test_archive_crc32_reference(void);
extern void test_archive_crc32_incremental(void);
extern void test_archive_octal_roundtrip(void);
extern void test_archive_unoctal_rejects(void);
extern void test_archive_header_roundtrip(void);
extern void test_archive_header_dir_and_split(void);
extern void test_archive_header_rejects(void);
extern void test_archive_entry_fits(void);
extern void test_archive_path_safe(void);
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
    RUN_TEST(test_shell_abort_initially_clear);
    RUN_TEST(test_shell_abort_request_and_clear);
    RUN_TEST(test_shell_command_busy_tracks);
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
    RUN_TEST(test_batch_label_is_line);
    RUN_TEST(test_batch_label_extract);
    RUN_TEST(test_batch_on_parse);
    RUN_TEST(test_batch_on_select);
    RUN_TEST(test_batch_on_dispatch_selection);
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
    RUN_TEST(test_variable_expansion_tilde_modifiers);
    UNITY_END();

    /* Debug log tests */
    UNITY_BEGIN();
    RUN_TEST(test_debug_log_push_and_read);
    RUN_TEST(test_debug_log_warning_count);
    RUN_TEST(test_debug_log_error_does_not_increment_warning);
    RUN_TEST(test_debug_log_info_does_not_increment_warning);
    RUN_TEST(test_debug_log_ring_overflow);
    RUN_TEST(test_debug_log_push_null);
    RUN_TEST(test_debug_get_entry_order_and_range);
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
    RUN_TEST(test_ansi_csi_trailing_complete);
    RUN_TEST(test_ansi_csi_trailing_split);
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
    RUN_TEST(test_editor_osk_key_from_label);
    RUN_TEST(test_editor_word_count);
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
    RUN_TEST(test_editor_lex_json);
    RUN_TEST(test_editor_replace_all);
    RUN_TEST(test_editor_comment_toggle);
    RUN_TEST(test_editor_match_jump);
    RUN_TEST(test_editor_undo_restores_clean);
    RUN_TEST(test_editor_newline_auto_indent);
    RUN_TEST(test_editor_lex_markdown);
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
    RUN_TEST(test_keyboard_mode_names);
    RUN_TEST(test_keyboard_mode_parse);
    RUN_TEST(test_keyboard_capability_mapping);
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
    RUN_TEST(test_calc_base_and_units);
    RUN_TEST(test_calc_env_variables);
    RUN_TEST(test_calc_command_assignment);
    RUN_TEST(test_calc_pol_rec_side_effects);
    RUN_TEST(test_calc_random);
    RUN_TEST(test_calc_format_number);
    RUN_TEST(test_calc_syntax_errors);
    RUN_TEST(test_calc_financial_tvm);
    RUN_TEST(test_calc_financial_npv_irr);
    RUN_TEST(test_calc_financial_depreciation);
    RUN_TEST(test_calc_financial_errors);
    RUN_TEST(test_calc_dates);
    RUN_TEST(test_calc_date_today_roundtrip);
    RUN_TEST(test_calc_date_errors);
    UNITY_END();

    /* Import interchange parsers (components/command/import_commands.c). */
    UNITY_BEGIN();
    RUN_TEST(test_import_vcf_prop_split_plain);
    RUN_TEST(test_import_vcf_prop_split_params);
    RUN_TEST(test_import_vcf_prop_split_group);
    RUN_TEST(test_import_vcf_prop_split_first_colon);
    RUN_TEST(test_import_vcf_prop_split_rejects);
    RUN_TEST(test_import_json_unescape_basic);
    RUN_TEST(test_import_json_unescape_unicode);
    RUN_TEST(test_import_json_unescape_safety);
    RUN_TEST(test_import_ics_datetime_full);
    RUN_TEST(test_import_ics_datetime_forms);
    RUN_TEST(test_import_ics_datetime_rejects);
    UNITY_END();

    /* `for /f` option parser / line splitter tests */
    UNITY_BEGIN();
    RUN_TEST(test_forf_options_defaults);
    RUN_TEST(test_forf_parse_options);
    RUN_TEST(test_forf_parse_star);
    RUN_TEST(test_forf_parse_errors);
    RUN_TEST(test_forf_split_line);
    RUN_TEST(test_forf_command_set);
    RUN_TEST(test_arg_apply_modifiers);
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
    RUN_TEST(test_db_field_get_basic);
    RUN_TEST(test_db_field_get_edges);
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
    RUN_TEST(test_alarm_advance_monthly_by_day);
    RUN_TEST(test_alarm_advance_monthly_nth);
    RUN_TEST(test_alarm_advance_yearly);
    RUN_TEST(test_alarm_advance_event_dispatch);
    UNITY_END();

    /* Modal option parsers (components/modal shared by all TUI verbs). */
    UNITY_BEGIN();
    RUN_TEST(test_modal_timeout_basic);
    RUN_TEST(test_modal_timeout_case_and_zero);
    RUN_TEST(test_modal_timeout_rejects);
    RUN_TEST(test_modal_var_basic);
    RUN_TEST(test_modal_var_rejects);
    UNITY_END();

    /* Power helpers (components/command/power_commands.c). */
    UNITY_BEGIN();
    RUN_TEST(test_power_parse_default);
    RUN_TEST(test_power_parse_valid);
    RUN_TEST(test_power_parse_clamp_and_reject);
    RUN_TEST(test_power_wake_cause_strings);
    UNITY_END();

    /* BMP header writer (components/command/serial_commands.c). */
    UNITY_BEGIN();
    RUN_TEST(test_serial_bmp_headers);
    RUN_TEST(test_serial_bmp_headers_small);
    UNITY_END();

    /* Headless-safe TUI state (components/tui). */
    UNITY_BEGIN();
    RUN_TEST(test_tui_default_color_roundtrip);
    RUN_TEST(test_tui_default_color_null_safe);
    RUN_TEST(test_tui_cursor_save_restore);
    RUN_TEST(test_tui_inactive_by_default);
    RUN_TEST(test_tui_rgb_to_dos_exact);
    RUN_TEST(test_tui_rgb_to_dos_nearest);
    RUN_TEST(test_tui_table_total_width);
    RUN_TEST(test_draw_hold_default_off);
    RUN_TEST(test_tui_table_parse_cursor);
    RUN_TEST(test_tui_table_parse_sel);
    RUN_TEST(test_tui_scroll_up_inactive_safe);
    UNITY_END();

    /* RAM clipboard (components/shell). */
    UNITY_BEGIN();
    RUN_TEST(test_clipboard_set_get);
    RUN_TEST(test_clipboard_empty_and_null);
    RUN_TEST(test_clipboard_file_flag);
    RUN_TEST(test_clipboard_copy_transcript);
    UNITY_END();

    /* History file format (components/shell helpers). */
    UNITY_BEGIN();
    RUN_TEST(test_history_file_roundtrip);
    RUN_TEST(test_history_file_skips_blanks);
    RUN_TEST(test_history_file_null_stream);
    UNITY_END();

    /* Completion providers + reverse-search matching (components/command,
     * components/shell). */
    UNITY_BEGIN();
    RUN_TEST(test_completion_empty_line);
    RUN_TEST(test_completion_first_token_includes_help_names);
    RUN_TEST(test_completion_match_index_bounds);
    RUN_TEST(test_ghost_first_token_prefix);
    RUN_TEST(test_ghost_usage_flag);
    RUN_TEST(test_history_search_matches_basic);
    RUN_TEST(test_history_search_matches_null);
    RUN_TEST(test_history_generation_advances);
    UNITY_END();

    /* Markdown rendering (components/markdown). */
    UNITY_BEGIN();
    RUN_TEST(test_markdown_plain_passthrough);
    RUN_TEST(test_markdown_emphasis);
    RUN_TEST(test_markdown_links);
    RUN_TEST(test_markdown_blocks);
    RUN_TEST(test_markdown_doc_tables);
    RUN_TEST(test_markdown_doc_fences);
    RUN_TEST(test_markdown_display_width);
    RUN_TEST(test_markdown_render_html);
    RUN_TEST(test_markdown_render_print);
    UNITY_END();

    /* File-type registry (components/filetype). */
    UNITY_BEGIN();
    RUN_TEST(test_filetype_batch);
    RUN_TEST(test_filetype_markdown);
    RUN_TEST(test_filetype_json_text);
    RUN_TEST(test_filetype_unknown);
    RUN_TEST(test_filetype_image);
    RUN_TEST(test_filetype_has_extension);
    UNITY_END();

    /* JSON validate/pretty core (command component). */
    UNITY_BEGIN();
    RUN_TEST(test_json_validate_ok);
    RUN_TEST(test_json_validate_bad);
    RUN_TEST(test_json_pretty);
    UNITY_END();

    /* RGB565 raster core (components/gfx). */
    UNITY_BEGIN();
    RUN_TEST(test_gfx_rgb_to_565);
    RUN_TEST(test_gfx_alloc_bounds);
    RUN_TEST(test_gfx_pixel_clip);
    RUN_TEST(test_gfx_line_endpoints);
    RUN_TEST(test_gfx_rect_fill_and_border);
    RUN_TEST(test_gfx_circle_outline_and_fill);
    RUN_TEST(test_gfx_bmp_parse_ok);
    RUN_TEST(test_gfx_bmp_parse_rejects);
    RUN_TEST(test_gfx_bmp_decode_565);
    RUN_TEST(test_gfx_bmp_ex_32bit_top_down);
    RUN_TEST(test_gfx_bmp_ex_large_and_scaled);
    RUN_TEST(test_gfx_bmp_fit);
    RUN_TEST(test_gfx_blit_scaled);
    RUN_TEST(test_gfx_blit_clip_transparent);
    RUN_TEST(test_gfx_565_to_888_row);
    RUN_TEST(test_gfx_hline_vline_clip);
    RUN_TEST(test_gfx_triangle_fill_outline);
    RUN_TEST(test_gfx_polygon_fill_outline);
    RUN_TEST(test_gfx_ellipse);
    RUN_TEST(test_gfx_flood_fill);
    RUN_TEST(test_gfx_font_table);
    RUN_TEST(test_gfx_text_render);
    RUN_TEST(test_gfx_view_map);
    RUN_TEST(test_gfx_view_clip_line);
    RUN_TEST(test_gfx_view_line_draws);
    RUN_TEST(test_gfx_view_nice_step);
    RUN_TEST(test_gfx_frame_stats_intervals);
    RUN_TEST(test_gfx_frame_stats_dropped);
    UNITY_END();

    /* Asset manifest core (command component). */
    UNITY_BEGIN();
    RUN_TEST(test_asset_crc32_vectors);
    RUN_TEST(test_asset_parse_ok);
    RUN_TEST(test_asset_parse_skip);
    RUN_TEST(test_asset_parse_bad);
    UNITY_END();

    /* CSV grid core (storage parser + command ref substitution). */
    UNITY_BEGIN();
    RUN_TEST(test_csv_split_simple);
    RUN_TEST(test_csv_split_quoted);
    RUN_TEST(test_csv_split_empty);
    RUN_TEST(test_csv_split_over_max);
    RUN_TEST(test_csv_split_exhaustion);
    RUN_TEST(test_csv_substitute_refs);
    RUN_TEST(test_csv_substitute_ranges);
    RUN_TEST(test_csv_format_field);
    RUN_TEST(test_bind_set_and_lookup);
    RUN_TEST(test_bind_rejects_non_fkey);
    RUN_TEST(test_bind_get_by_index);
    RUN_TEST(test_bind_chord_set_and_lookup);
    RUN_TEST(test_bind_chord_rejects);
    RUN_TEST(test_macro_idle_paths);
    RUN_TEST(test_macro_record_and_exclude);
    RUN_TEST(test_macro_record_overflow);
    RUN_TEST(test_macro_double_record);
    RUN_TEST(test_crypt_derive_deterministic);
    RUN_TEST(test_crypt_mem_roundtrip);
    RUN_TEST(test_crypt_mem_rejects);
    RUN_TEST(test_tcp_parse_target_ok);
    RUN_TEST(test_tcp_parse_target_rejects);
    RUN_TEST(test_tcp_unescape);
    RUN_TEST(test_tcp_sanitize);
    RUN_TEST(test_userial_parse_id_ok);
    RUN_TEST(test_userial_parse_id_rejects);
    RUN_TEST(test_userial_parse_coding_defaults);
    RUN_TEST(test_userial_parse_coding_values);
    RUN_TEST(test_userial_parse_coding_rejects);
    UNITY_END();

    /* Package helpers (command component). */
    UNITY_BEGIN();
    RUN_TEST(test_pkg_app_name_from_appinfo_ok);
    RUN_TEST(test_pkg_app_name_from_appinfo_bad);
    UNITY_END();

    /* Theme registry (font component). */
    UNITY_BEGIN();
    RUN_TEST(test_theme_registry);
    RUN_TEST(test_theme_lookup);
    RUN_TEST(test_theme_set_and_active);
    UNITY_END();

    /* Header layout policy (header component). */
    UNITY_BEGIN();
    RUN_TEST(test_header_layout_all_full);
    RUN_TEST(test_header_layout_compacts_sides);
    RUN_TEST(test_header_layout_center_yields_first);
    RUN_TEST(test_header_layout_requests_smaller_font);
    RUN_TEST(test_header_layout_degenerate);
    RUN_TEST(test_header_status_glyphs);
    RUN_TEST(test_header_status_wifi);
    RUN_TEST(test_header_status_bt_usb);
    RUN_TEST(test_header_status_sd);
    RUN_TEST(test_header_status_system_tone);
    RUN_TEST(test_header_notify_queue_fifo);
    RUN_TEST(test_header_notify_queue_overflow_drops_oldest);
    RUN_TEST(test_header_notify_queue_clear_and_blank);
    RUN_TEST(test_header_refresh_priority);
    RUN_TEST(test_header_refresh_clock_and_idle_off);
    RUN_TEST(test_clock_format_hm_snapshot);
    RUN_TEST(test_clock_timer_start_stop);
    RUN_TEST(test_clock_timer_lap_and_status);
    RUN_TEST(test_clock_timer_usage_and_restart);
    RUN_TEST(test_clock_rtc_restore_math);
    RUN_TEST(test_clock_rtc_bcd);
    UNITY_END();

    /* USTAR archive core (components/archive). */
    UNITY_BEGIN();
    RUN_TEST(test_archive_crc32_reference);
    RUN_TEST(test_archive_crc32_incremental);
    RUN_TEST(test_archive_octal_roundtrip);
    RUN_TEST(test_archive_unoctal_rejects);
    RUN_TEST(test_archive_header_roundtrip);
    RUN_TEST(test_archive_header_dir_and_split);
    RUN_TEST(test_archive_header_rejects);
    RUN_TEST(test_archive_entry_fits);
    RUN_TEST(test_archive_path_safe);
    UNITY_END();

    printf("\n=== All tests completed ===\n");
}
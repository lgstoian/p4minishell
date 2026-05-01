/**
 * @file shell.h
 * @brief Shell core module for P4MiniShell — transcript, history, debug log,
 *        UART console, boot banner, system info commands, and utility functions.
 *
 * Owns the shell's core runtime state: transcript buffer, command history,
 * debug log ring buffer, UART console bridge, and system status tracking.
 * Provides the public API that command.c and main.c use for shell operations.
 *
 * Features:
 *   - Transcript management (append, schedule, flush, reset, scroll)
 *   - Command history (10-entry recall with password masking)
 *   - Debug log (5-entry ring buffer for errors/warnings)
 *   - UART console bridge (stdin/stdout routed through shell)
 *   - Boot banner and system info commands (help, sysinfo, version, about, mem, debug)
 *   - Header status refresh timer
 *   - Shell utility functions (trim, split_args, text compare, percentage parse)
 *
 * Architecture:
 *   This module OWNS the shell transcript, history, and debug state.
 *   The command module (command.c) calls into this module for output.
 *   The main.c layer orchestrates boot and delegates to shell + command modules.
 */

#ifndef P4MINISHELL_SHELL_H
#define P4MINISHELL_SHELL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * TRANSCRIPT MANAGEMENT
 * ======================================================================== */

/** Append text directly to the on-screen transcript. Thread-safe. */
void shell_transcript_append_text(const char *text);

/** Append formatted text to the on-screen transcript. Thread-safe. */
void shell_transcript_appendf(const char *format, ...);

/**
 * Append ANSI/VT escape-sequence formatted text to the transcript.
 * SGR codes (ESC[...m) are interpreted and applied as LVGL textarea
 * style changes. Non-SGR escape sequences are passed through as-is.
 *
 * Thread-safe. Safe to call from any task context.
 */
void shell_transcript_append_ansi(const char *text);

/**
 * Append formatted ANSI/VT text to the transcript.
 * Combines printf-style formatting with ANSI color specifiers.
 * Uses the same @-prefixed format specifiers as ansi_format().
 *
 * Thread-safe. Safe to call from any task context.
 */
void shell_transcript_appendf_ansi(const char *format, ...);

/** Schedule text to be appended from a non-LVGL task context.
 *  Uses a staging buffer flushed via LVGL async callback. */
void shell_schedule_transcript_appendf(const char *format, ...);

/** Reset the transcript to empty. */
void shell_transcript_reset(void);

/** Scroll the transcript to the end. */
void shell_history_transcript_scroll_to_end(void);

/* ========================================================================
 * COMMAND HISTORY
 * ======================================================================== */

/** Store a command in the recall history. Masks passwords for wifi connect. */
void shell_store_command_history(const char *command);

/** Recall a command from history. direction: -1=older, 1=newer. */
void shell_recall_history(int direction);

/** Get the current history draft text. */
const char *shell_get_history_draft(void);

/* ========================================================================
 * DEBUG LOG
 * ======================================================================== */

/** Push an entry to the debug log ring buffer. */
void shell_debug_log_push(const char *tag, const char *message);

/** Record an error with ESP-IDF error code. */
void shell_record_errorf(const char *tag, int error, const char *format, ...);

/** Record a warning. */
void shell_record_warningf(const char *tag, const char *format, ...);

/** Record an informational message. */
void shell_record_infof(const char *tag, const char *format, ...);

/** Get the number of runtime warnings recorded. */
size_t shell_get_warning_count(void);

/** Print the debug log to the transcript. */
void shell_command_debug(void);

/* ========================================================================
 * UART CONSOLE BRIDGE
 * ======================================================================== */

/** Start the UART console reader task. */
void shell_uart_console_start(void);

/** Write text to the UART console output. */
void shell_uart_console_write_text(const char *text);

/** Print the shell prompt on the UART console. */
void shell_uart_console_print_prompt(void);

/** Submit a command from the UART console. */
void shell_uart_console_submit_command(const char *command);

/* ========================================================================
 * SYSTEM INFO COMMANDS
 * ======================================================================== */

/** Print the help text listing all available commands. */
void shell_command_help(void);

/** Print comprehensive system information. */
void shell_command_sysinfo(void);

/** Print version information. */
void shell_command_version(void);

/** Print about information. */
void shell_command_about(void);

/** Print memory statistics. */
void shell_command_mem(void);

/* ========================================================================
 * HEADER STATUS
 * ======================================================================== */

/** Refresh the header status bar with current system metrics. */
void shell_header_status_refresh(void);

/** Get the boot timestamp in microseconds for uptime calculation. */
int64_t shell_get_boot_timestamp_us(void);

/** Get the formatted current time string. */
const char *shell_get_time_string(void);

/** Check if time is NTP-synchronized. */
bool shell_time_is_synced(void);

/* ========================================================================
 * SHELL UTILITIES
 * ======================================================================== */

/** Case-insensitive string comparison. */
bool shell_text_equals_ignore_case(const char *left, const char *right);

/** Trim leading and trailing whitespace from a string in-place. */
char *shell_trim(char *text);

/** Split a command line into arguments. Returns argc. */
int shell_split_args(char *text, char **argv, int max_args);

/** Parse a percentage argument (0-100). Returns true on success. */
bool shell_parse_percentage_arg(const char *text, int *percentage_out);

/** Parse a size argument with min/max bounds. Returns true on success. */
bool shell_parse_size_arg(const char *text, size_t min_value, size_t max_value, size_t *value_out);

/** Join arguments from start_index into a single output string. */
void shell_join_args(char **argv, int start_index, int argc, char *output, size_t output_size);

/** Show a notification in the header bar. */
/** Format byte size into human-readable string (B, KiB, MiB, GiB). */
void shell_format_size(char *dst, size_t dst_size, size_t bytes);

/** Return "dir" or "file" for a mode_t value. */
const char *shell_entry_type(const struct stat *st);

void shell_header_notify(const char *text, uint32_t timeout_ms);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/** Initialize the shell module. Must be called once during boot. */
void shell_init(void);

/** Set default shell state (working directory, environment, PATH). */
void shell_set_default_state(void);

/** Check if the shell module is initialized. */
bool shell_is_initialized(void);

/* ========================================================================
 * USB KEYBOARD INPUT BRIDGE
 * ======================================================================== */

/**
 * Handle a USB keyboard input event for CLI injection.
 * Called from the USB module task context when a USB HID keyboard
 * key event occurs. Injects printable characters into the LVGL
 * input line and handles special keys (Enter, Backspace, arrows).
 *
 * @param key_code   USB HID key code (standard HID usage table).
 * @param modifiers  Modifier bitmask (USB_KEY_MOD_* flags).
 * @param pressed    true for key press (make), false for release (break).
 */
void shell_usb_keyboard_input(uint8_t key_code, uint8_t modifiers, bool pressed);

/* ========================================================================
 * POWERSHELL-STYLE PROMPT SUPPORT
 * ======================================================================== */

/**
 * Get the current working directory formatted for the PowerShell-style prompt.
 * Long paths are truncated with "..." prefix.
 * @return Static string with the formatted path (do not free).
 */
const char *shell_get_cwd_for_prompt(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_SHELL_H */

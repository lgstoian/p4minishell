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
 *   - Prompt-aware input line helpers for the LVGL command line
 *   - Boot banner and system info commands (help, sysinfo, version, about, mem, debug)
 *   - Shell utility functions (trim, split_args, text compare, percentage parse)
 *
 * Architecture:
 *   This module OWNS the shell transcript, history, debug state, UART console,
 *   and the LVGL input line text contract. The command module (command.c) owns
 *   every built-in command implementation and calls into this module for output.
 *   The main.c layer orchestrates boot, header refresh, and LVGL event routing.
 */

#ifndef P4MINISHELL_SHELL_H
#define P4MINISHELL_SHELL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * COMMAND MODULE HOOKS
 * ========================================================================
 * The shell core needs a handful of services that the command module owns
 * (command dispatch, current working directory, hardware telemetry). Routing
 * them through a registered operations table keeps the dependency one-way:
 * command.c depends on shell.c, never the reverse. This mirrors the
 * networking_host_ops_t pattern used by components/networking.
 */

typedef struct {
    /** Execute a command line with variable expansion and redirection. */
    void (*execute_command)(char *command);

    /** Queue a command line for execution on the command worker task.
     *  Used by the UART console so a blocking key wait (pause, choice,
     *  more, format confirmation, set /p) can still be answered from the
     *  serial input while the command runs on the worker task. */
    void (*execute_command_async)(char *command);

    /** Return the shell current working directory as an absolute VFS path. */
    const char *(*get_cwd)(void);

    /** Return the last speaker volume percentage applied by the shell. */
    int (*get_volume_percent)(void);

    /** Report whether the SD card is currently mounted by the shell. */
    bool (*sd_is_mounted)(void);

    /** Read battery telemetry. Any output pointer may be NULL. */
    esp_err_t (*battery_read)(int *battery_mv, int *percent, int *raw, int *gpio_mv);

    /* ------------------------------------------------------------------
     * External-module accessors.
     * These let the shell core read state from the networking, Bluetooth,
     * USB, and C6 OTA modules without including their headers, keeping the
     * dependency direction one-way. Every hook is NULL-checked before use;
     * a missing hook degrades gracefully (the corresponding status line is
     * simply skipped).
     * ------------------------------------------------------------------ */

    /** Report whether the hosted Wi-Fi station is associated with an AP. */
    bool (*wifi_is_connected)(void);

    /** Read the current RSSI through the networking module's accessor. */
    bool (*wifi_get_rssi)(int *rssi_out);

    /** Get a human-readable Wi-Fi state string from the networking module. */
    const char *(*wifi_state_string)(void);

    /** Append the networking module's sysinfo summary to the transcript. */
    void (*append_sysinfo_summary)(void);

    /** Report whether hosted Bluetooth is enabled. */
    bool (*bluetooth_is_enabled)(void);

    /** Report whether hosted Bluetooth has an active connection. */
    bool (*bluetooth_is_connected)(void);

    /** Report whether a USB device is connected to the host port. */
    bool (*usb_is_connected)(void);

    /** Report whether a USB HID keyboard is currently attached. */
    bool (*usb_is_keyboard_attached)(void);

    /** Convert a USB HID key code + modifiers to an ASCII character. */
    bool (*usb_key_to_ascii)(uint8_t key_code, uint8_t modifiers, char *out);

    /** Report whether a C6 OTA confirmation is pending. */
    bool (*c6ota_is_pending)(void);

    /** Report whether a C6 OTA transfer is in progress. */
    bool (*c6ota_is_busy)(void);
} shell_command_ops_t;

/**
 * Register the command module's operations table.
 * Called once by command_init() during boot. Passing NULL clears the table,
 * after which the dependent shell features degrade gracefully rather than
 * dereferencing a null pointer.
 */
void shell_register_command_ops(const shell_command_ops_t *ops);

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

/* ========================================================================
 * SEMANTIC OUTPUT HELPERS
 * ========================================================================
 * Thin wrappers over shell_transcript_appendf_ansi() that apply the shared
 * palette from components/ansi/ansi_palette.h. Commands call these instead
 * of choosing colours themselves, so every subsystem looks the same and a
 * palette change stays a one-file edit.
 *
 * Each wrapper emits its own reset, so callers never have to remember one.
 * All of them are safe from any task context and mirror to the UART console
 * exactly like the underlying ANSI append does.
 */

/** Print a section heading followed by a newline. */
void shell_print_heading(const char *format, ...);

/** Print a "label: value" pair, with the value in the important-value colour. */
void shell_print_field(const char *label, const char *format, ...);

/** Print a "label: number" pair, with the number in the numeric colour. */
void shell_print_field_num(const char *label, long value);

/** Print a success line. */
void shell_print_ok(const char *format, ...);

/** Print an error line. Does not touch errorlevel or the debug log. */
void shell_print_error(const char *format, ...);

/** Print a warning line. */
void shell_print_warning(const char *format, ...);

/** Print a muted or secondary line. */
void shell_print_muted(const char *format, ...);

/** Print a usage line, for an incorrectly invoked command. */
void shell_print_usage(const char *format, ...);

/** Reset the transcript to empty. */
void shell_transcript_reset(void);

/** Scroll the transcript to the end. */
void shell_history_transcript_scroll_to_end(void);

/** Get the current transcript length in bytes (used by output redirection). */
size_t shell_transcript_get_length(void);

/**
 * Get a read-only pointer into the transcript buffer at @p offset.
 * Used by the output redirection layer to capture the delta produced by a
 * single command. Returns NULL when @p offset is past the end of the buffer.
 */
const char *shell_transcript_get_text_from(size_t offset);

/* ========================================================================
 * COMMAND HISTORY
 * ======================================================================== */

/** Store a command in the recall history. Masks passwords for wifi connect. */
void shell_store_command_history(const char *command);

/** Recall a command from history. direction: -1=older, 1=newer. */
void shell_recall_history(int direction);

/** Get the current history draft text. */
const char *shell_get_history_draft(void);

/** Reset the history recall cursor and clear the saved draft. */
void shell_reset_history_cursor(void);

/**
 * Decide whether a submitted command should be stored in recall history.
 * Returns false for sensitive commands (wifi connect with a password) and
 * while a C6 OTA confirmation is pending.
 */
bool shell_command_should_store_history(const char *command);

/**
 * Produce the transcript-safe rendering of a command, masking the password
 * argument of `wifi connect <ssid> <password>`.
 */
void shell_format_command_for_transcript(const char *command, char *output, size_t output_size);

/* ========================================================================
 * INPUT LINE
 * ======================================================================== */

/** Set the LVGL input line to the shell prompt followed by @p command_text. */
void shell_input_line_set_text(const char *command_text);

/** Reset the LVGL input line to a bare prompt. */
void shell_input_line_reset(void);

/** Extract the user-typed portion of the input line (prompt stripped, trimmed). */
void shell_extract_input_text(char *output, size_t output_size);

/**
 * Repair the input line after an LVGL keyboard backspace ate into the prompt.
 * Does nothing when the prompt prefix is intact.
 */
void shell_input_line_repair_prompt(const char *text);

/* ========================================================================
 * INTERACTIVE KEYPRESS WAIT
 * ========================================================================
 * `pause`, `choice`, and `more` need to block until the user presses a key.
 * The shell core owns a small keypress queue that every input source feeds:
 * the UART console reader, the USB HID keyboard bridge, and the LVGL
 * on-screen keyboard. Commands consume it through shell_wait_for_key().
 */

/**
 * Begin an interactive keypress wait.
 *
 * Flushes any stale keys, marks the shell as waiting so the input sources
 * route keystrokes into the key queue instead of the command line, and
 * suppresses command submission for the duration.
 *
 * Must be paired with shell_key_wait_end() on every return path.
 */
void shell_key_wait_begin(void);

/** End an interactive keypress wait and restore normal input routing. */
void shell_key_wait_end(void);

/** Report whether an interactive keypress wait is currently active. */
bool shell_key_wait_is_active(void);

/**
 * Block until a key arrives or @p timeout_ms elapses.
 *
 * Only valid between shell_key_wait_begin() and shell_key_wait_end().
 *
 * @param timeout_ms  Maximum wait in milliseconds.
 * @param key_out     Receives the ASCII character. May be NULL.
 * @return true when a key was received, false on timeout.
 */
bool shell_wait_for_key(uint32_t timeout_ms, char *key_out);

/**
 * Report whether any interactive key source is currently attached.
 *
 * Returns true when the UART console task is running or a USB keyboard is
 * present. Commands use this to decide between a real keypress wait and the
 * bounded fallback delay, so a headless board never stalls.
 */
bool shell_key_input_available(void);

/**
 * Push a key into the wait queue. Called by the input sources.
 * Ignored when no keypress wait is active.
 *
 * @param key  ASCII character. Enter arrives as '\r'.
 * @return true when the key was consumed by an active wait.
 */
bool shell_key_wait_submit(char key);

/**
 * Read a line of text from the user, echoing it as it is typed.
 *
 * Opens and closes its own keypress wait, so input never reaches the command
 * dispatcher. Enter finishes the line, Backspace erases the last character,
 * and ESC cancels. Used by `set /p`.
 *
 * @param output       Receives the typed text, always null-terminated.
 * @param output_size  Size of @p output in bytes.
 * @param timeout_ms   Maximum wait for each individual keystroke.
 * @return true when a line was completed with Enter, false on cancel,
 *         timeout, or when no interactive key source is attached.
 */
bool shell_read_line(char *output, size_t output_size, uint32_t timeout_ms);

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

/**
 * Split a command line into arguments.
 *
 * Honors the shell quoting rules: double quotes group text and allow variable
 * expansion, single quotes group text literally, and a caret escapes the next
 * character. Quoting and escape markup is removed from the produced arguments,
 * so a handler receives the value the user meant.
 *
 * @param text      Command line, tokenized in place.
 * @param argv      Receives pointers into @p text.
 * @param max_args  Capacity of @p argv.
 * @return Argument count.
 */
int shell_split_args(char *text, char **argv, int max_args);

/* ========================================================================
 * QUOTING AND ESCAPING
 * ========================================================================
 * One scanner backs every place that has to find an unquoted operator:
 * redirection (`>`, `>>`, `<`), pipes (`|`), and chaining (`&`, `&&`, `||`).
 * Keeping it in the shell core means those surfaces can never disagree about
 * whether a given character is syntax or data.
 *
 * Rules, following COMMAND.COM with a PowerShell-style single-quote addition:
 *   "text"   Groups text. Variables still expand inside.
 *   'text'   Groups text literally. Variables do NOT expand inside.
 *   ^c       Makes character `c` literal, including ^&, ^|, ^>, ^", and ^^.
 */

/** Quote context reported while scanning a command line. */
typedef enum {
    SHELL_QUOTE_NONE = 0,   /**< Outside any quoted run. */
    SHELL_QUOTE_DOUBLE,     /**< Inside "..." — expansion still applies. */
    SHELL_QUOTE_SINGLE,     /**< Inside '...' — fully literal. */
} shell_quote_state_t;

/**
 * Find the next occurrence of @p target that is neither quoted nor escaped.
 *
 * @param text    Text to scan. May be NULL.
 * @param target  Character to locate.
 * @return Pointer to the match, or NULL when none exists.
 */
char *shell_find_unquoted_char(const char *text, char target);

/**
 * Find the next unquoted, unescaped occurrence of any character in @p targets.
 *
 * @param text     Text to scan. May be NULL.
 * @param targets  Null-terminated set of characters to look for.
 * @return Pointer to the first match, or NULL when none exists.
 */
char *shell_find_unquoted_any(const char *text, const char *targets);

/**
 * Report whether @p text contains an unquoted, unescaped @p target.
 */
bool shell_has_unquoted_char(const char *text, char target);

/**
 * Remove quoting and escape markup from @p text in place.
 *
 * Unwraps `"..."` and `'...'` runs and resolves `^c` to `c`. Used by the
 * argument tokenizer and by redirection-target handling so a quoted path
 * reaches the filesystem layer as a plain string.
 *
 * @return @p text, for call chaining.
 */
char *shell_unescape_in_place(char *text);

/* ========================================================================
 * COMMAND CHAINING
 * ========================================================================
 * `a & b` runs both, `a && b` runs b only if a succeeded, and `a || b` runs
 * b only if a failed. Splitting lives in the shell core beside the quote
 * scanner; the command module owns the execution loop and the success test.
 */

/** How a chained command relates to the one before it. */
typedef enum {
    SHELL_CHAIN_FIRST = 0,   /**< First segment; always runs. */
    SHELL_CHAIN_ALWAYS,      /**< Preceded by `&`. */
    SHELL_CHAIN_ON_SUCCESS,  /**< Preceded by `&&`. */
    SHELL_CHAIN_ON_FAILURE,  /**< Preceded by `||`. */
} shell_chain_op_t;

/** One command in a chain, with the operator that introduced it. */
typedef struct {
    char *command;            /**< Trimmed command text. */
    shell_chain_op_t op;      /**< Relationship to the previous segment. */
} shell_chain_segment_t;

/**
 * Split a command line on unquoted `&`, `&&`, and `||` separators.
 *
 * A single segment (no separator found) is reported as one entry with
 * SHELL_CHAIN_FIRST, so callers need no special case. Quoted or caret-escaped
 * separators are left as data.
 *
 * @param text           Command line, split in place.
 * @param segments       Receives the segments.
 * @param max_segments   Capacity of @p segments.
 * @param truncated_out  Set to true when the line had more separators than
 *                       @p max_segments allows. May be NULL.
 * @return Number of segments produced.
 */
int shell_split_chain(char *text,
                      shell_chain_segment_t *segments,
                      int max_segments,
                      bool *truncated_out);

/** Parse a percentage argument (0-100). Returns true on success. */
bool shell_parse_percentage_arg(const char *text, int *percentage_out);

/** Parse a size argument with min/max bounds. Returns true on success. */
bool shell_parse_size_arg(const char *text, size_t min_value, size_t max_value, size_t *value_out);

/** Join arguments from start_index into a single output string. */
void shell_join_args(char **argv, int start_index, int argc, char *output, size_t output_size);

/** Show a notification in the header bar. */
void shell_header_notify(const char *text, uint32_t timeout_ms);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/** Initialize the shell module. Must be called once during boot. */
void shell_init(void);

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
 * Long paths are truncated with "..." prefix. Thread-safe: writes into the
 * caller-provided buffer so no shared state exists between the UART console
 * task and the LVGL input-line task.
 *
 * @param buf      Receives the formatted path. Must not be NULL.
 * @param buf_size Size of @p buf in bytes.
 */
void shell_get_cwd_for_prompt(char *buf, size_t buf_size);

/* ========================================================================
 * RUNTIME PROMPT TEMPLATE
 * ========================================================================
 * The `prompt` command sets a DOS-style template that both the UART console
 * and the LVGL input line render. Supported metacharacters follow COMMAND.COM:
 *
 *   $p  current path      $g  >            $l  <            $b  |
 *   $n  drive letter      $d  date         $t  time         $v  version
 *   $s  space             $_  newline      $q  =            $$  $
 *   $a  &                 $c  (            $f  )            $e  ESC
 *   $h  backspace (removes the previous rendered character)
 */

/**
 * Set the prompt template.
 * Passing NULL or an empty string restores `P4_CONFIG_PROMPT_DEFAULT_TEMPLATE`.
 *
 * @param template_text  DOS-style template using `$` metacharacters.
 */
void shell_prompt_set_template(const char *template_text);

/** Get the raw prompt template as the user typed it. */
const char *shell_prompt_get_template(void);

/**
 * Render the active template into plain text for the LVGL input line.
 * @return Static string owned by the shell core (do not free).
 */
const char *shell_prompt_render_plain(void);

/** Reset the prompt template to the configured default. */
void shell_prompt_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_SHELL_H */

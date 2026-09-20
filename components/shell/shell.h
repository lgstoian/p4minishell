/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
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
#include <stdio.h>
#include "esp_err.h"
#include "header.h"   /* header_notify_level_t for shell_header_notify_level() */

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

    /** Report whether ANY physical keyboard is present: USB HID, a connected
     *  Bluetooth HID keyboard, or the M5Stack Tab5 keyboard module. When true
     *  the on-screen keyboard is auto-hidden. */
    bool (*physical_keyboard_present)(void);

    /** Report whether at least one background job is currently running. */
    bool (*bg_jobs_running)(void);

    /** Convert a USB HID key code + modifiers to an ASCII character. */
    bool (*usb_key_to_ascii)(uint8_t key_code, uint8_t modifiers, char *out);

    /** Look up a bound USB function-key line (F1..F12, the `bind` table).
     *  Fills @p out (up to @p out_size) and returns true when the key is
     *  bound; the shell submits the line via execute_command_async. */
    bool (*bind_lookup_fkey)(uint8_t key_code, char *out, size_t out_size);

    /** Look up a bound Ctrl+letter chord line (the `bind ^X` table).
     *  Fires only with Ctrl held on a letter key (^C never matches);
     *  submission is identical to bind_lookup_fkey. */
    bool (*bind_lookup_chord)(uint8_t key_code, uint8_t modifiers,
                              char *out, size_t out_size);

    /** Report whether a C6 OTA confirmation is pending. */
    bool (*c6ota_is_pending)(void);

    /** Report whether a C6 OTA transfer is in progress. */
    bool (*c6ota_is_busy)(void);

    /** Report shell user activity (power idle clock + display wake). */
    void (*pm_notify_activity)(void);

    /** Milliseconds until the idle display-off deadline; 0 when disabled or
     *  already off. Used by the adaptive header refresh scheduler. */
    int (*pm_ms_until_idle_off)(void);

    /** Ensure the network timezone/SNTP bootstrap is running (idempotent).
     *  Called once Wi-Fi is associated; returns true when fully configured. */
    bool (*time_auto_sync)(void);

    /**
     * Tab-completion provider. Fills @p out (up to @p out_size) with the
     * @p match_index-th completion (0-based) for the last whitespace-delimited
     * word of @p line and returns the total number of matches, or 0 when there
     * are none. The provider parses the whole line, so it can complete command
     * names, subcommands/flags, installed apps, aliases, and paths.
     */
    int (*complete_line)(const char *line, int match_index,
                         char *out, size_t out_size);

    /**
     * Inline ghost provider: fills @p out with the single best completion for
     * the last word of @p line and returns the number of matches. Kept
     * SD-free (command names + subcommands/flags only) so it is cheap enough
     * to run on every keystroke.
     */
    int (*ghost_line)(const char *line, char *out, size_t out_size);

    /** Report whether any native modal surface is currently open. */
    bool (*modal_is_active)(void);

    /** Handle a USB key press while a modal surface is open.
     *  @return true when the key was consumed by the surface. */
    bool (*modal_handle_usb_key)(uint8_t key_code, uint8_t modifiers, char ascii);

    /** Feed a line of serial console input to the active modal surface.
     *  Returns true when the line was consumed by the surface. */
    bool (*modal_handle_serial_line)(const char *line);

    /** Handle a host console line on the console-reader task itself while a
     *  modal is active (the command worker is blocked in the modal session).
     *  Used by the streaming `screenshot` so the host can capture modal
     *  surfaces. Called before modal_handle_serial_line; return true when the
     *  line was fully handled (the modal then never sees it). */
    bool (*modal_console_command)(const char *line);
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

/** Schedule ANSI-coloured text from a non-LVGL task (like the plain form but
 *  interprets @-specifiers via ansi_vformat and renders with colours). */
void shell_schedule_transcript_appendf_ansi(const char *format, ...);

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

/**
 * Reclaim internal heap from the transcript scrollback when the DMA-capable
 * heap runs low (see implementation). Call at the start of command execution,
 * before any printf of that command, so the reclaimed memory is available
 * before output is emitted.
 */
void shell_transcript_guard_internal(void);

/** Number of scrollback trims performed since boot (internal-heap guard). */
size_t shell_transcript_trim_count(void);

/**
 * Defer on-screen transcript repaints across a burst of appends (one shell
 * command's output). Buffers and the serial mirror stay live; only the LVGL
 * label repaint (O(buffer) span rebuild) is deferred. Nesting-safe.
 * Call shell_transcript_defer_end() on every exit path after begin().
 */
void shell_transcript_defer_begin(void);
void shell_transcript_defer_end(void);

/** Repaint now if a deferred update is pending (no-op otherwise). */
void shell_transcript_flush_now(void);

/* ========================================================================
 * BACKGROUND WORKERS (`start` pool)
 * ========================================================================
 * The deferral slots above are selected by task handle; these calls bind a
 * spawned worker to a slot. Handles are opaque here (void *) so this header
 * needs no FreeRTOS include; the task never blocks on another task.
 */

/** Register a background worker handle (idempotent while slots last). */
void shell_register_bg_task(void *task);

/** Unregister a background worker handle. */
void shell_unregister_bg_task(void *task);

/** True when the calling task is a registered background worker. */
bool shell_is_background_task(void);

/* ========================================================================
 * APP MODE (screen save/restore + full-screen surface)
 * ======================================================================== */

/** Save the current transcript (ANSI, colours preserved) into a heap buffer.
 *  Returns NULL when the transcript is empty or on allocation failure. */
char *shell_screen_save(void);

/** Clear the transcript and re-append the saved screen (no-op on NULL). */
void shell_screen_restore(const char *saved);

/** Release a screen copy from `shell_screen_save`. */
void shell_screen_discard(char *saved);

/** Enter app mode; with full_screen the shell input widgets are hidden so the
 *  transcript becomes a clean app surface. Save the screen first. */
void shell_app_mode_enter(bool full_screen);

/** Leave app mode and restore the shell input widgets. */
void shell_app_mode_exit(void);

/** Whether the shell is currently in (full-screen) app mode. */
bool shell_app_mode_active(void);

/** Scroll the transcript to the end. */
void shell_history_transcript_scroll_to_end(void);

/** Force the transcript to jump to the newest output (command submission).
 *  Unlike shell_history_transcript_scroll_to_end(), this pins the view to the
 *  bottom even when the user was reading earlier history. */
void shell_force_transcript_scroll_to_end(void);

/** Get the current transcript length in bytes (used by output redirection). */
size_t shell_transcript_get_length(void);

/**
 * Get a read-only pointer into the transcript buffer at @p offset.
 * Used by the output redirection layer to capture the delta produced by a
 * single command. Returns NULL when @p offset is past the end of the buffer.
 */
const char *shell_transcript_get_text_from(size_t offset);

/* ========================================================================
 * OUTPUT-REDIRECTION CAPTURE
 * ======================================================================== */

/**
 * Open the output-redirection capture window. While open, every plain-text
 * transcript append is mirrored into a dedicated heap buffer so a redirected
 * command's output is captured in full (independent of the transcript size).
 * Call before dispatching a redirected command.
 */
void shell_redirect_capture_begin(void);

/** Close the output-redirection capture window (after dispatch). */
void shell_redirect_capture_end(void);

/**
 * Return the captured output (NUL-terminated) and its length. The pointer
 * stays valid until the next shell_redirect_capture_begin()/reset().
 */
const char *shell_redirect_capture_get(size_t *out_len);

/** Release the capture buffer. Call after the redirected file is written. */
void shell_redirect_capture_reset(void);

/** Report whether the capture hit its size cap (some output was dropped). */
bool shell_redirect_capture_was_truncated(void);

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

/** Number of commands currently in the recall history. */
size_t shell_history_get_count(void);

/** Get a recall-history entry by index (0 = oldest). NULL when out of range. */
const char *shell_history_get(size_t index);

/**
 * Monotonic counter bumped whenever a command is appended to recall history.
 * The persistence layer compares it to its last-saved value to decide when a
 * debounced auto-save is needed (a count comparison is not enough once the
 * ring is full).
 */
size_t shell_history_generation(void);

/** Clear the recall history (frees all heap entries). */
void shell_history_clear(void);

/**
 * Write every recall-history entry as one line to an open stream (unit-
 * tested; the `history /save` command opens the SD file and calls this).
 * Skips NULL entries; a short write removes nothing here — the caller owns
 * the partial-file policy. Returns true when every line was written.
 */
bool shell_history_save_lines(FILE *fp);

/**
 * Read newline-terminated lines from an open stream into recall history
 * (unit-tested; the `history /load` command opens the SD file and calls
 * this). Trims each line, skips blanks. Returns the number of lines loaded.
 */
size_t shell_history_load_lines(FILE *fp);

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
 * Complete the last whitespace-delimited word of the input line through the
 * command module's `complete_line` provider (help-table commands, aliases,
 * apps, usage tokens, SD paths). The same entry USB Tab and the input-row Tab
 * button share; repeated presses cycle the matches. Runs on the LVGL task;
 * safe to call from main.c button handlers (no-op without a registered
 * provider or an empty line).
 */
void shell_input_line_tab_complete(void);

/**
 * Refresh the inline ghost suggestion for the input line: the best completion
 * remainder is shown as muted text after the caret (accepted with Right at
 * end-of-line or Tab). No-op when the line is empty, the caret is not at the
 * end, a modal owns the screen, or ghost completion is disabled.
 */
void shell_input_line_ghost_refresh(void);

/**
 * True when the reverse-history search (Ctrl+R) is active. While active the
 * input line shows the matched history entry and the search query is shown in
 * the input-row search label.
 */
bool shell_history_search_active(void);

/**
 * Case-insensitive substring test used by the reverse-history search filter
 * and by `history /search`. An empty @p query matches every entry.
 */
bool shell_history_search_matches(const char *entry, const char *query);

/**
 * Repair the input line after an LVGL keyboard backspace ate into the prompt.
 * Does nothing when the prompt prefix is intact.
 */
void shell_input_line_repair_prompt(const char *text);

/**
 * Insert @p text into the LVGL input line at the cursor position (DOS-style
 * paste). Safe from any task; no-op when the input line does not exist.
 */
void shell_input_line_paste(const char *text);

/* ========================================================================
 * RAM CLIPBOARD
 * ========================================================================
 * A single RAM clipboard backed by the `clip` / `paste` commands. It holds
 * either text (copied transcript lines, `clip <text>`, or a text file read by
 * `clip read`) or a file reference (`clip file <path>`, which `paste <dest>`
 * copies to a destination).
 */

/** Set the clipboard to @p text (text mode). Bounded by P4_CONFIG_CLIPBOARD_BYTES. */
void shell_clipboard_set(const char *text);

/** Set the clipboard to a file reference (file mode). */
void shell_clipboard_set_file(const char *path);

/** Get the clipboard text (the file path when in file mode). */
const char *shell_clipboard_get(void);

/** Report whether the clipboard currently holds a file reference. */
bool shell_clipboard_is_file(void);

/**
 * Copy the last @p n_lines lines of the transcript into the clipboard (text
 * mode). Returns false when the transcript is empty.
 */
bool shell_clipboard_copy_transcript(int n_lines);

/* ========================================================================
 * TRANSCRIPT CLICK REGIONS (for anchor command)
 * ========================================================================
 * The transcript can have clickable regions defined by the `anchor` command.
 * When the user clicks (mouse/touch) in the transcript area, the shell
 * checks if the click falls within any region and executes the associated
 * command. Regions are cleared on `cls`, `clear`, alt-screen enter/exit,
 * and `anchor /clear`.
 */

/**
 * Add a clickable region to the transcript.
 * @param text     The anchor text (displayed in transcript).
 * @param command  Command to execute when clicked.
 * @param continue_line  If true, the anchor text continues on the same line (for /c).
 */
void shell_transcript_add_anchor_region(const char *text, const char *command, bool continue_line);

/**
 * Clear all transcript click regions.
 * Called on `cls`, `clear`, alt-screen enter/exit, and `anchor /clear`.
 */
void shell_transcript_clear_click_regions(void);

/**
 * Hit-test a click position against all regions.
 * @param x  X coordinate in transcript coordinates.
 * @param y  Y coordinate in transcript coordinates.
 * @param action_out  Receives the command to execute (if hit).
 * @return true if a region was hit.
 */
bool shell_transcript_hit_test(int x, int y, const char **action_out);

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
 * ASCII fast path: for a multibyte key this returns its lead byte (which
 * never equals ASCII, so y/n/ESC compares stay correct; any-key waits only
 * need the boolean). Use shell_wait_for_key_utf8() for the full sequence.
 *
 * @param timeout_ms  Maximum wait in milliseconds.
 * @param key_out     Receives the ASCII character. May be NULL.
 * @return true when a key was received, false on timeout.
 */
bool shell_wait_for_key(uint32_t timeout_ms, char *key_out);

/**
 * Block until a key arrives or @p timeout_ms elapses, returning the full
 * UTF-8 sequence (up to 4 bytes + NUL).
 *
 * @param timeout_ms  Maximum wait in milliseconds.
 * @param buf         Receives the NUL-terminated byte sequence. May be NULL
 *                    (wait satisfied, sequence discarded).
 * @param buf_size    Size of @p buf; sequences are truncated to fit.
 * @return true when a key was received, false on timeout.
 */
bool shell_wait_for_key_utf8(uint32_t timeout_ms, char *buf, size_t buf_size);

/**
 * Report whether any interactive key source is currently attached.
 *
 * Returns true when the UART console task is running or a USB keyboard is
 * present. Commands use this to decide between a real keypress wait and the
 * bounded fallback delay, so a headless board never stalls.
 */
bool shell_key_input_available(void);

/**
 * Track whether a batch file is currently executing.
 *
 * Owned by the shell module (a plain flag) and maintained by the batch engine
 * when it pushes or pops a batch frame. Destructive-command confirmation gates
 * use this to refuse outright from inside a batch file, so an unattended
 * script can never drive a wipe even when a serial console is attached.
 */
void shell_set_batch_active(bool active);

/** Report whether a batch file is currently executing. */
bool shell_is_batch_active(void);

/* ========================================================================
 * FOREGROUND BREAK (Ctrl+C / Stop button)
 * ========================================================================
 * Cooperative abort for the foreground command worker. Any input context
 * (USB Ctrl+C with no key-wait active, the input-row Stop button) sets the
 * flag; the batch line loop, `for` bodies, and `delay` chunks poll it and
 * unwind with `^C`, consuming the request. The worker clears it when it
 * claims each command, so one break never poisons the next line. Plain
 * bools like the background kill flags (single-word access is atomic).
 */

/** Request a foreground break (sets the flag; idempotent). */
void shell_request_abort(void);

/** True when a foreground break was requested and not yet consumed. */
bool shell_abort_requested(void);

/** Clear a pending foreground break (worker claims a command). */
void shell_clear_abort(void);

/** Record that a foreground break actually stopped a command (sticky until the
 *  worker tears down the surfaces the aborted script left open). */
void shell_mark_foreground_break(void);

/** True while an aborted command's surface teardown is still owed. */
bool shell_foreground_break_pending(void);

/** Clear the sticky foreground-break flag (after teardown). */
void shell_clear_foreground_break(void);

/** Track whether the command worker is executing (drives the Stop button). */
void shell_set_command_busy(bool busy);

/** Report whether the command worker is executing. */
bool shell_is_command_busy(void);

/**
 * Push a key into the wait queue. Called by the input sources.
 * Ignored when no keypress wait is active.
 *
 * @param key  ASCII character. Enter arrives as '\r'.
 * @return true when the key was consumed by an active wait.
 */
bool shell_key_wait_submit(char key);

/**
 * Push a UTF-8 key sequence into the wait queue (on-screen keyboard
 * symbols). The first codepoint (up to 4 bytes) is stored as one queue
 * item so multibyte keys are never fragmented. Ignored when no wait is
 * active or the sequence is empty/invalid.
 *
 * @param bytes  UTF-8 bytes; only the first codepoint is used.
 * @param len    Available bytes (need not be NUL-terminated).
 * @return true when the key was consumed by an active wait.
 */
bool shell_key_wait_submit_utf8(const char *bytes, size_t len);

/**
 * Decode the first UTF-8 codepoint of @p s.
 *
 * @param s        Input bytes (need not be NUL-terminated).
 * @param cp_out   Receives the codepoint. May be NULL.
 * @param len_out  Receives the consumed byte count (1-4). May be NULL.
 * @return true on a valid complete codepoint, false on NUL/truncated/invalid.
 */
bool shell_utf8_decode(const char *s, size_t avail, uint32_t *cp_out, size_t *len_out);

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

/**
 * Read a line exactly like `shell_read_line` but WITHOUT echoing the typed
 * keys (password mode: backspace erases silently, ESC cancels, Enter completes
 * and prints a newline). Used by `set /p NAME=<prompt> /P` and the applib
 * password reader.
 *
 * @return true when a line was completed with Enter, false on cancel,
 *         timeout, or when no interactive key source is attached.
 */
bool shell_read_line_hidden(char *output, size_t output_size, uint32_t timeout_ms);

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

/** Number of entries currently held in the debug log ring. */
size_t shell_debug_get_count(void);

/** Copy the i-th oldest entry (0 = oldest) into @p out.
 *  Returns false when @p index is out of range. Pure reader for the
 *  `debug save` exporter (the SD file itself is opened by the command
 *  layer, mirroring shell_history_save_lines). */
bool shell_debug_get_entry(size_t index, char *out, size_t out_size);

/** Print the debug log to the transcript. */
void shell_command_debug(void);

/* ========================================================================
 * UART CONSOLE BRIDGE
 * ======================================================================== */

/** Start the UART console reader task. */
void shell_uart_console_start(void);

/** Write text to the UART console output. */
void shell_uart_console_write_text(const char *text);

/**
 * Write raw bytes to the UART console output, completing partial writes.
 *
 * Shared by the transcript mirror and the binary `send`/`screenshot` stream.
 * Retries the unwritten remainder so transient TX backpressure cannot truncate
 * a line or a framed payload; returns false only when no progress is made for
 * @p total_ms (0 means a single attempt). Callers choose the budget: the binary
 * path can wait seconds because the host is actively reading the frame, while
 * the transcript mirror must stay short so it never stalls the console reader
 * (the P4 USB-Serial/JTAG peripheral also wedges under a long full-ring stall).
 */
bool shell_uart_console_write_bytes(const void *data, size_t len,
                                    uint32_t total_ms);

/**
 * Claim the console's raw byte stream so the caller can read binary input from
 * stdin (e.g. the `receive` command transferring a file). While held, the
 * console reader task does not consume stdin. Must be paired with
 * shell_uart_console_rx_end() on every exit path.
 */
void shell_uart_console_rx_begin(void);

/** Release the console's raw byte stream (paired with rx_begin). */
void shell_uart_console_rx_end(void);

/** Print the shell prompt on the UART console. */
void shell_uart_console_print_prompt(void);

/** Submit a command from the UART console. */
void shell_uart_console_submit_command(const char *command);

/* ========================================================================
 * SYSTEM INFO COMMANDS
 * ======================================================================== */

/**
 * Print help text. `help` prints a quick summary; `help /all` (or `help /?`)
 * prints the full offline command reference; `help <command>` prints one entry.
 */
void shell_command_help(int argc, char **argv);

/**
 * Offline command-reference accessors. The help table is the single source of
 * truth for command names and usage text; tab completion and argument
 * completion read it rather than keeping a parallel list, so a new command
 * only needs its help entry expanded.
 *
 * @return number of entries; shell_help_entry_get() is valid for 0..count-1.
 */
size_t shell_help_entry_count(void);
bool shell_help_entry_get(size_t index, const char **name, const char **usage);

/**
 * Format a one-line build identity:
 * "P4MiniShell v0.31.0 | built <date> <time> | git <hash>".
 * Used by the header long-press notification and available to callers that
 * want the build identity without pulling in the full version command.
 */
void shell_get_build_identity(char *buf, size_t size);

/** Print comprehensive system information. */
void shell_command_sysinfo(void);

/** Print version information. */
void shell_command_version(void);

/** Print about information. */
void shell_command_about(void);

/** Print memory statistics. */
void shell_command_mem(void);

/**
 * List FreeRTOS tasks (`ps` / `tasks` / `top`). Read-only: name, state,
 * priority, core, stack high-water mark, and (for `top`) CPU% since the last
 * sample. `/b` emits uncoloured machine-parsable rows; `/O:` sorts by a key.
 *
 * Usage: ps|tasks|top [/b] [/O:key]
 *   /O:  sort by N (name), C (CPU), S (stack high-water), P (priority), or
 *        T (state); prefix `-` to reverse; bare `/O` sorts by name. `top`
 *        defaults to CPU descending; `ps`/`tasks` keep FreeRTOS order unless
 *        `/O:` is given. Returns an ERRORLEVEL: 0 ok, 2 usage.
 */
int shell_command_ps(int argc, char **argv);

/** One normalized task row used by the `/O:` sort. */
typedef struct {
    char name[32];
    char state[4];          /* "RUN", "RDY", "BLK", "SUS", "DEL", "?" */
    unsigned int priority;
    int core;               /* -1 when unpinned or unavailable */
    uint32_t highwater_bytes;
    int cpu_percent;
} shell_task_row_t;

/** Sort keys accepted by `ps`/`tasks`/`top` `/O:`. */
typedef enum {
    SHELL_TASK_SORT_NAME = 0,
    SHELL_TASK_SORT_CPU,
    SHELL_TASK_SORT_STACK,
    SHELL_TASK_SORT_PRIORITY,
    SHELL_TASK_SORT_STATE
} shell_task_sort_key_t;

/**
 * Compare two task rows for `qsort`. Returns <0, 0, >0 like strcmp; name is
 * the deterministic tie-breaker. Pure and exposed for the unit tests.
 */
int shell_task_row_compare(const shell_task_row_t *a, const shell_task_row_t *b,
                           shell_task_sort_key_t key, bool reverse);

/* ========================================================================
 * HEADER STATUS
 * ======================================================================== */

/** Refresh the header status bar with current system metrics. */
void shell_header_status_refresh(void);

/**
 * Start the background header-telemetry sampler (idempotent). It runs the
 * expensive heap/task-snapshot/battery reads off the LVGL task so they cannot
 * stall the MIPI-DSI framebuffer fetch. Call after command_init() so the shell
 * ops table (battery/c6ota accessors) is registered.
 */
void shell_start_telemetry(void);

/**
 * Next adaptive header poll interval in milliseconds, chosen from the current
 * situation (display idle-off, OTA/bg job, Wi-Fi connecting, startup, clock
 * minute boundary, pending idle-display-off). main reschedules its header
 * timer with this value. Must be called after shell_header_status_refresh().
 */
uint32_t shell_header_refresh_interval_ms(void);

/** Get the boot timestamp in microseconds for uptime calculation. */
int64_t shell_get_boot_timestamp_us(void);

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

/**
 * Count the arguments in a command line without mutating it, using the same
 * quote/escape rules as shell_split_args(). Lets the dispatcher detect when a
 * command has more arguments than the argv capacity so truncation is never
 * silent.
 * @param text  Command line (not modified).
 * @return The total number of arguments.
 */
int shell_count_args(const char *text);

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

/**
 * Strip caret escapes (`^c` -> `c`) from @p text in place WITHOUT removing quote
 * delimiters. Used by `echo`, which prints the raw remainder of the command line
 * and must keep `"..."` visible while still consuming a caret. A caret inside
 * single quotes or trailing at the end of the string is left literal.
 *
 * @return @p text, for call chaining.
 */
char *shell_unescape_carets_in_place(char *text);

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

/** Show an INFO notification in the header bar. */
void shell_header_notify(const char *text, uint32_t timeout_ms);

/** Show a notification with an explicit severity (info/warn/error color). */
void shell_header_notify_level(const char *text, uint32_t timeout_ms,
                               header_notify_level_t level);

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

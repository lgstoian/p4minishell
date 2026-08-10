/**
 * @file command.h
 * @brief Command parser and dispatcher for P4MiniShell.
 *
 * Owns the command execution pipeline — variable-expansion entry, output
 * redirection, parsing, dispatch, and the worker task — plus the commands
 * that are not tied to the filesystem or the batch language. Uses shell.c for
 * transcript output and debug logging.
 *
 * Features:
 *   - Command dispatch table with family routing (wifi, bluetooth, usb, c6ota, sd)
 *   - Dedicated worker task for command execution
 *   - Hardware commands: brightness, rotate, battery, volume, gpio, display
 *   - System commands: reboot, clear/cls, prompt, date, time
 *   - Output redirection (> / >>) applied around every dispatch
 *
 * Delegated ownership:
 *   - `components/storage/` — SD sessions, path resolution, current working
 *     directory, and every DOS file command
 *   - `components/batch/` — batch engine, environment variables, PATH,
 *     variable expansion, errorlevel, and the batch language commands
 *   - `components/shell/` — transcript, history, debug log, UART console,
 *     input line, and the system info commands
 *
 * Architecture:
 *   This module is the only dispatcher. main.c orchestrates boot and routes
 *   LVGL input events; it contains no command implementations.
 */

#ifndef P4MINISHELL_COMMAND_H
#define P4MINISHELL_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * COMMAND EXECUTION
 * ======================================================================== */

/**
 * Execute a command line: expand variables, apply output redirection, and
 * dispatch to the matching built-in. Runs synchronously on the calling task.
 *
 * @param command  Null-terminated command string (copied internally).
 */
void shell_execute_command(char *command);

/**
 * Dispatch an already-expanded command line to its built-in handler.
 * Does not perform variable expansion or output redirection.
 *
 * @param command  Null-terminated command string (modified in-place).
 * @return true if a command was recognized and executed.
 */
bool shell_execute_command_core(char *command);

/**
 * Queue a command for execution on the dedicated worker task.
 * Used by the LVGL input path so heavy commands never run on the LVGL
 * event-callback stack.
 *
 * @param command  Null-terminated command string (copied into the request).
 */
void shell_execute_command_async(char *command);

/**
 * Check if the C6 OTA module has a pending confirmation.
 * @return true if OTA confirmation is pending.
 */
bool shell_command_ota_is_pending(void);

/* ========================================================================
 * SHELL STATE ACCESSORS
 * ======================================================================== */

/**
 * Get the last speaker volume applied through the `volume` command.
 * @return Volume percentage in the range 0..100.
 */
int command_get_volume_percent(void);

/**
 * Set the speaker volume directly (boot CONFIG.SYS VOLUME= directive).
 *
 * Clamps @p percent to 0..100 and mirrors the validation in the interactive
 * `volume` command. No transcript output is produced.
 *
 * @param percent  Volume percentage in the range 0..100.
 */
void command_set_volume(int percent);

/**
 * Read the battery ADC and convert it to millivolts and a charge percentage.
 * Any output pointer may be NULL when that value is not needed.
 *
 * @param battery_mv_out  Scaled battery voltage in millivolts.
 * @param percent_out     Charge estimate in the range 0..100.
 * @param raw_out         Raw ADC sample.
 * @param gpio_mv_out     Voltage measured at the ADC pin before divider scaling.
 * @return ESP_OK on success, or the ESP-IDF error from ADC setup/read.
 */
esp_err_t command_battery_read(int *battery_mv_out, int *percent_out, int *raw_out, int *gpio_mv_out);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/**
 * Initialize the command module. Brings up `storage` and `batch`, registers
 * the batch pipeline hook and the shell operations table. Call once at boot.
 */
void command_init(void);

/** Check if the command module is initialized. */
bool command_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_COMMAND_H */

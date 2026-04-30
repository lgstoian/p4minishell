/**
 * @file command.h
 * @brief Command parser and dispatcher for P4MiniShell.
 *
 * Owns the command execution pipeline: parsing, dispatch, worker task,
 * and all built-in command implementations. Works with shell.c for output
 * and main.c for orchestration.
 *
 * Features:
 *   - Command dispatch table with family routing (wifi, bluetooth, usb, c6ota)
 *   - Dedicated worker task for command execution
 *   - Hardware commands: brightness, rotate, battery, volume, gpio
 *   - File commands: cd, dir, copy, move, del, ren, mkdir, rmdir, type, write, append, touch
 *   - SD commands: sd info, ls, stat, cat
 *   - Batch engine: set, path, echo, call
 *   - Environment variables and output redirection
 *
 * Architecture:
 *   This module OWNS the command execution pipeline. The shell module (shell.c)
 *   provides output functions. main.c orchestrates boot and routes input events.
 */

#ifndef P4MINISHELL_COMMAND_H
#define P4MINISHELL_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * COMMAND EXECUTION
 * ======================================================================== */

/**
 * Execute a command string. This is the main entry point for all command
 * processing. The command is copied and dispatched on a worker task.
 * @param command  Null-terminated command string.
 */
void shell_execute_command(char *command);

/**
 * Execute a command directly (synchronously, on the calling task).
 * Used by the UART console and LVGL input callbacks.
 * @param command  Null-terminated command string (modified in-place).
 * @return true if a command was recognized and executed.
 */
bool shell_execute_command_core(char *command);

/**
 * Check if the C6 OTA module has a pending confirmation.
 * @return true if OTA confirmation is pending.
 */
bool shell_command_ota_is_pending(void);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/** Initialize the command module. Must be called once during boot. */
void command_init(void);

/** Check if the command module is initialized. */
bool command_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_COMMAND_H */

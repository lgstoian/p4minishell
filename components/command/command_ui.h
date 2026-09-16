/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file command_ui.h
 * @brief UI query command handlers for display, keyboard, and windows.
 *
 * These handlers were extracted from command.c to reduce its compile-time
 * dependency surface. They route display, keyboard, and window manager
 * queries through the owning modules' public APIs.
 */

#ifndef P4MINISHELL_COMMAND_UI_H
#define P4MINISHELL_COMMAND_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Handle `display <info|resolution|refresh|power>` subcommands. */
bool shell_command_display(int argc, char **argv);

/** Handle `keyboard <show|hide|toggle|status>` subcommands. */
bool shell_command_keyboard(int argc, char **argv);

/** Handle `windows <info>` subcommands. */
bool shell_command_windows(int argc, char **argv);

/** Handle `cursor [block|bar] [blink <ms|off>]` (input-line cursor style). */
bool shell_command_cursor(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_COMMAND_UI_H */

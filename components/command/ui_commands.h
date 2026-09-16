/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file ui_commands.h
 * @brief `ui` verb family: touch automation / inspection (see ui_commands.c).
 */

#ifndef P4MINISHELL_UI_COMMANDS_H
#define P4MINISHELL_UI_COMMANDS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** `ui ...` shell command (runs on the worker task). */
bool shell_command_ui(int argc, char **argv);

/**
 * Console-reader entry used while a modal blocks the worker. Parses @p line;
 * when it is a `ui` command it runs it (output to the serial console) and
 * returns true. Called before the modal's own serial handler.
 */
bool ui_commands_console(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_UI_COMMANDS_H */

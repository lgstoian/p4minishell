/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file shell_core_internal.h
 * @brief Internal seam between shell_core.c (state) and shell.c (surfaces).
 *
 * shell_core.c owns the shell's hardware-free state: the command-module ops
 * table, the recall-history store, the abort/busy flags and the help registry.
 * Some of that state is also read or written by the surfaces that stay in
 * shell.c (the LVGL input line and history recall, the `help` command), so the
 * object is defined exactly once in shell_core.c and declared here. Nothing is
 * duplicated, and shell_core.c still needs no board, display or storage.
 *
 * The header is private to components/shell; no other component includes it.
 *
 * @module components/shell
 * @layer shell
 * @domain shell
 * @provides internal declarations for state owned by shell_core.c
 * @status stable
 * @test test/main/test_shell_abort.c, test/main/test_history_file.c
 * @hard-rule declarations only - no storage, display, USB or board BSP here
 */

#ifndef P4MINISHELL_SHELL_CORE_INTERNAL_H
#define P4MINISHELL_SHELL_CORE_INTERNAL_H

#include <stddef.h>

#include "p4minishell_config.h"
#include "shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * COMMAND MODULE OPS
 * ========================================================================
 * shell_register_command_ops() writes this table from shell.c; the parts of
 * the hardware-free core that need a command-module service (for example
 * shell_command_should_store_history() asking whether a C6 OTA confirmation is
 * pending) read it here.
 */
extern shell_command_ops_t shell_core_command_ops;

/* ========================================================================
 * COMMAND HISTORY STORE (shell_core.c) / RECALL CURSOR (shell.c)
 * ========================================================================
 * The store itself is heap-backed and owned by shell_core.c. The recall
 * cursor and the draft line are part of the same state, but only the code
 * that paints the input line touches them, so shell.c reads them by name.
 */
extern char **shell_core_command_history;
extern size_t shell_core_command_history_count;
extern int shell_core_command_history_cursor;
extern char shell_core_history_draft[P4_CONFIG_COMMAND_BYTES];

/* ========================================================================
 * HELP REGISTRY
 * ========================================================================
 * One table of (name, summary) pairs backs both `help` output in shell.c and
 * the accessors used by the completion providers. It lives in shell_core.c so
 * the host lane can read it without the LVGL/display half of shell.c.
 */
typedef struct {
    const char *name;      /* primary command name (aliases listed in summary) */
    const char *summary;   /* one-line usage + description */
} shell_help_entry_t;

extern const shell_help_entry_t shell_core_help_entries[];

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_SHELL_CORE_INTERNAL_H */

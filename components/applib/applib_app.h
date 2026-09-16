/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file applib_app.h
 * @brief Native-app ABI: entry point, registration, and dispatch.
 *
 * A native app is a C function with the signature `app_main_t`. It is linked
 * into the firmware, registered with `app_register()`, and becomes a shell
 * command: typing the app's name at the prompt (or calling it from a batch
 * file) invokes the entry point with `argc`/`argv` exactly like a program —
 * `argv[0]` is the app name, `argv[1]`.. are its arguments. Inside the entry
 * the app uses the other applib groups (`app_printf` for output, `app_env_*` /
 * `app_get_cwd` from applib_env.h for the process environment, `app_ini_*`
 * for persistent state, ...).
 *
 * The entry's return value becomes the command ERRORLEVEL, so a batch file can
 * branch on it with `if errorlevel N` exactly as with any other command. Apps
 * are dispatched after every built-in and batch-file lookup fails, so an app
 * name can never shadow a built-in or an on-SD `.bat`.
 */

#ifndef P4MINISHELL_APPLIB_APP_H
#define P4MINISHELL_APPLIB_APP_H

#include <stdbool.h>
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The native app entry point.
 *
 * @param argc  Number of arguments (>= 1; argv[0] is the app name).
 * @param argv  NULL-terminated argument vector, valid for the call.
 * @return The app's exit code, which becomes the shell ERRORLEVEL (0 = OK).
 */
typedef int (*app_main_t)(int argc, char **argv);

/**
 * Register a native app so its name becomes a shell command.
 *
 * @param name        Command name (the prompt name; case-insensitive at
 *                    dispatch). Capped at `P4_CONFIG_APP_NAME_BYTES`.
 * @param description One-line description (used by the `apps` listing),
 *                    capped at `P4_CONFIG_APP_DESC_BYTES`.
 * @param entry       The app's entry point (must be non-NULL).
 * @return true when registered, false when the table is full or the
 *         arguments are invalid.
 */
bool app_register(const char *name, const char *description, app_main_t entry);

/**
 * Look up a registered app by command name.
 * @return true when an app with that name exists.
 */
bool app_find(const char *name);

/**
 * Dispatch a command line to a registered app.
 *
 * @param argc           Argument count from the shell dispatcher.
 * @param argv           Argument vector; argv[0] is the command name.
 * @param errorlevel_out Receives the app's return value when dispatched.
 * @return true when argv[0] names a registered app (which was run), false
 *         when no app matched.
 */
bool app_dispatch(int argc, char **argv, int *errorlevel_out);

/**
 * Iterate the registered app table.
 * @param index 0-based slot index (0..P4_CONFIG_APP_MAX-1).
 * @param name_out, desc_out  Buffers (or NULL) receiving the app's name /
 *              description.
 * @return true when slot @p index holds a registered app.
 */
bool app_get(int index, char *name_out, size_t name_size,
             char *desc_out, size_t desc_size);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_APP_H */

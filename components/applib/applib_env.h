/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file applib_env.h
 * @brief Process-environment group of the applib (env table + current directory).
 *
 * The native-app ABI hands each app its argv through its entry point (see
 * applib_app.h) and live access to the shell's environment table and current
 * working directory through the helpers here. Environment access is routed
 * through the registered `applib_env_ops_t` table because the env table is
 * owned by `components/batch/` (which `applib` must never include); cwd is
 * owned by `components/storage/` and read through the same table for a uniform
 * contract. Every hook is NULL-checked, so the helpers degrade gracefully
 * before `command_init()` registers the table.
 */

#ifndef P4MINISHELL_APPLIB_ENV_H
#define P4MINISHELL_APPLIB_ENV_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Registered by `command_init()`; every hook is NULL-checked before use. */
typedef struct {
    /** Look up a variable in the shell's RAM environment table (NULL when
     *  unset or before registration). */
    const char *(*get_env)(const char *name);
    /** Set (or clear, with NULL/"") a variable in the shell environment. */
    esp_err_t   (*set_env)(const char *name, const char *value);
    /** The shell's current working directory (absolute path, or NULL). */
    const char *(*get_cwd)(void);
} applib_env_ops_t;

/** Register (or clear, with NULL) the environment ops used by the helpers. */
void applib_register_env_ops(const applib_env_ops_t *ops);

/**
 * Read a shell environment variable.
 * @return The value, or NULL when unset / the ops table is not registered.
 *         The returned pointer is owned by the shell env table and stays valid
 *         until that variable is changed — copy it if you keep it.
 */
const char *app_env_get(const char *name);

/**
 * Set a shell environment variable (empty/NULL clears it).
 * @return true on success (including clear), false on invalid name, a full
 *         table, or before registration.
 */
bool app_env_set(const char *name, const char *value);

/**
 * The shell's current working directory (absolute path).
 * @return A path string, or NULL before registration.
 */
const char *app_get_cwd(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_ENV_H */

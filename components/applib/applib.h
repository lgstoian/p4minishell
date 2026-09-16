/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file applib.h
 * @brief Native-app runtime library (the shell SDK surface) — umbrella header.
 *
 * `applib` is the formal runtime contract for native applications that run on
 * the P4MiniShell core (the `edit` editor is the reference implementation of
 * the modal app surface). It is organized as **lean, focused headers** so an
 * app includes only the groups it uses; this umbrella header pulls all of them
 * in for convenience.
 *
 *   - `applib_console.h` — `app_printf` / `app_printf_ansi` and the semantic
 *     print helpers. An app's stdout is the transcript, which is the
 *     redirection layer: an app invoked inside a command dispatch is captured
 *     by `>` / `>>` exactly like a built-in command.
 *   - `applib_mem.h`    — the shared memory-allocation policy
 *     (`app_alloc`/`app_strdup`/..., PSRAM-aware) and `app_report_*`
 *     debug-log error reporting.
 *   - `applib_time.h`   — time, timers, sleep, and a compact `app_sysinfo`
 *     summary.
 *   - `applib_net.h`    — Wi-Fi state accessors routed through the registered
 *     `applib_net_ops_t` table (this component never includes `networking.h`).
 *   - `applib_input.h`  — input with timeouts (`app_wait_key`,
 *     `app_read_line`) backed by the shell's bounded key queue.
 *   - `applib_state.h`  — persistent state: `app_ini_*` reads/updates
 *     `KEY=VALUE` INI files on the SD card and `app_temp_*` manages
 *     SD-backed temporary files (all storage on the SD card).
 *   - `applib_app.h`    — the native-app ABI: the `app_main_t` entry point,
 *     `app_register()` (makes an app a shell command), and dispatch.
 *   - `applib_env.h`    — the process environment: `app_env_get/set` and
 *     `app_get_cwd`, routed through a registered ops table (the env table is
 *     owned by `components/batch/`, cwd by `components/storage/`).
 *
 * Layering: `applib` depends only on `shell`, `clock`, and `storage` (for
 * the shared INI / temp-file mechanics, the same guarded-SD pattern the
 * `edit` app uses) plus FreeRTOS / heap / esp_timer. It is a leaf — no
 * component may require it except the app that links it and the command
 * module that registers the networking and environment hooks.
 */

#ifndef P4MINISHELL_APPLIB_H
#define P4MINISHELL_APPLIB_H

#include "applib_console.h"
#include "applib_mem.h"
#include "applib_time.h"
#include "applib_net.h"
#include "applib_input.h"
#include "applib_state.h"
#include "applib_db.h"
#include "applib_ui.h"
#include "applib_app.h"
#include "applib_env.h"
#include "applib_tui.h"

#endif /* P4MINISHELL_APPLIB_H */

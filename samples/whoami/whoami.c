/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file whoami.c
 * @brief Sample out-of-tree-style native app: the `whoami` command.
 *
 * This component is structured exactly like an external applib app: its own
 * directory, its own CMakeLists (REQUIRES applib only), no includes of
 * shell/batch/storage/networking headers. It only speaks the applib ABI
 * (console, env, persistent state) and registers one `app_main_t` entry.
 *
 * Run at the prompt:
 *   whoami            -> argv count, cwd, PATH, visit counter
 *   whoami reset      -> clear the visit counter (ERRORLEVEL 0)
 *
 * The visit counter lives in sd:/APPS/WHOAMI.INI via app_ini_*, so it
 * demonstrates the persistent-state group the same way `hello` in
 * main/native_apps.c demonstrates argv/env.
 */

#include "applib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHOAMI_INI_FILE "APPS/WHOAMI.INI"
#define WHOAMI_COUNT_KEY "visits"

static int whoami_main(int argc, char **argv)
{
    const char *cwd = app_get_cwd();
    const char *path = app_env_get("PATH");
    char count_text[24];
    int visits = 0;

    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        (void)app_ini_delete(WHOAMI_INI_FILE, WHOAMI_COUNT_KEY);
        app_printf("whoami: counter reset\n");
        return 0;
    }

    if (app_ini_get(WHOAMI_INI_FILE, WHOAMI_COUNT_KEY,
                    count_text, sizeof(count_text))) {
        visits = atoi(count_text);
    }
    visits++;
    snprintf(count_text, sizeof(count_text), "%d", visits);
    if (!app_ini_set(WHOAMI_INI_FILE, WHOAMI_COUNT_KEY, count_text)) {
        app_report_warning("whoami", "could not persist visit count");
    }

    app_printf("whoami: %d argument(s)\n", argc);
    app_printf("  cwd   = \"%s\"\n", cwd != NULL ? cwd : "(unavailable)");
    app_printf("  PATH  = \"%s\"\n", path != NULL ? path : "(unset)");
    app_printf("  visits = %d\n", visits);
    return 0;
}

void whoami_register(void)
{
    (void)app_register("whoami",
                       "applib sample: argv/env plus a persistent visit counter",
                       whoami_main);
}

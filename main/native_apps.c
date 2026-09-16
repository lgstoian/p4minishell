/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file native_apps.c
 * @brief Sample native apps that demonstrate the applib native-app ABI.
 *
 * A native app is a C function with the signature `app_main_t` registered via
 * `app_register()`. Once registered it becomes a shell command: typing its
 * name dispatches to the entry point with `argc`/`argv` (argv[0] = the app
 * name), and the return value becomes ERRORLEVEL. Inside the entry the app
 * uses the applib groups — `app_printf` for output, `app_env_*`/`app_get_cwd`
 * for the process environment, `app_ini_*` for persistent state — exactly the
 * contract a batch file gets through `%0..%9`/`%*`, the env table, and the
 * current directory.
 *
 * `hello` is the reference sample: it echoes its argv and the process
 * environment (cwd + PATH). Run it at the prompt:
 *
 *   hello            -> shows argv, cwd, PATH
 *   hello a b c      -> shows argv[0..3]
 */

#include "p4minishell.h"
#include "applib.h"

#include <string.h>

/* The sample native app entry point (the ABI). */
static int hello_main(int argc, char **argv)
{
    const char *cwd = app_get_cwd();
    const char *path = app_env_get("PATH");
    int i;

    app_printf("hello: %d argument(s)\n", argc);
    for (i = 0; i < argc; i++) {
        app_printf("  argv[%d] = \"%s\"\n", i, argv[i] != NULL ? argv[i] : "(null)");
    }
    app_printf("cwd  = \"%s\"\n", cwd != NULL ? cwd : "(unavailable)");
    app_printf("PATH = \"%s\"\n", path != NULL ? path : "(unset)");

    /* Demonstrate env writes + ERRORLEVEL propagation back to a batch file. */
    if (argc == 2 && strcmp(argv[1], "test") == 0) {
        app_env_set("HELLO_RESULT", "ok");
        app_printf("set HELLO_RESULT=ok\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "fail") == 0) {
        return 1;
    }
    return 0;
}

void native_apps_register(void)
{
    (void)app_register("hello",
                       "native-app ABI sample: show argv, env, and cwd",
                       hello_main);
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file webapp_commands.c
 * @brief Batch-facing webapp facade over the ONE browser/HTML/CSS/JS engine.
 *
 * A `.bat` app can present a page with `webapp open`, read the rendered cell
 * grid with `webapp dump`, inspect CSS with `webapp css`, and run in-page JS
 * with `webapp js`. Every subcommand delegates to the existing verb handler:
 * there is no second renderer, no second JS runtime and no new surface. This
 * is an additive scripting surface for batch + native + webapp hybrids.
 */

#include "command.h"
#include "shell.h"
#include "batch.h"
#include "browser.h"
#include "jsengine.h"
#include <string.h>

#define WEBAPP_MAX_ARGV 16

static int webapp_call_html(int argc, char **argv, int from)
{
    char *args[WEBAPP_MAX_ARGV];
    int n = 0;
    int i;

    args[n++] = "html";
    for (i = from; i < argc && n < WEBAPP_MAX_ARGV - 1; i++) {
        if (strcmp(argv[i], "/no") == 0) {
            args[n++] = "/no";
        }
    }
    for (i = from; i < argc && n < WEBAPP_MAX_ARGV - 1; i++) {
        if (strcmp(argv[i], "/no") != 0) {
            args[n++] = argv[i];
        }
    }
    return shell_command_html(n, args);
}

static int webapp_call_tui(int argc, char **argv, int from)
{
    char *args[WEBAPP_MAX_ARGV];
    int n = 0;
    int i;

    args[n++] = "tui";
    args[n++] = "dump";
    for (i = from; i < argc && n < WEBAPP_MAX_ARGV; i++) {
        args[n++] = argv[i];
    }
    shell_command_tui(n, args);
    return batch_get_errorlevel();
}

static int webapp_call_js(int argc, char **argv, int from)
{
    char *args[WEBAPP_MAX_ARGV];
    int n = 0;
    int i;

    args[n++] = "js";
    args[n++] = "eval";
    for (i = from; i < argc && n < WEBAPP_MAX_ARGV; i++) {
        args[n++] = argv[i];
    }
    return shell_command_js(n, args);
}

int shell_command_webapp(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage(
            "Usage: webapp open [/no] <file|url>   present a page (html engine)\n"
            "       webapp dump [from] [to] [/a]   read the rendered cell grid\n"
            "       webapp css                      show computed CSS\n"
            "       webapp js <code>                evaluate JS in the page engine\n"
            "       webapp event <id> <type>        dispatch click/input/change/submit\n"
            "       webapp tick                     run due JS timers, repaint");
        batch_set_errorlevel(2);
        return 2;
    }

    if (strcmp(argv[1], "open") == 0) {
        return webapp_call_html(argc, argv, 2);
    }
    if (strcmp(argv[1], "dump") == 0) {
        return webapp_call_tui(argc, argv, 2);
    }
    if (strcmp(argv[1], "js") == 0) {
        return webapp_call_js(argc, argv, 2);
    }
    if (strcmp(argv[1], "css") == 0) {
        char *args[2];
        args[0] = "browser";
        args[1] = "css";
        return shell_command_browser(2, args);
    }
    if (strcmp(argv[1], "event") == 0) {
        int rc;

        if (argc < 4) {
            shell_print_usage("webapp event <id> <click|load|input|change|submit>");
            batch_set_errorlevel(2);
            return 2;
        }
        rc = browser_dispatch_dom_event(argv[2], argv[3]);
        batch_set_errorlevel((uint8_t)rc);
        return rc;
    }
    if (strcmp(argv[1], "tick") == 0) {
        int fired = browser_tick_timers();

        shell_transcript_appendf("webapp: %d timer(s) fired\n", fired);
        batch_set_errorlevel(0);
        return 0;
    }

    shell_print_error("webapp: unknown subcommand '%s'", argv[1]);
    batch_set_errorlevel(2);
    return 2;
}

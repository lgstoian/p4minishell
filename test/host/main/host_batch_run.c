/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file host_batch_run.c
 * @brief Standalone (non-Unity) host runner for REAL .bat apps.
 *
 * Unlike host_main.c this is a standalone, non-Unity entry point: on the
 * linux IDF target the FreeRTOS port's main() calls our app_main(). It links
 * the same production batch sources the firmware and the Unity lane compile
 * - notably the real
 * interpreter in components/batch/batch.c - so a fixture runs through the ONE
 * implementation. No command language is reimplemented here.
 *
 * The one host-only piece is the `batch_command_ops_t.execute_command` hook:
 * firmware command.c owns the full line pipeline, but linking all of command.c
 * would drag LVGL/USB/network into the host lane. This file supplies a THIN
 * router: expand variables with the real shell_expand_variables(), tokenise
 * with the real shell_split_args(), and call the real shell_command_* verbs in
 * batch.c / batch_env.c. No verb semantics, parser, evaluator or echo renderer
 * is duplicated; adding a feature to a verb changes this file not at all.
 *
 * Input/output reach the board world only through host_batch_bridge.c: an
 * explicit POSIX fixture root installed by host_batch_fs_init(). With no root
 * the bridge is honest about having no card and the run fails visibly.
 *
 * Usage:
 *   host_batch_run [root] [/sdcard/HELLO.BAT]
 * root defaults to env P4_HOST_SD_ROOT, then tools/host/fixtures/batch.
 */
#define P4_STORAGE_NO_FATFS_TYPES 1

#include "batch.h"
#include "batch_env_internal.h"
#include "p4minishell_config.h"
#include "shell.h"
#include "shell_transcript_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Implemented in host_batch_bridge.c (host-only, not a public firmware API). */
extern bool host_batch_fs_init(const char *root);

#define HOST_BATCH_ARGV_MAX 24
#define HOST_DEFAULT_SD_ROOT "tools/host/fixtures/batch"
#define HOST_DEFAULT_SCRIPT "/sdcard/HELLO.BAT"

/**
 * Thin command router behind batch_command_ops_t.execute_command.
 *
 * This mirrors the shape of command.c's dispatch (trim, comments, expansion,
 * redirection-free split, verb lookup) but owns none of the verb logic: every
 * branch below is a direct call to the real shell_command_* function. Fixture
 * lines that need redirection, groups or the broader command set are reported
 * honestly as unsupported on this host lane rather than silently faked.
 */
static void host_batch_execute_command(char *command)
{
    char expanded[P4_CONFIG_COMMAND_BYTES];
    char *echo_line = NULL;
    char *argv[HOST_BATCH_ARGV_MAX];
    char *trimmed;
    char *first;
    int argc;
    bool is_echo;

    if (command == NULL) {
        return;
    }

    /* The real expansion core owns %VAR%, %0..%9, substrings and replacements.
     * Nested `if`/`call` bodies re-enter here through batch_run_nested(), so a
     * line is expanded exactly once per segment, as in command.c. */
    shell_expand_variables(command, expanded, sizeof(expanded));
    trimmed = shell_trim(expanded);
    if (*trimmed == '\0') {
        return;
    }

    /* Comments are opaque through end of line (cmd.exe parity). */
    if (shell_text_equals_ignore_case(trimmed, "rem") ||
        strncmp(trimmed, "::", 2) == 0) {
        return;
    }

    /* echo owns the whole raw remainder (including glued separators and long
     * lines). Keep an unsplit snapshot exactly as command.c does; the real
     * shell_command_echo_text() renderer is the only echo implementation. */
    is_echo = (strncasecmp(trimmed, "echo", 4) == 0 &&
               (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4]))) ||
              shell_command_echo_glued(trimmed, NULL, NULL);
    if (is_echo) {
        echo_line = strdup(trimmed);
    }

    /* The shared tokenizer strips DOS quoting with the same rules the firmware
     * dispatcher uses, so `if "p4"=="p4" echo ...` reaches shell_command_if()
     * as the joined `p4==p4` condition it already understands. */
    argc = shell_split_args(trimmed, argv, HOST_BATCH_ARGV_MAX);
    if (argc == 0) {
        free(echo_line);
        return;
    }
    first = argv[0];

    if (shell_text_equals_ignore_case(first, "set")) {
        shell_command_set(argc, argv);
    } else if (is_echo) {
        if (echo_line != NULL) {
            shell_command_echo_text(echo_line);
        } else {
            shell_command_echo(argc, argv);
        }
    } else if (shell_text_equals_ignore_case(first, "if")) {
        shell_command_if(argc, argv);
    } else if (shell_text_equals_ignore_case(first, "goto")) {
        shell_command_goto(argc, argv);
    } else if (shell_text_equals_ignore_case(first, "call")) {
        shell_command_call(argc, argv);
    } else if (shell_text_equals_ignore_case(first, "gosub")) {
        shell_command_gosub(argc, argv);
    } else if (shell_text_equals_ignore_case(first, "return")) {
        shell_command_return(argc, argv);
    } else if (shell_text_equals_ignore_case(first, "path")) {
        shell_command_path(argc, argv);
    } else if (shell_text_equals_ignore_case(first, "setlocal")) {
        shell_command_setlocal(argc, argv);
    } else if (shell_text_equals_ignore_case(first, "endlocal")) {
        shell_command_endlocal(argc, argv);
    } else if (shell_text_equals_ignore_case(first, "exit")) {
        shell_command_exit(argc, argv);
    } else {
        shell_transcript_appendf("batch: unsupported command on this host lane: %s\n",
                                 first);
        batch_set_errorlevel(P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND);
    }

    free(echo_line);
}

/**
 * The linux IDF target supplies main() from the FreeRTOS port and starts the
 * scheduler before calling app_main(), so argv is not handed to us directly.
 * Read the NUL-separated process cmdline (POSIX-only, this file is host-only)
 * to honour the documented `[root] [script]` positional arguments. Env vars
 * are accepted as a fallback for callers that cannot pass argv.
 */
static bool host_cmdline_arg(int index, char *out, size_t out_size)
{
    static char buf[4096];
    FILE *fh = fopen("/proc/self/cmdline", "rb");
    size_t length;
    size_t pos = 0;
    int arg = 0;

    if (fh == NULL || out == NULL || out_size == 0) {
        return false;
    }
    length = fread(buf, 1, sizeof(buf) - 1u, fh);
    fclose(fh);
    buf[length] = '\0';
    while (pos < length) {
        size_t len = strlen(buf + pos);

        if (arg == index) {
            snprintf(out, out_size, "%.*s", (int)len, buf + pos);
            return true;
        }
        pos += len + 1u;
        arg++;
    }
    return false;
}

void app_main(void)
{
    static const batch_command_ops_t ops = {
        .execute_command = host_batch_execute_command,
    };
    char root_arg[512];
    char script_arg[512];
    const char *root;
    const char *script;
    size_t transcript_len;
    const char *transcript;
    esp_err_t error;
    int level;

    root = getenv("P4_HOST_SD_ROOT");
    if (host_cmdline_arg(1, root_arg, sizeof(root_arg))) {
        root = root_arg;
    }
    if (root == NULL || root[0] == '\0') {
        root = HOST_DEFAULT_SD_ROOT;
    }
    script = getenv("P4_HOST_BATCH_SCRIPT");
    if (host_cmdline_arg(2, script_arg, sizeof(script_arg))) {
        script = script_arg;
    }
    if (script == NULL || script[0] == '\0') {
        script = HOST_DEFAULT_SCRIPT;
    }

    /* Explicit registration hook: the Unity lane never calls it, so its 19
     * honest stubs keep reporting an absent card. A missing/fixture-less root
     * makes this fail before any command runs - no false green. */
    if (!host_batch_fs_init(root)) {
        fprintf(stderr, "host_batch_run: fixture root is not a directory: %s\n", root);
        fflush(stderr);
        exit(2);
    }
    if (!shell_transcript_model_init()) {
        fprintf(stderr, "host_batch_run: cannot allocate the transcript buffers\n");
        fflush(stderr);
        exit(2);
    }
    shell_transcript_set_sink(NULL, NULL);
    shell_transcript_reset();

    if (!batch_env_pool_init()) {
        fprintf(stderr, "host_batch_run: cannot allocate the environment pool\n");
        fflush(stderr);
        exit(2);
    }
    /* batch_init() would install the firmware I/O table (and touch symbols that
     * do not exist on this lane), so initialise only the hardware-free core. */
    (void)shell_env_set("PATH", "sd:/");
    batch_set_default_echo(true);
    batch_set_errorlevel(0);
    batch_register_command_ops(&ops);

    error = shell_execute_batch_file(script, 0, NULL);
    if (error != ESP_OK) {
        shell_transcript_appendf("batch: %s could not be executed (%s)\n",
                                 script, esp_err_to_name(error));
    }

    level = batch_get_errorlevel();
    transcript_len = shell_transcript_get_length();
    transcript = shell_transcript_get_text_from(0);

    printf("=== BATCH TRANSCRIPT ===\n");
    if (transcript != NULL && transcript_len > 0) {
        fwrite(transcript, 1, transcript_len, stdout);
        if (transcript[transcript_len - 1] != '\n') {
            printf("\n");
        }
    }
    printf("=== BATCH DONE ===\n");
    printf("ERRORLEVEL %d\n", level);

    /* The linux IDF target keeps the scheduler alive after app_main returns;
     * exit explicitly so the gate sees EOF and the real exit status. */
    fflush(stdout);
    exit((error == ESP_OK) ? 0 : 1);
}

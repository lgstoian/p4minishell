/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file reg_commands.c
 * @module command
 * @layer command
 * @domain state
 * @provides `reg` DOS-shaped facade over the ONE storage_ini file core
 * @status experimental
 * @test test/main/test_batch_control.c, tools/host_batch_run.py
 * @hard-rule ai-context.md#hard-rules (no second store, parser or writer)
 * @brief Batch-facing registry facade: list/get/set/del on any INI file.
 *
 * "Registry" here means a DOS-shaped view of a KEY=VALUE file, not a Windows
 * hive. Every operation delegates to `storage_ini_file_get/set/delete/foreach`,
 * which is the single INI core used by `ini`, `appconfig`, `assoc`, `startup`,
 * services, APPINFO and the rest. This file adds no store format, no parser
 * and no second writer: `set`/`del` call the same guarded atomic temp+rename
 * path those callers use, so a failed write never leaves a partial file.
 *
 * Batch-friendly: `/b` emits uncoloured, machine-parsable lines; ERRORLEVEL
 * is 0 on success, 1 on I/O/not-found, 2 on usage.
 */

/* Only the guarded INI prototypes are used here; no FATFS types. */
#define P4_STORAGE_NO_FATFS_TYPES 1

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "batch.h"
#include "command.h"
#include "p4minishell_config.h"
#include "shell.h"
#include "storage.h"

/* A `/b` switch may appear before or after the subcommand (DOS habit). It is
 * peeled here so the rest of the parser never sees it. */
static bool reg_take_bare(int argc, char **argv, int *out_argc, char **out_argv)
{
    int n = 0;
    int i;
    bool bare = false;

    for (i = 1; i < argc; i++) {
        if (shell_text_equals_ignore_case(argv[i], "/b")) {
            bare = true;
        } else {
            out_argv[n++] = argv[i];
        }
    }
    *out_argc = n;
    out_argv[n] = NULL;
    return bare;
}

/* Both the firmware and the host bridge expect a resolved VFS path at the
 * storage_ini core. Reuse the ONE resolver instead of hand-mapping `sd:`. */
static char *reg_resolve_path(const char *path)
{
    char *resolved = malloc(P4_CONFIG_SD_PATH_BYTES);

    if (resolved == NULL) {
        return NULL;
    }
    if (path == NULL || shell_fs_resolve_path(path, resolved, P4_CONFIG_SD_PATH_BYTES) != ESP_OK) {
        free(resolved);
        return NULL;
    }
    return resolved;
}

static void reg_usage(void)
{
    shell_print_usage(
        "Usage: reg [/b] list <ini>\n"
        "       reg [/b] get <ini> <key>\n"
        "       reg [/b] set <ini> <key> <value...>\n"
        "       reg [/b] del <ini> <key>");
}

/* storage_ini_file_foreach callback. `bare` selects `key=value` with no ANSI
 * colour so a redirect or pipe stays parseable; the coloured form uses the
 * same label/value helpers every other status command uses. */
typedef struct {
    bool bare;
    int lines;
} reg_list_ctx_t;

static bool reg_list_cb(const char *key, const char *value, void *ctx)
{
    reg_list_ctx_t *state = (reg_list_ctx_t *)ctx;

    if (key == NULL) {
        return true;
    }
    if (state->bare) {
        shell_transcript_appendf("%s=%s\n", key, value != NULL ? value : "");
    } else {
        shell_print_field(key, "%s", value != NULL ? value : "");
    }
    state->lines++;
    return true;
}

static int reg_list(const char *path, bool bare)
{
    reg_list_ctx_t ctx;
    esp_err_t error;

    ctx.bare = bare;
    ctx.lines = 0;
    error = storage_ini_file_foreach(path, reg_list_cb, &ctx);
    if (error != ESP_OK) {
        shell_print_error("reg: cannot read %s (%s)", path, esp_err_to_name(error));
        return 1;
    }
    if (!bare && ctx.lines == 0) {
        shell_print_muted("reg: %s is empty", path);
    }
    return 0;
}

static int reg_get(const char *path, const char *key, bool bare)
{
    char *value;
    esp_err_t error;

    if (key == NULL || key[0] == '\0') {
        return 2;
    }
    /* Large buffers stay off the 32 KB command worker stack. */
    value = malloc(P4_CONFIG_INI_MAX_BYTES + 1);
    if (value == NULL) {
        shell_print_error("reg: out of memory");
        return 1;
    }
    error = storage_ini_file_get(path, key, value, P4_CONFIG_INI_MAX_BYTES + 1);
    if (error != ESP_OK) {
        shell_print_error("reg: %s not found in %s", key, path);
        free(value);
        return 1;
    }
    if (bare) {
        shell_transcript_appendf("%s\n", value);
    } else {
        shell_print_field(key, "%s", value);
    }
    free(value);
    return 0;
}

static int reg_set(const char *path, const char *key, int argc, char **argv,
                   bool bare)
{
    char *value;
    esp_err_t error;

    if (key == NULL || key[0] == '\0' || argc <= 0) {
        return 2;
    }
    value = malloc(P4_CONFIG_INI_MAX_BYTES + 1);
    if (value == NULL) {
        shell_print_error("reg: out of memory");
        return 1;
    }
    /* @p argv is the value words slice and @p argc its length. */
    shell_join_args(argv, 0, argc, value, P4_CONFIG_INI_MAX_BYTES + 1);
    /* No new store format and no hand-rolled writer: the ONE atomic,
     * free-space-guarded INI writer used by ini/assoc/startup does this. */
    error = storage_ini_file_set(path, key, value);
    free(value);
    if (error != ESP_OK) {
        shell_print_error("reg: could not write %s (%s)", path, esp_err_to_name(error));
        return 1;
    }
    if (!bare) {
        shell_print_ok("reg: %s set", key);
    }
    return 0;
}

static int reg_del(const char *path, const char *key, bool bare)
{
    esp_err_t error;

    if (key == NULL || key[0] == '\0') {
        return 2;
    }
    error = storage_ini_file_delete(path, key);
    if (error != ESP_OK) {
        shell_print_error("reg: %s not found in %s", key, path);
        return 1;
    }
    if (!bare) {
        shell_print_ok("reg: %s removed", key);
    }
    return 0;
}

int shell_command_reg(int argc, char **argv)
{
    char *words[24];
    int count = 0;
    bool bare;
    int result;

    if (argv == NULL || argc < 3) {
        reg_usage();
        batch_set_errorlevel(2);
        return 2;
    }
    /* `words[0]` is the subcommand; argv[0] ("reg") is deliberately dropped
     * by reg_take_bare so the /b switch cannot shift the argument indices. */
    bare = reg_take_bare(argc, argv, &count, words);
    if (count < 2) {
        reg_usage();
        batch_set_errorlevel(2);
        return 2;
    }

    {
        char *path = reg_resolve_path(words[1]);

        if (path == NULL) {
            shell_print_error("reg: cannot resolve %s", words[1]);
            batch_set_errorlevel(1);
            return 1;
        }
        if (shell_text_equals_ignore_case(words[0], "list")) {
            if (count != 2) {
                free(path);
                reg_usage();
                batch_set_errorlevel(2);
                return 2;
            }
            result = reg_list(path, bare);
        } else if (shell_text_equals_ignore_case(words[0], "get")) {
            if (count != 3) {
                free(path);
                reg_usage();
                batch_set_errorlevel(2);
                return 2;
            }
            result = reg_get(path, words[2], bare);
        } else if (shell_text_equals_ignore_case(words[0], "set")) {
            if (count < 4) {
                free(path);
                reg_usage();
                batch_set_errorlevel(2);
                return 2;
            }
            result = reg_set(path, words[2], count - 3, &words[3], bare);
        } else if (shell_text_equals_ignore_case(words[0], "del") ||
                   shell_text_equals_ignore_case(words[0], "delete")) {
            if (count != 3) {
                free(path);
                reg_usage();
                batch_set_errorlevel(2);
                return 2;
            }
            result = reg_del(path, words[2], bare);
        } else {
            shell_print_error("reg: unknown subcommand '%s'", words[0]);
            reg_usage();
            result = 2;
        }
        free(path);
    }

    batch_set_errorlevel((uint8_t)result);
    return result;
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file lib_commands.c
 * @module command
 * @layer command
 * @domain state
 * @provides `lib` facade over the existing `call <file.bat>::<routine>` engine
 * @status experimental
 * @test test/main/test_batch_control.c, tools/host_batch_run.py
 * @hard-rule ai-context.md#hard-rules (no dynamic linking, no second runtime)
 * @brief Batch-facing shared-routine library manifest, backed by storage_ini.
 *
 * Here "DLL" means a shared **batch / applib routine library**, never native
 * code loading. A library is a `.bat` file whose labelled routines are invoked
 * with the existing `call <file.bat>::<routine>` path; that path already gives
 * variable isolation and final-ERRORLEVEL propagation. There is no `dlopen`,
 * no ELF loader, no second app runtime and no second dispatcher: `lib load`
 * only synthesises the `call` argv and hands it to the one batch call engine.
 *
 * Discovery is one INI manifest (`sd:/APPS/LIBS.INI`) read through the same
 * `storage_ini_file_*` core every other persistent map uses. Each line is
 * `NAME=<file.bat>::<routine>`; the file path is stored as a full `sd:/...`
 * path so resolution is deterministic (the loaded routine keeps the running
 * app's APPDRIVE/APPROOT/APPNAME, as `call file::routine` always has).
 *
 * Batch-friendly: `/b` emits uncoloured `name=target` lines; ERRORLEVEL is
 * 0 ok, 1 missing/invalid/duplicate target, 2 usage.
 */

/* Only the guarded INI prototypes are used here; no FATFS types. */
#define P4_STORAGE_NO_FATFS_TYPES 1

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "batch.h"
#include "command.h"
#include "p4minishell_config.h"
#include "shell.h"
#include "storage.h"

#define LIB_MANIFEST_PATH "sd:/APPS/LIBS.INI"
#define LIB_MAX_ARGV 24

/* The manifest core uses the same name/value bounds as the other INI maps;
 * these buffers are heap-allocated by callers, never left on the worker
 * stack. */
#define LIB_TARGET_BYTES (P4_CONFIG_INI_MAX_BYTES + 1)

static void lib_usage(void)
{
    shell_print_usage(
        "Usage: lib [/b] list\n"
        "       lib [/b] info <name>\n"
        "       lib [/b] load <name> [args...]");
}

/** Split a manifest target into file and routine. Pure: no I/O. */
static bool lib_parse_target(const char *target, char *file, size_t file_size,
                             char *routine, size_t routine_size)
{
    const char *sep;
    size_t file_len;

    if (target == NULL || file == NULL || routine == NULL ||
        file_size == 0 || routine_size == 0) {
        return false;
    }
    sep = strstr(target, "::");
    if (sep == NULL) {
        return false;
    }
    file_len = (size_t)(sep - target);
    if (file_len == 0 || file_len >= file_size ||
        snprintf(routine, routine_size, "%s", sep + 2) < 0 ||
        (size_t)strlen(sep + 2) >= routine_size || routine[0] == '\0') {
        return false;
    }
    memcpy(file, target, file_len);
    file[file_len] = '\0';
    return true;
}

/** A `/b` switch may appear before or after the subcommand. */
static bool lib_take_bare(int argc, char **argv, int *out_argc, char **out_argv)
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

/* FNV-1a of a target string. Keeping hashes (not full strings) in the
 * duplicate table avoids a multi-kilobyte stack local on the worker path. */
static uint32_t lib_target_hash(const char *text)
{
    uint32_t hash = 2166136261u;
    const unsigned char *cursor = (const unsigned char *)text;

    while (cursor != NULL && *cursor != '\0') {
        hash ^= *cursor++;
        hash *= 16777619u;
    }
    return hash;
}

typedef struct {
    bool bare;
    bool duplicate;
    int lines;
    uint32_t targets[16];
    int target_count;
} lib_list_ctx_t;

static bool lib_list_cb(const char *name, const char *target, void *ctx)
{
    lib_list_ctx_t *state = (lib_list_ctx_t *)ctx;
    char *file;
    char routine[128];
    uint32_t hash;
    int i;

    if (name == NULL || target == NULL) {
        return true;
    }
    file = malloc(LIB_TARGET_BYTES);
    if (file == NULL) {
        state->duplicate = true;
        return true;
    }
    if (!lib_parse_target(target, file, LIB_TARGET_BYTES, routine, sizeof(routine))) {
        if (!state->bare) {
            shell_print_error("lib: %s has an invalid target '%s'", name, target);
        }
        state->duplicate = true;
        free(file);
        return true;
    }
    free(file);
    hash = lib_target_hash(target);
    for (i = 0; i < state->target_count; i++) {
        if (state->targets[i] == hash) {
            /* One file/routine pair may not be published under two names:
             * that would hide which caller registered a shared routine. */
            if (!state->bare) {
                shell_print_error("lib: duplicate library path %s (%s)", target, name);
            }
            state->duplicate = true;
            return true;
        }
    }
    if (state->target_count < 16) {
        state->targets[state->target_count++] = hash;
    }
    if (state->bare) {
        shell_transcript_appendf("%s=%s\n", name, target);
    } else {
        shell_print_field(name, "%s", target);
    }
    state->lines++;
    return true;
}

static int lib_list(const char *path, bool bare)
{
    lib_list_ctx_t ctx;
    esp_err_t error;

    memset(&ctx, 0, sizeof(ctx));
    ctx.bare = bare;
    error = storage_ini_file_foreach(path, lib_list_cb, &ctx);
    if (error != ESP_OK) {
        shell_print_error("lib: cannot read %s (%s)", LIB_MANIFEST_PATH,
                          esp_err_to_name(error));
        return 1;
    }
    if (!bare && ctx.lines == 0) {
        shell_print_muted("lib: no shared routines in %s", LIB_MANIFEST_PATH);
    }
    return ctx.duplicate ? 1 : 0;
}

static int lib_query(const char *path, const char *name, bool bare,
                     char *target, size_t size)
{
    esp_err_t error;

    (void)bare;
    if (name == NULL || name[0] == '\0' || target == NULL || size == 0) {
        return 2;
    }
    error = storage_ini_file_get(path, name, target, size);
    if (error != ESP_OK) {
        shell_print_error("lib: no such routine library '%s'", name);
        return 1;
    }
    return 0;
}

static int lib_info(const char *path, const char *name, bool bare)
{
    char *target;
    char *file;
    char routine[128];
    int result;

    target = malloc(LIB_TARGET_BYTES);
    file = malloc(LIB_TARGET_BYTES);
    if (target == NULL || file == NULL) {
        free(target);
        free(file);
        shell_print_error("lib: out of memory");
        return 1;
    }
    result = lib_query(path, name, bare, target, LIB_TARGET_BYTES);
    if (result != 0) {
        free(target);
        free(file);
        return result;
    }
    if (!lib_parse_target(target, file, LIB_TARGET_BYTES, routine, sizeof(routine))) {
        shell_print_error("lib: invalid target for '%s'", name);
        free(target);
        free(file);
        return 1;
    }
    if (bare) {
        shell_transcript_appendf("%s=%s\n", name, target);
    } else {
        shell_print_field("lib.name:", "%s", name);
        shell_print_field("lib.file:", "%s", file);
        shell_print_field("lib.routine:", "%s", routine);
        shell_print_field("lib.call:", "call %s", target);
    }
    free(target);
    free(file);
    return 0;
}

static int lib_load(const char *path, const char *name, int argc, char **argv,
                    bool bare)
{
    char *target;
    char *call_argv[LIB_MAX_ARGV];
    int call_argc = 0;
    int result;
    int i;

    target = malloc(LIB_TARGET_BYTES);
    if (target == NULL) {
        shell_print_error("lib: out of memory");
        return 1;
    }
    result = lib_query(path, name, bare, target, LIB_TARGET_BYTES);
    if (result != 0) {
        free(target);
        return result;
    }
    /* The target already carries `<file>::<routine>`; synthesising `call`
     * here reuses the ONE library-routine path (isolated scope, propagated
     * ERRORLEVEL) instead of inventing a loader. */
    call_argv[call_argc++] = "call";
    call_argv[call_argc++] = target;
    for (i = 0; i < argc && call_argc < LIB_MAX_ARGV - 1; i++) {
        call_argv[call_argc++] = argv[i];
    }
    shell_command_call(call_argc, call_argv);
    result = batch_get_errorlevel();
    free(target);
    return result;
}

int shell_command_lib(int argc, char **argv)
{
    char *words[LIB_MAX_ARGV];
    int count = 0;
    bool bare;
    int result;

    if (argv == NULL || argc < 2) {
        lib_usage();
        batch_set_errorlevel(2);
        return 2;
    }
    /* `words[0]` is the subcommand; argv[0] ("lib") is dropped so the /b
     * switch cannot shift the argument indices. */
    bare = lib_take_bare(argc, argv, &count, words);
    if (count < 1) {
        lib_usage();
        batch_set_errorlevel(2);
        return 2;
    }

    {
        /* The ONE resolver maps `sd:` to the VFS path both the firmware core
         * and the host bridge expect. */
        char *path = malloc(P4_CONFIG_SD_PATH_BYTES);

        if (path == NULL ||
            shell_fs_resolve_path(LIB_MANIFEST_PATH, path, P4_CONFIG_SD_PATH_BYTES) != ESP_OK) {
            free(path);
            shell_print_error("lib: cannot resolve %s", LIB_MANIFEST_PATH);
            batch_set_errorlevel(1);
            return 1;
        }
        if (shell_text_equals_ignore_case(words[0], "list")) {
            if (count != 1) {
                free(path);
                lib_usage();
                batch_set_errorlevel(2);
                return 2;
            }
            result = lib_list(path, bare);
        } else if (shell_text_equals_ignore_case(words[0], "info")) {
            if (count != 2) {
                free(path);
                lib_usage();
                batch_set_errorlevel(2);
                return 2;
            }
            result = lib_info(path, words[1], bare);
        } else if (shell_text_equals_ignore_case(words[0], "load")) {
            if (count < 2) {
                free(path);
                lib_usage();
                batch_set_errorlevel(2);
                return 2;
            }
            result = lib_load(path, words[1], count - 2, &words[2], bare);
        } else {
            shell_print_error("lib: unknown subcommand '%s'", words[0]);
            lib_usage();
            result = 2;
        }
        free(path);
    }

    batch_set_errorlevel((uint8_t)result);
    return result;
}

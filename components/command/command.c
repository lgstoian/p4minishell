/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file command.c
 * @brief Command parser and dispatcher implementation for P4MiniShell.
 *
 * Owns the command execution pipeline and the commands that are not tied to
 * the filesystem or the batch language: variable-expansion entry, output
 * redirection parsing, dispatch, the worker task, hardware commands, system
 * commands, and the GPIO/RGB/camera families.
 *
 * Delegated ownership:
 *   - `components/storage/` owns SD sessions, path resolution, the current
 *     working directory, and every DOS file command
 *   - `components/batch/` owns the batch engine, environment variables, PATH,
 *     variable expansion, errorlevel, and the batch language commands
 *   - `components/shell/` owns transcript, history, debug log, UART console,
 *     the input line, and the system info commands
 *
 * State owned here:
 *   - Battery ADC handles and calibration state
 *   - Speaker codec handle and last applied volume
 *   - Light-sleep request tracking
 */

#include "command.h"
#include "command_ui.h"
#include "ui_commands.h"
#include "config_cmd.h"
#include "security_commands.h"
#include "batch.h"
#include "filetype.h"
#include "calc.h"
#include "applib.h"
#include "storage.h"
#include "storage_commands.h"
#include "shell.h"
#include "editor.h"
#include "editor_view.h"
#include "ansi_palette.h"
#include "ansi.h"
#include "audio.h"
#include "display.h"
#include "header.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "networking.h"
#include "driver/usb_serial_jtag.h"
#include <errno.h>
#include <stdarg.h>
#include "http_server.h"
#include "netdiag.h"
#include "led.h"
#include "alarm.h"
#include "bluetooth.h"
#include "c6ota.h"
#include "tui.h"
#include "clock.h"
#include "usb.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#if SOC_ADC_SUPPORTED
#include "soc/adc_channel.h"
#endif
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* Backward-compatibility aliases */
#define COMMAND_TAG                     P4_CONFIG_SHELL_TAG
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_COMMAND_TASK_STACK_BYTES  P4_CONFIG_COMMAND_TASK_STACK
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_BATCH_LINE_BYTES          P4_CONFIG_BATCH_LINE_BYTES
#define SHELL_ARGV_MAX                  P4_CONFIG_COMMAND_ARGV_MAX
#define SHELL_REBOOT_DELAY_MS           P4_CONFIG_REBOOT_DELAY_MS
#define SHELL_POWER_IDLE_DISPLAY_OFF_SECS  P4_CONFIG_POWER_IDLE_DISPLAY_OFF_SECS

/** Maximum redirection operators parsed from one command line. */
#define SHELL_CHAIN_SEGMENT_MAX         P4_CONFIG_CHAIN_SEGMENT_MAX

/* ========================================================================
 * TYPES
 * ======================================================================== */

/** One queued command line for the worker task. */
typedef struct {
    char command[SHELL_COMMAND_BYTES];
} command_request_t;

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/** Persistent worker task and its command queue. Replaces the per-command
 *  task creation pattern: commands are posted to the queue and processed
 *  sequentially by a single long-lived task, eliminating task-creation
 *  overhead (~1-2ms per command) and reducing heap fragmentation. */
#define SHELL_COMMAND_QUEUE_DEPTH  P4_CONFIG_COMMAND_QUEUE_DEPTH
#define SHELL_COMMAND_QUEUE_SEND_TIMEOUT_MS P4_CONFIG_COMMAND_QUEUE_SEND_TIMEOUT_MS
static QueueHandle_t s_command_queue = NULL;
static TaskHandle_t s_command_worker_handle = NULL;

/* Hardware handles */

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

/* Token bound for the redirection parser below. */
#define SHELL_REDIRECT_TOKEN_MAX        4

/* ========================================================================
 * REDIRECTION PARSING
 * ========================================================================
 * Handles the three DOS redirection operators on one pass:
 *   >   truncate output to a file
 *   >>  append output to a file
 *   <   read input from a file
 *
 * Quote state is tracked so an operator inside a quoted argument is treated
 * as data. The scan walks the whole line rather than stopping at the first
 * operator, so `sort < in.txt > out.txt` works in either order.
 */

/**
 * Remove quoting and escape markup from a redirection target.
 *
 * A target such as `"my file.txt"`, `'my file.txt'`, or `my^ file.txt` all
 * reach the filesystem layer as the literal path.
 */
static char *shell_redirect_unquote(char *target)
{
    return shell_unescape_in_place(target);
}

/**
 * Split a command line into its command text and redirection targets.
 *
 * @param command        Line to parse, modified in place.
 * @param command_part   Receives the trimmed command text.
 * @param redirect_target Receives the `>` / `>>` target, or NULL.
 * @param append_mode    Receives true for `>>`.
 * @param input_source   Receives the `<` source, or NULL.
 * @return true when any redirection operator was found.
 */
/**
 * Find the next redirection operator that is neither quoted nor escaped.
 *
 * Delegates to the shared shell-core scanner so redirection, pipes, and
 * chaining all agree on what counts as syntax.
 *
 * @return Pointer to the operator character, or NULL when none remains.
 */
static char *shell_redirect_find_operator(char *cursor)
{
    return shell_find_unquoted_any(cursor, "><");
}

/** One redirection operator located during the scanning pass. */
typedef struct {
    char *position;   /**< The operator character within the line. */
    char *target;     /**< First character of the target text. */
    bool is_output;   /**< true for `>` / `>>`, false for `<`. */
    bool is_append;   /**< true for `>>`. */
} shell_redirect_token_t;

/**
 * Parse `>`, `>>`, and `<` out of a command line.
 *
 * Works in two passes so the operator characters can be located before any
 * of them is overwritten. The first pass records every unquoted operator and
 * where its target begins; the second pass writes a terminator over each
 * operator, which simultaneously ends the text that preceded it. Because
 * every operator becomes a NUL, each target is naturally terminated by the
 * next operator without any byte having to be restored.
 *
 * The last occurrence of each direction wins, matching COMMAND.COM.
 */
static bool shell_parse_redirection(char *command,
                                    char **command_part,
                                    char **redirect_target,
                                    bool *append_mode,
                                    char **input_source)
{
    shell_redirect_token_t tokens[SHELL_REDIRECT_TOKEN_MAX];
    size_t token_count = 0;
    char *cursor;
    size_t index;

    if (command_part == NULL || redirect_target == NULL ||
        append_mode == NULL || input_source == NULL) {
        return false;
    }

    *command_part = command;
    *redirect_target = NULL;
    *append_mode = false;
    *input_source = NULL;

    if (command == NULL) {
        return false;
    }

    /* Pass 1: locate every unquoted operator. */
    cursor = shell_redirect_find_operator(command);
    while (cursor != NULL && token_count < SHELL_REDIRECT_TOKEN_MAX) {
        shell_redirect_token_t *token = &tokens[token_count++];

        token->position = cursor;
        token->is_output = (*cursor == '>');
        token->is_append = token->is_output && (cursor[1] == '>');
        token->target = cursor + (token->is_append ? 2 : 1);

        cursor = shell_redirect_find_operator(token->target);
    }

    if (token_count == 0) {
        *command_part = shell_trim(command);
        return false;
    }

    /* Pass 2: cut the line at every operator. A `>>` needs both characters
     * blanked so the extra '>' cannot leak into the preceding text. */
    for (index = 0; index < token_count; index++) {
        tokens[index].position[0] = '\0';
        if (tokens[index].is_append) {
            tokens[index].position[1] = '\0';
        }
    }

    /* Pass 3: publish the targets. */
    for (index = 0; index < token_count; index++) {
        char *target = shell_redirect_unquote(shell_trim(tokens[index].target));

        if (tokens[index].is_output) {
            *redirect_target = target;
            *append_mode = tokens[index].is_append;
        } else {
            *input_source = target;
        }
    }

    *command_part = shell_trim(command);
    return true;
}

/**
 * Report whether a line contains a `|` pipe separator that is real syntax.
 *
 * Quote- and escape-aware, so `echo "a | b"`, `echo 'a | b'`, and `echo a^|b`
 * are not mistaken for pipelines.
 */
static bool shell_command_has_pipe(const char *command)
{
    /* Fast reject: most batch lines contain no '|' at all, so a plain
     * strchr avoids running the quote/escape state machine over every line.
     * Only a line that actually has a '|' pays the quote-aware pass. */
    if (command == NULL || strchr(command, '|') == NULL) {
        return false;
    }
    return shell_has_unquoted_char(command, '|');
}

/* Command implementations owned by this module */
static void shell_command_clip(int argc, char **argv);
static void shell_command_paste(int argc, char **argv);
static void shell_command_history(int argc, char **argv);
static void shell_command_reboot(void);
static void shell_command_clear(void);
static void shell_command_prompt_cmd(int argc, char **argv);

/* Editor command + ops-table hooks. */
static void shell_command_edit(int argc, char **argv);

/* ========================================================================
 * CLIPBOARD COMMANDS: clip, paste
 * ========================================================================
 * `clip` reads/writes the RAM clipboard (text, transcript lines, or a file
 * reference); `paste` injects it into the input line or copies a clipped file
 * to a destination. All verbs are batch-safe and redirectable.
 */

/** Basename of a path, accepting both '/' and '\' separators. */
static const char *shell_clip_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');

    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }
    return (slash != NULL) ? slash + 1 : path;
}

static void shell_command_clip(int argc, char **argv)
{
    /* `clip` with no argument prints the clipboard. */
    if (argc == 1) {
        const char *clip = shell_clipboard_get();

        if (clip[0] == '\0') {
            shell_transcript_appendf_ansi(SH_LBL "clipboard:" SH_RST " " SH_MUTE "(empty)" SH_RST "\n");
        } else if (shell_clipboard_is_file()) {
            shell_transcript_appendf_ansi(SH_LBL "clipboard:" SH_RST " " SH_PATH "%s" SH_RST
                                     " " SH_MUTE "(file)" SH_RST "\n", clip);
        } else {
            shell_transcript_appendf_ansi(SH_LBL "clipboard:" SH_RST " " SH_VAL "%s" SH_RST "\n", clip);
        }
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "copy")) {
        int lines = 1;

        if (argc == 3) {
            char *end = NULL;
            long parsed = strtol(argv[2], &end, 10);

            if (*end != '\0' || parsed < 1 || parsed > P4_CONFIG_CLIP_COPY_LINES_MAX) {
                shell_print_error("clip: line count must be 1..%d", P4_CONFIG_CLIP_COPY_LINES_MAX);
                batch_set_errorlevel(2);
                return;
            }
            lines = (int)parsed;
        } else if (argc > 3) {
            shell_print_usage("Usage: clip copy [N]");
            batch_set_errorlevel(2);
            return;
        }

        if (!shell_clipboard_copy_transcript(lines)) {
            shell_print_error("clip: the transcript is empty");
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "clip:" SH_RST " copied " SH_NUM "%d" SH_RST " line%s to the clipboard\n",
                                 lines, lines == 1 ? "" : "s");
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "file")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];

        if (argc != 3) {
            shell_print_usage("Usage: clip file <path>");
            batch_set_errorlevel(2);
            return;
        }
        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("clip: invalid path %s", argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        shell_clipboard_set_file(resolved);
        shell_transcript_appendf_ansi(SH_LBL "clip:" SH_RST " file " SH_PATH "%s" SH_RST "\n", resolved);
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "read")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        char *buf;
        uint64_t file_size;
        size_t got;

        if (argc != 3) {
            shell_print_usage("Usage: clip read <file>");
            batch_set_errorlevel(2);
            return;
        }
        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("clip: invalid path %s", argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        file_size = storage_get_file_size(resolved);
        if (file_size == 0) {
            shell_print_error("clip: file not found %s", argv[2]);
            batch_set_errorlevel(1);
            return;
        }
        if (file_size >= P4_CONFIG_CLIPBOARD_BYTES) {
            shell_print_error("clip: file is too large for the clipboard (%d bytes)",
                              (int)P4_CONFIG_CLIPBOARD_BYTES);
            batch_set_errorlevel(1);
            return;
        }

        buf = malloc(P4_CONFIG_CLIPBOARD_BYTES);
        if (buf == NULL) {
            shell_print_error("clip: out of memory reading the file");
            batch_set_errorlevel(1);
            return;
        }
        {
            FILE *file = fopen(resolved, "rb");

            if (file == NULL) {
                free(buf);
                shell_print_error("clip: cannot open %s", resolved);
                batch_set_errorlevel(1);
                return;
            }
            got = fread(buf, 1, P4_CONFIG_CLIPBOARD_BYTES - 1, file);
            fclose(file);
        }
        buf[got] = '\0';
        shell_clipboard_set(buf);
        free(buf);
        shell_transcript_appendf_ansi(SH_LBL "clip:" SH_RST " read " SH_PATH "%s" SH_RST " (" SH_NUM "%d" SH_RST " B)\n",
                                 resolved, (int)got);
        batch_set_errorlevel(0);
        return;
    }

    /* Anything else is literal clipboard text: `clip hello world`. A first
     * argument that looks like a subcommand (a typo or a query such as
     * `clip status`) must not silently clobber the current clipboard with the
     * word itself — report a usage error instead. */
    {
        static const char *const reserved[] = {
            "copy", "file", "read",
            "status", "list", "show", "clear", "help", "?", "view", "print", "info",
        };
        size_t reserved_index;
        bool looks_like_subcommand = false;

        for (reserved_index = 0;
             reserved_index < sizeof(reserved) / sizeof(reserved[0]);
             reserved_index++) {
            if (shell_text_equals_ignore_case(argv[1], reserved[reserved_index])) {
                looks_like_subcommand = true;
                break;
            }
        }
        if (looks_like_subcommand) {
            shell_print_usage("Usage: clip [text] | clip copy [N] | clip file <path> | clip read <file>");
            batch_set_errorlevel(2);
            return;
        }
    }
    {
        // Heap-allocated: a clipboard-sized stack local would eat 2 KB of
        // the shared command worker task stack on the recursive batch path.
        char *text = malloc(P4_CONFIG_CLIPBOARD_BYTES);
        if (text == NULL) {
            shell_print_error("clip: out of memory");
            shell_record_errorf("clip", ESP_ERR_NO_MEM, "Out of memory for clipboard text");
            batch_set_errorlevel(1);
            return;
        }

        shell_join_args(argv, 1, argc, text, P4_CONFIG_CLIPBOARD_BYTES);
        shell_clipboard_set(text);
        shell_transcript_appendf_ansi(SH_LBL "clip:" SH_RST " clipboard set (" SH_NUM "%d" SH_RST " B)\n",
                                 (int)strlen(text));
        batch_set_errorlevel(0);
        free(text);
    }
}

static void shell_command_paste(int argc, char **argv)
{
    const char *clip = shell_clipboard_get();

    /* `paste <dest>` copies a clipped file reference to a destination. */
    if (argc == 2) {
        char resolved_dest[P4_CONFIG_SD_PATH_BYTES];
        char dest_file[P4_CONFIG_SD_PATH_BYTES * 2 + 16];
        struct stat dst_st;
        esp_err_t error;

        if (!shell_clipboard_is_file()) {
            shell_print_error("paste: the clipboard is not a file (use `clip file <path>`)");
            batch_set_errorlevel(1);
            return;
        }
        if (shell_fs_resolve_path(argv[1], resolved_dest, sizeof(resolved_dest)) != ESP_OK) {
            shell_print_error("paste: invalid destination path");
            batch_set_errorlevel(2);
            return;
        }

        if (stat(resolved_dest, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
            snprintf(dest_file, sizeof(dest_file), "%s/%s",
                     resolved_dest, shell_clip_basename(clip));
        } else {
            snprintf(dest_file, sizeof(dest_file), "%s", resolved_dest);
        }

        error = shell_fs_copy_file(clip, dest_file);
        if (error != ESP_OK) {
            shell_print_error("paste: failed to copy %s -> %s (%s)",
                              clip, dest_file, esp_err_to_name(error));
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "paste:" SH_RST " " SH_PATH "%s" SH_RST " -> " SH_PATH "%s" SH_RST "\n",
                                 clip, dest_file);
        batch_set_errorlevel(0);
        return;
    }

    if (argc != 1) {
        shell_print_usage("Usage: paste [<destination>]");
        batch_set_errorlevel(2);
        return;
    }

    /* `paste` injects the clipboard into the input line at the cursor. */
    if (clip[0] == '\0') {
        shell_print_error("paste: the clipboard is empty");
        batch_set_errorlevel(1);
        return;
    }
    shell_input_line_paste(clip);
    batch_set_errorlevel(0);
}

/* ========================================================================
 * TAB COMPLETION PROVIDER
 * ========================================================================
 * The help table is the single source of command names and usage text; there
 * is no parallel command list. Argument/subcommand completion is derived from
 * each command's help usage string, so expanding a help entry automatically
 * expands completion. Installed apps and available bundles are scanned live.
 */

/** Completion collector: a bounded list of heap-copied matches. */
typedef struct {
    const char *word;
    size_t word_len;
    char **matches;
    int max;
    int count;
} shell_complete_ctx_t;

static void shell_complete_add(shell_complete_ctx_t *ctx, const char *candidate)
{
    if (candidate == NULL || ctx->count >= ctx->max) {
        return;
    }
    if (strncasecmp(candidate, ctx->word, ctx->word_len) != 0) {
        return;
    }
    ctx->matches[ctx->count] = strdup(candidate);
    if (ctx->matches[ctx->count] != NULL) {
        ctx->count++;
    }
}

/** Add SD file/directory matches for the word (directories get a trailing /). */
static void shell_complete_add_paths(shell_complete_ctx_t *ctx)
{
    const char *slash = strrchr(ctx->word, '/');
    const char *bslash = strrchr(ctx->word, '\\');
    const char *base;
    char dir_vfs[P4_CONFIG_SD_PATH_BYTES];
    char dir_resolved[P4_CONFIG_SD_PATH_BYTES];
    char *candidate = malloc(P4_CONFIG_SD_PATH_BYTES + 8);
    char *full = malloc(P4_CONFIG_SD_PATH_BYTES + 256);
    DIR *dir;
    struct dirent *entry;
    struct stat st;

    if (candidate == NULL || full == NULL) {
        free(candidate);
        free(full);
        return;
    }

    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }

    if (slash != NULL) {
        size_t dir_len = (size_t)(slash - ctx->word);

        if (dir_len >= sizeof(dir_vfs)) {
            dir_len = sizeof(dir_vfs) - 1;
        }
        memcpy(dir_vfs, ctx->word, dir_len);
        dir_vfs[dir_len] = '\0';
        base = slash + 1;
    } else {
        snprintf(dir_vfs, sizeof(dir_vfs), "%s",
                 shell_get_cwd() != NULL ? shell_get_cwd() : ".");
        base = ctx->word;
    }
    if (dir_vfs[0] == '\0') {
        snprintf(dir_vfs, sizeof(dir_vfs), ".");
    }
    if (shell_fs_resolve_path(dir_vfs, dir_resolved, sizeof(dir_resolved)) != ESP_OK) {
        free(candidate);
        free(full);
        return;
    }

    dir = opendir(dir_resolved);
    if (dir == NULL) {
        free(candidate);
        free(full);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        size_t dir_prefix_len;
        size_t candidate_len;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (strncasecmp(entry->d_name, base, strlen(base)) != 0) {
            continue;
        }

        /* Candidate = the typed directory prefix (if any) + the entry name. */
        dir_prefix_len = slash != NULL ? (size_t)(slash - ctx->word) + 1 : 0;
        if (dir_prefix_len + strlen(entry->d_name) + 2 >= P4_CONFIG_SD_PATH_BYTES + 8) {
            continue;
        }
        memcpy(candidate, ctx->word, dir_prefix_len);
        snprintf(candidate + dir_prefix_len, P4_CONFIG_SD_PATH_BYTES + 8 - dir_prefix_len,
                 "%s", entry->d_name);
        candidate_len = strlen(candidate);

        /* Append '/' to directories so completion can keep going. */
        snprintf(full, P4_CONFIG_SD_PATH_BYTES + 256, "%s/%s", dir_resolved, entry->d_name);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode) &&
            candidate_len + 1 < P4_CONFIG_SD_PATH_BYTES + 8) {
            candidate[candidate_len] = '/';
            candidate[candidate_len + 1] = '\0';
        }

        shell_complete_add(ctx, candidate);
    }

    closedir(dir);
    free(candidate);
    free(full);
}

/** Split a mutable line into whitespace-delimited, NUL-terminated tokens. */
static int command_split_tokens(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;

    while (*p != '\0' && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        tok[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p != '\0') {
            *p = '\0';
            p++;
        }
    }
    return n;
}

/** True when a raw usage token contains a value placeholder or a form we do
 *  not want as a completion (flags, subcommands, and enum values do). */
static bool command_usage_token_skip(const char *raw)
{
    static const char *const bad = "#%=\"\\.`:"; /* value/format punctuation */
    size_t len = strlen(raw);

    if (len == 0) {
        return true;
    }
    /* `<app>` (single placeholder) is skipped; `<on|sleep|off>` was already
     * split on '|' before this call. */
    if (raw[0] == '<' && raw[len - 1] == '>' && strchr(raw, '|') == NULL) {
        return true;
    }
    for (const char *b = bad; *b != '\0'; b++) {
        if (strchr(raw, *b) != NULL) {
            return true;
        }
    }
    return false;
}

/** Add the completable tokens derived from one help usage string. */
static void command_usage_tokens(const char *usage, const char *cmd,
                                 shell_complete_ctx_t *ctx)
{
    char buf[256];
    const char *dash;
    size_t ulen;
    char *p;

    if (usage == NULL) {
        return;
    }
    dash = strstr(usage, " - ");
    ulen = dash != NULL ? (size_t)(dash - usage) : strlen(usage);
    if (ulen >= sizeof(buf)) {
        ulen = sizeof(buf) - 1;
    }
    memcpy(buf, usage, ulen);
    buf[ulen] = '\0';

    for (p = buf; *p != '\0';) {
        char *start;
        char save;
        char token[64];
        size_t t = 0;

        while (*p == ' ' || *p == '\t' || *p == '|') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '|') {
            p++;
        }
        save = *p;
        *p = '\0';

        if (!command_usage_token_skip(start)) {
            /* Keep only completion-safe characters. */
            for (const char *c = start; *c != '\0' && t < sizeof(token) - 1; c++) {
                if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
                    (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '/') {
                    token[t++] = *c;
                }
            }
            token[t] = '\0';
            if (t > 0 && !(token[0] >= '0' && token[0] <= '9') &&
                strcasecmp(token, cmd) != 0) {
                shell_complete_add(ctx, token);
            }
        }

        *p = save;
        if (*p != '\0') {
            p++;
        }
    }
}

/** Add installed-app / available-bundle names for `launch`/`open`/`pkg`. */
static void command_complete_apps(shell_complete_ctx_t *ctx, const char *cmd,
                                  const char *sub)
{
    static char names[COMMAND_APP_MAX][COMMAND_APP_NAME_BYTES];
    int n;
    int i;

    if (strcasecmp(cmd, "launch") == 0 || strcasecmp(cmd, "open") == 0 ||
        strcasecmp(cmd, "run") == 0) {
        n = pkg_list_installed(names, COMMAND_APP_MAX);
        for (i = 0; i < n; i++) {
            shell_complete_add(ctx, names[i]);
        }
        return;
    }
    if (strcasecmp(cmd, "pkg") == 0) {
        n = (sub != NULL && strcasecmp(sub, "install") == 0)
                ? pkg_list_available(names, COMMAND_APP_MAX)
                : pkg_list_installed(names, COMMAND_APP_MAX);
        for (i = 0; i < n; i++) {
            shell_complete_add(ctx, names[i]);
        }
    }
}

/**
 * Tab-completion provider: fills @p out with the @p match_index-th completion
 * for the last word of @p line and returns the total number of matches.
 * First token: help-table command names, aliases, installed apps, paths.
 * Later tokens: subcommands/flags from the command's help usage, installed
 * apps / available bundles for launch/open/pkg, plus file/dir paths.
 */
int command_complete_line(const char *line, int match_index, char *out,
                          size_t out_size)
{
    char work[SHELL_COMMAND_BYTES];
    char *tok[32];
    bool trailing_space;
    int n;
    int cur_index;
    const char *word;
    shell_complete_ctx_t ctx;
    size_t index;
    int total;

    if (line == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    snprintf(work, sizeof(work), "%s", line);

    {
        size_t len = strlen(work);
        trailing_space = (len > 0 && (work[len - 1] == ' ' || work[len - 1] == '\t'));
    }

    n = command_split_tokens(work, tok, 32);
    if (n == 0) {
        return 0;
    }
    cur_index = trailing_space ? n : n - 1;
    word = trailing_space ? "" : tok[n - 1];

    ctx.word = word;
    ctx.word_len = strlen(word);
    ctx.max = P4_CONFIG_COMPLETION_MAX_MATCHES;
    ctx.count = 0;
    ctx.matches = calloc((size_t)ctx.max, sizeof(char *));
    if (ctx.matches == NULL) {
        return 0;
    }

    if (cur_index == 0) {
        size_t help_count = shell_help_entry_count();

        for (index = 0; index < help_count; index++) {
            const char *name = NULL;

            if (shell_help_entry_get(index, &name, NULL) && name != NULL) {
                shell_complete_add(&ctx, name);
            }
        }
        for (index = 0; index < (size_t)P4_CONFIG_ALIAS_MAX; index++) {
            char name[P4_CONFIG_ALIAS_NAME_BYTES];

            if (shell_alias_get_by_index((int)index, name, sizeof(name), NULL, 0)) {
                shell_complete_add(&ctx, name);
            }
        }
    } else {
        const char *cmd = tok[0];
        const char *sub = (n >= 2) ? tok[1] : NULL;
        size_t help_count = shell_help_entry_count();

        for (index = 0; index < help_count; index++) {
            const char *name = NULL;
            const char *usage = NULL;

            if (shell_help_entry_get(index, &name, &usage) &&
                name != NULL && strcasecmp(name, cmd) == 0) {
                command_usage_tokens(usage, cmd, &ctx);
                break;
            }
        }
        command_complete_apps(&ctx, cmd, sub);
    }

    /* Paths always complete; for the first token this also covers .bat files. */
    shell_complete_add_paths(&ctx);

    total = ctx.count;
    if (match_index >= 0 && match_index < total) {
        snprintf(out, out_size, "%s", ctx.matches[match_index]);
    }

    for (index = 0; index < (size_t)ctx.count; index++) {
        free(ctx.matches[index]);
    }
    free(ctx.matches);
    return total;
}

/** SD-free best-match provider for the inline ghost suggestion. */
int command_ghost_line(const char *line, char *out, size_t out_size)
{
    char work[SHELL_COMMAND_BYTES];
    char *tok[32];
    bool trailing_space;
    int n;
    int cur_index;
    const char *word;
    shell_complete_ctx_t ctx;
    size_t index;
    int total;

    if (line == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    snprintf(work, sizeof(work), "%s", line);
    {
        size_t len = strlen(work);
        trailing_space = (len > 0 && (work[len - 1] == ' ' || work[len - 1] == '\t'));
    }
    n = command_split_tokens(work, tok, 32);
    if (n == 0) {
        return 0;
    }
    cur_index = trailing_space ? n : n - 1;
    word = trailing_space ? "" : tok[n - 1];

    ctx.word = word;
    ctx.word_len = strlen(word);
    ctx.max = P4_CONFIG_COMPLETION_MAX_MATCHES;
    ctx.count = 0;
    ctx.matches = calloc((size_t)ctx.max, sizeof(char *));
    if (ctx.matches == NULL) {
        return 0;
    }

    if (cur_index == 0) {
        size_t help_count = shell_help_entry_count();

        for (index = 0; index < help_count; index++) {
            const char *name = NULL;

            if (shell_help_entry_get(index, &name, NULL) && name != NULL) {
                shell_complete_add(&ctx, name);
            }
        }
        for (index = 0; index < (size_t)P4_CONFIG_ALIAS_MAX; index++) {
            char name[P4_CONFIG_ALIAS_NAME_BYTES];

            if (shell_alias_get_by_index((int)index, name, sizeof(name), NULL, 0)) {
                shell_complete_add(&ctx, name);
            }
        }
    } else {
        const char *cmd = tok[0];
        const char *usage = NULL;
        const char *name = NULL;
        size_t help_count = shell_help_entry_count();

        for (index = 0; index < help_count; index++) {
            if (shell_help_entry_get(index, &name, &usage) &&
                name != NULL && strcasecmp(name, cmd) == 0) {
                command_usage_tokens(usage, cmd, &ctx);
                break;
            }
        }
    }

    total = ctx.count;
    if (total > 0) {
        snprintf(out, out_size, "%s", ctx.matches[0]);
    }
    for (index = 0; index < (size_t)ctx.count; index++) {
        free(ctx.matches[index]);
    }
    free(ctx.matches);
    return total;
}


/* ========================================================================
 * EDITOR COMMAND AND OPS-TABLE HOOKS
 * ======================================================================== */

/** `edit <path>` - open a DOS-style inline text editor. */
static void shell_command_edit(int argc, char **argv)
{
    const char *path = NULL;
    int errorlevel = 0;

    if (argc > 2) {
        shell_print_usage("Usage: edit <path>");
        batch_set_errorlevel(2);
        return;
    }

    if (argc == 2) {
        path = argv[1];
    }

    /* The editor runs on the command worker task; it blocks until quit. */
    esp_err_t err = editor_session_run(path, &errorlevel);
    if (err != ESP_OK && errorlevel == 0) {
        errorlevel = 1;
    }
    batch_set_errorlevel(errorlevel);
}

/* Forward declarations for modal functions */
extern bool modal_is_active(void);
extern bool modal_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii);
extern bool modal_handle_serial_line(const char *line);
extern int modal_filebrowser_run(const char *title, const char *start_path,
                                  char *selected_path, size_t path_size,
                                  uint32_t timeout_ms);

/* ========================================================================
 * HISTORY COMMAND: history [list] / /save [file] / /load [file] / /clear
 * ======================================================================== */

static void shell_command_history(int argc, char **argv)
{
    if (argc == 1) {
        size_t index;

        shell_print_heading("Command history");
        for (index = 0; index < shell_history_get_count(); index++) {
            const char *line = shell_history_get(index);

            if (line != NULL) {
                shell_transcript_appendf_ansi(SH_NUM "%3u" SH_RST "  %s\n",
                                         (unsigned)(index + 1), line);
            }
        }
        if (shell_history_get_count() == 0) {
            shell_transcript_appendf_ansi(SH_MUTE "history: empty\n" SH_RST);
        }
        batch_set_errorlevel(0);
        return;
    }

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "/clear")) {
        shell_history_clear();
        shell_transcript_appendf_ansi(SH_OK "history cleared\n" SH_RST);
        batch_set_errorlevel(0);
        return;
    }

    if (argc >= 3 && shell_text_equals_ignore_case(argv[1], "/search")) {
        size_t count = shell_history_get_count();
        size_t match_count = 0;

        if (count == 0) {
            shell_transcript_appendf_ansi(SH_MUTE "history: empty\n" SH_RST);
            batch_set_errorlevel(1);
            return;
        }
        for (size_t i = count; i > 0; i--) {
            const char *line = shell_history_get(i - 1);

            if (line == NULL) {
                continue;
            }
            /* Skip the just-submitted `history /search` line: the query text
             * appears in it, so it would always self-match (the interactive
             * Ctrl+R search does not see the unsubmitted line either). */
            if (i == count && strncasecmp(line, "history", 7) == 0 &&
                shell_history_search_matches(line, "/search")) {
                continue;
            }
            if (shell_history_search_matches(line, argv[2])) {
                shell_transcript_appendf_ansi(SH_NUM "%3u" SH_RST "  %s\n",
                                              (unsigned)i, line);
                match_count++;
            }
        }
        if (match_count == 0) {
            shell_transcript_appendf_ansi(SH_MUTE "history: no match for '%s'\n" SH_RST,
                                          argv[2]);
            batch_set_errorlevel(1);
        } else {
            batch_set_errorlevel(0);
        }
        return;
    }

    if (argc >= 2 && (shell_text_equals_ignore_case(argv[1], "/save") ||
                      shell_text_equals_ignore_case(argv[1], "/load"))) {
        bool saving = shell_text_equals_ignore_case(argv[1], "/save");
        const char *file = (argc >= 3) ? argv[2] : P4_CONFIG_HISTORY_PROFILE;
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;
        FILE *fp;

        if (shell_fs_resolve_path(file, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("history: invalid path %s", file);
            batch_set_errorlevel(2);
            return;
        }

        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("history: SD card not present - insert and retry");
            batch_set_errorlevel(1);
            return;
        }

        if (saving) {
            size_t bytes = shell_history_get_count() * 2;   /* rough free-space need */

            if (!storage_check_free_space((uint64_t)bytes + 4096, 0, "history")) {
                shell_sd_end(&session, "history");
                shell_print_error("history: not enough free space on the SD card");
                batch_set_errorlevel(1);
                return;
            }
            fp = fopen(resolved, "w");
            if (fp == NULL) {
                shell_sd_end(&session, "history");
                shell_print_error("history: cannot open %s for writing", resolved);
                batch_set_errorlevel(1);
                return;
            }
            if (!shell_history_save_lines(fp)) {
                fclose(fp);
                (void)unlink(resolved);
                shell_sd_end(&session, "history");
                shell_print_error("history: write failed, removed the partial file");
                batch_set_errorlevel(1);
                return;
            }
            fclose(fp);
            shell_sd_end(&session, "history");
            shell_transcript_appendf_ansi(SH_LBL "history:" SH_RST " saved " SH_NUM "%u" SH_RST
                                     " command%s to " SH_PATH "%s" SH_RST "\n",
                                     (unsigned)shell_history_get_count(),
                                     shell_history_get_count() == 1 ? "" : "s", resolved);
        } else {
            size_t loaded;

            fp = fopen(resolved, "r");
            if (fp == NULL) {
                shell_sd_end(&session, "history");
                shell_print_error("history: cannot open %s for reading", resolved);
                batch_set_errorlevel(1);
                return;
            }
            loaded = shell_history_load_lines(fp);
            fclose(fp);
            shell_sd_end(&session, "history");
            shell_transcript_appendf_ansi(SH_LBL "history:" SH_RST " loaded " SH_NUM "%u" SH_RST
                                     " command%s from " SH_PATH "%s" SH_RST "\n",
                                     (unsigned)loaded, loaded == 1 ? "" : "s", resolved);
        }
        batch_set_errorlevel(0);
        return;
    }

    shell_print_usage("Usage: history | history /save [file] | history /load [file] | history /search <text> | history /clear");
    batch_set_errorlevel(2);
}

/* ========================================================================
 * HISTORY PERSISTENCE (auto-load at boot, debounced auto-save)
 * ======================================================================== */

static size_t s_history_saved_generation;
static esp_timer_handle_t s_history_persist_timer;

void command_history_autoload(void)
{
#if P4_CONFIG_HISTORY_AUTOSAVE
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *fp;

    if (shell_fs_resolve_path(P4_CONFIG_HISTORY_PROFILE, resolved,
                              sizeof(resolved)) != ESP_OK) {
        return;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return;
    }
    fp = fopen(resolved, "r");
    if (fp != NULL) {
        (void)shell_history_load_lines(fp);
        fclose(fp);
    }
    shell_sd_end(&session, "history");
    s_history_saved_generation = shell_history_generation();
#endif
}

void command_history_save_now(void)
{
#if P4_CONFIG_HISTORY_AUTOSAVE
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *fp;

    if (shell_history_get_count() == 0) {
        s_history_saved_generation = shell_history_generation();
        return;
    }
    if (shell_fs_resolve_path(P4_CONFIG_HISTORY_PROFILE, resolved,
                              sizeof(resolved)) != ESP_OK) {
        return;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return;
    }
    fp = fopen(resolved, "w");
    if (fp != NULL) {
        if (!shell_history_save_lines(fp)) {
            fclose(fp);
            (void)unlink(resolved);
            shell_sd_end(&session, "history");
            return;
        }
        fclose(fp);
        s_history_saved_generation = shell_history_generation();
    }
    shell_sd_end(&session, "history");
#endif
}

static void command_history_persist_cb(void *arg)
{
    (void)arg;
    if (shell_history_generation() != s_history_saved_generation) {
        command_history_save_now();
    }
}


bool shell_launch_app(const char *app_name)
{
    char *command = malloc(P4_CONFIG_COMMAND_BYTES);
    if (command == NULL) {
        shell_print_error("launch: out of memory");
        return false;
    }
    snprintf(command, P4_CONFIG_COMMAND_BYTES, "launch %s", app_name);
    shell_execute_command(command);
    free(command);
    return true;
}

/* ========================================================================
 * APP DISCOVERY (`launch`) + NATIVE APP LISTING (`apps`)
 * ========================================================================
 * A `.bat` in a PATH directory or in the conventional `sd:/APPS` directory
 * is the loadable app unit (batch-first route: no executable loader).
 * Discovery scans PATH entries plus `sd:/APPS` for `*.bat` through the
 * shared wildcard machinery, so no new filesystem code exists here; the
 * table is heap-allocated because a launched batch re-enters the dispatcher
 * per line and a stack table would overflow the worker task. Optional
 * `sd:/APPS/<name>.APPINFO` metadata (`title=`) is read through the shared
 * INI core. `apps` lists the linked-in native apps from the applib table.
 */

#define SHELL_LAUNCH_NAME_BYTES  96
#define SHELL_LAUNCH_TITLE_BYTES 96

typedef struct {
    char name[SHELL_LAUNCH_NAME_BYTES];   /* Base name without extension */
    char path[SHELL_SD_PATH_BYTES];       /* Full resolved .bat path */
    char title[SHELL_LAUNCH_TITLE_BYTES]; /* APPINFO title, or "" */
} shell_launch_entry_t;

static int shell_launch_find(const shell_launch_entry_t *table, int count, const char *name)
{
    int i;

    if (table == NULL || name == NULL) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (shell_text_equals_ignore_case(table[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static bool shell_launch_has_bat_ext(const char *base)
{
    /* Executable script types (.bat/.cmd) via the central registry
     * (components/filetype); both suffixes are 4 chars so the strip math
     * in shell_launch_add holds for either. */
    if (base == NULL) {
        return false;
    }
    return filetype_is_executable(filetype_of(base));
}

static void shell_launch_add(shell_launch_entry_t *table, int *count, const char *full_path)
{
    const char *slash;
    const char *base;
    size_t blen;
    size_t n;
    char appinfo[SHELL_SD_PATH_BYTES];
    char title[SHELL_LAUNCH_TITLE_BYTES];

    if (table == NULL || count == NULL || full_path == NULL) {
        return;
    }
    if (*count >= P4_CONFIG_LAUNCH_MAX) {
        return;
    }
    slash = strrchr(full_path, '/');
    base = (slash != NULL) ? slash + 1 : full_path;
    if (!shell_launch_has_bat_ext(base)) {
        return;
    }
    blen = strlen(base) - 4;
    if (blen == 0) {
        return;
    }
    n = blen;
    if (n >= SHELL_LAUNCH_NAME_BYTES) {
        n = SHELL_LAUNCH_NAME_BYTES - 1;
    }
    memcpy(table[*count].name, base, n);
    table[*count].name[n] = '\0';
    if (shell_launch_find(table, *count, table[*count].name) >= 0) {
        return; /* First location wins (PATH order, then APPS). */
    }
    snprintf(table[*count].path, sizeof(table[*count].path), "%s", full_path);
    title[0] = '\0';
    snprintf(appinfo, sizeof(appinfo), "sd:/APPS/%s.APPINFO", table[*count].name);
    if (storage_ini_file_get(appinfo, "title", title, sizeof(title)) != ESP_OK) {
        title[0] = '\0';
    }
    snprintf(table[*count].title, sizeof(table[*count].title), "%s", title);
    (*count)++;
}

/**
 * Fill @p table (P4_CONFIG_LAUNCH_MAX entries, caller-allocated) with the
 * installed script apps: every `;`-separated PATH directory, then the
 * conventional `sd:/APPS` directory. Both executable types (.bat/.cmd,
 * see components/filetype). A missing/unreadable directory simply
 * contributes nothing. @return The number of entries stored.
 */
static bool shell_launch_scan_pattern(shell_launch_entry_t *table, int *count,
                                      const char *pattern)
{
    char **files = NULL;
    int nfiles = 0;

    if (storage_expand_wildcard(pattern, &files, &nfiles) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < nfiles && *count < P4_CONFIG_LAUNCH_MAX; i++) {
        shell_launch_add(table, count, files[i]);
    }
    storage_free_wildcard_expansion(files, nfiles);
    return true;
}

static int shell_launch_discover(shell_launch_entry_t *table)
{
    const char *path_env;
    char *dirs = NULL;
    int count = 0;

    if (table == NULL) {
        return 0;
    }
    memset(table, 0, sizeof(*table) * (size_t)P4_CONFIG_LAUNCH_MAX);

    path_env = shell_env_get("PATH");
    dirs = strdup((path_env != NULL && path_env[0] != '\0') ? path_env : "sd:/");
    if (dirs == NULL) {
        shell_print_error("launch: out of memory");
        return 0;
    }
    {
        char *save = NULL;
        char *dir = strtok_r(dirs, ";", &save);

        while (dir != NULL && count < P4_CONFIG_LAUNCH_MAX) {
            char pattern[SHELL_SD_PATH_BYTES];
            size_t dl;

            while (*dir != '\0' && isspace((unsigned char)*dir)) {
                dir++;
            }
            dl = strlen(dir);
            while (dl > 0 && isspace((unsigned char)dir[dl - 1])) {
                dir[dl - 1] = '\0';
                dl--;
            }
            if (dir[0] != '\0') {
                /* Trailing-slash tolerant join that keeps the resolver happy:
                 * the default PATH is the drive root, whose directory part
                 * must stay in root form for the wildcard splitter. Only the
                 * logical length shrinks here — no bytes are written past the
                 * token. */
                while (dl > 0 && dir[dl - 1] == '/') {
                    dl--;
                }
                if (dl == 0) {
                    dir = strtok_r(NULL, ";", &save);
                    continue;
                }
                if (dir[dl - 1] == ':') {
                    snprintf(pattern, sizeof(pattern), "%.*s//*.bat", (int)dl, dir);
                    if (!shell_launch_scan_pattern(table, &count, pattern)) {
                        shell_record_warningf("launch", "Could not scan %s", dir);
                    }
                    snprintf(pattern, sizeof(pattern), "%.*s//*.cmd", (int)dl, dir);
                    shell_launch_scan_pattern(table, &count, pattern);
                } else {
                    snprintf(pattern, sizeof(pattern), "%.*s/*.bat", (int)dl, dir);
                    if (!shell_launch_scan_pattern(table, &count, pattern)) {
                        shell_record_warningf("launch", "Could not scan %s", dir);
                    }
                    snprintf(pattern, sizeof(pattern), "%.*s/*.cmd", (int)dl, dir);
                    shell_launch_scan_pattern(table, &count, pattern);
                }
            }
            dir = strtok_r(NULL, ";", &save);
        }
    }
    free(dirs);

    if (count < P4_CONFIG_LAUNCH_MAX) {
        shell_launch_scan_pattern(table, &count, "sd:/APPS/*.bat");
    }
    if (count < P4_CONFIG_LAUNCH_MAX) {
        shell_launch_scan_pattern(table, &count, "sd:/APPS/*.cmd");
    }
    return count;
}

static void shell_command_launch(int argc, char **argv)
{
    shell_launch_entry_t *table;
    int count;
    int i;

    table = calloc((size_t)P4_CONFIG_LAUNCH_MAX, sizeof(*table));
    if (table == NULL) {
        shell_print_error("launch: out of memory");
        batch_set_errorlevel(1);
        return;
    }
    count = shell_launch_discover(table);

    /* `launch /list` — bare machine-parsable list for scripting. */
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "/list")) {
        for (i = 0; i < count; i++) {
            if (table[i].title[0] != '\0') {
                shell_transcript_appendf("%s  -  %s\n", table[i].name, table[i].title);
            } else {
                shell_transcript_appendf("%s\n", table[i].name);
            }
        }
        batch_set_errorlevel(count > 0 ? 0 : 1);
        free(table);
        return;
    }

    /* `launch <name> [args...]` — run one app by name. */
    if (argc >= 2 && argv[1][0] != '/') {
        char batch_path[SHELL_SD_PATH_BYTES];
        bool found = shell_resolve_batch_path(argv[1], batch_path, sizeof(batch_path));

        if (!found) {
            /* Fall back to the conventional APPS directory. The name becomes
             * part of a path: reject separators and dot segments, mirroring
             * the appconfig validator, so a name can never escape APPS. */
            bool bad = (strchr(argv[1], '/') != NULL) || (strchr(argv[1], '\\') != NULL) ||
                       (strcmp(argv[1], ".") == 0) || (strcmp(argv[1], "..") == 0) ||
                       (argv[1][0] == '\0');
            if (!bad) {
                char cand[3][SHELL_SD_PATH_BYTES];

                snprintf(cand[0], sizeof(cand[0]), "sd:/APPS/%s", argv[1]);
                snprintf(cand[1], sizeof(cand[1]), "sd:/APPS/%s.BAT", argv[1]);
                snprintf(cand[2], sizeof(cand[2]), "sd:/APPS/%s.CMD", argv[1]);
                for (i = 0; i < 3 && !found; i++) {
                    char resolved[SHELL_SD_PATH_BYTES];
                    FILE *probe;

                    if (shell_fs_resolve_path(cand[i], resolved, sizeof(resolved)) != ESP_OK) {
                        continue;
                    }
                    probe = fopen(resolved, "rb");
                    if (probe != NULL) {
                        fclose(probe);
                        snprintf(batch_path, sizeof(batch_path), "%s", resolved);
                        found = true;
                    }
                }
            }
        }
        if (found) {
            shell_execute_batch_file(batch_path, argc - 2, &argv[2]);
            free(table);
            return;
        }
        shell_print_error("launch: app not found: %s", argv[1]);
        batch_set_errorlevel(1);
        free(table);
        return;
    }

    if (argc >= 2) {
        shell_print_usage("Usage: launch | launch <name> [args] | launch /list");
        batch_set_errorlevel(2);
        free(table);
        return;
    }

    /* `launch` — numbered menu over the discovery table. Bounded input so a
     * batch file or headless board can never stall here. */
    if (count == 0) {
        shell_print_warning("launch: no apps installed (place a .bat/.cmd on PATH or in sd:/APPS)");
        batch_set_errorlevel(1);
        free(table);
        return;
    }
    for (i = 0; i < count; i++) {
        if (table[i].title[0] != '\0') {
            shell_transcript_appendf_ansi(SH_NUM "%d." SH_RST " " SH_EXE "%s" SH_RST "  -  %s\n",
                                          i + 1, table[i].name, table[i].title);
        } else {
            shell_transcript_appendf_ansi(SH_NUM "%d." SH_RST " " SH_EXE "%s" SH_RST "\n",
                                          i + 1, table[i].name);
        }
    }
    {
        char answer[16];
        char *end = NULL;
        long sel;

        shell_transcript_appendf("Select app (1-%d, 0 cancels): ", count);
        if (!shell_read_line(answer, sizeof(answer), P4_CONFIG_KEY_WAIT_TIMEOUT_MS)) {
            shell_transcript_append_text("\n");
            batch_set_errorlevel(1);
            free(table);
            return;
        }
        sel = strtol(answer, &end, 10);
        if (end == answer || sel < 0 || sel > count) {
            shell_print_error("launch: invalid selection");
            batch_set_errorlevel(1);
            free(table);
            return;
        }
        if (sel == 0) {
            batch_set_errorlevel(1);
            free(table);
            return;
        }
        {
            char batch_path[SHELL_SD_PATH_BYTES];

            snprintf(batch_path, sizeof(batch_path), "%s", table[sel - 1].path);
            free(table);
            shell_execute_batch_file(batch_path, 0, NULL);
            return;
        }
    }
}

static void shell_command_apps(void)
{
    char name[P4_CONFIG_APP_NAME_BYTES];
    char desc[P4_CONFIG_APP_DESC_BYTES];
    int shown = 0;

    for (int i = 0; i < P4_CONFIG_APP_MAX; i++) {
        if (app_get(i, name, sizeof(name), desc, sizeof(desc))) {
            shell_transcript_appendf_ansi(SH_EXE "%s" SH_RST "  -  %s\n", name, desc);
            shown++;
        }
    }
    if (shown == 0) {
        shell_print_muted("No native apps registered");
    }
    batch_set_errorlevel(0);
}

/**
 * `delay <ms>` — pure deterministic wait (unlike `sleep`, which is
 * light-sleep and tears down Wi-Fi). Used for melodies and demos.
 * Clamped to P4_CONFIG_DELAY_MAX_MS. ERRORLEVEL: 0 ok / 2 usage.
 */
static void shell_command_delay(int argc, char **argv)
{
    long ms = 0;

    if (argc != 2 || argv[1][0] == '\0') {
        shell_print_usage("Usage: delay <ms>");
        batch_set_errorlevel(2);
        return;
    }
    /* Plain non-negative integer only (no expression grammar); overflow-safe
     * digit accumulation with an early clamp break. */
    for (const char *p = argv[1]; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            shell_print_usage("Usage: delay <ms>");
            batch_set_errorlevel(2);
            return;
        }
        if (ms > P4_CONFIG_DELAY_MAX_MS / 10) {
            ms = (long)P4_CONFIG_DELAY_MAX_MS + 1;
            break;
        }
        ms = ms * 10 + (*p - '0');
    }
    if (ms > (long)P4_CONFIG_DELAY_MAX_MS) {
        ms = (long)P4_CONFIG_DELAY_MAX_MS;
    }
    /* Chunked so a background task (`start`) stays killable during long
     * waits: `taskkill` is checked every 100 ms. The foreground break
     * (Ctrl+C / Stop) shares the chunks. On the main worker the bg check
     * is always false, so foreground `delay` behaves as before. */
    {
        uint32_t remaining = (uint32_t)ms;

        while (remaining > 0) {
            uint32_t chunk = remaining > 100U ? 100U : remaining;

            vTaskDelay(pdMS_TO_TICKS(chunk));
            remaining -= chunk;
            if (batch_bg_kill_requested()) {
                shell_transcript_append_text("delay: stopped\n");
                batch_set_errorlevel(1);
                return;
            }
            if (shell_abort_requested()) {
                /* Foreground break: do NOT consume it here. Leave the request
                 * set so the batch line loop unwinds the whole script with a
                 * `^C`; consuming it (the old behavior) stopped only the delay
                 * and let the rest of the script run, so the Stop button looked
                 * dead. The worker tears the surfaces down after the command. */
                shell_mark_foreground_break();
                batch_set_errorlevel(1);
                return;
            }
        }
    }
    batch_set_errorlevel(0);
}

/* ========================================================================
 * BACKGROUND JOBS: start, taskkill
 * ========================================================================
 * `start <line>` resumes a pooled background worker (created suspended at
 * init, while internal RAM still fits full command stacks) that runs the
 * line through the normal command pipeline (so a batch file runs as a
 * script, anything else as a command). The job owns a private batch ctx, env/alias snapshot
 * discipline, transcript defer slot, and storage redirect slot (see
 * batch.c/shell.c/storage.c); modal/key-wait/appmode verbs refuse
 * headlessly instead of blocking the shared UI.
 *
 * Stops are cooperative: `taskkill <job>` sets the slot's kill flag, which
 * the batch line loop and `delay` chunks poll. A job running a single
 * long native command (not a batch file) runs it to completion. */

typedef struct {
    TaskHandle_t handle;
    StaticTask_t tcb;      /* internal .bss: tiny, stays in fast RAM */
    StackType_t *stack;    /* PSRAM: full command stack, see pool init */
    char name[16];
    bool available;  /* pool task created at boot */
    bool active;     /* slot claimed by `start` (cleared when runner exits) */
    bool running;    /* task executing a job (cleared by the runner on exit) */
    char *pending;   /* PSRAM job line handed to the runner (pool i -> slot i+1) */
} bg_job_t;

static bg_job_t s_bg_jobs[P4_CONFIG_BG_TASKS];

#if P4_CONFIG_SD_OP_BOOST
/**
 * Restore a task's base priority if an SD session leaked its boost. Sessions
 * restore on shell_sd_end, but an early return that skips end would leave the
 * task elevated; forcing the base back after every command bounds any leak to
 * one command. Pass the priority sampled before execution.
 */
static void command_task_priority_backstop(UBaseType_t base_priority)
{
    if (uxTaskPriorityGet(NULL) != base_priority) {
        vTaskPrioritySet(NULL, base_priority);
    }
}
#endif

static void command_bg_worker_task(void *arg)
{
    /* Pool index doubles as the batch slot minus one; the slot is claimed
     * by `start` before we are resumed, so bind it on every wake. */
    int slot = (int)(intptr_t)arg + 1;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_bg_jobs[slot - 1].pending == NULL) {
            continue;
        }

        /* Bind the private per-task state before touching anything shared. */
        batch_bg_bind(slot);
        shell_register_bg_task(xTaskGetCurrentTaskHandle());

#if P4_CONFIG_SD_OP_BOOST
        UBaseType_t bg_base_priority = uxTaskPriorityGet(NULL);
#endif
        shell_execute_command(s_bg_jobs[slot - 1].pending);
#if P4_CONFIG_SD_OP_BOOST
        command_task_priority_backstop(bg_base_priority);
#endif

        free(s_bg_jobs[slot - 1].pending);
        s_bg_jobs[slot - 1].pending = NULL;
        s_bg_jobs[slot - 1].running = false;
        s_bg_jobs[slot - 1].active = false;
        batch_bg_release(slot);
        shell_unregister_bg_task(xTaskGetCurrentTaskHandle());
    }
}

/* Create the suspended bg pool. Task stacks must be internal RAM for
 * xTaskCreate, but internal is long gone by the time the shell starts
 * (measured: ~26 KB free, 12 KB largest block vs 32768 needed), so the pool
 * uses PSRAM stacks via xTaskCreateStatic (needs
 * CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM, pinned in sdkconfig.defaults).
 * PSRAM goes inaccessible while flash cache is disabled (OTA flash writes),
 * so `start` is refused while an OTA is pending and OTA is refused while a
 * bg job runs (see the c6ota dispatch guard) — the two never overlap. */
static void command_bg_pool_init(void)
{
    int index;

    for (index = 0; index < P4_CONFIG_BG_TASKS; index++) {
        s_bg_jobs[index].stack = heap_caps_malloc(SHELL_COMMAND_TASK_STACK_BYTES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        snprintf(s_bg_jobs[index].name, sizeof(s_bg_jobs[index].name),
                 "bg%d", index);
        if (s_bg_jobs[index].stack == NULL) {
            ESP_LOGE(COMMAND_TAG, "bg pool bg%d: no PSRAM for %u-byte stack",
                     index, (unsigned)SHELL_COMMAND_TASK_STACK_BYTES);
            s_bg_jobs[index].available = false;
            continue;
        }
        s_bg_jobs[index].handle = xTaskCreateStatic(
            command_bg_worker_task, s_bg_jobs[index].name,
            SHELL_COMMAND_TASK_STACK_BYTES / sizeof(StackType_t),
            (void *)(intptr_t)index, tskIDLE_PRIORITY + 2,
            s_bg_jobs[index].stack, &s_bg_jobs[index].tcb);
        if (s_bg_jobs[index].handle == NULL) {
            ESP_LOGE(COMMAND_TAG, "bg pool bg%d: xTaskCreateStatic failed", index);
            heap_caps_free(s_bg_jobs[index].stack);
            s_bg_jobs[index].stack = NULL;
            s_bg_jobs[index].available = false;
            continue;
        }
        s_bg_jobs[index].available = true;
        vTaskSuspend(s_bg_jobs[index].handle);
    }
}

/** True when any bg job is currently running (OTA guard). */
static bool command_bg_any_running(void)
{
    int index;

    for (index = 0; index < P4_CONFIG_BG_TASKS; index++) {
        if (s_bg_jobs[index].running) {
            return true;
        }
    }
    return false;
}

static const bg_job_t *command_bg_find(const char *job, int *slot_out)
{
    /* Accept the job name (`bg0`) or the bare slot number (`0`). Slot
     * numbers are 0-based in the job name, 1-based in the batch engine. */
    char *end = NULL;
    long number = strtol(job, &end, 10);
    size_t index;

    if (end != NULL && *end == '\0' && number >= 0 && number < P4_CONFIG_BG_TASKS) {
        if (s_bg_jobs[number].active) {
            if (slot_out != NULL) {
                *slot_out = (int)number + 1;
            }
            return &s_bg_jobs[number];
        }
        return NULL;
    }
    for (index = 0; index < P4_CONFIG_BG_TASKS; index++) {
        if (s_bg_jobs[index].active &&
            shell_text_equals_ignore_case(job, s_bg_jobs[index].name)) {
            if (slot_out != NULL) {
                *slot_out = (int)index + 1;
            }
            return &s_bg_jobs[index];
        }
    }
    return NULL;
}

static void shell_command_start(int argc, char **argv)
{
    char *line;
    int slot;

    if (argc < 2) {
        shell_print_usage("Usage: start <command> [args...]");
        batch_set_errorlevel(2);
        return;
    }
    /* PSRAM stacks go inaccessible during OTA flash writes: never overlap. */
    if (c6ota_is_confirmation_pending()) {
        shell_transcript_append_text("start: refused while a C6 OTA is pending (finish or cancel it first)\n");
        batch_set_errorlevel(1);
        return;
    }
    slot = batch_bg_alloc();
    if (slot < 0) {
        shell_transcript_appendf("start: no background slots free (max %d)\n",
                                 P4_CONFIG_BG_TASKS);
        batch_set_errorlevel(1);
        return;
    }
    if (!s_bg_jobs[slot - 1].available) {
        batch_bg_release(slot);
        shell_transcript_append_text("start: background pool unavailable (boot task create failed, see boot log)\n");
        batch_set_errorlevel(1);
        return;
    }
    line = malloc(SHELL_COMMAND_BYTES);
    if (line == NULL) {
        batch_bg_release(slot);
        shell_transcript_append_text("start: out of memory\n");
        batch_set_errorlevel(1);
        return;
    }
    shell_join_args(argv, 1, argc, line, SHELL_COMMAND_BYTES);

    s_bg_jobs[slot - 1].pending = line;
    s_bg_jobs[slot - 1].active = true;
    s_bg_jobs[slot - 1].running = true;
    vTaskResume(s_bg_jobs[slot - 1].handle);
    shell_transcript_appendf("[%s] started: %s\n", s_bg_jobs[slot - 1].name, line);
    batch_set_errorlevel(0);
}

static void shell_command_taskkill(int argc, char **argv)
{
    const bg_job_t *job;
    int slot = 0;

    if (argc != 2) {
        shell_print_usage("Usage: taskkill <job>   (name like bg0, or slot number)");
        batch_set_errorlevel(2);
        return;
    }
    job = command_bg_find(argv[1], &slot);
    if (job == NULL) {
        shell_transcript_appendf("taskkill: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    if (!job->running) {
        shell_transcript_appendf("taskkill: [%s] already finished\n", job->name);
        s_bg_jobs[slot - 1].active = false;
        batch_set_errorlevel(1);
        return;
    }
    batch_bg_request_kill(slot);
    shell_transcript_appendf("taskkill: [%s] stop requested\n", job->name);
    batch_set_errorlevel(0);
}



/* ========================================================================
 * SYSTEM COMMANDS: reboot, clear, prompt, date, time
 * ================================================================ */

/* ========================================================================
 * SYSTEM COMMANDS: reboot, clear, prompt, date, time
 * ======================================================================== */

static void shell_command_reboot(void)
{
    shell_transcript_appendf_ansi(SH_ERR "Rebooting..." SH_RST "\n");

    /* Flush recall history to SD before the reset (auto-save is debounced). */
    command_history_save_now();

    /* Anchor wall time so the RTC replay after the reset starts fresh. */
    clock_rtc_anchor_now();

    /* Give the transcript and UART console time to flush the message
     * before the reset takes effect. */
    vTaskDelay(pdMS_TO_TICKS(SHELL_REBOOT_DELAY_MS));
    esp_restart();
}

static void shell_command_clear(void)
{
    shell_transcript_reset();
    shell_record_infof("shell", "Transcript cleared");
}

/**
 * `prompt` — show or set the DOS prompt template.
 *
 * Usage:
 *   prompt              Show the active template and its rendered form
 *   prompt <template>   Set the template ($p path, $g >, $t time, ...)
 *   prompt /?           List the supported metacharacters
 *
 * The template drives both the UART console prompt and the LVGL input line,
 * so the two surfaces can never disagree. Rendering lives in the shell core.
 */
static void shell_command_prompt_cmd(int argc, char **argv)
{
    char template_text[P4_CONFIG_PROMPT_TEMPLATE_BYTES];

    if (argc >= 2 && strcmp(argv[1], "/?") == 0) {
        shell_print_usage("Usage: prompt [template]");
        shell_transcript_append_text("  $p  current path      $g  >              $l  <\n");
        shell_transcript_append_text("  $n  drive letter      $b  |              $q  =\n");
        shell_transcript_append_text("  $d  date              $t  time           $v  version\n");
        shell_transcript_append_text("  $a  &                 $c  (              $f  )\n");
        shell_transcript_append_text("  $s  space             $_  newline        $$  $\n");
        shell_transcript_append_text("  $h  backspace         $e  escape\n");
        shell_transcript_append_text("  prompt with no argument restores nothing; use 'prompt $p$g' for the default style\n");
        return;
    }

    if (argc == 1) {
        shell_print_field("prompt: template is", "%s", shell_prompt_get_template());
        shell_print_field("prompt: renders as", "%s", shell_prompt_render_plain());
        return;
    }

    /* Join the remaining arguments so an unquoted template with spaces,
     * such as `prompt $p $g`, is preserved the way DOS accepts it. */
    shell_join_args(argv, 1, argc, template_text, sizeof(template_text));

    shell_prompt_set_template(template_text);

    /* Repaint the input line immediately so the change is visible without
     * waiting for the next command. */
    shell_input_line_reset();

    shell_print_ok("prompt: template set to '%s'", shell_prompt_get_template());
    shell_print_field("prompt: renders as", "%s", shell_prompt_render_plain());
}

/* ========================================================================
 * HTTPGET / WGET
 * ========================================================================
 * `httpget <url> [localfile]` — the HTTP engine lives in
 * components/networking (the sole owner of the esp_http_client surface); this
 * file only dispatches, renders the body, and saves it to SD through the
 * storage write path with the usual free-space guardrails. ERRORLEVEL is 0 on
 * an HTTP 2xx, 1 on any failure, 2 on a usage error.
 */

/**
 * Print an httpget response body to the transcript, bounded and sanitized so a
 * binary or control-character payload cannot corrupt the transcript or inject
 * ANSI sequences. Plain text keeps `httpget url > file` redirectable.
 */
static void shell_print_http_body(const uint8_t *body, size_t size)
{
    char chunk[160];
    size_t limit = size;
    size_t index;
    size_t used = 0;

    if (body == NULL || size == 0) {
        return;
    }
    if (limit > P4_CONFIG_HTTP_PRINT_BODY_BYTES) {
        limit = P4_CONFIG_HTTP_PRINT_BODY_BYTES;
    }

    for (index = 0; index < limit; index++) {
        char ch = (char)body[index];

        /* Keep printable ASCII and the common text separators; render
         * everything else (including ESC and NUL) as a harmless dot. */
        if ((unsigned char)ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
            ch = '.';
        }
        chunk[used++] = ch;
        if (used >= sizeof(chunk) - 1) {
            chunk[used] = '\0';
            shell_transcript_append_text(chunk);
            used = 0;
        }
    }
    if (used > 0) {
        chunk[used] = '\0';
        shell_transcript_append_text(chunk);
    }
    if (limit < size) {
        shell_print_muted("httpget: body truncated at %u bytes (use httpget <url> <file> for the full body)",
                          (unsigned int)limit);
    }
}

/* ========================================================================
 * COMMAND DISPATCH
 * ======================================================================== */

/** True when a trimmed line is a comment (`rem ...` or `:: ...`).
 *
 * cmd.exe comments are opaque to end of line: the rest must not be expanded,
 * chained (`&`), piped (`|`) or redirected (`<`/`>`). Comments are therefore
 * recognised before the chain/pipeline/redirection parsers ever see the text,
 * which is why this check lives at both command entry points. */
static bool shell_comment_line(const char *trimmed)
{
    if (trimmed == NULL) {
        return false;
    }
    if (trimmed[0] == ':' && trimmed[1] == ':') {
        return true;
    }
    return strncasecmp(trimmed, "rem", 3) == 0 &&
           (trimmed[3] == '\0' || isspace((unsigned char)trimmed[3]));
}

bool shell_execute_command_core(char *command)
{
    char *argv[SHELL_ARGV_MAX];
    int argc;
    char *trimmed;
    char *family_command = NULL;
    char *echo_line = NULL;

    if (command == NULL) {
        return false;
    }

    /* Executing any command is user activity: keep the power idle clock from
     * turning the display off while a command (possibly a long one) runs. */
    shell_power_notify_activity();

    trimmed = shell_trim(command);
    if (trimmed[0] == '\0') {
        return false;
    }

    /* `rem` and `::` are comments through end of line (opaque, like cmd.exe). */
    if (shell_comment_line(trimmed)) {
        return true;
    }

    /* A device passcode lock gates the dispatcher. The first token decides
     * (the allowlist is security/unlock/help/cls/clear/version/about); boot
     * scripting runs before the lock is engaged, so CONFIG.SYS/AUTOEXEC are
     * never blocked. */
    {
        char gate_token[24];
        size_t gi = 0;

        while (trimmed[gi] != '\0' && !isspace((unsigned char)trimmed[gi]) &&
               gi < sizeof(gate_token) - 1) {
            gate_token[gi] = trimmed[gi];
            gi++;
        }
        gate_token[gi] = '\0';
        if (!security_command_allowed(gate_token)) {
            batch_set_errorlevel(1);
            return true;
        }
    }

    /* A pending C6 OTA confirmation swallows the line before any command
     * lookup so a stray "YES" cannot be dispatched as a shell command. */
    if (c6ota_try_handle_input(trimmed)) {
        return true;
    }

    /* An unquoted pipe operator splits the line into pipeline stages. */
    if (shell_command_has_pipe(trimmed)) {
        shell_execute_pipe(trimmed);
        return true;
    }

    /* Module-routed family handlers (wifi, bluetooth/bt, usb, sd, disk) parse
     * the full command line themselves, so they need the original text. The
     * shell_split_args() call below writes token terminators into the buffer
     * in place — after it runs, `command` would be truncated to the first
     * token ("wifi status" -> "wifi"). Preserve a heap copy of the trimmed
     * line for those branches. The copy is only made for family prefixes, and
     * every family branch frees it, so normal commands never allocate. */
    bool want_family = (strncmp(trimmed, "wifi", 4) == 0 &&
                        (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4]))) ||
                       (strncmp(trimmed, "bluetooth", 9) == 0 &&
                        (trimmed[9] == '\0' || isspace((unsigned char)trimmed[9]))) ||
                       (strncmp(trimmed, "bt", 2) == 0 &&
                        (trimmed[2] == '\0' || isspace((unsigned char)trimmed[2]))) ||
                       (strncmp(trimmed, "usb", 3) == 0 &&
                        (trimmed[3] == '\0' || isspace((unsigned char)trimmed[3]))) ||
                       (strncmp(trimmed, "sd", 2) == 0 &&
                        (trimmed[2] == '\0' || isspace((unsigned char)trimmed[2]))) ||
                       (strncmp(trimmed, "disk", 4) == 0 &&
                        (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4])));
    if (want_family) {
        family_command = strdup(trimmed);
    }

    /* Echo must print the whole remainder of a long line: a 4096-byte `echo`
     * with more tokens than the argv capacity would be truncated by the split
     * below. Snapshot the raw (unsplit) line so the echo branch can fall back
     * to it. Only allocated when the command is `echo` or `calc` (which needs
     * the raw line for quoted string arguments); the echo/calc branches (and
     * the argc==0 guard below) are the only paths that free it. */
    bool want_echo = (strncasecmp(trimmed, "echo", 4) == 0 &&
                      (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4]))) ||
                     (strncasecmp(trimmed, "calc", 4) == 0 &&
                      (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4])));
    if (want_echo) {
        echo_line = strdup(trimmed);
    }

    /* Snapshot OOM guard: a NULL copy must never reach a family handler or
     * the echo/calc branches. Fail the command with an errorlevel instead. */
    if ((want_family && family_command == NULL) || (want_echo && echo_line == NULL)) {
        shell_print_error("command: out of memory");
        shell_record_errorf("command", ESP_ERR_NO_MEM, "Out of memory snapshotting command line");
        batch_set_errorlevel(1);
        free(family_command);
        free(echo_line);
        return true;
    }

    /* A command with more arguments than the argv capacity would be silently
     * truncated; surface that so it is never invisible. Echo is exempt: it
     * prints the whole remainder via the raw-line snapshot, so no truncation.
     * Counted on the raw line BEFORE shell_split_args() mutates it. */
    if (echo_line == NULL && shell_count_args(trimmed) > SHELL_ARGV_MAX) {
        shell_transcript_appendf_ansi(SH_WARN "command: more than %d arguments "
                                     "were supplied; extra arguments are ignored" SH_RST "\n",
                                     SHELL_ARGV_MAX);
        shell_record_warningf("shell", "Command argument list truncated at %d", SHELL_ARGV_MAX);
    }

    argc = shell_split_args(trimmed, argv, SHELL_ARGV_MAX);
    if (argc == 0) {
        free(family_command);
        free(echo_line);
        return false;
    }

    /* ---- System commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "help")) {
        shell_command_help(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "cls") || shell_text_equals_ignore_case(argv[0], "clear")) {
        shell_command_clear();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "reboot")) {
        shell_command_reboot();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "version") || shell_text_equals_ignore_case(argv[0], "ver")) {
        shell_command_version();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "about")) {
        shell_command_about();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sysinfo")) {
        shell_command_sysinfo();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "mem")) {
        shell_command_mem();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "debug")) {
        shell_command_debug();
        return true;
    }

    /* FreeRTOS task introspection: `ps`, `tasks`, and `top` are the same
     * read-only listing (top adds a summary header and sorts by CPU). */
    if (shell_text_equals_ignore_case(argv[0], "ps") ||
        shell_text_equals_ignore_case(argv[0], "tasks") ||
        shell_text_equals_ignore_case(argv[0], "top")) {
        batch_set_errorlevel(shell_command_ps(argc, argv));
        return true;
    }

    /* ---- Hardware commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "brightness")) {
        shell_command_brightness(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rotate")) {
        shell_command_rotate(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "battery")) {
        shell_command_battery(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "power")) {
        shell_command_power(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sleep")) {
        shell_command_sleep(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "deepsleep")) {
        shell_command_deepsleep(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "volume")) {
        shell_command_volume(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "beep")) {
        shell_command_beep(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "tone")) {
        shell_command_tone(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "wavplay")) {
        shell_command_wavplay(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "audio")) {
        shell_command_audio(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "clip")) {
        shell_command_clip(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "paste")) {
        shell_command_paste(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "history")) {
        shell_command_history(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "gpio")) {
        shell_execute_gpio_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "pwm")) {
        shell_execute_pwm_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "freq")) {
        shell_execute_freq_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "adc")) {
        shell_execute_adc_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "i2c")) {
        shell_execute_i2c_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "spi")) {
        shell_execute_spi_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rgb")) {
        shell_execute_rgb_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "camera")) {
        shell_execute_camera_command(argc, argv);
        return true;
    }

    /* ---- Display/keyboard/windows commands ----
     * Implemented in command_ui.c to keep display.h, keyboard.h, and windows.h
     * out of this translation unit. */
    if (shell_text_equals_ignore_case(argv[0], "display")) {
        return shell_command_display(argc, argv);
    }

    if (shell_text_equals_ignore_case(argv[0], "keyboard")) {
        return shell_command_keyboard(argc, argv);
    }

    if (shell_text_equals_ignore_case(argv[0], "windows")) {
        return shell_command_windows(argc, argv);
    }

    if (shell_text_equals_ignore_case(argv[0], "cursor")) {
        return shell_command_cursor(argc, argv);
    }

    if (shell_text_equals_ignore_case(argv[0], "ui")) {
        return shell_command_ui(argc, argv);
    }

    /* ---- Font roles + UI theme ---- */
    if (shell_text_equals_ignore_case(argv[0], "font")) {
        shell_command_font(argc, argv);
        return true;
    }

    /* ---- UI theme (list / show / set) ---- */
    if (shell_text_equals_ignore_case(argv[0], "theme")) {
        shell_command_theme(argc, argv);
        return true;
    }

    /* ---- Header layout mode / visibility ---- */
    if (shell_text_equals_ignore_case(argv[0], "header")) {
        shell_command_header(argc, argv);
        return true;
    }

    /* ---- Markdown rendering ---- */
    if (shell_text_equals_ignore_case(argv[0], "markdown")) {
        shell_command_markdown(argc, argv);
        return true;
    }

    /* ---- JSON validate/pretty ---- */
    if (shell_text_equals_ignore_case(argv[0], "json")) {
        shell_command_json(argc, argv);
        return true;
    }

    /* ---- CSV grid (rows/cols/cell/eval); int return becomes ERRORLEVEL. */
    if (shell_text_equals_ignore_case(argv[0], "csv")) {
        batch_set_errorlevel(shell_command_csv(argc, argv));
        return true;
    }

    /* ---- Portable store export; int return becomes ERRORLEVEL. */
    if (shell_text_equals_ignore_case(argv[0], "export")) {
        batch_set_errorlevel(shell_command_export(argc, argv));
        return true;
    }

    /* ---- Portable store import; int return becomes ERRORLEVEL. */
    if (shell_text_equals_ignore_case(argv[0], "import")) {
        batch_set_errorlevel(shell_command_import(argc, argv));
        return true;
    }

    /* ---- USTAR backup archives; int return becomes ERRORLEVEL.
     *  (`restore` stays the trash-undelete alias; extraction is
     *  `archive extract`.) */
    if (shell_text_equals_ignore_case(argv[0], "archive") ||
        shell_text_equals_ignore_case(argv[0], "backup")) {
        batch_set_errorlevel(shell_command_archive(argc, argv));
        return true;
    }

    /* ---- Password file encryption; int return becomes ERRORLEVEL. */
    if (shell_text_equals_ignore_case(argv[0], "crypt")) {
        batch_set_errorlevel(shell_command_crypt(argc, argv));
        return true;
    }

    /* ---- Screenshot command ---- */
    if (shell_text_equals_ignore_case(argv[0], "screenshot") ||
        shell_text_equals_ignore_case(argv[0], "scr") ||
        shell_text_equals_ignore_case(argv[0], "capture")) {
        shell_command_screenshot(argc, argv);
        return true;
    }

    /* ---- Receive (serial binary transfer to SD) ---- */
    if (shell_text_equals_ignore_case(argv[0], "receive")) {
        shell_command_receive(argc, argv);
        return true;
    }

    /* ---- Send (serial binary transfer from SD / diagnostic report) ---- */
    if (shell_text_equals_ignore_case(argv[0], "send")) {
        shell_command_send(argc, argv);
        return true;
    }

    /* ---- Module-routed commands ----
     * These receive the original unsplit line because their own parsers
     * need the full text (for example `wifi connect <ssid> <password>`). */
    if (shell_text_equals_ignore_case(argv[0], "wifi")) {
        esp_err_t wifi_error = networking_handle_wifi_command(family_command);
        free(family_command);
        batch_set_errorlevel(wifi_error == ESP_OK ? 0 : 1);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "bluetooth") || shell_text_equals_ignore_case(argv[0], "bt")) {
        bluetooth_handle_command(family_command);
        free(family_command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "usb")) {
        /* The CDC-ACM serial subfamily lives in the command layer (it needs
         * the key queue and SD input sourcing), so route it here; every
         * other `usb` verb stays in the usb module. */
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "userial")) {
            batch_set_errorlevel(shell_command_userial(argc, argv));
        } else {
            usb_handle_command(family_command);
        }
        free(family_command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "c6ota")) {
        /* PSRAM bg stacks go inaccessible during OTA flash writes: the two
         * must never overlap, so OTA waits for background jobs. */
        if (command_bg_any_running()) {
            shell_transcript_append_text("c6ota: stop background jobs first (taskkill bg0 ...)\n");
            batch_set_errorlevel(1);
        } else {
            free(family_command);
            c6ota_perform(argc >= 2 ? argv[1] : NULL);
            return true;
        }
        free(family_command);
        return true;
    }

    /* ---- SD tools ---- */
    if (shell_text_equals_ignore_case(argv[0], "sd")) {
        shell_command_sd(family_command);
        free(family_command);
        return true;
    }

    /* ---- Disk and partition tools (diskpart style) ---- */
    if (shell_text_equals_ignore_case(argv[0], "disk")) {
        batch_set_errorlevel(shell_command_disk(family_command));
        free(family_command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sdeject")) {
        free(family_command);
        shell_command_sd_eject();
        return true;
    }

    /* ---- DOS-style file commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "cd") || shell_text_equals_ignore_case(argv[0], "chdir")) {
        shell_command_cd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "dir")) {
        shell_command_dir(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "copy")) {
        shell_command_copy(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "move")) {
        shell_command_move(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "del") || shell_text_equals_ignore_case(argv[0], "erase")) {
        batch_set_errorlevel(shell_command_del(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "ren") || shell_text_equals_ignore_case(argv[0], "rename")) {
        shell_command_rename(argc, argv, argv[0]);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "md") || shell_text_equals_ignore_case(argv[0], "mkdir")) {
        shell_command_mkdir(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rd") || shell_text_equals_ignore_case(argv[0], "rmdir")) {
        batch_set_errorlevel(shell_command_rmdir(argc, argv));
        return true;
    }

    /* ---- Recycle bin ---- */
    if (shell_text_equals_ignore_case(argv[0], "undelete") || shell_text_equals_ignore_case(argv[0], "restore")) {
        batch_set_errorlevel(shell_command_undelete(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "trash") || shell_text_equals_ignore_case(argv[0], "recycle")) {
        batch_set_errorlevel(shell_command_trash(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "type")) {
        shell_command_type_file(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "write")) {
        shell_command_write_file(argc, argv, false);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "append")) {
        shell_command_write_file(argc, argv, true);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "touch")) {
        shell_command_touch(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "edit")) {
        shell_command_edit(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "draw")) {
        shell_command_draw(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "gfx")) {
        shell_command_gfx(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "plot")) {
        shell_command_plot(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "crc32")) {
        shell_command_crc32(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "asset")) {
        shell_command_asset(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "pkg")) {
        shell_command_pkg(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "anchor")) {
        shell_command_anchor(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "browse")) {
        shell_command_browse(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "db")) {
        shell_command_db(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "alarm")) {
        shell_command_alarm(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "cal")) {
        shell_command_cal(argc, argv);
        return true;
    }

    /* ---- Extended DOS commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "attrib")) {
        shell_command_attrib(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "label")) {
        shell_command_label(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "xcopy")) {
        batch_set_errorlevel(shell_command_xcopy(argc, argv));
        return true;
    }

    /* ---- Volume management ---- */
    if (shell_text_equals_ignore_case(argv[0], "chkdsk") ||
        shell_text_equals_ignore_case(argv[0], "scandisk")) {
        shell_command_chkdsk(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "format")) {
        batch_set_errorlevel(shell_command_format(argc, argv));
        return true;
    }

    /* ---- Environment and batch commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "set")) {
        shell_command_set(argc, argv);
        return true;
    }

    /* `calc` — batch-native calculator (float expression evaluator in
     * components/batch/calc.c). The raw-line snapshot (taken before the
     * tokenizer) carries quoted string arguments intact, so this is the one
     * batch verb that parses the original text itself. */
    if (shell_text_equals_ignore_case(argv[0], "calc")) {
        int calc_level = 0;

        if (echo_line != NULL) {
            calc_level = shell_command_calc_line(echo_line);
            free(echo_line);
        } else {
            /* Defensive fallback (never expected: the snapshot is taken under
             * the same "calc" prefix rule as this dispatch). Rejoin the tokens
             * with a synthetic "calc" prefix so the parser still works. */
            char *joined = malloc(SHELL_COMMAND_BYTES);
            char *line = malloc(SHELL_COMMAND_BYTES + 8);

            if (joined == NULL || line == NULL) {
                free(joined);
                free(line);
                shell_print_error("calc: out of memory");
                calc_level = 1;
            } else {
                shell_join_args(argv, 1, argc, joined, SHELL_COMMAND_BYTES);
                snprintf(line, SHELL_COMMAND_BYTES + 8, "calc %s", joined);
                calc_level = shell_command_calc_line(line);
                free(joined);
                free(line);
            }
        }
        batch_set_errorlevel(calc_level);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "path")) {
        shell_command_path(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "echo") ||
        (strncasecmp(argv[0], "echo.", 5) == 0 && argv[0][5] == '\0')) {
        if (strncasecmp(argv[0], "echo.", 5) == 0) {
            /* The DOS `echo.` idiom prints a blank line. */
            if (echo_line != NULL) {
                free(echo_line);
            }
            shell_transcript_append_text("\n");
        } else if (echo_line != NULL) {
            /* A raw-line snapshot exists when the command started with "echo";
             * use it so a long echo line is never truncated by the argv cap. */
            shell_command_echo_text(echo_line);
            free(echo_line);
        } else {
            shell_command_echo(argc, argv);
        }
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "alias")) {
        shell_command_alias(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "unalias")) {
        shell_command_unalias(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "bind")) {
        shell_command_bind(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "macro")) {
        shell_command_macro(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "call")) {
        shell_command_call(argc, argv);
        return true;
    }

    /* Batch comment lines are accepted interactively as no-ops. */
    if (shell_text_equals_ignore_case(argv[0], "rem") || strncmp(argv[0], "::", 2) == 0) {
        return true;
    }

    /* ---- Batch control flow ---- */
    if (shell_text_equals_ignore_case(argv[0], "if")) {
        shell_command_if(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "goto")) {
        shell_command_goto(argc, argv);
        return true;
    }

    /* BASIC-named jump/return/dispatch siblings of goto/call (additive). */
    if (shell_text_equals_ignore_case(argv[0], "gosub")) {
        shell_command_gosub(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "return")) {
        shell_command_return(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "on")) {
        shell_command_on(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "shift")) {
        shell_command_shift(argc, argv);
        return true;
    }

    /* ---- Extended built-in commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "pause")) {
        shell_command_pause(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "choice")) {
        shell_command_choice(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "notify")) {
        shell_command_notify(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "gfind")) {
        shell_command_gfind(argc, argv);
        return true;
    }

    /* Owner identity + device passcode/lock + private-record concealment. */
    if (shell_text_equals_ignore_case(argv[0], "owner")) {
        shell_command_owner(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "security")) {
        shell_command_security(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "setlocal")) {
        shell_command_setlocal(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "endlocal")) {
        shell_command_endlocal(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "prompt")) {
        shell_command_prompt_cmd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "config")) {
        shell_command_config(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "for")) {
        shell_command_for(argc, argv);
        return true;
    }

    /* Time / date / timezone / SNTP commands live in the clock component. */
    if (shell_text_equals_ignore_case(argv[0], "date")) {
        clock_command_date(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "time")) {
        clock_command_time(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sntp") ||
        shell_text_equals_ignore_case(argv[0], "ntpsync")) {
        clock_command_sntp(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "timezone")) {
        clock_command_timezone(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rtc")) {
        clock_command_rtc(argc, argv);
        return true;
    }

    /* Stopwatch runs live in the clock component; the int return becomes
     * ERRORLEVEL so batch files can branch on it. */
    if (shell_text_equals_ignore_case(argv[0], "timer") ||
        shell_text_equals_ignore_case(argv[0], "stopwatch")) {
        batch_set_errorlevel(clock_command_timer(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "exit")) {
        shell_command_exit(argc, argv);
        return true;
    }

    /* `proc` — batch process-stack introspection (the batch process
     * abstraction): list the active batch files, or report the current
     * process's args/name/depth/errorlevel/echo/stdin source. */
    if (shell_text_equals_ignore_case(argv[0], "proc")) {
        shell_command_proc(argc, argv);
        return true;
    }

    /* `ini` — persistent state in KEY=VALUE files on the SD card (get/set/
     * del/list/load/save). */
    if (shell_text_equals_ignore_case(argv[0], "ini")) {
        shell_command_ini(argc, argv);
        return true;
    }

    /* `appconfig` — per-app settings file (sd:/APPS/<APP>.INI) without
     * hand-rolling file parsing. */
    if (shell_text_equals_ignore_case(argv[0], "appconfig")) {
        shell_command_appconfig(argc, argv);
        return true;
    }

    /* `temp` — SD-backed temporary files (new/clean/path). */
    if (shell_text_equals_ignore_case(argv[0], "temp")) {
        shell_command_temp(argc, argv);
        return true;
    }

    /* `ansi` / `menu` — menu/form primitives (CHOICE + ANSI was the DOS
     * way): styled text output and a numbered form, both rendered in the
     * transcript display. */
    if (shell_text_equals_ignore_case(argv[0], "ansi")) {
        shell_command_ansi(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "menu")) {
        shell_command_menu(argc, argv);
        return true;
    }

    /* `appmode` — enter/exit app mode (save/restore screen, full-screen,
     * auto-cleanup on batch exit). */
    if (shell_text_equals_ignore_case(argv[0], "appmode")) {
        shell_command_appmode(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "dialog")) {
        shell_command_dialog(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "list")) {
        shell_command_list(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "ask")) {
        shell_command_ask(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "form")) {
        shell_command_form(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "view")) {
        shell_command_view(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "open")) {
        shell_command_open(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "hexview")) {
        shell_command_hexview(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "image")) {
        shell_command_image(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "color")) {
        shell_command_color(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "locate")) {
        shell_command_locate(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "tui")) {
        shell_command_tui(argc, argv);
        return true;
    }

    /* ---- File utility commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "find")) {
        batch_set_errorlevel(shell_command_find(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "findstr")) {
        batch_set_errorlevel(shell_command_findstr(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "more")) {
        batch_set_errorlevel(shell_command_more(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "tree")) {
        shell_command_tree(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "fc")) {
        batch_set_errorlevel(shell_command_fc(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "comp")) {
        batch_set_errorlevel(shell_command_comp(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sort")) {
        batch_set_errorlevel(shell_command_sort(argc, argv));
        return true;
    }

    /* ---- Network connectivity commands: ping, dns ----
     * These live in components/networking (the sole owner of the lwIP /
     * esp_ping surface) and return an esp_err_t that the dispatcher maps onto
     * ERRORLEVEL, so `ping host && echo up`, `ping host || echo down`, and the
     * equivalent batch-file forms work exactly like DOS. Redirection and pipes
     * capture their transcript output automatically. */
    if (shell_text_equals_ignore_case(argv[0], "ping")) {
        int count = 0;
        esp_err_t error;

        if (argc < 2) {
            shell_print_usage("Usage: ping <host-or-ip> [count]");
            batch_set_errorlevel(2);
            return true;
        }
        if (argc > 3) {
            shell_print_usage("Usage: ping <host-or-ip> [count]");
            batch_set_errorlevel(2);
            return true;
        }
        if (argc == 3) {
            char *end = NULL;
            long parsed = strtol(argv[2], &end, 10);

            if (end == NULL || *end != '\0' || parsed < 1) {
                shell_print_usage("Usage: ping <host-or-ip> [count]  (count 1..%d)", P4_CONFIG_PING_COUNT_MAX);
                batch_set_errorlevel(2);
                return true;
            }
            count = (int)parsed;
        }

        error = networking_wifi_ping(argv[1], count);
        batch_set_errorlevel(error == ESP_OK ? 0 : 1);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "dns") || shell_text_equals_ignore_case(argv[0], "nslookup")) {
        esp_err_t error;

        if (argc != 2) {
            shell_print_usage("Usage: dns <hostname>");
            batch_set_errorlevel(2);
            return true;
        }

        error = networking_wifi_dns_lookup(argv[1]);
        batch_set_errorlevel(error == ESP_OK ? 0 : 1);
        return true;
    }

    /* ---- TCP terminal (one-shot request/response; networking owns sockets) */
    if (shell_text_equals_ignore_case(argv[0], "tcpterm")) {
        int port = 0;
        int idle_ms = 0;
        char *text = NULL;
        size_t text_cap = 0;
        size_t text_len = 0;
        esp_err_t error;
        int i;

        if (argc < 3 || !networking_tcp_parse_target(argv[1], argv[2], &port)) {
            shell_print_usage("Usage: tcpterm <host> <port> [/t:secs] [text...]");
            batch_set_errorlevel(2);
            return true;
        }
        text_cap = P4_CONFIG_TCP_TX_MAX_BYTES + 1;
        text = heap_caps_malloc(text_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (text == NULL) {
            text = malloc(text_cap);
        }
        if (text == NULL) {
            shell_print_error("tcpterm: out of memory");
            batch_set_errorlevel(1);
            return true;
        }
        text[0] = '\0';
        for (i = 3; i < argc; i++) {
            const char *arg = argv[i];
            if (arg[0] == '/' || arg[0] == '-') {
                if (strncasecmp(arg + 1, "t:", 2) == 0 && arg[3] != '\0') {
                    char *end = NULL;
                    long secs = strtol(arg + 3, &end, 10);
                    if (end == arg + 3 || *end != '\0' || secs < 1 || secs > 120) {
                        shell_print_usage("Usage: tcpterm <host> <port> [/t:secs] [text...]");
                        heap_caps_free(text);
                        batch_set_errorlevel(2);
                        return true;
                    }
                    idle_ms = (int)secs * 1000;
                    continue;
                }
                /* Unknown /flag: treat as request text (ports/services talk
                 * in slashes; refusing would break passthrough). */
            }
            {
                char unesc[512];
                size_t piece;
                networking_tcp_unescape(arg, unesc, sizeof(unesc));
                piece = strlen(unesc) + (text_len > 0 ? 1 : 0);
                if (text_len + piece >= text_cap) {
                    shell_print_error("tcpterm: request exceeds %d bytes",
                                      P4_CONFIG_TCP_TX_MAX_BYTES);
                    heap_caps_free(text);
                    batch_set_errorlevel(1);
                    return true;
                }
                if (text_len > 0) {
                    text[text_len++] = ' ';
                }
                memcpy(text + text_len, unesc, strlen(unesc));
                text_len += strlen(unesc);
                text[text_len] = '\0';
            }
        }
        if (text_len == 0) {
            /* No inline text: feed the active < file / pipe source instead. */
            char resolved[SHELL_SD_PATH_BYTES];
            if (storage_resolve_input_source(NULL, resolved, sizeof(resolved)) == ESP_OK) {
                shell_sd_session_t session;
                FILE *file = NULL;
                if (shell_sd_begin(&session) == ESP_OK) {
                    file = fopen(resolved, "rb");
                }
                if (file != NULL) {
                    size_t n = fread(text, 1, P4_CONFIG_TCP_TX_MAX_BYTES, file);
                    text_len = n;
                    text[text_len] = '\0';
                    fclose(file);
                }
                shell_sd_end(&session, "tcpterm");
            }
        }
        error = networking_tcp_term(argv[1], port,
                                    (const uint8_t *)text, text_len, idle_ms);
        heap_caps_free(text);
        batch_set_errorlevel(error == ESP_OK ? 0 : 1);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "httpget") || shell_text_equals_ignore_case(argv[0], "wget")) {
        networking_http_result_t result;
        esp_err_t error;

        if (argc < 2 || argc > 3) {
            shell_print_usage("Usage: httpget <url> [localfile]");
            batch_set_errorlevel(2);
            return true;
        }

        error = networking_http_get(argv[1], &result);
        if (error != ESP_OK) {
            networking_http_result_free(&result);
            batch_set_errorlevel(1);
            return true;
        }

        if (argc == 3) {
            /* Save the exact body to the SD card through the storage write
             * path: cwd-relative resolution, guarded session, free-space
             * precheck, and partial-destination cleanup on failure. */
            char resolved[SHELL_SD_PATH_BYTES];
            shell_sd_session_t session;
            FILE *file = NULL;

            if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("httpget: invalid path %s", argv[2]);
                networking_http_result_free(&result);
                batch_set_errorlevel(1);
                return true;
            }

            if (shell_sd_begin(&session) != ESP_OK) {
                shell_print_error("httpget: SD card not present - insert and retry");
                networking_http_result_free(&result);
                batch_set_errorlevel(1);
                return true;
            }

            /* Refuse before opening so an overwrite cannot destroy the existing
             * file and then fail for lack of room. */
            {
                uint64_t reclaim = storage_get_file_size(resolved);

                if (!storage_check_free_space(result.body_size, reclaim, "httpget")) {
                    shell_sd_end(&session, "httpget");
                    networking_http_result_free(&result);
                    batch_set_errorlevel(1);
                    return true;
                }
            }

            file = fopen(resolved, "wb");
            if (file == NULL) {
                shell_print_error("httpget: failed to open %s", resolved);
                shell_sd_end(&session, "httpget");
                networking_http_result_free(&result);
                batch_set_errorlevel(1);
                return true;
            }

            if (result.body_size > 0 &&
                fwrite(result.body, 1, result.body_size, file) != result.body_size) {
                fclose(file);
                (void)remove(resolved);
                shell_print_error("httpget: write failed - removed partial %s", resolved);
                shell_sd_end(&session, "httpget");
                networking_http_result_free(&result);
                batch_set_errorlevel(1);
                return true;
            }

            fclose(file);
            shell_sd_end(&session, "httpget");
            shell_print_ok("httpget: saved %u bytes to %s",
                           (unsigned int)result.body_size, resolved);
        } else {
            /* Print the body to the transcript (bounded and sanitized). A
             * trailing newline keeps the next prompt on its own line. */
            shell_print_http_body(result.body, result.body_size);
            shell_transcript_append_text("\n");
        }

        networking_http_result_free(&result);
        batch_set_errorlevel(0);
        return true;
    }

    /* ---- Network services and diagnostics ----
     * `httpd` drives the SD HTTP file server (start/stop/status) and
     * `netstat` / `ipconfig` report lwIP state. All of it lives in
     * components/networking; the dispatcher only maps results onto
     * ERRORLEVEL so `httpd start && ...` works in batch files. */
    if (shell_text_equals_ignore_case(argv[0], "httpd")) {
        esp_err_t error = ESP_OK;

        if (argc == 2 && shell_text_equals_ignore_case(argv[1], "start")) {
            error = networking_httpd_start();
        } else if (argc == 2 && shell_text_equals_ignore_case(argv[1], "stop")) {
            error = networking_httpd_stop();
        } else if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
            networking_httpd_status();
        } else {
            shell_print_usage("Usage: httpd start | httpd stop | httpd status");
            batch_set_errorlevel(2);
            return true;
        }
        batch_set_errorlevel(error == ESP_OK ? 0 : 1);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "netstat")) {
        networking_netstat();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "ipconfig")) {
        networking_ipconfig();
        return true;
    }

    /* ---- App discovery (`launch`) and native app listing (`apps`) ----
     * Builtins, so a stray `<name>.BAT` on the SD card can never shadow them;
     * native applib apps dispatch after the .bat lookup below. */
    if (shell_text_equals_ignore_case(argv[0], "launch")) {
        shell_command_launch(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "apps")) {
        shell_command_apps();
        free(family_command);
        free(echo_line);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "delay")) {
        shell_command_delay(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "start")) {
        shell_command_start(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "taskkill")) {
        shell_command_taskkill(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    /* ---- Batch file direct execution ---- */
    {
        char batch_path[SHELL_SD_PATH_BYTES];

        if (shell_resolve_batch_path(argv[0], batch_path, sizeof(batch_path))) {
            shell_execute_batch_file(batch_path, argc - 1, &argv[1]);
            return true;
        }
    }

    /* ---- Registered native apps (applib ABI) ----
     * Dispatched after every built-in and .bat lookup fails, so an app name
     * can never shadow either. The return value becomes ERRORLEVEL. */
    {
        int app_errorlevel = 0;

        if (app_dispatch(argc, argv, &app_errorlevel)) {
            batch_set_errorlevel(app_errorlevel);
            free(family_command);
            free(echo_line);
            return true;
        }
    }

    free(family_command);
    return false;
}

/* ========================================================================
 * COMMAND EXECUTION PIPELINE
 * ========================================================================
 * A command line goes through five stages before dispatch:
 *   1. Chain splitting on unquoted &, &&, and || — one segment at a time
 *   2. Variable expansion (%VAR%, %0..%9, %*) — owned by components/batch
 *   3. Redirection parsing (>, >>, and <)
 *   4. Input redirection published to components/storage for the stage
 *   5. Dispatch, then capture of the transcript delta for output redirection.
 *
 * Expansion happens per segment rather than once for the whole line. That way
 * a variable whose value contains an `&` cannot inject a new command, which
 * is the same reason COMMAND.COM parses separators before expanding.
 */

/**
 * Run one command with expansion, redirection, and dispatch.
 *
 * Sets errorlevel so conditional chaining has something to test: a recognized
 * command that does not set it explicitly reports success, and an unrecognized
 * one reports P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND.
 *
 * @return true when the command was recognized and executed.
 */
static bool shell_execute_command_segment(char *command)
{
    /* Expansion needs one line-sized buffer. It lives on the heap because
     * this function sits on the batch recursion path: a batch file calls back
     * into the pipeline for every line, and four nested levels of a large
     * stack buffer would overflow the command worker task's stack. The buffer
     * is command-sized because an interactive command line can be up to
     * P4_CONFIG_COMMAND_BYTES (batch lines are the smaller surface). */
    const size_t work_size = SHELL_COMMAND_BYTES;
    char *command_buffer = NULL;
    char *command_part = NULL;
    char *redirect_target = NULL;
    char *input_source = NULL;
    bool append_mode = false;
    bool input_redirect_set = false;
    bool recognized;
    int errorlevel_before;

    if (command == NULL) {
        return false;
    }

    command_buffer = malloc(work_size);
    if (command_buffer == NULL) {
        shell_transcript_append_text("shell: out of memory expanding the command line\n");
        shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory expanding a command line");
        batch_set_errorlevel(1);
        return false;
    }

    /* Expand directly into the working buffer: no second allocation and no
     * whole-line copy (this runs once per loop iteration). */
    shell_expand_variables(command, command_buffer, work_size);
    shell_parse_redirection(command_buffer, &command_part, &redirect_target, &append_mode, &input_source);

    /* Publish the `<` source so the text-processing commands can pick it up
     * when the user gave no filename argument. A pipeline stage sets the same
     * slot, so both spellings reach one code path. */
    if (input_source != NULL && input_source[0] != '\0') {
        char resolved_input[SHELL_SD_PATH_BYTES];
        esp_err_t error = shell_fs_resolve_path(input_source, resolved_input, sizeof(resolved_input));

        if (error != ESP_OK) {
            shell_print_error("redirection: invalid input path %s", input_source);
            shell_record_warningf("shell", "Invalid input redirection path %s", input_source);
            batch_set_errorlevel(1);
            free(command_buffer);
            return false;
        }

        storage_set_input_redirect(resolved_input);
        input_redirect_set = true;
    }

    /* A redirected command's output is captured into a dedicated heap buffer
     * (independent of the transcript), so a large output survives the 16 KB
     * transcript truncation. Open the capture window before dispatch. */
    if (redirect_target != NULL && redirect_target[0] != '\0') {
        shell_redirect_capture_begin();
    }

    /* draw_buf errorlevel rather than clearing it. Clearing would destroy the
     * value that the very next `if errorlevel N` is meant to read, and DOS
     * only changes errorlevel when a command actually reports a status. A
     * command counts as failed for chaining purposes when it leaves a new
     * non-zero errorlevel behind. */
    errorlevel_before = batch_get_errorlevel();

    /* Batch label repaints across this segment's output: appends stay live in
     * the buffers and on serial, but the O(buffer) LVGL span rebuild happens
     * once per segment (plus a periodic live flush) instead of per line. */
    shell_transcript_defer_begin();
    recognized = shell_execute_command_core(command_part);
    if (!recognized) {
        shell_transcript_appendf_ansi(SH_ERR "Unknown command:" SH_RST " %s\n", command_part);
        shell_record_warningf("shell", "Unknown command: %s", command_part);
        batch_set_errorlevel(P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND);
    }

    /* The input slot belongs to exactly one command. Clear it here so a
     * failed dispatch cannot leak the source into the next line. */
    if (input_redirect_set) {
        storage_clear_input_redirect();
    }

    if (redirect_target != NULL && redirect_target[0] != '\0') {
        size_t captured_len = 0;
        const char *captured = shell_redirect_capture_get(&captured_len);
        esp_err_t error;

        shell_redirect_capture_end();

        error = shell_write_redirect_output(redirect_target, captured, append_mode);
        if (shell_redirect_capture_was_truncated()) {
            shell_print_warning("redirection: output exceeded %d bytes and was truncated",
                                P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES);
            shell_record_warningf("shell", "Redirected output truncated at %d bytes",
                                  P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES);
        }
        if (error != ESP_OK) {
            shell_print_error("redirection: failed to write %s (%s)",
                                     redirect_target,
                                     esp_err_to_name(error));
            shell_record_warningf("shell", "Failed to redirect command output to %s", redirect_target);
            batch_set_errorlevel(1);
        }
        shell_redirect_capture_reset();
    }

    /* End the repaint deferral: one span rebuild for the whole segment. */
    shell_transcript_defer_end();

    free(command_buffer);

    /* A command "succeeded" when it was recognized and did not raise a new
     * non-zero errorlevel. Comparing against the draw_buf means a stale value
     * from an earlier line cannot make this command look failed. */
    if (!recognized) {
        return false;
    }

    {
        int errorlevel_after = batch_get_errorlevel();

        return (errorlevel_after == 0) || (errorlevel_after == errorlevel_before);
    }
}

void shell_execute_command(char *command)
{
    shell_chain_segment_t segments[SHELL_CHAIN_SEGMENT_MAX];
    /* The chain buffer must hold a full command line (up to
     * P4_CONFIG_COMMAND_BYTES); batch lines are a separate, smaller surface. */
    const size_t chain_size = SHELL_COMMAND_BYTES;
    char *chain_buffer;
    bool truncated = false;
    bool previous_succeeded;
    int segment_count;
    int index;

    if (command == NULL) {
        return;
    }

    /* A comment swallows the whole line before alias expansion, chain
     * splitting (`&`), pipes and redirection: `rem x > y | z` must do nothing. */
    if (shell_comment_line(shell_trim(command))) {
        return;
    }

    /* Reclaim internal heap from the transcript scrollback when the internal
     * heap runs low, before this command prints anything. Without this the
     * accumulated LVGL span overhead can starve the heap until a tiny stdio
     * allocation (newlib FILE lock) aborts the board mid-command. */
    shell_transcript_guard_internal();

    /* Chain splitting works on a private copy: shell_split_chain() writes
     * terminators in place, and the caller's buffer may be a batch line that
     * the executor still needs intact. Heap-allocated because this function
     * is on the batch recursion path. */
    chain_buffer = malloc(chain_size);
    if (chain_buffer == NULL) {
        shell_transcript_append_text("shell: out of memory splitting the command line\n");
        shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory splitting a command chain");
        return;
    }

    /* DOSKEY-style macro expansion: replace a leading alias with its value
     * before parsing, so `alias ll=dir /s` then typing `ll` runs `dir /s`.
     * Only expands at the interactive prompt, never inside a batch file. */
    if (!batch_alias_expand_command(command, chain_buffer, chain_size)) {
        snprintf(chain_buffer, chain_size, "%s", command);
    }

    segment_count = shell_split_chain(chain_buffer,
                                      segments,
                                      SHELL_CHAIN_SEGMENT_MAX,
                                      &truncated);
    if (segment_count <= 0) {
        free(chain_buffer);
        return;
    }

    if (truncated) {
        shell_transcript_appendf("chain: at most %d chained commands are supported\n",
                                 SHELL_CHAIN_SEGMENT_MAX);
        shell_record_warningf("shell", "Command chain truncated at %d segments", SHELL_CHAIN_SEGMENT_MAX);
    }

    /* Tracks the outcome of the most recently executed link. A skipped link
     * leaves it untouched, so `a && b && c` correctly skips c when a failed. */
    previous_succeeded = true;

    for (index = 0; index < segment_count; index++) {
        char *segment = segments[index].command;

        /* Decide whether this link runs, based on the previous outcome. */
        switch (segments[index].op) {
        case SHELL_CHAIN_ON_SUCCESS:
            if (!previous_succeeded) {
                continue;
            }
            break;
        case SHELL_CHAIN_ON_FAILURE:
            if (previous_succeeded) {
                continue;
            }
            break;
        case SHELL_CHAIN_FIRST:
        case SHELL_CHAIN_ALWAYS:
        default:
            break;
        }

        if (segment == NULL || segment[0] == '\0') {
            /* An empty link is only an error when a separator implied one. */
            if (segments[index].op != SHELL_CHAIN_FIRST) {
                shell_transcript_append_text("chain: empty command between separators\n");
                batch_set_errorlevel(1);
                previous_succeeded = false;
            }
            continue;
        }

        previous_succeeded = shell_execute_command_segment(segment);
    }

    free(chain_buffer);
}

/** Persistent worker task: waits on the command queue and executes commands
 *  sequentially. Each queued item is a heap-allocated, command-sized request
 *  owned by this task; the queue itself stores only pointers so a 4096-byte
 *  command does not reserve 16 KB of internal RAM inside the queue. */
/** Tear down any foreground surface an aborted command left open (gfx canvas,
 *  TUI, full-screen app mode) so the shell prompt becomes visible again after a
 *  Stop / Ctrl+C or a display rotation rebuild. Idempotent; safe when nothing
 *  is open. */
void command_close_foreground_surfaces(void)
{
    gfx_force_close();
    if (tui_is_active()) {
        tui_deinit();
    }
    if (shell_app_mode_active()) {
        shell_app_mode_exit();
    }
}

static void command_worker_task(void *arg)
{
    (void)arg;

    while (true) {
        command_request_t *request = NULL;

        if (xQueueReceive(s_command_queue, &request, portMAX_DELAY) == pdTRUE && request != NULL) {
#if P4_CONFIG_SD_OP_BOOST
            UBaseType_t base_priority = uxTaskPriorityGet(NULL);
#endif
            /* Claim the worker: the Stop button shows, and any stale break
             * from an earlier tap is dropped so it cannot poison this line. */
            shell_set_command_busy(true);
            shell_clear_abort();
            shell_execute_command(request->command);
            shell_set_command_busy(false);
            /* A foreground break that stopped the command may have left a gfx
             * canvas / TUI / app mode open; close it so the prompt returns. */
            if (shell_foreground_break_pending()) {
                command_close_foreground_surfaces();
                shell_clear_foreground_break();
            }
#if P4_CONFIG_SD_OP_BOOST
            command_task_priority_backstop(base_priority);
#endif
            heap_caps_free(request);
        }
    }
}

void shell_execute_command_async(char *command)
{
    command_request_t *request;

    if (command == NULL || command[0] == '\0') {
        return;
    }

    if (s_command_queue == NULL) {
        shell_print_error("shell: command worker not initialized");
        shell_record_errorf("shell", ESP_FAIL, "Command worker not initialized");
        return;
    }
    /* A command-sized request is ~4 KB; allocate it from PSRAM so a burst of
     * queued commands (pasted lines, modal chains, scripted drivers) never
     * fragments or exhausts the internal DMA-capable heap. Fall back to the
     * internal heap only if PSRAM is unavailable. */
    request = heap_caps_malloc(sizeof(*request),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (request == NULL) {
        request = malloc(sizeof(*request));
    }
    if (request == NULL) {
        shell_print_error("shell: out of memory queuing the command");
        shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory queuing async command");
        return;
    }

    snprintf(request->command, sizeof(request->command), "%s", command);

    /* Macro recorder hook: captures typed/tapped lines (never `macro`
     * control lines) for `macro stop` replay. Runs on the submitter task;
     * the recorder holds its lock only for the buffer append. */
    batch_macro_record_line(command);

    /* The queue stores the pointer only; the worker task owns and frees the
     * request after executing it. Wait boundedly for a slot (submit runs on
     * the LVGL/UART tasks, never the worker, so this cannot deadlock); only
     * a persistently-stuck worker still drops, loudly. */
    if (xQueueSend(s_command_queue, &request,
                   pdMS_TO_TICKS(SHELL_COMMAND_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
        shell_print_error("shell: command queue full, command dropped");
        shell_record_warningf("shell", "Command queue full, dropped: %s", command);
        heap_caps_free(request);
    }
}

bool shell_command_ota_is_pending(void)
{
    return c6ota_is_confirmation_pending();
}

/* ========================================================================
 * SHELL STATE ACCESSORS
 * ======================================================================== */

int command_get_volume_percent(void)
{
    return audio_get_volume();
}

void command_set_volume(int percent)
{
    if (audio_set_volume(percent) != ESP_OK) {
        ESP_LOGW(COMMAND_TAG, "command_set_volume: speaker init or codec set failed");
    }
}

/* ========================================================================
 * CLOCK HOST-RENDER WRAPPERS
 * ========================================================================
 * The clock component's date/time/timezone/sntp commands render through the
 * clock_host_ops_t table. These wrappers map each entry onto the matching
 * shell print/record helper, passing the clock-supplied text as a %s argument
 * so a literal '%' or '@' in it stays data (same injection guard the shell
 * print helpers use internally).
 */

static void command_clock_print_heading(const char *text)
{
    shell_print_heading("%s", text);
}

static void command_clock_print_field(const char *label, const char *value)
{
    shell_print_field(label, "%s", value);
}

static void command_clock_print_ok(const char *text)
{
    shell_print_ok("%s", text);
}

static void command_clock_print_error(const char *text)
{
    shell_print_error("%s", text);
}

static void command_clock_print_warning(const char *text)
{
    shell_print_warning("%s", text);
}

static void command_clock_print_muted(const char *text)
{
    shell_print_muted("%s", text);
}

static void command_clock_print_usage(const char *text)
{
    shell_print_usage("%s", text);
}

static void command_clock_record_error(const char *domain, esp_err_t error, const char *message)
{
    shell_record_errorf(domain, error, "%s", message);
}

static void command_clock_record_warning(const char *domain, const char *message)
{
    shell_record_warningf(domain, "%s", message);
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/* ========================================================================
 * NETWORK TIMEZONE AUTO-DETECT + SNTP BOOTSTRAP
 * ========================================================================
 * The user never sets a zone: on Wi-Fi association this looks the location up
 * over the network (P4_CONFIG_TIMEZONE_URL), applies the detected UTC offset,
 * and starts SNTP. It runs on a dedicated PSRAM-stack task because the HTTP
 * fetch blocks and must never run on the LVGL task. After success it re-probes
 * periodically so DST transitions and travel stay correct.
 */

static TaskHandle_t s_time_sync_task;
static volatile bool s_time_synced;

static void command_time_sync_task(void *arg)
{
    int attempts = 0;

    (void)arg;

    for (;;) {
        int offset_seconds = 0;
        char iana[64] = { 0 };

        /* Never probe while an OTA holds flash/PSRAM. */
        if (c6ota_is_busy()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (networking_time_detect(&offset_seconds, iana, sizeof(iana)) == ESP_OK) {
            char msg[96];
            int abs_off = (offset_seconds < 0) ? -offset_seconds : offset_seconds;

            time_set_utc_offset(offset_seconds, iana);
            time_start_sntp();
            s_time_synced = true;
            attempts = 0;

            snprintf(msg, sizeof(msg), "Time zone: %.40s UTC%c%d:%02d",
                     iana[0] != '\0' ? iana : "local",
                     (offset_seconds >= 0) ? '+' : '-',
                     abs_off / 3600, (abs_off % 3600) / 60);
            shell_header_notify(msg, P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS);

            vTaskDelay(pdMS_TO_TICKS((uint32_t)P4_CONFIG_TIMEZONE_RESYNC_SECS * 1000u));
            continue;
        }

        /* No zone yet: still start SNTP so at least UTC time is correct. */
        time_start_sntp();
        if (++attempts >= P4_CONFIG_TIMEZONE_MAX_ATTEMPTS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }

    s_time_sync_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/** Ensure the auto timezone/SNTP task is running. Cheap; safe from any task. */
bool command_time_auto_sync(void)
{
    if (s_time_synced) {
        return true;
    }
    if (s_time_sync_task != NULL) {
        return false;
    }
    if (xTaskCreateWithCaps(command_time_sync_task, "timesync", 6144, NULL,
                            tskIDLE_PRIORITY + 1, &s_time_sync_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_time_sync_task = NULL;
        time_start_sntp(); /* fall back to UTC time */
        return false;
    }
    return false;
}

/**
 * Long-press on a header status indicator. The header is a passive leaf and
 * knows nothing about commands, so the kind -> command mapping lives here and
 * runs the matching full status command on the worker task.
 */
static void command_header_status_action(header_status_kind_t kind)
{
    const char *line;
    char buffer[24];

    switch (kind) {
    case HEADER_STATUS_WIFI:      line = "wifi status";      break;
    case HEADER_STATUS_BLUETOOTH: line = "bluetooth status"; break;
    case HEADER_STATUS_USB:       line = "usb status";       break;
    case HEADER_STATUS_SD:        line = "sd info";          break;
    case HEADER_STATUS_ACTIVITY:  line = "ps";               break;
    case HEADER_STATUS_MEM:       line = "mem";              break;
    case HEADER_STATUS_CPU:       line = "top";              break;
    case HEADER_STATUS_BATTERY:   line = "battery";          break;
    default:                      return;
    }
    /* shell_execute_command_async() copies the text into its queue request
     * before returning and never takes ownership, so a stack buffer is safe. */
    snprintf(buffer, sizeof(buffer), "%s", line);
    shell_execute_command_async(buffer);
}

/**
 * Console-local command handler for the streaming `screenshot`, invoked by the
 * shell console-reader task while a modal blocks the command worker. The reader
 * is independent of the worker, so this is what lets the host capture a modal
 * surface. Only the exact bare streaming tokens are claimed; `screenshot
 * <file>`, captured forms, and everything else return false and keep their
 * normal path (worker queue for commands, modal for surface input).
 */
static bool command_modal_console_command(const char *line)
{
    char *argv[1];

    if (line == NULL) {
        return false;
    }
    /* Touch automation must run while a modal blocks the worker: parse and
     * drive it on this console-reader task (returns false when not a `ui`
     * line). */
    if (ui_commands_console(line)) {
        return true;
    }
    if (!(shell_text_equals_ignore_case(line, "screenshot") ||
          shell_text_equals_ignore_case(line, "scr") ||
          shell_text_equals_ignore_case(line, "capture"))) {
        return false;
    }
    /* argc == 1 selects the serial-streaming form (no SD write). The function
     * keeps its own single implementation in serial_commands.c. */
    argv[0] = (char *)line;
    shell_command_screenshot(1, argv);
    return true;
}

void command_init(void)
{
    static const shell_command_ops_t shell_ops = {
        .execute_command    = shell_execute_command,
        .execute_command_async = shell_execute_command_async,
        .get_cwd            = shell_get_cwd,
        .get_volume_percent = command_get_volume_percent,
        .sd_is_mounted      = storage_sd_is_mounted,
        .battery_read       = command_battery_read,

        /* External-module accessors. These let the shell core read state from
         * the networking, Bluetooth, USB, and C6 OTA modules without including
         * their headers, keeping the dependency direction one-way. */
        .wifi_is_connected      = networking_wifi_is_connected,
        .wifi_get_rssi          = networking_wifi_get_rssi,
        .wifi_state_string      = networking_wifi_state_string,
        .append_sysinfo_summary = networking_append_sysinfo_summary,
        .bluetooth_is_enabled   = bluetooth_is_enabled,
        .bluetooth_is_connected = bluetooth_is_connected,
        .usb_is_connected       = usb_is_connected,
        .usb_is_keyboard_attached = usb_is_keyboard_attached,
        .bg_jobs_running        = command_bg_any_running,
        .usb_key_to_ascii       = usb_key_to_ascii_full,
        .bind_lookup_fkey       = shell_bind_lookup_fkey,
        .bind_lookup_chord      = shell_bind_lookup_chord,
        .c6ota_is_pending       = c6ota_is_confirmation_pending,
        .c6ota_is_busy          = c6ota_is_busy,
        .pm_notify_activity     = shell_power_notify_activity,
        .pm_ms_until_idle_off   = shell_power_ms_until_idle_off,
        .time_auto_sync         = command_time_auto_sync,
        .complete_line          = command_complete_line,
        .ghost_line             = command_ghost_line,
        .modal_is_active        = modal_is_active,
        .modal_handle_usb_key   = modal_handle_usb_key,
        .modal_handle_serial_line = modal_handle_serial_line,
        .modal_console_command  = command_modal_console_command,
    };
    static const batch_command_ops_t batch_ops = {
        .execute_command = shell_execute_command,
    };

    if (s_initialized) {
        return;
    }

    /* Bring the owned-state modules up before anything can dispatch:
     * storage owns the current working directory and SD mount tracking,
     * batch owns the environment table, PATH, and errorlevel. */
    storage_init();
    batch_init();
    security_init();

    /* Create the persistent command queue and worker task. The queue
     * replaces per-command task creation: commands are posted to the
     * queue and processed sequentially by one long-lived task, eliminating
     * task-creation overhead and heap fragmentation. */
    s_command_queue = xQueueCreate(SHELL_COMMAND_QUEUE_DEPTH, sizeof(command_request_t *));
    if (s_command_queue == NULL) {
        ESP_LOGE(COMMAND_TAG, "Failed to create command queue");
    } else if (xTaskCreate(command_worker_task,
                           "shell_cmd",
                           SHELL_COMMAND_TASK_STACK_BYTES,
                           NULL,
                           tskIDLE_PRIORITY + 2,
                           &s_command_worker_handle) != pdPASS) {
        ESP_LOGE(COMMAND_TAG, "Failed to create command worker task");
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
    }

    /* Suspended bg pool for `start`: same stack as the main worker (bg jobs
     * run the identical pipeline, including deep batch+TUI), created now
     * while internal RAM still fits. */
    command_bg_pool_init();

    /* Publish the command pipeline to the batch engine so nested contexts
     * (if bodies, for bodies, pipe stages, batch lines) inherit variable
     * expansion and output redirection. */
    batch_register_command_ops(&batch_ops);

    /* Publish this module's services to the shell core. Keeping the
     * dependency one-way (command -> shell) avoids a component cycle. */
    shell_register_command_ops(&shell_ops);

    /* Long-pressing a header indicator runs the matching status command. The
     * header owns the tap/long-press gestures; this table gives it something
     * to call without the header depending on command.c. */
    header_register_status_action(command_header_status_action);

    /* Initialize the idle display-off state: the clock starts "active" and
     * the configured default timeout applies immediately. */
    shell_power_notify_activity();
    shell_power_set_idle_timeout(SHELL_POWER_IDLE_DISPLAY_OFF_SECS);

    /* Publish the shell render helpers to the clock component. The clock
     * commands (date/time/timezone/sntp/timer) live in components/clock and stay a
     * leaf; they print through this table. Each wrapper passes the text as a
     * %s argument so a literal '%' or '@' in it is treated as data. The
     * set_env hook lets `timer ... /v:NAME` store its result without the
     * clock component including batch.h. */
    {
        static const clock_host_ops_t clock_ops = {
            .emit_text          = shell_transcript_append_text,
            .print_heading      = command_clock_print_heading,
            .print_field        = command_clock_print_field,
            .print_ok           = command_clock_print_ok,
            .print_error        = command_clock_print_error,
            .print_warning      = command_clock_print_warning,
            .print_muted        = command_clock_print_muted,
            .print_usage        = command_clock_print_usage,
            .record_error       = command_clock_record_error,
            .record_warning     = command_clock_record_warning,
            .equals_ignore_case = shell_text_equals_ignore_case,
            .set_env            = shell_env_set,
        };
        clock_register_host_ops(&clock_ops);
    }

    /* Publish the networking state accessors to the applib runtime. Native
     * apps read Wi-Fi state through applib (app_wifi_*) without including
     * networking.h; the table keeps the dependency one-way (command owns
     * networking) and every hook is NULL-checked inside applib. */
    {
        static const applib_net_ops_t applib_net_ops = {
            .wifi_is_connected  = networking_wifi_is_connected,
            .wifi_get_rssi      = networking_wifi_get_rssi,
            .wifi_state_string  = networking_wifi_state_string,
        };
        applib_register_net_ops(&applib_net_ops);
    }

    /* Publish the process-environment accessors to the applib runtime. The
     * env table is owned by components/batch and cwd by components/storage;
     * applib reaches both through this table (never those headers), and every
     * hook is NULL-checked inside applib. */
    {
        static const applib_env_ops_t applib_env_ops = {
            .get_env = shell_env_get,
            .set_env = shell_env_set,
            .get_cwd = shell_get_cwd,
        };
        applib_register_env_ops(&applib_env_ops);
    }

    /* Publish the worker-queue hook to the alarm store. The store itself
     * starts lazily on the first `alarm`/`cal` command (see
     * shell_alarm_ensure_init() in alarm_commands.c): creating the checker
     * task here, before USB host init, fragments the internal DMA heap and
     * breaks USB HCD bring-up (verified on hardware). */
    {
        static const alarm_host_ops_t alarm_ops = {
            .execute_async = shell_execute_command_async,
        };
        alarm_register_host_ops(&alarm_ops);
    }

    /* Debounced history persistence: save to SD only when the recall ring
     * changed, so typing does not hammer the card. */
#if P4_CONFIG_HISTORY_AUTOSAVE
    {
        const esp_timer_create_args_t hist_args = {
            .callback = command_history_persist_cb,
            .name = "hist_save",
        };
        if (esp_timer_create(&hist_args, &s_history_persist_timer) == ESP_OK) {
            esp_timer_start_periodic(s_history_persist_timer, 5 * 1000 * 1000);
        }
    }
#endif

    s_initialized = true;
    ESP_LOGI(COMMAND_TAG, "Command module initialized");
}

bool command_is_initialized(void)
{
    return s_initialized;
}

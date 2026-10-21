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
#include "modal_surf.h"
#include "filetype.h"
#include "calc.h"
#include "applib.h"
#include "storage.h"
#include "storage_commands.h"
#include "shell.h"
#include "editor.h"
#include "editor_view.h"
#include "editor_spell.h"
#include "ansi_palette.h"
#include "ansi.h"
#include "audio.h"
#include "display.h"
#include "header.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "math_disp.h"
#include "networking.h"
#include "netsvc.h"
#include "driver/usb_serial_jtag.h"
#include <errno.h>
#include <stdarg.h>
#include "http_server.h"
#include "netdiag.h"
#include "led.h"
#include "alarm.h"
#include "bluetooth.h"
#include "tab5kbd.h"
#include "c6ota.h"
#include "tui.h"
#include "tui_modal.h"
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
#include "p4heap.h"
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
static StaticTask_t s_worker_tcb;
/* Static .bss stack: the 28 KB must be one contiguous internal block, and the
 * heap is too fragmented after boot (largest free ~22 KB) to allocate it
 * lazily. The internal-RAM budget is kept workable by moving the history save
 * off the timer task (4 KB timer stack instead of 8 KB) and sizing the Wi-Fi
 * init task to 10 KB, so the boot-time SDMMC/C6 DMA allocations still fit. */
static StackType_t s_worker_stack[(P4_CONFIG_COMMAND_TASK_STACK + sizeof(StackType_t) - 1) / sizeof(StackType_t)];


/* Hardware handles */

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

/* Token bound for the redirection parser below. */
#define SHELL_REDIRECT_TOKEN_MAX        4

/* Defined near the lifecycle section; needed by the lazy worker ensure. */
static void command_worker_task(void *arg);

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
    bool has_handle;  /**< true when a `0`/`1`/`2` handle prefix was consumed. */
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
bool shell_parse_redirection(char *command,
                             char **command_part,
                             char **redirect_target,
                             bool *append_mode,
                             char **input_source,
                             bool *missing_target_out)
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
    if (missing_target_out != NULL) {
        *missing_target_out = false;
    }

    if (command == NULL) {
        return false;
    }

    /* Pass 1: locate every unquoted operator. A `0`/`1`/`2` glued directly
     * before the operator is a cmd.exe stream handle: with no separate
     * stderr stream both handles merge into the transcript, so the digit is
     * recorded for removal rather than leaking into the command text. */
    cursor = shell_redirect_find_operator(command);
    while (cursor != NULL && token_count < SHELL_REDIRECT_TOKEN_MAX) {
        shell_redirect_token_t *token = &tokens[token_count++];

        token->position = cursor;
        token->is_output = (*cursor == '>');
        token->is_append = token->is_output && (cursor[1] == '>');
        token->target = cursor + (token->is_append ? 2 : 1);
        token->has_handle = (cursor > command &&
                             (cursor[-1] == '0' || cursor[-1] == '1' || cursor[-1] == '2'));

        cursor = shell_redirect_find_operator(token->target);
    }

    if (token_count == 0) {
        *command_part = shell_trim(command);
        return false;
    }

    /* Pass 2: cut the line at every operator. A `>>` needs both characters
     * blanked so the extra '>' cannot leak into the preceding text; a handle
     * digit is blanked for the same reason. */
    for (index = 0; index < token_count; index++) {
        tokens[index].position[0] = '\0';
        if (tokens[index].is_append) {
            tokens[index].position[1] = '\0';
        }
        if (tokens[index].has_handle) {
            tokens[index].position[-1] = '\0';
        }
    }

    /* Pass 3: publish the targets. An empty target (`echo hi >`) is a DOS
     * syntax error: flag it so the caller refuses the line. */
    for (index = 0; index < token_count; index++) {
        char *target = shell_redirect_unquote(shell_trim(tokens[index].target));

        if (target[0] == '\0' && missing_target_out != NULL) {
            *missing_target_out = true;
        }
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
 * Classify a redirection target for DOS device handling: `NUL` (any case,
 * any extension) discards output / reads EOF, `CON` keeps the console path,
 * everything else is an SD file. Pure.
 */
shell_redirect_target_kind_t shell_redirect_target_kind(const char *target)
{
    const char *dot;
    size_t head_len;

    if (target == NULL || target[0] == '\0') {
        return SHELL_REDIRECT_FILE;
    }
    dot = strchr(target, '.');
    head_len = (dot != NULL) ? (size_t)(dot - target) : strlen(target);
    if (head_len == 3 && strncasecmp(target, "NUL", 3) == 0) {
        return SHELL_REDIRECT_NUL;
    }
    if (head_len == 3 && strncasecmp(target, "CON", 3) == 0) {
        return SHELL_REDIRECT_CON;
    }
    return SHELL_REDIRECT_FILE;
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
static void shell_command_debug_save(int argc, char **argv);
static void shell_command_reboot(void);
static void shell_command_clear(void);
static void shell_command_prompt_cmd(int argc, char **argv);

/* Editor command + ops-table hooks. */
static void shell_command_edit(int argc, char **argv);
static void shell_command_recover(int argc, char **argv);
static void shell_command_spell(int argc, char **argv);

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

/**
 * `complete <partial line>` — print every Tab-completion match for the line
 * as a tap-confirm hyperlink whose target runs the completed text. The same
 * provider backs Tab and ghost, so the list can never disagree with the
 * keyboard. Later-token matches target the full reconstructed line; a
 * first-token match targets the command word itself.
 */
static void shell_command_complete(int argc, char **argv)
{
    /* One line-sized stack local only: match/target are heap (the worker
     * stack already nests near its edge under batch recursion, the launch
     * table lesson). */
    char line[SHELL_COMMAND_BYTES];
    char *match;
    char *target;
    int total;
    int shown;
    int i;

    if (argc < 2) {
        shell_print_usage("Usage: complete <partial command line>");
        batch_set_errorlevel(2);
        return;
    }
    match = malloc(SHELL_COMMAND_BYTES);
    target = malloc(SHELL_COMMAND_BYTES);
    if (match == NULL || target == NULL) {
        free(match);
        free(target);
        shell_print_error("complete: out of memory");
        batch_set_errorlevel(1);
        return;
    }
    shell_join_args(argv, 1, argc, line, sizeof(line));
    total = command_complete_line(line, 0, match, SHELL_COMMAND_BYTES);
    if (total <= 0) {
        shell_transcript_appendf_ansi(SH_MUTE "complete: no matches for '%s'\n" SH_RST, line);
        batch_set_errorlevel(1);
        free(match);
        free(target);
        return;
    }
    shown = 0;
    for (i = 0; i < total && i < P4_CONFIG_COMPLETION_MAX_MATCHES; i++) {
        size_t len;

        command_complete_line(line, i, match, SHELL_COMMAND_BYTES);
        if (match[0] == '\0') {
            continue;
        }
        /* Reconstruct the runnable line: append after a trailing blank,
         * else replace the last (partial) word. */
        len = strlen(line);
        if (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
            snprintf(target, SHELL_COMMAND_BYTES, "%s%s", line, match);
        } else {
            char *cut = strrchr(line, ' ');
            char *tab = strrchr(line, '\t');

            if (tab != NULL && (cut == NULL || tab > cut)) {
                cut = tab;
            }
            if (cut != NULL) {
                size_t head = (size_t)(cut - line) + 1u;

                if (head >= SHELL_COMMAND_BYTES) {
                    continue;
                }
                memcpy(target, line, head);
                snprintf(target + head, SHELL_COMMAND_BYTES - head, "%s", match);
            } else {
                snprintf(target, SHELL_COMMAND_BYTES, "%s", match);
            }
        }
        shell_transcript_appendf_ansi(SH_NUM "%d" SH_RST ": ", i);
        shell_transcript_append_link(match, target);
        shell_transcript_appendf_ansi("\n");
        shown++;
    }
    shell_transcript_appendf_ansi(SH_MUTE "complete: %d match%s (tap one to run it)\n" SH_RST,
                                 shown, shown == 1 ? "" : "es");
    batch_set_errorlevel(0);
    free(match);
    free(target);
}


/* ========================================================================
 * EDITOR COMMAND AND OPS-TABLE HOOKS
 * ======================================================================== */

/** `edit <path>` - open a DOS-style inline text editor. */
/** `spell learn <word> | spell forget <word> | spell list user` — manage the
 *  user dictionary overlay from the shell (batch-compatible; runs on the
 *  command worker where SD file I/O is allowed). */
static void shell_command_spell(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: spell learn <word> | spell forget <word> | spell list user");
        batch_set_errorlevel(2);
        return;
    }
    /* The overlay is unloaded when an editor session closes; reload it here
     * (on the worker, where SD I/O is allowed) so shell list/forget see the
     * persisted user dictionary. */
    (void)spell_user_load();
    if (shell_text_equals_ignore_case(argv[1], "learn")) {
        if (argc != 3) {
            shell_print_usage("Usage: spell learn <word>");
            batch_set_errorlevel(2);
            return;
        }
        if (spell_user_learn(argv[2], strlen(argv[2]))) {
            shell_transcript_appendf_ansi(SH_LBL "spell:" SH_RST " learned '%s'\n", argv[2]);
            batch_set_errorlevel(0);
        } else {
            shell_print_error("spell: cannot learn '%s'", argv[2]);
            batch_set_errorlevel(1);
        }
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "forget")) {
        if (argc != 3) {
            shell_print_usage("Usage: spell forget <word>");
            batch_set_errorlevel(2);
            return;
        }
        if (spell_user_forget(argv[2], strlen(argv[2]))) {
            shell_transcript_appendf_ansi(SH_LBL "spell:" SH_RST " forgot '%s'\n", argv[2]);
            batch_set_errorlevel(0);
        } else {
            shell_print_error("spell: '%s' is not in the user dictionary", argv[2]);
            batch_set_errorlevel(1);
        }
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "list")) {
        if (argc == 3 && shell_text_equals_ignore_case(argv[2], "user")) {
            size_t need = spell_user_list(NULL, 0);
            char *buf = malloc(need + 1);

            if (buf == NULL) {
                shell_print_error("spell: out of memory");
                batch_set_errorlevel(1);
                return;
            }
            spell_user_list(buf, need + 1);
            shell_transcript_appendf_ansi(SH_LBL "spell.user:" SH_RST " %u words\n%s",
                                          (unsigned)spell_user_count(), buf);
            free(buf);
            batch_set_errorlevel(0);
            return;
        }
        shell_print_usage("Usage: spell list user");
        batch_set_errorlevel(2);
        return;
    }
    shell_print_usage("Usage: spell learn <word> | spell forget <word> | spell list user");
    batch_set_errorlevel(2);
}

/** One editor crash file (sd:/tmp/edit/ plus the .autosave suffix). */
typedef struct {
    char orig[P4_CONFIG_SD_PATH_BYTES]; /**< Original path from the header ("" when unrecognized) */
    char file[P4_CONFIG_SD_PATH_BYTES]; /**< Crash file absolute path */
    long size;                          /**< Crash file bytes */
    time_t mtime;                       /**< Crash file modification time */
    bool recognized;                    /**< Header parsed */
} recover_entry_t;

#define RECOVER_MAX_ENTRIES 64
#define RECOVER_HEADER_PREFIX "#P4AUTOSAVE "

/** Parse a crash-file header line into @p orig_out. @return true when recognized. */
static bool recover_parse_header(const char *line, char *orig_out, size_t orig_size)
{
    static const char prefix[] = RECOVER_HEADER_PREFIX;
    size_t n = strlen(prefix);
    size_t len;

    if (orig_out == NULL || orig_size == 0) {
        return false;
    }
    orig_out[0] = '\0';
    if (line == NULL || strncmp(line, prefix, n) != 0) {
        return false;
    }
    len = strlen(line + n);
    while (len > 0 && (line[n + len - 1] == '\n' || line[n + len - 1] == '\r')) {
        len--;
    }
    if (len + 1 > orig_size) {
        return false;
    }
    memcpy(orig_out, line + n, len);
    orig_out[len] = '\0';
    return true;
}

/** Scan the recovery dir into @p entries. @return count (0 when none/absent). */
static int recover_scan(recover_entry_t *entries, int cap)
{
    char dir[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    DIR *d;
    struct dirent *e;
    int n = 0;

    if (entries == NULL || cap <= 0) {
        return 0;
    }
    if (storage_recovery_dir(dir, sizeof(dir)) != ESP_OK) {
        return 0;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return 0;
    }
    d = opendir(dir);
    if (d == NULL) {
        shell_sd_end(&session, "recover");
        return 0;
    }
    while ((e = readdir(d)) != NULL && n < cap) {
        size_t L = strlen(e->d_name);
        struct stat st;
        FILE *f;
        char line[P4_CONFIG_SD_PATH_BYTES + 16];

        if (L <= 9 || strcmp(e->d_name + L - 9, ".autosave") != 0) {
            continue;
        }
        if (snprintf(entries[n].file, sizeof(entries[n].file), "%s/%s",
                     dir, e->d_name) >= (int)sizeof(entries[n].file)) {
            continue;
        }
        if (stat(entries[n].file, &st) != 0) {
            continue;
        }
        entries[n].size = (long)st.st_size;
        entries[n].mtime = st.st_mtime;
        entries[n].recognized = false;
        entries[n].orig[0] = '\0';
        f = fopen(entries[n].file, "rb");
        if (f != NULL) {
            if (fgets(line, sizeof(line), f) != NULL) {
                entries[n].recognized =
                    recover_parse_header(line, entries[n].orig, sizeof(entries[n].orig));
            }
            fclose(f);
        }
        n++;
    }
    closedir(d);
    shell_sd_end(&session, "recover");
    return n;
}

/** True when @p e is the entry @p arg names (resolved original path, or the
 *  crash filename itself). */
static bool recover_entry_matches(const recover_entry_t *e, const char *resolved_arg,
                                  const char *raw_arg)
{
    if (e == NULL || resolved_arg == NULL) {
        return false;
    }
    if (e->recognized && e->orig[0] != '\0' &&
        strcmp(e->orig, "(unnamed)") != 0) {
        char ro[P4_CONFIG_SD_PATH_BYTES];
        if (shell_fs_resolve_path(e->orig, ro, sizeof(ro)) == ESP_OK &&
            strcmp(ro, resolved_arg) == 0) {
            return true;
        }
    }
    if (raw_arg != NULL) {
        const char *base = strrchr(e->file, '/');
        base = (base != NULL) ? base + 1 : e->file;
        if (strcmp(raw_arg, base) == 0) {
            return true;
        }
    }
    return false;
}

/** Print one entry line for `recover` list output. */
static void recover_print_entry(const recover_entry_t *e)
{
    char when[24];
    struct tm *tm = localtime(&e->mtime);

    if (tm != NULL) {
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
    } else {
        snprintf(when, sizeof(when), "?");
    }
    if (e->recognized && e->orig[0] != '\0') {
        shell_transcript_appendf_ansi(SH_LBL "recover:" SH_RST " " SH_PATH "%s" SH_RST
                                      " " SH_MUTE "%ld bytes, %s" SH_RST "\n",
                                      e->orig, e->size, when);
    } else {
        const char *base = strrchr(e->file, '/');
        base = (base != NULL) ? base + 1 : e->file;
        shell_transcript_appendf_ansi(SH_LBL "recover:" SH_RST " " SH_PATH "%s" SH_RST
                                      " " SH_WARN "(unrecognized)" SH_RST "\n",
                                      base);
    }
}

/** `recover [list] | recover restore <path> | recover discard <path> | recover clear`. */
static void shell_command_recover_inner(int argc, char **argv,
                                        recover_entry_t *entries)
{
    int count;
    int i;

    if (argc == 1 ||
        (argc == 2 && shell_text_equals_ignore_case(argv[1], "list"))) {
        count = recover_scan(entries, RECOVER_MAX_ENTRIES);
        if (count == 0) {
            shell_transcript_appendf_ansi(SH_MUTE "recover: no crash files\n" SH_RST);
        } else {
            for (i = 0; i < count; i++) {
                recover_print_entry(&entries[i]);
            }
        }
        batch_set_errorlevel(0);
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "restore")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];

        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("recover: invalid path %s", argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        count = recover_scan(entries, RECOVER_MAX_ENTRIES);
        for (i = 0; i < count; i++) {
            if (recover_entry_matches(&entries[i], resolved, argv[2])) {
                break;
            }
        }
        if (i >= count) {
            shell_print_error("recover: no crash file for %s", argv[2]);
            batch_set_errorlevel(1);
            return;
        }
        if (!entries[i].recognized || entries[i].orig[0] == '\0' ||
            strcmp(entries[i].orig, "(unnamed)") == 0) {
            shell_print_error("recover: crash file has no restorable path");
            batch_set_errorlevel(1);
            return;
        }
        {
            /* Binary-safe restore: exact body bytes through a temp file and
             * the shared atomic replace (same guarantee as editor save). */
            shell_sd_session_t session;
            FILE *f = fopen(entries[i].file, "rb");
            char *data = NULL;
            long body_off = 0;
            long body_len = 0;
            char tmp[P4_CONFIG_SD_PATH_BYTES + 5];
            char outpath[P4_CONFIG_SD_PATH_BYTES];
            bool ok = false;

            if (f == NULL) {
                shell_print_error("recover: cannot read crash file");
                batch_set_errorlevel(1);
                return;
            }
            if (shell_fs_resolve_path(entries[i].orig, outpath, sizeof(outpath)) != ESP_OK) {
                fclose(f);
                shell_print_error("recover: cannot restore %s", argv[2]);
                batch_set_errorlevel(1);
                return;
            }
            if (shell_sd_begin(&session) != ESP_OK) {
                fclose(f);
                shell_print_error("recover: SD unavailable");
                batch_set_errorlevel(1);
                return;
            }
            if (fseek(f, 0, SEEK_END) == 0) {
                long total = ftell(f);
                if (total > 0 && total <= (long)P4_CONFIG_EDITOR_MAX_BYTES + 4096) {
                    data = malloc((size_t)total);
                    if (data != NULL && fseek(f, 0, SEEK_SET) == 0 &&
                        fread(data, 1, (size_t)total, f) == (size_t)total) {
                        char *nl = memchr(data, '\n', (size_t)total);
                        if (nl != NULL) {
                            body_off = (long)(nl + 1 - data);
                            body_len = total - body_off;
                            ok = true;
                        }
                    }
                }
            }
            fclose(f);
            if (ok && snprintf(tmp, sizeof(tmp), "%s.tmp", outpath) < (int)sizeof(tmp)) {
                FILE *w = fopen(tmp, "wb");
                if (w != NULL) {
                    ok = (body_len == 0 ||
                          fwrite(data + body_off, 1, (size_t)body_len, w) == (size_t)body_len) &&
                         fclose(w) == 0;
                    if (!ok) {
                        remove(tmp);
                    }
                } else {
                    ok = false;
                }
            } else {
                ok = false;
            }
            free(data);
            if (ok) {
                ok = (storage_replace_file(tmp, outpath) == ESP_OK);
            }
            shell_sd_end(&session, "recover");
            if (ok) {
                shell_transcript_appendf_ansi(SH_LBL "recover:" SH_RST " restored " SH_PATH "%s" SH_RST
                                              " (%ld bytes)\n", entries[i].orig, body_len);
                batch_set_errorlevel(0);
            } else {
                shell_print_error("recover: restore failed for %s", argv[2]);
                batch_set_errorlevel(1);
            }
        }
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "discard")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;

        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("recover: invalid path %s", argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        count = recover_scan(entries, RECOVER_MAX_ENTRIES);
        for (i = 0; i < count; i++) {
            if (recover_entry_matches(&entries[i], resolved, argv[2])) {
                break;
            }
        }
        if (i >= count) {
            shell_print_error("recover: no crash file for %s", argv[2]);
            batch_set_errorlevel(1);
            return;
        }
        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("recover: SD unavailable");
            batch_set_errorlevel(1);
            return;
        }
        if (remove(entries[i].file) != 0) {
            shell_sd_end(&session, "recover");
            shell_print_error("recover: cannot discard %s", argv[2]);
            batch_set_errorlevel(1);
            return;
        }
        shell_sd_end(&session, "recover");
        shell_transcript_appendf_ansi(SH_LBL "recover:" SH_RST " discarded " SH_PATH "%s" SH_RST "\n",
                                      argv[2]);
        batch_set_errorlevel(0);
        return;
    }
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "clear")) {
        shell_sd_session_t session;

        count = recover_scan(entries, RECOVER_MAX_ENTRIES);
        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("recover: SD unavailable");
            batch_set_errorlevel(1);
            return;
        }
        for (i = 0; i < count; i++) {
            remove(entries[i].file);
        }
        shell_sd_end(&session, "recover");
        shell_transcript_appendf_ansi(SH_MUTE "recover: cleared %d crash file(s)\n" SH_RST, count);
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: recover [list] | recover restore <path> | recover discard <path> | recover clear");
    batch_set_errorlevel(2);
}

static void shell_command_recover(int argc, char **argv)
{
    recover_entry_t *entries = malloc(sizeof(*entries) * RECOVER_MAX_ENTRIES);

    if (entries == NULL) {
        shell_print_error("recover: out of memory");
        batch_set_errorlevel(1);
        return;
    }
    shell_command_recover_inner(argc, argv, entries);
    free(entries);
}

static void shell_command_edit(int argc, char **argv)
{
    const char *pos[3];
    const char *template_name = NULL;
    editor_session_opts_t opts;
    int errorlevel = 0;
    int pcount = 0;
    int i;

    opts.focus = false;
    opts.template_name = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "/focus") == 0) {
            opts.focus = true;
        } else if (strcmp(argv[i], "/template") == 0) {
            if (i + 1 >= argc) {
                shell_print_usage("Usage: edit <path> [/focus] [/template <name>]");
                batch_set_errorlevel(2);
                return;
            }
            template_name = argv[++i];
        } else if (pcount < 3) {
            pos[pcount++] = argv[i];
        } else {
            shell_print_usage("Usage: edit <path> [/focus] [/template <name>]");
            batch_set_errorlevel(2);
            return;
        }
    }
    opts.template_name = template_name;

    /* Record modes (exact arity only, so `edit db` still opens a file
     * literally named "db"): */
    if (pcount == 3 && shell_text_equals_ignore_case(pos[0], "db")) {
        batch_set_errorlevel(shell_command_edit_db(pos[1], pos[2],
                                                   opts.focus,
                                                   opts.template_name));
        return;
    }
    if (pcount == 2 &&
        (shell_text_equals_ignore_case(pos[0], "alarm") ||
         shell_text_equals_ignore_case(pos[0], "alarms") ||
         shell_text_equals_ignore_case(pos[0], "cal"))) {
        batch_set_errorlevel(shell_command_edit_alarm(pos[1], opts.focus,
                                                      opts.template_name));
        return;
    }
    if (pcount > 1) {
        shell_print_usage("Usage: edit <path> [/focus] [/template <name>]");
        batch_set_errorlevel(2);
        return;
    }

    /* The editor runs on the command worker task; it blocks until quit. */
    {
        const char *path = pcount == 1 ? pos[0] : NULL;
        esp_err_t err = editor_session_run_opts(path, &opts, &errorlevel);
        if (err != ESP_OK && errorlevel == 0) {
            errorlevel = 1;
        }
        batch_set_errorlevel(errorlevel);
    }
}

/* Forward declarations for modal functions */
extern bool modal_is_active(void);extern bool modal_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii);
extern bool modal_handle_serial_line(const char *line);
extern int modal_filebrowser_run(const char *title, const char *start_path,
                                  char *selected_path, size_t path_size,
                                  uint32_t timeout_ms);
extern int modal_hexview_run_buf(const char *title, const uint8_t *bytes, size_t len,
                                  uint32_t base_addr, uint32_t timeout_ms);

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
 * DEBUG LOG EXPORT: `debug save [file] [txt|csv|json]`
 * ========================================================================
 * The ring lives in the shell core (shell_debug_get_count/get_entry); this
 * command only opens the SD file and renders it, mirroring the history
 * /save flow (guarded session, free-space precheck, partial-file cleanup).
 * Entries are stored plain ("tag: message"), so no ANSI stripping is needed.
 */

/** CSV-escape @p text into @p out (quotes when it contains , " or newline). */
static void shell_debug_csv_field(const char *text, char *out, size_t out_size)
{
    size_t pos = 0;
    bool quote = false;
    const char *p;

    if (out == NULL || out_size == 0) return;
    for (p = text; *p != '\0'; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            quote = true;
            break;
        }
    }
    if (quote && pos + 1 < out_size) out[pos++] = '"';
    for (p = text; *p != '\0' && pos + 2 < out_size; p++) {
        if (*p == '"') {
            if (pos + 2 >= out_size) break;
            out[pos++] = '"';
            out[pos++] = '"';
        } else if (*p == '\n' || *p == '\r') {
            out[pos++] = ' ';
        } else {
            out[pos++] = *p;
        }
    }
    if (quote && pos + 1 < out_size) out[pos++] = '"';
    out[pos] = '\0';
}

/** JSON-escape @p text into @p out (quotes, backslash, control chars). */
static void shell_debug_json_string(const char *text, char *out, size_t out_size)
{
    size_t pos = 0;
    const char *p;

    if (out == NULL || out_size == 0) return;
    if (pos + 1 < out_size) out[pos++] = '"';
    for (p = text; *p != '\0' && pos + 6 < out_size; p++) {
        if (*p == '"' || *p == '\\') {
            out[pos++] = '\\';
            out[pos++] = *p;
        } else if (*p == '\n') {
            out[pos++] = '\\';
            out[pos++] = 'n';
        } else if (*p == '\r') {
            out[pos++] = '\\';
            out[pos++] = 'r';
        } else if ((unsigned char)*p < 0x20) {
            if (pos + 6 >= out_size) break;
            snprintf(out + pos, out_size - pos, "\\u%04x", (unsigned)*p);
            pos += 6;
        } else {
            out[pos++] = *p;
        }
    }
    if (pos + 1 < out_size) out[pos++] = '"';
    out[pos] = '\0';
}

static void shell_command_debug_save(int argc, char **argv)
{
    const char *file = P4_CONFIG_DEBUG_LOG_PROFILE;
    const char *format = "txt";
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    char entry[P4_CONFIG_DEBUG_ENTRY_BYTES + 32];
    char cell[P4_CONFIG_DEBUG_ENTRY_BYTES * 2 + 8];
    size_t count;
    size_t i;
    FILE *fp;
    int arg;
    bool file_given = false;

    for (arg = 2; arg < argc; arg++) {
        if (shell_text_equals_ignore_case(argv[arg], "txt") ||
            shell_text_equals_ignore_case(argv[arg], "csv") ||
            shell_text_equals_ignore_case(argv[arg], "json")) {
            format = argv[arg];
        } else if (!file_given) {
            file = argv[arg];
            file_given = true;
        } else {
            shell_print_usage("Usage: debug | debug save [file] [txt|csv|json]");
            batch_set_errorlevel(2);
            return;
        }
    }

    count = shell_debug_get_count();
    if (shell_fs_resolve_path(file, resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("debug: invalid path %s", file);
        batch_set_errorlevel(2);
        return;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error("debug: SD card not present - insert and retry");
        batch_set_errorlevel(1);
        return;
    }
    if (!storage_check_free_space((uint64_t)count * 256 + 4096, 0, "debug")) {
        shell_sd_end(&session, "debug");
        shell_print_error("debug: not enough free space on the SD card");
        batch_set_errorlevel(1);
        return;
    }
    fp = fopen(resolved, "w");
    if (fp == NULL) {
        shell_sd_end(&session, "debug");
        shell_print_error("debug: cannot open %s for writing", resolved);
        batch_set_errorlevel(1);
        return;
    }

    if (shell_text_equals_ignore_case(format, "json")) {
        fprintf(fp, "{\"entries\":%u,\"warnings\":%u,\"log\":[",
                (unsigned)count, (unsigned)shell_get_warning_count());
    } else if (shell_text_equals_ignore_case(format, "csv")) {
        fprintf(fp, "index,entry\n");
    } else {
        fprintf(fp, "# P4MiniShell debug log: %u entries, %u warnings\n",
                (unsigned)count, (unsigned)shell_get_warning_count());
    }
    for (i = 0; i < count; i++) {
        if (!shell_debug_get_entry(i, entry, sizeof(entry))) {
            break;
        }
        if (shell_text_equals_ignore_case(format, "json")) {
            shell_debug_json_string(entry, cell, sizeof(cell));
            fprintf(fp, "%s%s", i == 0 ? "" : ",", cell);
        } else if (shell_text_equals_ignore_case(format, "csv")) {
            shell_debug_csv_field(entry, cell, sizeof(cell));
            fprintf(fp, "%u,%s\n", (unsigned)i, cell);
        } else {
            fprintf(fp, "[%u] %s\n", (unsigned)i, entry);
        }
    }
    if (shell_text_equals_ignore_case(format, "json")) {
        fprintf(fp, "]}\n");
    }
    if (fflush(fp) != 0) {
        fclose(fp);
        (void)unlink(resolved);
        shell_sd_end(&session, "debug");
        shell_print_error("debug: write failed, removed the partial file");
        batch_set_errorlevel(1);
        return;
    }
    fclose(fp);
    shell_sd_end(&session, "debug");
    shell_transcript_appendf_ansi(SH_LBL "debug:" SH_RST " saved " SH_NUM "%u" SH_RST
                                  " entr%s to " SH_PATH "%s" SH_RST "\n",
                                  (unsigned)count, count == 1 ? "y" : "ies", resolved);
    batch_set_errorlevel(0);
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

/* The history save writes the SD card and needs a few KB of stack. Running it
 * directly on the esp_timer task made that task's high-water ~6.4 KB, forcing
 * CONFIG_ESP_TIMER_TASK_STACK_SIZE=8192 (~8 KB of internal RAM permanently).
 * A one-shot PSRAM-stack task does the SD write instead (SD, unlike flash, is
 * safe from a PSRAM stack), so the timer task stays shallow. Falls back to the
 * timer task only if the one-shot task cannot be created. */
/* The debounced save runs on a PSRAM-stack task, but its FILE is a newlib
 * internal-RAM allocation. Skip it while the internal heap is critically low
 * (F53: a long pkg/batch burst could reach ~0 and abort in __sfp); the
 * generation stays dirty and the next timer tick retries. */
static bool command_history_heap_ok(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= (8 * 1024);
}

static void command_history_persist_task(void *arg)
{
    (void)arg;
    if (command_history_heap_ok() &&
        shell_history_generation() != s_history_saved_generation) {
        command_history_save_now();
    }
    vTaskDelete(NULL);
}

static void command_history_persist_cb(void *arg)
{
    (void)arg;
    if (shell_history_generation() == s_history_saved_generation) {
        return;
    }
    if (!command_history_heap_ok()) {
        return;
    }
    if (xTaskCreateWithCaps(command_history_persist_task, "hist_save", 8192,
                            NULL, tskIDLE_PRIORITY + 1, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        command_history_save_now();
    }
}


bool shell_launch_app_with_args(const char *app_name, const char *args)
{
    char *command = malloc(P4_CONFIG_COMMAND_BYTES);
    if (command == NULL) {
        shell_print_error("launch: out of memory");
        return false;
    }
    if (args != NULL && args[0] != '\0') {
        snprintf(command, P4_CONFIG_COMMAND_BYTES, "launch %s %s", app_name, args);
    } else {
        snprintf(command, P4_CONFIG_COMMAND_BYTES, "launch %s", app_name);
    }
    shell_execute_command(command);
    free(command);
    return true;
}

bool shell_launch_app(const char *app_name)
{
    return shell_launch_app_with_args(app_name, NULL);
}

/* ========================================================================
 * FILE ASSOCIATIONS (`assoc` / `ftype` + `open` dispatch)
 * ========================================================================
 * DOS-shaped app registration without a registry: extension→app pairs live
 * in `sd:/ASSOC.INI` through the shared storage_ini core (same shape as
 * SERVICES.INI — no new parser, no new store), apps optionally declare
 * `handles=` in their APPINFO, and `open` consults the mapping before the
 * viewer fallback. Executable types never dispatch (source view only). */

#define ASSOC_REG_FILE "sd:/ASSOC.INI"

/** Longest extension key (`.' + stem), within the INI 32-byte key cap. */
#define ASSOC_EXT_BYTES 24

/**
 * Normalize an extension argument to `.lower` form: accepts `txt` or
 * `.txt`, lowercases, requires dot + alnum/`_.+-` stem. Pure.
 * @return true with @p out filled (always NUL-terminated).
 */
bool command_assoc_normalize_ext(const char *text, char *out, size_t out_size)
{
    size_t len = 0;
    size_t i;

    if (text == NULL || out == NULL || out_size == 0) {
        return false;
    }
    if (text[0] == '.') {
        text++;
    }
    if (text[0] == '\0') {
        return false;
    }
    out[0] = '.';
    for (i = 0; text[i] != '\0'; i++) {
        char c = text[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '+' || c == '-')) {
            return false;
        }
        len++;
        if (len + 2 > out_size || len + 2 > ASSOC_EXT_BYTES) {
            return false;
        }
        out[1 + len - 1] = c;
    }
    if (len == 0) {
        return false;
    }
    out[1 + len] = '\0';
    return true;
}

/* Split an `assoc` argument (forms in command.h): `.ext` → show,
 * `.ext=` → clear, `.ext=app` → set. @p app_out is filled only for set. */
assoc_arg_form_t command_assoc_split_arg(const char *text, char *ext_out, size_t ext_size,
                                         char *app_out, size_t app_size)
{
    const char *eq;

    if (text == NULL || ext_out == NULL || ext_size == 0) {
        return ASSOC_ARG_BAD;
    }
    eq = strchr(text, '=');
    if (eq == NULL) {
        return command_assoc_normalize_ext(text, ext_out, ext_size) ?
               ASSOC_ARG_SHOW : ASSOC_ARG_BAD;
    }
    {
        char left[ASSOC_EXT_BYTES];
        size_t left_len = (size_t)(eq - text);

        if (left_len == 0 || left_len >= sizeof(left)) {
            return ASSOC_ARG_BAD;
        }
        memcpy(left, text, left_len);
        left[left_len] = '\0';
        if (!command_assoc_normalize_ext(left, ext_out, ext_size)) {
            return ASSOC_ARG_BAD;
        }
    }
    if (eq[1] == '\0') {
        return ASSOC_ARG_CLEAR;
    }
    if (app_out == NULL || app_size == 0) {
        return ASSOC_ARG_BAD;
    }
    snprintf(app_out, app_size, "%s", eq + 1);
    if (app_out[0] == '\0') {
        return ASSOC_ARG_BAD;
    }
    return ASSOC_ARG_SET;
}

/** Read the app mapped to @p ext (already normalized) into @p app_out.
 *  @return true on a hit; false covers missing file and unmapped ext. */
static bool assoc_read(const char *ext, char *app_out, size_t app_size)
{
    char value[256];

    if (storage_ini_file_get(ASSOC_REG_FILE, ext, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    if (value[0] == '\0') {
        return false;
    }
    snprintf(app_out, app_size, "%s", value);
    return app_out[0] != '\0';
}

/**
 * Map @p path's extension to its app (extension extracted + normalized
 * here, so `open` needs no parsing). Never matches executable types.
 */
bool assoc_lookup_app(const char *path, char *app_out, size_t app_size)
{
    const char *slash;
    const char *dot;
    char ext[ASSOC_EXT_BYTES];

    if (path == NULL || app_out == NULL || app_size == 0) {
        return false;
    }
    if (filetype_is_executable(filetype_of(path))) {
        return false;
    }
    slash = strrchr(path, '/');
    dot = strrchr((slash != NULL) ? slash : path, '.');
    if (dot == NULL || dot[1] == '\0') {
        return false;
    }
    if (!command_assoc_normalize_ext(dot, ext, sizeof(ext))) {
        return false;
    }
    return assoc_read(ext, app_out, app_size);
}

/** Declared handlers of one app (`handles=` in its APPINFO, "" when absent). */
static void assoc_app_handles(const char *app, char *out, size_t out_size)
{
    char rel[SHELL_SD_PATH_BYTES];

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (app == NULL || app[0] == '\0') {
        return;
    }
    snprintf(rel, sizeof(rel), "sd:/APPS/%s.APPINFO", app);
    if (storage_ini_file_get(rel, "handles", out, out_size) != ESP_OK) {
        out[0] = '\0';
    }
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
    char app_type[16];                    /* APPINFO type ("" when absent) */
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
    /* Hybrid shims advertise their kind; anything else stays unmarked. */
    table[*count].app_type[0] = '\0';
    {
        char ptype[16];

        ptype[0] = '\0';
        if (storage_ini_file_get(appinfo, "type", ptype, sizeof(ptype)) == ESP_OK &&
            shell_text_equals_ignore_case(ptype, "hybrid")) {
            snprintf(table[*count].app_type, sizeof(table[*count].app_type), "hybrid");
        }
    }
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

bool shell_app_known(const char *app)
{
    shell_launch_entry_t *table;
    int count;
    int i;
    bool known = false;

    if (app == NULL || app[0] == '\0') {
        return false;
    }
    table = calloc((size_t)P4_CONFIG_LAUNCH_MAX, sizeof(*table));
    if (table == NULL) {
        return false;
    }
    count = shell_launch_discover(table);
    for (i = 0; i < count; i++) {
        if (shell_text_equals_ignore_case(table[i].name, app)) {
            known = true;
            break;
        }
    }
    free(table);
    return known;
}

/** `assoc` bare-listing callback: one `.ext=APP` row per entry. */
static bool assoc_list_cb(const char *key, const char *value, void *ctx)
{
    (void)ctx;
    shell_transcript_appendf("%s=%s\n", key, value);
    return true;
}

/** `ftype <app>` callback: print `APP=ext` for entries mapped to @p ctx app. */
static bool assoc_ftype_cb(const char *key, const char *value, void *ctx)
{
    const char *app = (const char *)ctx;

    if (app != NULL && shell_text_equals_ignore_case(value, app)) {
        shell_transcript_appendf("%s=%s\n", app, key);
    }
    return true;
}

static void shell_command_assoc(int argc, char **argv)
{
    char ext[ASSOC_EXT_BYTES];
    char app[256];

    if (argc == 1) {
        if (storage_ini_file_foreach(ASSOC_REG_FILE, assoc_list_cb, NULL) != ESP_OK) {
            shell_print_muted("assoc: no associations");
        }
        batch_set_errorlevel(0);
        return;
    }
    if (argc != 2) {
        shell_print_usage("Usage: assoc [.ext[=app]]   (bare lists; .ext= clears)");
        batch_set_errorlevel(2);
        return;
    }
    switch (command_assoc_split_arg(argv[1], ext, sizeof(ext), app, sizeof(app))) {
    case ASSOC_ARG_SHOW:
        if (assoc_read(ext, app, sizeof(app))) {
            shell_transcript_appendf("%s=%s\n", ext, app);
            batch_set_errorlevel(0);
        } else {
            shell_print_muted("assoc: no association for %s", ext);
            batch_set_errorlevel(1);
        }
        return;
    case ASSOC_ARG_CLEAR:
        if (storage_ini_file_delete(ASSOC_REG_FILE, ext) != ESP_OK) {
            shell_print_error("assoc: cannot clear %s", ext);
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf("assoc: %s cleared\n", ext);
        batch_set_errorlevel(0);
        return;
    case ASSOC_ARG_SET:
        if (!shell_app_known(app)) {
            shell_print_warning("assoc: no such app '%s' (mapping kept anyway)", app);
        }
        if (storage_ini_file_set(ASSOC_REG_FILE, ext, app) != ESP_OK) {
            shell_print_error("assoc: cannot map %s", ext);
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf("assoc: %s=%s\n", ext, app);
        batch_set_errorlevel(0);
        return;
    case ASSOC_ARG_BAD:
    default:
        shell_print_usage("Usage: assoc [.ext[=app]]   (bare lists; .ext= clears)");
        batch_set_errorlevel(2);
        return;
    }
}

static void shell_command_ftype(int argc, char **argv)
{
    char handles[256];

    if (argc == 1) {
        shell_launch_entry_t *table;
        int count;
        int i;
        int shown = 0;

        /* The launch table is sizable (~16 KB); PSRAM first so `ftype` keeps
         * working when the internal heap is tight after a long suite. */
        table = p4heap_calloc_psram((size_t)P4_CONFIG_LAUNCH_MAX, sizeof(*table));
        if (table == NULL) {
            table = calloc((size_t)P4_CONFIG_LAUNCH_MAX, sizeof(*table));
        }
        if (table == NULL) {
            shell_print_error("ftype: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        count = shell_launch_discover(table);
        for (i = 0; i < count; i++) {
            assoc_app_handles(table[i].name, handles, sizeof(handles));
            if (handles[0] != '\0') {
                shell_transcript_appendf("%s=%s\n", table[i].name, handles);
                shown++;
            }
        }
        free(table);
        if (shown == 0) {
            shell_print_muted("ftype: no app declares handles= (see ABI.md)");
        }
        batch_set_errorlevel(0);
        return;
    }
    if (argc != 2 && !(argc == 3 && shell_text_equals_ignore_case(argv[2], "/b"))) {
        shell_print_usage("Usage: ftype [app] [/b]   (bare lists declared handlers)");
        batch_set_errorlevel(2);
        return;
    }
    {
        /* /b drops the two English headers: assignments only, plus one
         * handles: row carrying the APPINFO declarations. */
        bool bare = (argc == 3);

        if (!bare) {
            assoc_app_handles(argv[1], handles, sizeof(handles));
            if (handles[0] != '\0') {
                shell_transcript_appendf("%s declares: %s\n", argv[1], handles);
            } else {
                shell_print_muted("ftype: %s declares no handles=", argv[1]);
            }
            shell_transcript_appendf("%s opens:\n", argv[1]);
        } else {
            assoc_app_handles(argv[1], handles, sizeof(handles));
            shell_transcript_appendf("%s=handles:%s\n", argv[1], handles);
        }
    }
    if (storage_ini_file_foreach(ASSOC_REG_FILE, assoc_ftype_cb, argv[1]) != ESP_OK) {
        if (argc != 3) {
            shell_print_muted("ftype: no associations file yet");
        }
    }
    batch_set_errorlevel(0);
}

/* ========================================================================
 * STARTUP APPS (`startup` verb + sd:/STARTUP.INI + boot runner)
 * ========================================================================
 * DOS STARTUP-folder semantics without a folder: one app name per
 * `name=1` line in `sd:/STARTUP.INI` (storage_ini core, like SERVICES.INI),
 * run in file order after AUTOEXEC.BAT and before the LAUNCH_APP offer by
 * `boot_run_startup()` (boot privilege, pre-lock, like services — but
 * one-shot foreground apps, NOT supervised TSRs: no second supervisor). */

#define STARTUP_REG_FILE "sd:/STARTUP.INI"

/** `startup list` row printer (storage_ini foreach callback). */
typedef struct {
    bool any;
} startup_list_ctx_t;

static bool startup_list_cb(const char *key, const char *value, void *vctx)
{
    startup_list_ctx_t *ctx = (startup_list_ctx_t *)vctx;

    (void)value;
    shell_transcript_appendf("%s\n", key);
    if (ctx != NULL) {
        ctx->any = true;
    }
    return true;
}

static void shell_command_startup(int argc, char **argv)
{
    /* Bare, `list`, and `/b` all emit the same bare-name rows (already
     * machine-readable); /b exists for switch symmetry with pkg/ftype. */
    if (argc == 1 ||
        (argc == 2 && shell_text_equals_ignore_case(argv[1], "list")) ||
        (argc == 2 && shell_text_equals_ignore_case(argv[1], "/b")) ||
        (argc == 3 && shell_text_equals_ignore_case(argv[1], "list") &&
         shell_text_equals_ignore_case(argv[2], "/b"))) {
        startup_list_ctx_t ctx = { false };

        if (storage_ini_file_foreach(STARTUP_REG_FILE, startup_list_cb, &ctx) != ESP_OK ||
            !ctx.any) {
            shell_print_muted("startup: no startup apps (startup add <app>)");
        }
        batch_set_errorlevel(0);
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "add")) {
        if (!shell_app_known(argv[2])) {
            shell_print_warning("startup: no such app '%s' (entry kept anyway)", argv[2]);
        }
        if (storage_ini_file_set(STARTUP_REG_FILE, argv[2], "1") != ESP_OK) {
            shell_print_error("startup: cannot add %s", argv[2]);
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf("startup: %s runs at boot\n", argv[2]);
        batch_set_errorlevel(0);
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "remove")) {
        if (storage_ini_file_delete(STARTUP_REG_FILE, argv[2]) != ESP_OK) {
            shell_print_error("startup: cannot remove %s", argv[2]);
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf("startup: %s removed\n", argv[2]);
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: startup [list] [/b] | add <app> | remove <app>");
    batch_set_errorlevel(2);
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

    /* `launch /list` — bare machine-parsable list for scripting. Hybrid
     * shims carry a ` [hybrid]` tag; plain batch lines stay exactly as
     * before so `for /f` scripts keep parsing. `/v` appends the APPINFO
     * version (`v-` when absent) without touching the default shape. */
    if ((argc == 2 || (argc == 3 && shell_text_equals_ignore_case(argv[2], "/v"))) &&
        shell_text_equals_ignore_case(argv[1], "/list")) {
        bool verbose = (argc == 3);

        for (i = 0; i < count; i++) {
            const char *tag = (table[i].app_type[0] != '\0') ? " [hybrid]" : "";
            if (table[i].title[0] != '\0') {
                shell_transcript_appendf("%s  -  %s%s", table[i].name, table[i].title, tag);
            } else {
                shell_transcript_appendf("%s%s", table[i].name, tag);
            }
            if (verbose) {
                char verel[P4_CONFIG_SD_PATH_BYTES];
                char ver[32];

                ver[0] = '\0';
                snprintf(verel, sizeof(verel), "sd:/APPS/%s.APPINFO", table[i].name);
                (void)storage_ini_file_get(verel, "version", ver, sizeof(ver));
                shell_transcript_appendf(" v%s", ver[0] ? ver : "-");
            }
            shell_transcript_appendf("\n");
        }
        batch_set_errorlevel(count > 0 ? 0 : 1);
        free(table);
        return;
    }

    /* `launch <name> [args...]` — run one app by name. Path scratch lives
     * on the heap, not in this frame: it persists for the whole app run
     * (freed after), and batch apps already nest the 32 KB worker stack
     * to the edge (a stack-resident table here once crashed the same way). */
    if (argc >= 2 && argv[1][0] != '/') {
        char *batch_path = malloc(SHELL_SD_PATH_BYTES);
        bool found = false;

        if (batch_path != NULL) {
            found = shell_resolve_batch_path(argv[1], batch_path, SHELL_SD_PATH_BYTES);
        }
        if (!found && batch_path != NULL) {
            /* Fall back to the conventional APPS directory. The name becomes
             * part of a path: reject separators and dot segments, mirroring
             * the appconfig validator, so a name can never escape APPS. */
            bool bad = (strchr(argv[1], '/') != NULL) || (strchr(argv[1], '\\') != NULL) ||
                       (strcmp(argv[1], ".") == 0) || (strcmp(argv[1], "..") == 0) ||
                       (argv[1][0] == '\0');
            if (!bad) {
                char *cand = malloc(3u * SHELL_SD_PATH_BYTES);

                if (cand != NULL) {
                    char *slot[3] = { cand, cand + SHELL_SD_PATH_BYTES,
                                      cand + 2u * SHELL_SD_PATH_BYTES };

                    snprintf(slot[0], SHELL_SD_PATH_BYTES, "sd:/APPS/%s", argv[1]);
                    snprintf(slot[1], SHELL_SD_PATH_BYTES, "sd:/APPS/%s.BAT", argv[1]);
                    snprintf(slot[2], SHELL_SD_PATH_BYTES, "sd:/APPS/%s.CMD", argv[1]);
                    for (i = 0; i < 3 && !found; i++) {
                        char resolved[SHELL_SD_PATH_BYTES];
                        FILE *probe;

                        if (shell_fs_resolve_path(slot[i], resolved, sizeof(resolved)) != ESP_OK) {
                            continue;
                        }
                        probe = fopen(resolved, "rb");
                        if (probe != NULL) {
                            fclose(probe);
                            snprintf(batch_path, SHELL_SD_PATH_BYTES, "%s", resolved);
                            found = true;
                        }
                    }
                    free(cand);
                }
            }
        }
        if (found && batch_path != NULL) {
            shell_execute_batch_file(batch_path, argc - 2, &argv[2]);
            free(batch_path);
            free(table);
            return;
        }
        if (batch_path == NULL) {
            shell_print_error("launch: out of memory");
        } else {
            shell_print_error("launch: app not found: %s", argv[1]);
        }
        free(batch_path);
        batch_set_errorlevel(1);
        free(table);
        return;
    }

    if (argc >= 2) {
        shell_print_usage("Usage: launch | launch <name> [args] | launch /list [/v]");
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
        {
            /* Hybrid shims show their kind; the numbers (what `for /f`
             * scripts and fingers select) never move. The app name is a
             * tap-confirm hyperlink (`launch <name>`); the machine-parsable
             * `launch /list` above stays plain so scripts keep parsing. */
            char target[SHELL_LAUNCH_NAME_BYTES + 8];
            const char *tag = (table[i].app_type[0] != '\0') ? " [hybrid]" : "";

            snprintf(target, sizeof(target), "launch %s", table[i].name);
            shell_transcript_appendf_ansi(SH_NUM "%d." SH_RST " ", i + 1);
            shell_transcript_append_link(table[i].name, target);
            if (table[i].title[0] != '\0') {
                shell_transcript_appendf_ansi(SH_EXE "%s" SH_RST "  -  %s\n",
                                              tag, table[i].title);
            } else {
                shell_transcript_appendf_ansi(SH_EXE "%s" SH_RST "\n", tag);
            }
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
            /* Auto-kill deadline (`start /t:secs`) unwinds like a kill. */
            batch_bg_poll_timeout();
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
    bool suspended;  /* frozen by `suspend` (pool slots only, see below) */
    char *pending;   /* PSRAM job line handed to the runner (pool i -> slot i+1) */
    int exit_code;   /* job's final errorlevel, published before running clears */
} bg_job_t;

static bg_job_t s_bg_jobs[P4_CONFIG_BG_TASKS];

/* Per-slot CPU accounting for the task manager. The baseline runtime
 * counters are anchored at job spawn (first successful sample) and diffed
 * against the live FreeRTOS counters on each query, so the reported share
 * covers the job's whole life, not just the last `ps` window. Anchors are
 * dropped on release; a suspended job's frozen counters make its share
 * decay toward zero while the snapshot total keeps advancing. */
typedef struct {
    bool valid;
    uint32_t base_runtime;
    uint32_t base_total;
    int last_cpu;
} tmgr_slot_acct_t;

static tmgr_slot_acct_t s_tmgr_acct[P4_CONFIG_BG_TASKS];

/** Drop a slot's accounting anchor (call on release; launch re-anchors). */
static void tmgr_acct_invalidate(int slot)
{
    if (slot < 1 || slot > P4_CONFIG_BG_TASKS) {
        return;
    }
    s_tmgr_acct[slot - 1].valid = false;
    s_tmgr_acct[slot - 1].last_cpu = 0;
}

/** Anchor (or re-anchor) a slot's CPU baseline from the live counters. */
static void tmgr_acct_anchor(int slot)
{
    tmgr_slot_acct_t *acct;
    uint32_t runtime = 0;
    uint32_t total = 0;

    if (slot < 1 || slot > P4_CONFIG_BG_TASKS) {
        return;
    }
    acct = &s_tmgr_acct[slot - 1];
    acct->valid = false;
    acct->last_cpu = 0;
    if (s_bg_jobs[slot - 1].handle == NULL) {
        return;
    }
    if (shell_task_runtime_for_handle(s_bg_jobs[slot - 1].handle,
                                      &runtime, &total)) {
        acct->base_runtime = runtime;
        acct->base_total = total;
        acct->valid = true;
    }
}

/** Per-slot CPU% since the anchor (0-based pool index; -1 when unknown).
 *  Anchors lazily on the first query so a spawn that raced the snapshot
 *  still gets a baseline. */
static int tmgr_slot_cpu_percent(int index)
{
    tmgr_slot_acct_t *acct;
    uint32_t runtime = 0;
    uint32_t total = 0;
    uint32_t task_delta;
    uint32_t total_delta;

    if (index < 0 || index >= P4_CONFIG_BG_TASKS) {
        return -1;
    }
    if (!s_bg_jobs[index].active || s_bg_jobs[index].handle == NULL) {
        return -1;
    }
    if (!shell_task_runtime_for_handle(s_bg_jobs[index].handle,
                                       &runtime, &total)) {
        return -1;
    }
    acct = &s_tmgr_acct[index];
    if (!acct->valid) {
        acct->base_runtime = runtime;
        acct->base_total = total;
        acct->valid = true;
        acct->last_cpu = 0;
        return 0;
    }
    if (total <= acct->base_total) {
        return acct->last_cpu;
    }
    task_delta = runtime - acct->base_runtime;
    total_delta = total - acct->base_total;
    if (total_delta == 0) {
        return acct->last_cpu;
    }
    acct->last_cpu = (int)((task_delta * 100u) / total_delta);
    return acct->last_cpu;
}

/** Live pool-slot priority as a tskIDLE offset (0 when no task yet). */
static unsigned tmgr_slot_prio(int index)
{
    TaskHandle_t handle;
    int offset;

    if (index < 0 || index >= P4_CONFIG_BG_TASKS) {
        return 0;
    }
    handle = s_bg_jobs[index].handle;
    if (handle == NULL) {
        return 0;
    }
    offset = (int)uxTaskPriorityGet(handle) - (int)tskIDLE_PRIORITY;
    return (offset < 0) ? 0u : (unsigned)offset;
}

/** Live pool-slot stack high-water bytes (0 when no task yet). */
static uint32_t tmgr_slot_stack_hw(int index)
{
    TaskHandle_t handle;

    if (index < 0 || index >= P4_CONFIG_BG_TASKS) {
        return 0;
    }
    handle = s_bg_jobs[index].handle;
    if (handle == NULL) {
        return 0;
    }
    return (uint32_t)uxTaskGetStackHighWaterMark(handle) *
           (uint32_t)sizeof(StackType_t);
}

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
        tmgr_acct_anchor(slot);
        shell_register_bg_task(xTaskGetCurrentTaskHandle());

#if P4_CONFIG_SD_OP_BOOST
        UBaseType_t bg_base_priority = uxTaskPriorityGet(NULL);
#endif
        shell_execute_command(s_bg_jobs[slot - 1].pending);
#if P4_CONFIG_SD_OP_BOOST
        command_task_priority_backstop(bg_base_priority);
#endif

        /* Publish the job's errorlevel for `start /wait` BEFORE clearing
         * running: the waiter spins on running, so this order closes the
         * reuse race (no lock needed — single writer, polled reader). */
        s_bg_jobs[slot - 1].exit_code = batch_get_errorlevel();
        free(s_bg_jobs[slot - 1].pending);
        s_bg_jobs[slot - 1].pending = NULL;
        s_bg_jobs[slot - 1].running = false;
        s_bg_jobs[slot - 1].active = false;
        s_bg_jobs[slot - 1].suspended = false;
        tmgr_acct_invalidate(slot);
        batch_bg_release(slot);
        /* Wake one `wait tmgr_exit` waiter per finished job (auto-clears —
         * the supervisor and the task manager share this rendezvous). */
        (void)sync_event_signal(P4_CONFIG_TMGR_EVENT_EXIT);
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
 * bg job runs (see the c6ota dispatch guard) — the two never overlap.
 *
 * Only the first P4_CONFIG_BG_EAGER_TASKS tasks are created here: every
 * pool task costs internal heap outside its PSRAM stack, and the boot
 * window must keep a contiguous 12 KB block for the Wi-Fi task. All slots
 * get their stacks now (PSRAM, no internal impact); the remaining tasks
 * are created lazily on first claim (command_bg_launch). */
static void command_bg_pool_init(void)
{
    int index;

    for (index = 0; index < P4_CONFIG_BG_TASKS; index++) {
        s_bg_jobs[index].stack = p4heap_alloc_psram(SHELL_COMMAND_TASK_STACK_BYTES);

        snprintf(s_bg_jobs[index].name, sizeof(s_bg_jobs[index].name),
                 "bg%d", index);
        if (s_bg_jobs[index].stack == NULL) {
            ESP_LOGE(COMMAND_TAG, "bg pool bg%d: no PSRAM for %u-byte stack",
                     index, (unsigned)SHELL_COMMAND_TASK_STACK_BYTES);
            s_bg_jobs[index].available = false;
            continue;
        }
        if (index >= P4_CONFIG_BG_EAGER_TASKS) {
            /* Lazy slot: stack is ready, the task arrives on first claim. */
            s_bg_jobs[index].handle = NULL;
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

/**
 * True when ANY physical keyboard is attached: a USB HID keyboard, a connected
 * Bluetooth HID keyboard, or the M5Stack Tab5 keyboard module. The shell hides
 * the on-screen keyboard while this holds.
 */
static bool command_physical_keyboard_present(void)
{
    if (usb_is_keyboard_attached()) {
        return true;
    }
    if (bluetooth_is_device_connected()) {
        return true;
    }
    return tab5kbd_is_ready();
}

/** True when any bg job is currently running (OTA guard). */
/* True while an OTA owns flash (netsvc pauses; PSRAM off-limits). */
static bool command_netsvc_ota_active(void)
{
    return c6ota_is_confirmation_pending() || c6ota_is_busy();
}

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

/**
 * Reconcile one slot's command flags with batch truth. A slot whose batch
 * context is gone holds no job by definition (worker exit always releases
 * it); leftover command flags would otherwise wedge the slot forever with
 * no verb able to claim or clear it. Batch wins ties.
 *
 * @return true when the slot was stale and got cleared.
 */
static bool command_bg_heal_stale(int slot)
{
    if (slot < 1 || slot > P4_CONFIG_BG_TASKS) {
        return false;
    }
    if (s_bg_jobs[slot - 1].active && !batch_bg_slot_in_use(slot)) {
        s_bg_jobs[slot - 1].active = false;
        s_bg_jobs[slot - 1].running = false;
        s_bg_jobs[slot - 1].suspended = false;
        free(s_bg_jobs[slot - 1].pending);
        s_bg_jobs[slot - 1].pending = NULL;
        tmgr_acct_invalidate(slot);
        shell_transcript_appendf("task manager: [%s] stale entry cleared\n",
                                 s_bg_jobs[slot - 1].name);
        return true;
    }
    return false;
}

/** True when a pool task handle is currently FreeRTOS-suspended. The
 *  snapshot-free single query behind truthful `start` states and the
 *  resume-if-stuck paths below. NULL handle reads as not suspended. */
static bool command_bg_task_suspended(const bg_job_t *job)
{
    if (job == NULL || job->handle == NULL) {
        return false;
    }
    return eTaskGetState(job->handle) == eSuspended;
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

/**
 * Find all active background jobs matching a DOS-style wildcard pattern.
 * Calls @p cb for every match; stops early if @p cb returns false.
 * Returns the number of matches.
 */
static int command_bg_find_pattern(const char *pattern,
                                   bool (*cb)(const bg_job_t *job, int slot, void *ctx),
                                   void *ctx)
{
    int matches = 0;
    size_t index;

    for (index = 0; index < P4_CONFIG_BG_TASKS; index++) {
        if (!s_bg_jobs[index].active) {
            continue;
        }
        if (!shell_wildcard_match(pattern, s_bg_jobs[index].name)) {
            continue;
        }
        matches++;
        if (cb != NULL) {
            if (!cb(&s_bg_jobs[index], (int)index + 1, ctx)) {
                break;
            }
        }
    }
    return matches;
}

/** Callback for command_bg_find_pattern: request kill on one matching job. */
static bool kill_one_job(const bg_job_t *job, int slot, void *ctx);

/** Launch failure reasons (the `start` verb maps these to messages; the
 *  supervisor hook only needs slot-or--1). */
typedef enum {
    BG_LAUNCH_OK = 0,
    BG_LAUNCH_NO_LINE,
    BG_LAUNCH_OTA,
    BG_LAUNCH_FULL,
    BG_LAUNCH_UNAVAIL,
    BG_LAUNCH_NOMEM,
} bg_launch_rc_t;

/** Launch @p line as a background job: claim a slot, hand the line over,
 *  resume the pooled worker. Shared by `start` and the service-supervisor
 *  hook (one implementation, never two pool-claim paths). */
static int command_bg_launch(const char *line, bg_launch_rc_t *rc_out)
{
    char *copy;
    int slot;
    bg_launch_rc_t rc = BG_LAUNCH_OK;

    if (line == NULL || line[0] == '\0') {
        rc = BG_LAUNCH_NO_LINE;
    } else if (c6ota_is_confirmation_pending()) {
        /* PSRAM stacks go inaccessible during OTA flash writes. */
        rc = BG_LAUNCH_OTA;
    } else {
        bg_job_t *job = NULL;

        slot = batch_bg_alloc();
        if (slot < 0) {
            rc = BG_LAUNCH_FULL;
            /* One rendezvous per refused launch: batch automation waiting
             * on `tmgr_pool_full` wakes exactly once (auto-clear). */
            (void)sync_event_signal(P4_CONFIG_TMGR_EVENT_POOL_FULL);
            shell_header_notify_level("task manager: no background slots free",
                                      3500, HEADER_NOTIFY_WARN);
        } else {
            job = &s_bg_jobs[slot - 1];
            if (!job->available && job->stack != NULL && job->handle == NULL) {
                /* Lazy pool slot (past P4_CONFIG_BG_EAGER_TASKS): the stack
                 * was preallocated at boot, but the task itself is created
                 * on first claim, past the boot heap window. */
                job->handle = xTaskCreateStatic(
                    command_bg_worker_task, job->name,
                    SHELL_COMMAND_TASK_STACK_BYTES / sizeof(StackType_t),
                    (void *)(intptr_t)(slot - 1), tskIDLE_PRIORITY + 2,
                    job->stack, &job->tcb);
                if (job->handle != NULL) {
                    job->available = true;
                }
            }
            if (!job->available) {
                batch_bg_release(slot);
                rc = BG_LAUNCH_UNAVAIL;
            } else {
                copy = malloc(SHELL_COMMAND_BYTES);
                if (copy == NULL) {
                    batch_bg_release(slot);
                    rc = BG_LAUNCH_NOMEM;
                } else {
                    snprintf(copy, SHELL_COMMAND_BYTES, "%s", line);
                    job->pending = copy;
                    job->active = true;
                    job->running = true;
                    job->suspended = false;
                    job->exit_code = 0;
                    vTaskResume(job->handle);
                }
            }
        }
    }
    if (rc_out != NULL) {
        *rc_out = rc;
    }
    return (rc == BG_LAUNCH_OK) ? slot : -1;
}

/** batch_bg_ops hook: supervisor launches (pool full/OTA -> -1, retried
 *  next tick, never counted against the restart budget). */
static int command_bg_hook_start(const char *line)
{
    return command_bg_launch(line, NULL);
}

/** batch_bg_ops hook: true while the slot's job still runs. */
static bool command_bg_hook_running(int slot)
{
    if (slot < 1 || slot > P4_CONFIG_BG_TASKS) {
        return false;
    }
    return s_bg_jobs[slot - 1].running;
}

static void shell_command_start(int argc, char **argv)
{
    int slot = -1;
    bool wait = false;
    uint32_t timeout_ms = 0;
    int first = 1;

    /* Bare `start` lists the pool (no new verb needed). `start /b` emits
     * uncoloured machine rows for the task manager:
     *   bgN|state|exit|prio|cpu|stack|ovl|himem|line
     * state is idle|running|suspended|unavailable; numeric fields are -1
     * when the slot has no live task. */
    if (argc < 2 || (argc == 2 && (argv[1][0] == '/' || argv[1][0] == '-') &&
                     (argv[1][1] == 'b' || argv[1][1] == 'B') && argv[1][2] == '\0')) {
        int index;
        bool bare = (argc == 2);

        if (!bare) {
            shell_transcript_appendf_ansi(SH_HEAD "Background jobs" SH_RST "\n");
        }
        for (index = 0; index < P4_CONFIG_BG_TASKS; index++) {
            /* Heal stale flags so the pool never shows phantom entries. */
            command_bg_heal_stale(index + 1);

            const char *state = !s_bg_jobs[index].available ? "unavailable" :
                                !s_bg_jobs[index].active ? "idle" :
                                command_bg_task_suspended(&s_bg_jobs[index]) ? "suspended" :
                                s_bg_jobs[index].running ? "running" : "idle";
            const char *line = s_bg_jobs[index].pending != NULL ?
                               s_bg_jobs[index].pending : "";

            if (bare) {
                int cpu = tmgr_slot_cpu_percent(index);
                int prio = (s_bg_jobs[index].active &&
                            s_bg_jobs[index].handle != NULL) ?
                           (int)tmgr_slot_prio(index) : -1;
                uint32_t stack = tmgr_slot_stack_hw(index);
                size_t ovl = batch_bg_overlay_bytes(index + 1);
                size_t himem = himem_slot_bytes(index + 1);

                shell_transcript_appendf("%s|%s|%d|%d|%d|%u|%u|%u|%s\n",
                                          s_bg_jobs[index].name, state,
                                          s_bg_jobs[index].exit_code,
                                          prio, cpu,
                                          (unsigned int)stack,
                                          (unsigned int)ovl,
                                          (unsigned int)himem, line);
            } else if (s_bg_jobs[index].active) {
                int cpu = tmgr_slot_cpu_percent(index);

                shell_transcript_appendf_ansi("  " SH_NUM "%s" SH_RST ": %s prio %u cpu %d%%: %s\n",
                                              s_bg_jobs[index].name, state,
                                              tmgr_slot_prio(index), cpu, line);
            } else {
                shell_transcript_appendf_ansi("  " SH_NUM "%s" SH_RST ": %s\n",
                                              s_bg_jobs[index].name, state);
            }
        }
        batch_set_errorlevel(0);
        return;
    }
    /* Leading DOS-style options: /wait (join) and /t:secs (auto-kill,
     * same parser as the modal verbs — one /t: implementation). */
    while (first < argc) {
        if (shell_text_equals_ignore_case(argv[first], "/wait")) {
            wait = true;
            first++;
        } else if (modal_parse_timeout_arg(argv[first], &timeout_ms)) {
            first++;
        } else {
            break;
        }
    }
    if (first >= argc) {
        shell_print_usage("Usage: start [/wait] [/t:secs] <command> [args...]");
        batch_set_errorlevel(2);
        return;
    }
    {
        char line[SHELL_COMMAND_BYTES];
        bg_launch_rc_t rc = BG_LAUNCH_OK;
        int launched;

        shell_join_args(argv, first, argc, line, sizeof(line));
        launched = command_bg_launch(line, &rc);
        if (launched < 0) {
            switch (rc) {
            case BG_LAUNCH_OTA:
                shell_transcript_append_text("start: refused while a C6 OTA is pending (finish or cancel it first)\n");
                break;
            case BG_LAUNCH_FULL:
                shell_transcript_appendf("start: no background slots free (max %d)\n",
                                         P4_CONFIG_BG_TASKS);
                break;
            case BG_LAUNCH_UNAVAIL:
                shell_transcript_append_text("start: background pool unavailable (boot task create failed, see boot log)\n");
                break;
            case BG_LAUNCH_NOMEM:
                shell_transcript_append_text("start: out of memory\n");
                break;
            default:
                shell_transcript_append_text("start: cannot launch job\n");
                break;
            }
            batch_set_errorlevel(1);
            return;
        }
        slot = launched;
    }

    if (timeout_ms > 0) {
        uint32_t now = xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;

        batch_bg_set_deadline(slot, now + timeout_ms);
    }
    if (!wait) {
        shell_transcript_appendf("[%s] started: %s\n", s_bg_jobs[slot - 1].name,
                                 s_bg_jobs[slot - 1].pending != NULL ?
                                 s_bg_jobs[slot - 1].pending : "");
        batch_set_errorlevel(0);
        return;
    }
    /* /wait: cooperative join. Poll the runner's flag in delay chunks (same
     * shape as `delay`, so taskkill and the deadline keep working while we
     * wait). Ctrl+C breaks the WAIT only — the job keeps running detached,
     * like CMD. A bg waiter whose own slot is killed stops waiting too. */
    shell_transcript_appendf("[%s] waiting: %s\n", s_bg_jobs[slot - 1].name,
                             s_bg_jobs[slot - 1].pending != NULL ?
                             s_bg_jobs[slot - 1].pending : "");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!s_bg_jobs[slot - 1].running) {
            break;
        }
        if (shell_abort_requested()) {
            shell_mark_foreground_break();
            shell_transcript_appendf("start: wait interrupted, [%s] still running\n",
                                     s_bg_jobs[slot - 1].name);
            batch_set_errorlevel(1);
            return;
        }
        if (batch_bg_kill_requested()) {
            shell_transcript_append_text("start: wait stopped\n");
            batch_set_errorlevel(1);
            return;
        }
    }
    {
        int code = s_bg_jobs[slot - 1].exit_code;

        shell_transcript_appendf("[%s] finished: EL=%d\n", s_bg_jobs[slot - 1].name, code);
        batch_set_errorlevel(code);
    }
}

static void shell_command_taskkill(int argc, char **argv)
{
    const bg_job_t *job;
    int slot = 0;

    if (argc != 2) {
        shell_print_usage("Usage: taskkill <job>   (name like bg0, slot number, or pattern like bg*)");
        batch_set_errorlevel(2);
        return;
    }
    job = command_bg_find(argv[1], &slot);
    if (job == NULL) {
        /* No exact match — try wildcard pattern (taskkill bg*, taskkill *.bat). */
        int killed = command_bg_find_pattern(argv[1], kill_one_job, NULL);
        if (killed == 0) {
            shell_transcript_appendf("taskkill: no job matched '%s'\n", argv[1]);
            batch_set_errorlevel(1);
        } else {
            shell_transcript_appendf("taskkill: stopped %d job%s\n", killed,
                                     killed == 1 ? "" : "s");
            batch_set_errorlevel(0);
        }
        return;
    }
    if (command_bg_heal_stale(slot)) {
        job = command_bg_find(argv[1], &slot);
        if (job == NULL) {
            shell_transcript_appendf("taskkill: no such job '%s'\n", argv[1]);
            batch_set_errorlevel(1);
            return;
        }
    }
    if (!job->running) {
        shell_transcript_appendf("taskkill: [%s] already finished\n", job->name);
        s_bg_jobs[slot - 1].active = false;
        batch_set_errorlevel(1);
        return;
    }
    batch_bg_request_kill(slot);
    if ((s_bg_jobs[slot - 1].suspended || command_bg_task_suspended(job)) &&
        job->handle != NULL) {
        vTaskResume(job->handle);
        s_bg_jobs[slot - 1].suspended = false;
        shell_transcript_appendf("taskkill: [%s] stop requested (resumed to stop)\n",
                                 job->name);
    } else {
        shell_transcript_appendf("taskkill: [%s] stop requested\n", job->name);
    }
    batch_set_errorlevel(0);
}

/** Callback for command_bg_find_pattern: request kill one matching job. */
static bool kill_one_job(const bg_job_t *job, int slot, void *ctx)
{
    (void)ctx;
    if (!job->running) {
        return true;
    }
    batch_bg_request_kill(slot);
    if ((s_bg_jobs[slot - 1].suspended || command_bg_task_suspended(job)) &&
        job->handle != NULL) {
        vTaskResume(job->handle);
        s_bg_jobs[slot - 1].suspended = false;
    }
    return true;
}

/* ========================================================================
 * TASK MANAGER: suspend, resume, nice (+renice), taskmgr
 * ========================================================================
 * Freeze/thaw and bounded priority for POOL slots only (amended ai-context
 * rule): the verbs validate the slot is a live pool job on every call and
 * refuse anything else, so system tasks (LVGL, idle, telemetry, workers)
 * are untouchable — `ps`/`tasks`/`top` stay read-only as required. A job
 * can never suspend its own slot (nothing could resume it); `taskkill`
 * still applies to a suspended job on resume, and deadlines pause while
 * suspended (no poll runs). Priority stays inside
 * [P4_CONFIG_TMGR_PRIO_MIN, P4_CONFIG_TMGR_PRIO_MAX] so a niced job can
 * neither starve the command worker nor collide with the SD boost level. */

int command_tmgr_clamp_priority(int prio)
{
    if (prio < (int)P4_CONFIG_TMGR_PRIO_MIN) {
        return (int)P4_CONFIG_TMGR_PRIO_MIN;
    }
    if (prio > (int)P4_CONFIG_TMGR_PRIO_MAX) {
        return (int)P4_CONFIG_TMGR_PRIO_MAX;
    }
    return prio;
}

bool command_tmgr_parse_priority(const char *text, int current, int *out)
{
    char *end = NULL;
    long value;

    if (text == NULL || out == NULL) {
        return false;
    }
    if (text[0] == '\0') {
        return false;
    }
    if (text[0] == '+' || text[0] == '-') {
        /* Relative adjustment, clamped into range (strtol alone would
         * accept a bare sign or leading space — require a digit). */
        if (!isdigit((unsigned char)text[1])) {
            return false;
        }
        value = strtol(text, &end, 10);
        if (end == NULL || *end != '\0') {
            return false;
        }
        *out = command_tmgr_clamp_priority(current + (int)value);
        return true;
    }
    /* Absolute priority inside the bounds (never clamped silently). */
    if (!isdigit((unsigned char)text[0])) {
        return false;
    }
    value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0') {
        return false;
    }
    if (value < (long)P4_CONFIG_TMGR_PRIO_MIN ||
        value > (long)P4_CONFIG_TMGR_PRIO_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
}

static void shell_command_suspend(int argc, char **argv)
{
    const bg_job_t *job;
    int slot = 0;

    if (argc != 2) {
        shell_print_usage("Usage: suspend <job>   (name like bg0, or slot number)");
        batch_set_errorlevel(2);
        return;
    }
    job = command_bg_find(argv[1], &slot);
    if (job == NULL) {
        shell_transcript_appendf("suspend: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    if (command_bg_heal_stale(slot)) {
        shell_transcript_appendf("suspend: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    job = command_bg_find(argv[1], &slot);
    if (job == NULL) {
        shell_transcript_appendf("suspend: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    if (!job->running || job->handle == NULL) {
        shell_transcript_appendf("suspend: [%s] not running\n", job->name);
        batch_set_errorlevel(1);
        return;
    }
    if (batch_bg_current_slot() == slot) {
        shell_transcript_append_text("suspend: cannot suspend own slot (nothing could resume it)\n");
        batch_set_errorlevel(1);
        return;
    }
    if (s_bg_jobs[slot - 1].suspended) {
        shell_transcript_appendf("suspend: [%s] already suspended\n", job->name);
        batch_set_errorlevel(1);
        return;
    }
    vTaskSuspend(job->handle);
    s_bg_jobs[slot - 1].suspended = true;
    shell_transcript_appendf("suspend: [%s] suspended (taskkill wakes it to stop)\n",
                             job->name);
    batch_set_errorlevel(0);
}

static void shell_command_resume(int argc, char **argv)
{
    const bg_job_t *job;
    int slot = 0;

    if (argc != 2) {
        shell_print_usage("Usage: resume <job>   (name like bg0, or slot number)");
        batch_set_errorlevel(2);
        return;
    }
    job = command_bg_find(argv[1], &slot);
    if (job == NULL) {
        shell_transcript_appendf("resume: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    if (command_bg_heal_stale(slot)) {
        shell_transcript_appendf("resume: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    job = command_bg_find(argv[1], &slot);
    if (job == NULL) {
        shell_transcript_appendf("resume: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    if (!job->running || job->handle == NULL) {
        shell_transcript_appendf("resume: [%s] not running\n", job->name);
        batch_set_errorlevel(1);
        return;
    }
    /* No suspended-flag gate: resuming a running task is a harmless no-op,
     * while gating on the flag would strand a task frozen by any path the
     * flag missed. The flag still clears so state stays truthful. */
    vTaskResume(job->handle);
    s_bg_jobs[slot - 1].suspended = false;
    shell_transcript_appendf("resume: [%s] running again\n", job->name);
    batch_set_errorlevel(0);
}

static void shell_command_nice(int argc, char **argv)
{
    const bg_job_t *job;
    int slot = 0;
    int current;
    int want;

    if (argc != 3) {
        shell_print_usage("Usage: nice <job> <1..3|+n|-n>   (pool priority; renice is an alias)");
        batch_set_errorlevel(2);
        return;
    }
    job = command_bg_find(argv[1], &slot);
    if (job == NULL) {
        shell_transcript_appendf("nice: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    if (command_bg_heal_stale(slot)) {
        shell_transcript_appendf("nice: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    job = command_bg_find(argv[1], &slot);
    if (job == NULL) {
        shell_transcript_appendf("nice: no such job '%s'\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    if (!job->running || job->handle == NULL) {
        shell_transcript_appendf("nice: [%s] not running\n", job->name);
        batch_set_errorlevel(1);
        return;
    }
    current = (int)uxTaskPriorityGet(job->handle) - (int)tskIDLE_PRIORITY;
    if (!command_tmgr_parse_priority(argv[2], current, &want)) {
        shell_transcript_appendf("nice: priority '%s' outside %d..%d (or use +n/-n)\n",
                                 argv[2],
                                 (int)P4_CONFIG_TMGR_PRIO_MIN,
                                 (int)P4_CONFIG_TMGR_PRIO_MAX);
        batch_set_errorlevel(1);
        return;
    }
    vTaskPrioritySet(job->handle,
                     (UBaseType_t)((int)tskIDLE_PRIORITY + want));
    shell_transcript_appendf("nice: [%s] priority %d -> %d\n", job->name,
                             current, want);
    batch_set_errorlevel(0);
}

static void shell_command_taskmgr(int argc, char **argv)
{
    /* Entry point for the task manager app: identical to `launch TASKMGR`
     * (discovery, .BAT shim, no shadowing), spelled the DOS way. */
    char *launch_argv[] = { (char *)"launch", (char *)"TASKMGR" };

    (void)argc;
    (void)argv;
    shell_command_launch(2, launch_argv);
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
 * TITLE
 * ========================================================================
 * `title [text]` — DOS session-title parity. The text is shell-core
 * session state (like the prompt template): `title` alone reports it,
 * `title <text>` stores it (truncated to P4_CONFIG_TITLE_BYTES),
 * `title ""`... an empty value clears it. It is also listed by `sysinfo`.
 * Display ownership is unchanged: the header keeps status, the transcript
 * keeps output; the title is metadata scripts and the host can read.
 */
static void shell_command_title_cmd(int argc, char **argv)
{
    char title_text[P4_CONFIG_TITLE_BYTES];

    if (argc == 1) {
        const char *current = shell_title_get();
        if (current == NULL || current[0] == '\0') {
            shell_print_muted("title: (none)");
        } else {
            shell_print_field("title:", "%s", current);
        }
        return;
    }

    /* Join so an unquoted multi-word title survives like the prompt body. */
    shell_join_args(argv, 1, argc, title_text, sizeof(title_text));
    shell_title_set(title_text);
    if (shell_title_get()[0] == '\0') {
        shell_print_ok("title: cleared");
    } else {
        shell_print_ok("title: set to '%s'", shell_title_get());
    }
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

/* ---- httpget cookie-jar persistence -------------------------------------
 * The jar itself lives in components/networking; persistence lives here so the
 * SD write path (guarded session + storage helpers) is reused and networking
 * gains no new file dependency. Format: one `domain<TAB>name<TAB>value` line
 * per cookie. The jar is loaded lazily once and saved only when networking
 * reports it changed. */
#define HTTPGET_COOKIE_FILE   "sd:/HTTPCOOK.TXT"
#define HTTPGET_MAX_HEADERS   NETWORKING_HTTP_MAX_EXTRA_HEADERS

static bool s_httpget_cookies_loaded = false;

/* Read a whole SD text file into a heap buffer (caller frees). */
static char *httpget_read_text_file(const char *path)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *f;
    char *buf;
    size_t cap = 512;
    size_t len = 0;
    int c;

    if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        return NULL;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return NULL;
    }
    f = fopen(resolved, "r");
    if (f == NULL) {
        shell_sd_end(&session, "httpget");
        return NULL;
    }
    buf = p4heap_alloc_any(cap);
    if (buf == NULL) {
        fclose(f);
        shell_sd_end(&session, "httpget");
        return NULL;
    }
    while ((c = fgetc(f)) != EOF) {
        if (len + 1 >= cap) {
            size_t ncap = cap * 2;
            char *grown = realloc(buf, ncap);
            if (grown == NULL) {
                break;
            }
            buf = grown;
            cap = ncap;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    fclose(f);
    shell_sd_end(&session, "httpget");
    return buf;
}

static void httpget_load_cookies(void)
{
    char *text;
    char *line;

    if (s_httpget_cookies_loaded) {
        return;
    }
    s_httpget_cookies_loaded = true;

    text = httpget_read_text_file(HTTPGET_COOKIE_FILE);
    if (text == NULL) {
        return;
    }
    line = text;
    while (line != NULL && *line != '\0') {
        char *nl = strchr(line, '\n');
        char *tab1;
        char *tab2;

        if (nl != NULL) {
            *nl = '\0';
        }
        {
            size_t len = strlen(line);
            while (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
        }
        tab1 = strchr(line, '\t');
        tab2 = (tab1 != NULL) ? strchr(tab1 + 1, '\t') : NULL;
        if (tab1 != NULL && tab2 != NULL) {
            *tab1 = '\0';
            *tab2 = '\0';
            if (line[0] != '\0' && tab1[1] != '\0') {
                (void)networking_http_cookie_add(line, tab1 + 1, tab2 + 1, 0);
            }
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    free(text);
    /* Loading is not a change worth writing straight back. */
    (void)networking_http_cookie_take_dirty();
}

static void httpget_save_cookies(void)
{
    char *text;
    size_t cap = (size_t)P4_CONFIG_HTTP_COOKIE_MAX * 480 + 1;
    size_t pos = 0;
    int count;
    int i;

    if (!networking_http_cookie_take_dirty()) {
        return;
    }
    text = p4heap_alloc_any(cap);
    if (text == NULL) {
        return;
    }
    count = networking_http_cookie_count();
    for (i = 0; i < count && pos + 2 < cap; i++) {
        networking_cookie_t cookie;
        int written;

        if (!networking_http_cookie_at(i, &cookie)) {
            continue;
        }
        written = snprintf(text + pos, cap - pos, "%s\t%s\t%s\n",
                           cookie.domain, cookie.name, cookie.value);
        if (written < 0 || (size_t)written >= cap - pos) {
            break;
        }
        pos += (size_t)written;
    }
    if (pos == 0) {
        text[0] = '\0';
    }
    (void)storage_write_text_file(HTTPGET_COOKIE_FILE, text);
    free(text);
}

/* ---- `/stream` sink: a file when one is named, else the transcript ------ */
typedef struct {
    FILE *file;
    size_t printed;
} httpget_stream_t;

static esp_err_t httpget_stream_cb(const uint8_t *chunk, size_t len,
                                   size_t total, void *ctx)
{
    httpget_stream_t *sink = (httpget_stream_t *)ctx;
    size_t i;

    (void)total;
    if (sink->file != NULL) {
        if (fwrite(chunk, 1, len, sink->file) != len) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (sink->printed >= P4_CONFIG_HTTP_PRINT_BODY_BYTES) {
        return ESP_OK;   /* transcript cap reached; keep draining */
    }
    {
        char line[129];
        size_t used = 0;

        for (i = 0; i < len && sink->printed < P4_CONFIG_HTTP_PRINT_BODY_BYTES; i++) {
            char ch = (char)chunk[i];

            if ((unsigned char)ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
                ch = '.';
            }
            line[used++] = ch;
            sink->printed++;
            if (used >= sizeof(line) - 1) {
                line[used] = '\0';
                shell_transcript_append_text(line);
                used = 0;
            }
        }
        if (used > 0) {
            line[used] = '\0';
            shell_transcript_append_text(line);
        }
    }
    return ESP_OK;
}

/** `httpget` / `wget` with the flag surface documented in command.md:
 *
 *   httpget [options] <url> [localfile]
 *     /h:NAME=VALUE     extra request header (repeatable, max 4)
 *     /post NAME=VALUE  send a urlencoded POST body (repeatable)
 *     /stream [file]    stream the response instead of buffering it
 *     /cookie [clear]   show or clear the persisted cookie jar
 */
static int shell_command_httpget(int argc, char **argv)
{
    const char *url = NULL;
    const char *localfile = NULL;
    const char *stream_file = NULL;
    networking_http_header_t headers[HTTPGET_MAX_HEADERS];
    networking_form_field_t fields[P4_CONFIG_HTTP_FORM_FIELDS_MAX];
    size_t header_count = 0;
    size_t field_count = 0;
    bool do_post = false;
    bool do_stream = false;
    bool show_cookie = false;
    bool clear_cookie = false;
    int i;

    for (i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (strncmp(arg, "/h:", 3) == 0) {
            char *eq = strchr(arg + 3, '=');
            if (eq == NULL || eq == arg + 3) {
                shell_print_error("httpget: /h: expects /h:NAME=VALUE");
                return 2;
            }
            *eq = '\0';
            if (header_count < HTTPGET_MAX_HEADERS) {
                headers[header_count].name = arg + 3;
                headers[header_count].value = eq + 1;
                header_count++;
            }
        } else if (strcmp(arg, "/post") == 0) {
            char *spec;
            char *eq;

            if (i + 1 >= argc) {
                shell_print_error("httpget: /post expects NAME=VALUE");
                return 2;
            }
            spec = argv[++i];
            eq = strchr(spec, '=');
            if (eq == NULL || eq == spec) {
                shell_print_error("httpget: /post expects NAME=VALUE");
                return 2;
            }
            *eq = '\0';
            if (field_count < P4_CONFIG_HTTP_FORM_FIELDS_MAX) {
                fields[field_count].name = spec;
                fields[field_count].value = eq + 1;
                field_count++;
            }
            do_post = true;
        } else if (strcmp(arg, "/stream") == 0) {
            do_stream = true;
            if (i + 1 < argc &&
                strncmp(argv[i + 1], "http://", 7) != 0 &&
                strncmp(argv[i + 1], "https://", 8) != 0 &&
                argv[i + 1][0] != '/') {
                stream_file = argv[++i];
            } else if (i + 1 < argc &&
                       (strncmp(argv[i + 1], "/sd/", 4) == 0 ||
                        strncmp(argv[i + 1], "/sdcard/", 8) == 0)) {
                /* An absolute SD path is a file, not an option. */
                stream_file = argv[++i];
            }
        } else if (strcmp(arg, "/cookie") == 0) {
            show_cookie = true;
            if (i + 1 < argc && shell_text_equals_ignore_case(argv[i + 1], "clear")) {
                clear_cookie = true;
                show_cookie = false;
                i++;
            }
        } else if (arg[0] == '/' && strncmp(arg, "/sd/", 4) != 0 &&
                   strncmp(arg, "/sdcard/", 8) != 0) {
            shell_print_error("httpget: unknown option %s", arg);
            return 2;
        } else if (url == NULL) {
            url = arg;
        } else if (localfile == NULL) {
            localfile = arg;
        } else {
            shell_print_error("httpget: too many arguments");
            return 2;
        }
    }

    if (do_post && do_stream) {
        shell_print_error("httpget: /post and /stream are mutually exclusive");
        return 2;
    }

    httpget_load_cookies();

    if (clear_cookie) {
        networking_http_cookie_clear();
        httpget_save_cookies();
        shell_print_ok("httpget: cookie jar cleared");
        if (url == NULL) {
            return 0;
        }
    }

    if (show_cookie) {
        int count = networking_http_cookie_count();
        int n;

        if (count == 0) {
            shell_print_muted("httpget: no stored cookies\n");
        }
        for (n = 0; n < count; n++) {
            networking_cookie_t cookie;

            if (!networking_http_cookie_at(n, &cookie)) {
                continue;
            }
            shell_transcript_appendf("%s\t%s=%s\n", cookie.domain,
                                     cookie.name, cookie.value);
        }
        if (url == NULL) {
            return 0;
        }
    }

    if (url == NULL) {
        shell_print_usage("Usage: httpget [/h:N=V] [/post N=V] [/stream [file]] [/cookie [clear]] <url> [localfile]");
        return 2;
    }

    if (do_stream) {
        httpget_stream_t sink = { 0 };
        shell_sd_session_t session;
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        bool session_open = false;
        esp_err_t error;

        if (stream_file != NULL) {
            if (shell_fs_resolve_path(stream_file, resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("httpget: invalid path %s", stream_file);
                return 1;
            }
            if (shell_sd_begin(&session) != ESP_OK) {
                shell_print_error("httpget: SD card not present - insert and retry");
                return 1;
            }
            session_open = true;
            sink.file = fopen(resolved, "wb");
            if (sink.file == NULL) {
                shell_print_error("httpget: failed to open %s", resolved);
                shell_sd_end(&session, "httpget");
                return 1;
            }
        }

        error = networking_http_get_stream(url, httpget_stream_cb, &sink);
        if (sink.file != NULL) {
            fclose(sink.file);
        }
        if (error == ESP_OK && sink.file != NULL) {
            shell_print_ok("httpget: streamed to %s", resolved);
        } else if (sink.file != NULL) {
            (void)remove(resolved);
            shell_print_error("httpget: stream failed - removed partial %s", resolved);
        } else if (error == ESP_OK) {
            shell_transcript_append_text("\n");
        }
        if (session_open) {
            shell_sd_end(&session, "httpget");
        }
        httpget_save_cookies();
        return error == ESP_OK ? 0 : 1;
    }

    {
        networking_http_result_t result;
        esp_err_t error;

        memset(&result, 0, sizeof(result));
        if (do_post) {
            if (field_count == 0) {
                shell_print_error("httpget: /post needs at least one NAME=VALUE");
                return 2;
            }
            error = networking_http_post_form(url, fields, field_count, &result);
        } else {
            error = networking_http_get_ex(url,
                                           header_count > 0 ? headers : NULL,
                                           header_count,
                                           NULL, NULL, NULL, &result);
        }
        httpget_save_cookies();
        if (error != ESP_OK) {
            networking_http_result_free(&result);
            return 1;
        }

        if (localfile != NULL) {
            char resolved[P4_CONFIG_SD_PATH_BYTES];
            shell_sd_session_t session;
            FILE *file = NULL;

            if (shell_fs_resolve_path(localfile, resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("httpget: invalid path %s", localfile);
                networking_http_result_free(&result);
                return 1;
            }
            if (shell_sd_begin(&session) != ESP_OK) {
                shell_print_error("httpget: SD card not present - insert and retry");
                networking_http_result_free(&result);
                return 1;
            }
            {
                uint64_t reclaim = storage_get_file_size(resolved);

                if (!storage_check_free_space(result.body_size, reclaim, "httpget")) {
                    shell_sd_end(&session, "httpget");
                    networking_http_result_free(&result);
                    return 1;
                }
            }
            file = fopen(resolved, "wb");
            if (file == NULL) {
                shell_print_error("httpget: failed to open %s", resolved);
                shell_sd_end(&session, "httpget");
                networking_http_result_free(&result);
                return 1;
            }
            if (result.body_size > 0 &&
                fwrite(result.body, 1, result.body_size, file) != result.body_size) {
                fclose(file);
                (void)remove(resolved);
                shell_print_error("httpget: write failed - removed partial %s", resolved);
                shell_sd_end(&session, "httpget");
                networking_http_result_free(&result);
                return 1;
            }
            fclose(file);
            shell_sd_end(&session, "httpget");
            shell_print_ok("httpget: saved %u bytes to %s",
                           (unsigned int)result.body_size, resolved);
        } else {
            shell_print_http_body(result.body, result.body_size);
            shell_transcript_append_text("\n");
        }

        networking_http_result_free(&result);
        return 0;
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

/** `uptime` — one-line seconds-since-boot (the same figure `version`
 *  reports, for scripts that do not want to parse the banner). */
static void shell_command_uptime(int argc, char **argv)
{
    char formatted[48];

    if (argc != 1) {
        shell_print_usage("Usage: uptime");
        batch_set_errorlevel(2);
        return;
    }
    (void)argv;
    time_get_uptime_formatted(formatted, sizeof(formatted));
    shell_print_field("uptime", "%s", formatted);
    batch_set_errorlevel(0);
}

/** `hostname [name]` — show the device hostname, or set it (persisted as a
 *  HOSTNAME= line in CONFIG.SYS, reloaded at boot by the generic KEY=VALUE
 *  fallback). Names stay short ([A-Za-z0-9_-], fits P4_CONFIG_ENV_NAME_BYTES)
 *  so the value renders anywhere a label does. */
static void shell_command_hostname(int argc, char **argv)
{
    const char *current;

    if (argc == 1) {
        current = shell_env_get("HOSTNAME");
        shell_print_field("hostname", "%s",
                          (current != NULL && current[0] != '\0') ? current :
                          P4_CONFIG_HOSTNAME_DEFAULT);
        batch_set_errorlevel(0);
        return;
    }
    if (argc != 2 || !sync_name_ok(argv[1]) ||
        strlen(argv[1]) >= (size_t)P4_CONFIG_ENV_NAME_BYTES) {
        shell_print_usage("Usage: hostname [name]  (letters, digits, _ and -, max 31 chars)");
        batch_set_errorlevel(2);
        return;
    }
    if (shell_env_set("HOSTNAME", argv[1]) != ESP_OK ||
        !config_persist_set("HOSTNAME", argv[1])) {
        shell_print_error("hostname: cannot persist %s", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    shell_print_ok("hostname: %s", argv[1]);
    batch_set_errorlevel(0);
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
                     shell_command_echo_glued(trimmed, NULL, NULL) ||
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

    if (shell_text_equals_ignore_case(argv[0], "complete")) {
        shell_command_complete(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "uptime")) {
        shell_command_uptime(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "hostname")) {
        shell_command_hostname(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "debug")) {
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "save")) {
            shell_command_debug_save(argc, argv);
        } else {
            batch_set_errorlevel(shell_command_debug(argc, argv));
        }
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

    if (shell_text_equals_ignore_case(argv[0], "shutdown") ||
        shell_text_equals_ignore_case(argv[0], "poweroff")) {
        shell_command_shutdown(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "mic")) {
        shell_command_mic(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "imu")) {
        shell_execute_imu_command(argc, argv);
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

    /* ---- TLS trust store management ---- */
    if (shell_text_equals_ignore_case(argv[0], "certs")) {
        shell_command_certs(argc, argv);
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

    /* ---- JavaScript eval/run/call ---- */
    if (shell_text_equals_ignore_case(argv[0], "js")) {
        batch_set_errorlevel(shell_command_js(argc, argv));
        return true;
    }

    /* ---- Browser ---- */
    if (shell_text_equals_ignore_case(argv[0], "browser")) {
        batch_set_errorlevel(shell_command_browser(argc, argv));
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

    /* ---- Serial PIM sync; int return becomes ERRORLEVEL. */
    if (shell_text_equals_ignore_case(argv[0], "pim")) {
        batch_set_errorlevel(shell_command_pim(argc, argv));
        return true;
    }

    /* ---- P4Sync handshake; int return becomes ERRORLEVEL. */
    if (shell_text_equals_ignore_case(argv[0], "sync")) {
        batch_set_errorlevel(shell_command_sync(argc, argv));
        return true;
    }

    /* ---- Board-to-board chat (chat) ----
     * argv-verb dispatcher: chat host|peer|send|poll|log|clear|status|onmsg.
     * Sets ERRORLEVEL 0 (ok) / 1 (state/failure) / 2 (usage). */
    if (shell_text_equals_ignore_case(argv[0], "chat")) {
        batch_set_errorlevel(shell_command_chat(argc, argv));
        return true;
    }

    /* ---- EPUB e-book reader (epub) ----
     * argv-verb dispatcher: epub open|close|info|toc|read|next|prev|goto|extract.
     * Sets ERRORLEVEL 0 (ok) / 1 (state/failure) / 2 (usage). */
    if (shell_text_equals_ignore_case(argv[0], "epub")) {
        batch_set_errorlevel(shell_command_epub(argc, argv));
        return true;
    }

    /* ---- Natural Display math (math) ----
     * argv-verb dispatcher: math new|open|close|edit|render|eval|cursor|insert.
     * Sets ERRORLEVEL 0 (ok) / 1 (state/failure) / 2 (usage). */
    if (shell_text_equals_ignore_case(argv[0], "math")) {
        batch_set_errorlevel(shell_command_math(argc, argv));
        return true;
    }

    /* ---- Computer Algebra System (cas) ----
     * argv-verb dispatcher: cas open|close|simplify|expand|factor|diff|integrate|solve|series|substitute|evaluate|latex.
     * Sets ERRORLEVEL 0 (ok) / 1 (state/failure) / 2 (usage). */
    if (shell_text_equals_ignore_case(argv[0], "cas")) {
        batch_set_errorlevel(shell_command_cas(argc, argv));
        return true;
    }

    /* ---- Event service (persistent MQTT + outbox); int becomes ERRORLEVEL. */
    if (shell_text_equals_ignore_case(argv[0], "net")) {
        batch_set_errorlevel(shell_command_net(argc, argv));
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
         * must never overlap, so OTA waits for background jobs. The event
         * service pauses on its own hook, but its PSRAM buffers stay
         * allocated, so an active session refuses here too. */
        if (command_bg_any_running()) {
            shell_transcript_append_text("c6ota: stop background jobs first (taskkill bg0 ...)\n");
            batch_set_errorlevel(1);
        } else if (netsvc_is_active()) {
            shell_transcript_append_text("c6ota: disconnect the event service first (net disconnect)\n");
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

    if (shell_text_equals_ignore_case(argv[0], "spell")) {
        shell_command_spell(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "recover")) {
        shell_command_recover(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "link")) {
        shell_command_link(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "vol")) {
        shell_command_vol();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "df")) {
        batch_set_errorlevel(shell_command_df(argc, argv));
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

    /* `env` / `printenv` — Unix names for a bare `set` listing. */
    if (shell_text_equals_ignore_case(argv[0], "env") ||
        shell_text_equals_ignore_case(argv[0], "printenv")) {
        if (argc != 1) {
            shell_print_usage("Usage: env");
            batch_set_errorlevel(2);
            return true;
        }
        shell_command_set(1, argv);
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
        shell_command_echo_glued(argv[0], NULL, NULL)) {
        /* The raw-line snapshot (taken before the tokenizer above) carries the
         * exact text, including a DOS glued separator (`echo.`/`echo/`/`echo(`/
         * `echo:`) and any long remainder the argv cap would truncate. One
         * renderer owns every echo spelling; this arm never rebuilds it. */
        if (echo_line != NULL) {
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

    if (shell_text_equals_ignore_case(argv[0], "switch")) {
        shell_command_switch(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "slots")) {
        shell_command_slots(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "title")) {
        shell_command_title_cmd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "config")) {
        shell_command_config(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "dim")) {
        shell_command_dim(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "str")) {
        shell_command_str(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "map")) {
        shell_command_map(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "trap")) {
        shell_command_trap(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "break")) {
        shell_command_break(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "continue")) {
        shell_command_continue(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "for")) {
        shell_command_for(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "while")) {
        shell_command_while(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "until")) {
        shell_command_until(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "screen")) {
        shell_command_screen(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "locale")) {
        batch_set_errorlevel(shell_command_locale(argc, argv));
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

    /* `himem` — XMS-style extended memory over PSRAM (the batch engine
     * owns the handle table; this is dispatch only). */
    if (shell_text_equals_ignore_case(argv[0], "himem")) {
        shell_command_himem(argc, argv);
        return true;
    }

    /* Batch synchronization + resident services (state lives in the batch
     * engine; these arms are dispatch only). */
    if (shell_text_equals_ignore_case(argv[0], "lock")) {
        shell_command_lock(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "unlock")) {
        shell_command_unlock(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "signal")) {
        shell_command_signal(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "wait")) {
        shell_command_wait(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "service")) {
        shell_command_service(argc, argv);
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

    /* ---- Workspace verbs (wig/pane/hud/key/tap/prompt/event/trace) ---- */
    if (shell_text_equals_ignore_case(argv[0], "wig")) {
        shell_command_wig(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "pane")) {
        shell_command_pane(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "hud")) {
        shell_command_hud(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "key")) {
        shell_command_key(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "tap")) {
        shell_command_tap(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "prompt") && argc >= 2 &&
        (shell_text_equals_ignore_case(argv[1], "ws") ||
         shell_text_equals_ignore_case(argv[1], "/text") ||
         shell_text_equals_ignore_case(argv[1], "/mask"))) {
        shell_command_prompt_ws(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "event")) {
        shell_command_event(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "trace")) {
        shell_command_trace(argc, argv);
        return true;
    }

    /* ---- Modal verbs (mode/mbind/mcmd/mdialog/guard/scope/palette/mmacro/pending/mstatus/repeat/cancel/buffer) ---- */
    if (shell_text_equals_ignore_case(argv[0], "mode")) {
        shell_command_mode(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "mbind")) {
        shell_command_mbind(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "mcmd")) {
        shell_command_mcmd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "mdialog")) {
        shell_command_mdialog(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "guard")) {
        shell_command_guard(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "scope")) {
        shell_command_scope(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "palette")) {
        shell_command_palette(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "mmacro")) {
        shell_command_mmacro(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "pending")) {
        shell_command_pending(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "mstatus")) {
        shell_command_mstatus(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "repeat")) {
        shell_command_repeat(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "cancel")) {
        shell_command_cancel(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "buffer")) {
        shell_command_buffer(argc, argv);
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
        text = p4heap_alloc_psram(text_cap);
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
        batch_set_errorlevel(shell_command_httpget(argc, argv));
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

    /* ---- File associations (`assoc` / `ftype`) ----
     * DOS-shaped app registration over sd:/ASSOC.INI (storage_ini core);
     * `open` consults the mapping before its viewer fallback. */
    if (shell_text_equals_ignore_case(argv[0], "assoc")) {
        shell_command_assoc(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "ftype")) {
        shell_command_ftype(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    /* ---- Startup apps (one-shot boot apps, not supervised services) ---- */
    if (shell_text_equals_ignore_case(argv[0], "startup")) {
        shell_command_startup(argc, argv);
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

    /* `kill` — Unix name for taskkill (same cooperative semantics). */
    if (shell_text_equals_ignore_case(argv[0], "kill")) {
        shell_command_taskkill(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    /* ---- Task manager: freeze/thaw/bounded-priority for pool slots ----
     * Pool slots only (validated per call); system tasks stay untouchable. */
    if (shell_text_equals_ignore_case(argv[0], "suspend")) {
        shell_command_suspend(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "resume")) {
        shell_command_resume(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "nice") ||
        shell_text_equals_ignore_case(argv[0], "renice")) {
        shell_command_nice(argc, argv);
        free(family_command);
        free(echo_line);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "taskmgr")) {
        shell_command_taskmgr(argc, argv);
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

/**
 * Run one `(...)` group: strip a single outer paren pair and execute the
 * inside through the full pipeline (chains, pipes, redirection per link).
 * Syntactic depth is refused before executing (bounded worker stack).
 * Mirrors the segment success rule: success is no *new* non-zero errorlevel.
 */
static bool shell_execute_command_group(const char *open, const char *close)
{
    size_t inner_len = (size_t)(close - 1 - (open + 1));
    char *inner;
    int errorlevel_before;
    int errorlevel_after;

    if (shell_group_depth(open) > (int)P4_CONFIG_GROUP_DEPTH_MAX) {
        shell_print_error("group: nesting past %d levels is refused",
                          (int)P4_CONFIG_GROUP_DEPTH_MAX);
        shell_record_warningf("shell", "Group nesting past %d refused",
                              (int)P4_CONFIG_GROUP_DEPTH_MAX);
        batch_set_errorlevel(2);
        return false;
    }
    inner = malloc(inner_len + 1);
    if (inner == NULL) {
        shell_transcript_append_text("shell: out of memory running a command group\n");
        shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory running a command group");
        batch_set_errorlevel(1);
        return false;
    }
    memcpy(inner, open + 1, inner_len);
    inner[inner_len] = '\0';
    errorlevel_before = batch_get_errorlevel();
    shell_execute_command(inner);
    free(inner);
    errorlevel_after = batch_get_errorlevel();
    return (errorlevel_after == 0) || (errorlevel_after == errorlevel_before);
}

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
    bool missing_target = false;
    bool have_output = false;
    bool discard_output = false;
    bool empty_command = false;
    char input_temp_file[SHELL_SD_PATH_BYTES];
    bool input_temp_created = false;
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

    /* `(...)` group as a chain link: only when the expanded segment is
     * EXACTLY one balanced group (leading `(` whose match ends the
     * segment). Anything else — `echo (a)`, `(a) extra`, unbalanced —
     * flows through the normal redirect/dispatch path untouched, so
     * literal parens keep working. */
    {
        char *grouped = shell_trim(command_buffer);
        const char *close = (grouped[0] == '(') ? shell_group_match(grouped) : NULL;

        if (close != NULL) {
            const char *rest = close;

            while (*rest == ' ' || *rest == '\t') {
                rest++;
            }
            if (*rest == '\0') {
                bool group_ok = shell_execute_command_group(grouped, close);

                free(command_buffer);
                return group_ok;
            }
        }
    }
    shell_parse_redirection(command_buffer, &command_part, &redirect_target,
                            &append_mode, &input_source, &missing_target);

    /* A bare operator with no target is a DOS syntax error: the line never
     * runs and no file is touched. */
    if (missing_target) {
        shell_print_error("redirection: missing target file");
        shell_record_warningf("shell", "Redirection operator without a target");
        batch_set_errorlevel(2);
        free(command_buffer);
        return false;
    }

    /* Publish the `<` source so the text-processing commands can pick it up
     * when the user gave no filename argument. A pipeline stage sets the same
     * slot, so both spellings reach one code path. DOS devices: `< CON`
     * leaves the slot clear (readers fall back to the console key queue),
     * `< NUL` reserves an empty temp file so readers see end-of-file. */
    if (input_source != NULL && input_source[0] != '\0') {
        shell_redirect_target_kind_t in_kind = shell_redirect_target_kind(input_source);

        if (in_kind == SHELL_REDIRECT_CON) {
            /* Console input: no slot, the key queue is the source. */
        } else if (in_kind == SHELL_REDIRECT_NUL) {
            if (storage_temp_path(input_temp_file, sizeof(input_temp_file), "nul") == ESP_OK) {
                storage_set_input_redirect(input_temp_file);
                input_redirect_set = true;
                input_temp_created = true;
            }
        } else {
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
    }

    /* DOS output devices: `> CON` runs unredirected (the transcript is the
     * console), `> NUL` captures and drops. Anything else captures for the
     * redirect file. A redirected command's output is captured into a
     * dedicated heap buffer (independent of the transcript), so a large
     * output survives the 16 KB transcript truncation. Open the capture
     * window before dispatch. */
    if (redirect_target != NULL && redirect_target[0] != '\0') {
        shell_redirect_target_kind_t out_kind = shell_redirect_target_kind(redirect_target);

        if (out_kind == SHELL_REDIRECT_CON) {
            redirect_target = NULL;
        } else {
            have_output = true;
            discard_output = (out_kind == SHELL_REDIRECT_NUL);
        }
    }
    if (have_output) {
        shell_redirect_capture_begin();
    }

    /* A line of only redirections (`> f`) truncates/creates the file like
     * DOS instead of dispatching an empty command. */
    empty_command = (command_part == NULL || command_part[0] == '\0');

    /* draw_buf errorlevel rather than clearing it. Clearing would destroy the
     * value that the very next `if errorlevel N` is meant to read, and DOS
     * only changes errorlevel when a command actually reports a status. A
     * command counts as failed for chaining purposes when it leaves a new
     * non-zero errorlevel behind. */
    errorlevel_before = batch_get_errorlevel();

    /* Batch label repaints across this segment's output: appends stay live in
     * the buffers and on serial, but the O(buffer) LVGL span rebuild happens
     * once per segment (plus a periodic live flush) instead of per line. */
    if (!empty_command) {
        shell_transcript_defer_begin();
        recognized = shell_execute_command_core(command_part);
        if (!recognized) {
    shell_transcript_appendf_ansi(SH_ERR "Unknown command:" SH_RST " %s\n", command_part);
    /* A mistyped command is user input, not a fault: report it in the
     * transcript and debug history without raising a warning-level log. */
    shell_record_infof("shell", "Unknown command: %s", command_part);
            batch_set_errorlevel(P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND);
        }
    } else {
        recognized = true;
    }

    /* The input slot belongs to exactly one command. Clear it here so a
     * failed dispatch cannot leak the source into the next line. */
    if (input_redirect_set) {
        storage_clear_input_redirect();
    }
    if (input_temp_created) {
        (void)remove(input_temp_file);
        input_temp_created = false;
    }

    if (have_output) {
        size_t captured_len = 0;
        const char *captured = shell_redirect_capture_get(&captured_len);
        esp_err_t error = ESP_OK;

        shell_redirect_capture_end();

        if (!discard_output) {
            error = shell_write_redirect_output(redirect_target, captured, append_mode);
            if (shell_redirect_capture_was_truncated()) {
                shell_print_warning("redirection: output exceeded %d bytes and was truncated",
                                    P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES);
                shell_record_warningf("shell", "Redirected output truncated at %d bytes",
                                      P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES);
            }
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

/**
 * Self-healing command worker integrity check (inspired by MS-DOS init).
 *
 * Validates the worker task handle, command queue, and worker TCB stack
 * canary. If the queue is missing but the task is alive, re-creates the
 * queue. If the task died, logs the failure for diagnostics. Called once
 * during init and exposed for periodic health checks.
 *
 * Returns: 0 healthy, 1 repaired (queue re-created), -1 fatal (task dead).
 */
/* Create the worker on first use. Its internal-RAM stack is reserved now,
 * after the boot-time SDMMC/C6 DMA buffers have been allocated, instead of at
 * link time (see s_worker_stack). */
static bool command_worker_ensure(void)
{
    if (s_command_worker_handle != NULL) {
        return true;
    }
    if (s_command_queue == NULL) {
        /* Self-heal: a soft reset can lose the queue while this module stays
         * initialised. Re-create it rather than refusing every command. */
        s_command_queue = xQueueCreate(SHELL_COMMAND_QUEUE_DEPTH,
                                       sizeof(command_request_t *));
        if (s_command_queue == NULL) {
            ESP_LOGE(COMMAND_TAG, "self-heal: failed to create command queue");
            return false;
        }
    }
    s_command_worker_handle = xTaskCreateStatic(command_worker_task, "shell_cmd",
                                                SHELL_COMMAND_TASK_STACK_BYTES / sizeof(StackType_t),
                                                NULL, tskIDLE_PRIORITY + 2,
                                                s_worker_stack, &s_worker_tcb);
    if (s_command_worker_handle == NULL) {
        ESP_LOGE(COMMAND_TAG, "Failed to create command worker task");
        return false;
    }
    return true;
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

    if (!command_worker_ensure()) {
        shell_print_error("shell: command worker unavailable (out of memory)");
        shell_record_errorf("shell", ESP_ERR_NO_MEM, "Command worker unavailable");
        return;
    }
    /* A command-sized request is small and must-succeed; allocate it via
     * the central allocator (any pool) so a burst of queued commands never
     * exhausts one pool. NULL propagates to the error path below. */
    request = p4heap_alloc_any(sizeof(*request));
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

/**
 * Ops-table adapters for the shell `debug` verbs. Each adapts a storage or
 * modal service to the shell-owned hook signature (the networking_host_ops_t
 * pattern); the implementations stay in their owning modules.
 */
static esp_err_t command_debug_read_sectors(uint32_t start_lba, uint8_t *buffer,
                                            uint32_t sector_count)
{
    return storage_disk_read_sectors(STORAGE_VOLUME_SD, start_lba, buffer,
                                     sector_count);
}

static int command_debug_show_hexview(const char *title, const uint8_t *bytes,
                                      size_t len, uint32_t base_addr)
{
    return modal_hexview_run_buf(title, bytes, len, base_addr, 0);
}

static bool command_debug_confirm_destructive(const char *operation,
                                              const char *warning,
                                              const char *detail)
{
    return shell_confirm_destructive(operation, warning, detail);
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
        .battery_is_charging = command_battery_is_charging,

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
        .physical_keyboard_present = command_physical_keyboard_present,
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
        .debug_read_sectors     = command_debug_read_sectors,
        .debug_show_hexview     = command_debug_show_hexview,
        .debug_confirm_destructive = command_debug_confirm_destructive,
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

    /* Create the persistent command queue. The worker task (and its large
     * internal stack) is created lazily on the first command by
     * command_worker_ensure(), after the boot-time SDMMC/C6 DMA buffers have
     * claimed their memory. The worker still runs on internal RAM: it performs
     * flash/NVS writes, where a PSRAM stack is inaccessible. */
    s_command_queue = xQueueCreate(SHELL_COMMAND_QUEUE_DEPTH, sizeof(command_request_t *));
    if (s_command_queue == NULL) {
        ESP_LOGE(COMMAND_TAG, "Failed to create command queue");
    } else if (!command_worker_ensure()) {
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

    /* Publish background-job control to the batch service supervisor
     * (same one-way pattern: supervisor decides, the pool runs). */
    {
        static const batch_bg_ops_t bg_ops = {
            .bg_start   = command_bg_hook_start,
            .bg_running = command_bg_hook_running,
        };
        batch_register_bg_ops(&bg_ops);
    }

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

    /* Publish the message service accessors to the applib runtime. Native
     * apps publish/subscribe through applib (app_msg_*) without including
     * networking.h; netsvc_publish returns esp_err_t (an int), so the
     * table assigns directly with the same 0-ok contract. */
    {
        static const applib_msg_ops_t applib_msg_ops = {
            .msg_publish   = netsvc_publish,
            .msg_subscribe = netsvc_subscribe,
            .msg_connected = netsvc_is_connected,
        };
        applib_register_msg_ops(&applib_msg_ops);
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

    /* Publish the upward hooks to the event service. The service task starts
     * lazily on the first `net` verb; inbound messages reach batch through
     * the async hook with NET_TOPIC/NET_LEN set, merges honor the device
     * lock, and the task pauses while an OTA owns flash. */
    {
        static const netsvc_host_ops_t netsvc_ops = {
            .execute_async      = shell_execute_command_async,
            .can_reveal_private = security_can_reveal_private,
            .set_env            = shell_env_set,
            .ota_active         = command_netsvc_ota_active,
        };
        netsvc_register_host_ops(&netsvc_ops);
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

    /* Initialize the modal layer with command-layer callbacks. */
    extern bool modal_layer_init(void);
    modal_layer_init();

    ESP_LOGI(COMMAND_TAG, "Command module initialized");
}

bool command_is_initialized(void)
{
    return s_initialized;
}

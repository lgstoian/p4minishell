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
#include "config_cmd.h"
#include "batch.h"
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
#define SHELL_REDIRECT_TOKEN_MAX        4
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
#define SHELL_COMMAND_QUEUE_DEPTH  4
static QueueHandle_t s_command_queue = NULL;
static TaskHandle_t s_command_worker_handle = NULL;

/* Hardware handles */
static bool s_light_sleep_requested;

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

/* Hardware helpers */


/* Pipeline helpers */
static bool shell_parse_redirection(char *command,
                                    char **command_part,
                                    char **redirect_target,
                                    bool *append_mode,
                                    char **input_source);
static bool shell_command_has_pipe(const char *command);

/* Command implementations owned by this module */
static void shell_command_clip(int argc, char **argv);
static void shell_command_paste(int argc, char **argv);
static void shell_command_history(int argc, char **argv);
static void shell_command_reboot(void);
static void shell_command_clear(void);
static void shell_command_prompt_cmd(int argc, char **argv);

/* Editor command + ops-table hooks. */
static void shell_command_edit(int argc, char **argv);
static bool editor_is_active(void);
static bool editor_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii);



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
 * ======================================================================== */

/** Built-in command names offered by Tab completion for the first token. */
static const char *const shell_builtin_commands[] = {
    "about", "adc", "alias", "ansi", "append", "appconfig", "appmode", "attrib", "audio", "battery", "beep",
    "bluetooth", "brightness", "bt", "c6ota", "calc", "call", "capture", "cd", "chdir",    "chkdsk", "choice", "clear", "clip", "cls", "comp", "config", "copy", "date", "debug",
    "deepsleep", "del", "dir", "disk", "display", "dns", "echo", "edit",
    "endlocal",
    "erase", "exit", "fc", "find", "findstr", "for", "format", "freq", "goto", "gpio",
    "help", "history", "httpd", "httpget", "i2c", "if", "ini", "ipconfig", "keyboard",
    "label", "md", "mem", "menu", "mkdir", "more", "move", "netstat", "nslookup",
    "ntpsync", "paste", "path", "pause", "ping", "power", "prompt", "ps", "pwm",
    "rd", "reboot", "receive", "recycle", "rem", "ren", "rename", "restore", "rgb", "rmdir",
    "rotate", "scandisk", "scr", "screenshot", "sd", "sdeject", "send", "set", "setlocal",
    "shift", "sleep", "sntp", "sort", "spi", "sysinfo", "tasks", "time", "timezone",
    "tone", "top", "touch", "trash", "tree", "type", "unalias", "undelete", "usb",
    "ver", "version", "volume", "wavplay", "wget", "wifi", "windows", "write", "xcopy",
    "proc", "temp",
};

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

/**
 * Tab-completion provider: fills @p out with the @p match_index-th completion
 * of @p word (commands/aliases for the first token, plus SD file/dir paths)
 * and returns the total number of matches.
 */
static int shell_complete_word(const char *word, bool first_token, int match_index,
                               char *out, size_t out_size)
{
    shell_complete_ctx_t ctx;
    size_t index;
    int total;

    if (word == NULL || out == NULL || out_size == 0) {
        return 0;
    }

    ctx.word = word;
    ctx.word_len = strlen(word);
    ctx.max = P4_CONFIG_COMPLETION_MAX_MATCHES;
    ctx.count = 0;
    ctx.matches = calloc((size_t)ctx.max, sizeof(char *));
    if (ctx.matches == NULL) {
        return 0;
    }

    if (first_token) {
        for (index = 0; index < sizeof(shell_builtin_commands) / sizeof(shell_builtin_commands[0]); index++) {
            shell_complete_add(&ctx, shell_builtin_commands[index]);
        }
        for (index = 0; index < (size_t)P4_CONFIG_ALIAS_MAX; index++) {
            char name[P4_CONFIG_ALIAS_NAME_BYTES];

            if (shell_alias_get_by_index((int)index, name, sizeof(name), NULL, 0)) {
                shell_complete_add(&ctx, name);
            }
        }
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

/* ========================================================================
 * EDITOR COMMAND AND OPS-TABLE HOOKS
 * ======================================================================== */

/** `edit <path>` � open a DOS-style inline text editor. */
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

bool editor_is_active(void)
{
    return editor_session_is_active() || editor_view_is_open();
}

bool editor_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii)
{
    if (!editor_view_is_open()) {
        return false;
    }
    return editor_view_handle_usb_key(key_code, modifiers, ascii);
}

/* Serial console control verbs accepted while the editor is open. */
static bool editor_serial_is_verb(const char *line, const char *verb)
{
    return line[0] == '\\' && strcasecmp(line + 1, verb) == 0;
}

/* Per-call context for async serial-line dispatch to the LVGL task. */
typedef struct {
    char *line;
} editor_serial_line_ctx_t;

/* Runs on the LVGL task: feeds one serial line's characters into the editor. */
static void editor_serial_line_cb(void *user_data)
{
    editor_serial_line_ctx_t *ctx = (editor_serial_line_ctx_t *)user_data;
    const char *line;
    size_t i;
    size_t len;

    if (ctx == NULL || ctx->line == NULL) {
        free(ctx);
        return;
    }


    line = ctx->line;
    len = strlen(line);

    if (editor_serial_is_verb(line, "q") || editor_serial_is_verb(line, "quit")) {
        if (editor_view_is_open()) {
            editor_view_handle_usb_key(0x29, 0, 0); /* Esc -> quit */
        } else {
            editor_view_set_quit_requested();
        }
    } else if (editor_serial_is_verb(line, "s") || editor_serial_is_verb(line, "save")) {
        if (editor_view_is_open()) {
            editor_view_handle_usb_key(0, 0x01, 's'); /* Ctrl+S */
        } else {
            editor_view_set_save_requested();
        }
    } else if (editor_serial_is_verb(line, "u") || editor_serial_is_verb(line, "undo")) {
        editor_view_handle_usb_key(0, 0x01, 'z');
    } else if (editor_serial_is_verb(line, "f") || editor_serial_is_verb(line, "find")) {
        editor_view_handle_usb_key(0, 0x01, 'f'); /* Ctrl+F */
    } else if (editor_serial_is_verb(line, "g") || editor_serial_is_verb(line, "goto")) {
        editor_view_handle_usb_key(0, 0x01, 'g'); /* Ctrl+G -> Go to line */
    } else if (editor_serial_is_verb(line, "o") || editor_serial_is_verb(line, "saveas")) {
        editor_view_handle_usb_key(0, 0x01, 'o'); /* Ctrl+O -> Save As */
    } else if (editor_serial_is_verb(line, "r") || editor_serial_is_verb(line, "redo")) {
        editor_view_handle_usb_key(0, 0x03, 'z'); /* Ctrl+Shift+Z */
    } else if (editor_serial_is_verb(line, "a") || editor_serial_is_verb(line, "selectall")) {
        editor_view_handle_usb_key(0, 0x01, 'a');
    } else {
        if (!editor_view_is_open()) {
            free(ctx->line);
            free(ctx);
            return;
        }
        /* A line of typed text: insert each character, then a newline. */
        for (i = 0; i < len; i++) {
            char ch = line[i];
            if (ch == '\\' && i == 0 && len > 1) {
                /* "\foo" that was not a known verb inserts a literal backslash
                 * and the rest of the line as text. */
                editor_view_handle_usb_key(0, 0, '\\');
                continue;
            }
            if (ch >= 0x20) {
                editor_view_handle_usb_key(0, 0, ch);
            }
        }
        editor_view_handle_usb_key(0x28, 0, '\n'); /* Enter -> newline */
    }

    free(ctx->line);
    free(ctx);
}

/* Forward declarations for modal functions */
extern bool modal_is_active(void);
extern bool modal_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii);
extern bool modal_handle_serial_line(const char *line);
extern int modal_filebrowser_run(const char *title, const char *start_path,
                                  char *selected_path, size_t path_size,
                                  uint32_t timeout_ms);

bool editor_handle_serial_line(const char *line)
{
    editor_serial_line_ctx_t *ctx;

    /* Accept lines whenever a session is active (even while the view is
     * still opening), so a quick '\q' after 'edit' is never misrouted as a
     * shell command and lost behind the blocked worker. */
    if ((!editor_session_is_active() && !editor_view_is_open()) || line == NULL) {
        return false;
    }

    /* Defer the whole line to the LVGL task: the editor's document and widget
     * state live there, and rebuilding rows on the UART console task would
     * block it past the watchdog and race the render cycle. */
    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return true; /* Consumed (drop) rather than misrouted. */
    }
    ctx->line = strdup(line);
    if (ctx->line == NULL) {
        free(ctx);
        return true;
    }

    if (lv_async_call(editor_serial_line_cb, ctx) != LV_RESULT_OK) {
        free(ctx->line);
        free(ctx);
    }
    return true;
}

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

    shell_print_usage("Usage: history | history /save [file] | history /load [file] | history /clear");
    batch_set_errorlevel(2);
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
 * SYSTEM COMMANDS: reboot, clear, prompt, date, time
 * ================================================================ */

/* ========================================================================
 * SYSTEM COMMANDS: reboot, clear, prompt, date, time
 * ======================================================================== */

static void shell_command_reboot(void)
{
    shell_transcript_appendf_ansi(SH_ERR "Rebooting..." SH_RST "\n");

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
        usb_handle_command(family_command);
        free(family_command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "c6ota")) {
        free(family_command);
        c6ota_perform(argc >= 2 ? argv[1] : NULL);
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

    if (shell_text_equals_ignore_case(argv[0], "anchor")) {
        shell_command_anchor(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "browse")) {
        bool has_v = false;
        for (int i = 1; i < argc; i++) if (strncasecmp(argv[i], "/v:", 3) == 0) has_v = true;
        if (has_v) shell_command_browse_batch(argc, argv);
        else shell_command_browse(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "view")) {
        shell_command_view(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "hexview")) {
        shell_command_hexview(argc, argv);
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

    /* ---- Batch file direct execution ---- */
    {
        char batch_path[SHELL_SD_PATH_BYTES];

        if (shell_resolve_batch_path(argv[0], batch_path, sizeof(batch_path))) {
            shell_execute_batch_file(batch_path, argc - 1, &argv[1]);
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
    /* Expansion needs two line-sized buffers. They live on the heap because
     * this function sits on the batch recursion path: a batch file calls back
     * into the pipeline for every line, and four nested levels of two large
     * stack buffers would overflow the command worker task's stack. The
     * buffers are command-sized because an interactive command line can be up
     * to P4_CONFIG_COMMAND_BYTES (batch lines are the smaller surface). */
    const size_t work_size = SHELL_COMMAND_BYTES;
    char *expanded = NULL;
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

    expanded = malloc(work_size);
    command_buffer = malloc(work_size);
    if (expanded == NULL || command_buffer == NULL) {
        free(expanded);
        free(command_buffer);
        shell_transcript_append_text("shell: out of memory expanding the command line\n");
        shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory expanding a command line");
        batch_set_errorlevel(1);
        return false;
    }

    shell_expand_variables(command, expanded, work_size);
    snprintf(command_buffer, work_size, "%s", expanded);
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
            free(expanded);
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

    free(expanded);
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
static void command_worker_task(void *arg)
{
    (void)arg;

    while (true) {
        command_request_t *request = NULL;

        if (xQueueReceive(s_command_queue, &request, portMAX_DELAY) == pdTRUE && request != NULL) {
            shell_execute_command(request->command);
            free(request);
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
    request = malloc(sizeof(*request));
    if (request == NULL) {
        shell_print_error("shell: out of memory queuing the command");
        shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory queuing async command");
        return;
    }

    snprintf(request->command, sizeof(request->command), "%s", command);

    /* The queue stores the pointer only; the worker task owns and frees the
     * request after executing it. On a full queue the request is dropped. */
    if (xQueueSend(s_command_queue, &request, 0) != pdTRUE) {
        shell_print_error("shell: command queue full, command dropped");
        shell_record_warningf("shell", "Command queue full, dropped: %s", command);
        free(request);
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
        .usb_key_to_ascii       = usb_key_to_ascii_full,
        .c6ota_is_pending       = c6ota_is_confirmation_pending,
        .c6ota_is_busy          = c6ota_is_busy,
        .pm_notify_activity     = shell_power_notify_activity,
        .complete_word          = shell_complete_word,
        .modal_is_active        = modal_is_active,
        .modal_handle_usb_key   = modal_handle_usb_key,
        .modal_handle_serial_line = modal_handle_serial_line,
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

    /* Publish the command pipeline to the batch engine so nested contexts
     * (if bodies, for bodies, pipe stages, batch lines) inherit variable
     * expansion and output redirection. */
    batch_register_command_ops(&batch_ops);

    /* Publish this module's services to the shell core. Keeping the
     * dependency one-way (command -> shell) avoids a component cycle. */
    shell_register_command_ops(&shell_ops);

    /* Initialize the idle display-off state: the clock starts "active" and
     * the configured default timeout applies immediately. */
    shell_power_notify_activity();
    shell_power_set_idle_timeout(SHELL_POWER_IDLE_DISPLAY_OFF_SECS);

    /* Publish the shell render helpers to the clock component. The clock
     * commands (date/time/timezone/sntp) live in components/clock and stay a
     * leaf; they print through this table. Each wrapper passes the text as a
     * %s argument so a literal '%' or '@' in it is treated as data. */
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

    s_initialized = true;
    ESP_LOGI(COMMAND_TAG, "Command module initialized");
}

bool command_is_initialized(void)
{
    return s_initialized;
}

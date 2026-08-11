/**
 * @file batch.c
 * @brief Batch engine, environment variables, and batch language commands.
 *
 * Owns the `.bat` interpreter: file execution with nesting, label scanning,
 * `goto`, `for` loops, the multi-stage `|` pipe operator, the RAM-only
 * environment variable table, PATH, variable expansion, errorlevel tracking,
 * `setlocal`/`endlocal` environment scoping, and every batch language
 * command. All transcript output and debug logging go through shell.c; all SD
 * access goes through storage.c; interactive keypress waits go through the
 * shell core's key queue.
 *
 * State owned here:
 *   - Environment variables and PATH (RAM-only, 24 slots)
 *   - The setlocal snapshot stack
 *   - Active batch frame stack, errorlevel, pending goto target, stop mode
 */

#include "batch.h"
#include "storage.h"
#include "shell.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

/* Backward-compatibility aliases */
#define BATCH_TAG                       P4_CONFIG_SHELL_TAG
#define SHELL_PROMPT                    P4_CONFIG_SHELL_PROMPT
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_ENV_VAR_MAX               P4_CONFIG_ENV_VAR_MAX
#define SHELL_ENV_NAME_BYTES            P4_CONFIG_ENV_NAME_BYTES
#define SHELL_ENV_VALUE_BYTES           P4_CONFIG_ENV_VALUE_BYTES
#define SHELL_ALIAS_MAX                 P4_CONFIG_ALIAS_MAX
#define SHELL_ALIAS_NAME_BYTES          P4_CONFIG_ALIAS_NAME_BYTES
#define SHELL_ALIAS_VALUE_BYTES         P4_CONFIG_ALIAS_VALUE_BYTES
#define SHELL_ALIAS_PROFILE             P4_CONFIG_ALIAS_PROFILE
#define SHELL_BATCH_LINE_BYTES          P4_CONFIG_BATCH_LINE_BYTES
#define SHELL_BATCH_ARGS_MAX            P4_CONFIG_BATCH_ARGS_MAX
#define SHELL_BATCH_DEPTH_MAX           P4_CONFIG_BATCH_DEPTH_MAX
#define SHELL_BATCH_LABEL_MAX           P4_CONFIG_BATCH_LABEL_MAX
#define SHELL_BATCH_LABEL_BYTES         P4_CONFIG_BATCH_LABEL_BYTES
#define SHELL_PAUSE_DELAY_MS            P4_CONFIG_PAUSE_DELAY_MS
#define SHELL_PIPE_SETTLE_DELAY_MS      P4_CONFIG_PIPE_SETTLE_DELAY_MS
#define SHELL_PIPE_STAGE_MAX            P4_CONFIG_PIPE_STAGE_MAX
#define SHELL_SETLOCAL_DEPTH_MAX        P4_CONFIG_SETLOCAL_DEPTH_MAX
#define SHELL_KEY_WAIT_TIMEOUT_MS       P4_CONFIG_KEY_WAIT_TIMEOUT_MS

/* ========================================================================
 * TYPES
 * ======================================================================== */

/** One RAM-only environment variable slot. */
typedef struct {
    bool used;
    char name[SHELL_ENV_NAME_BYTES];
    char value[SHELL_ENV_VALUE_BYTES];
} shell_env_var_t;

/**
 * A `:label` position recorded while scanning a batch file.
 *
 * The name is bounded by P4_CONFIG_BATCH_LABEL_BYTES rather than the full
 * command width: a label is a single identifier, and sizing it at 256 bytes
 * made the label table alone 8 KB, which overflowed the command worker task
 * stack when the frame was placed on it.
 */
typedef struct {
    char name[SHELL_BATCH_LABEL_BYTES];
    long file_pos;
} shell_batch_label_t;

/** Execution context for one nested batch file. */
typedef struct shell_batch_frame {
    bool echo_enabled;
    int argc;
    int depth;
    char args[SHELL_BATCH_ARGS_MAX][SHELL_COMMAND_BYTES];
    struct shell_batch_frame *parent;
    shell_batch_label_t labels[SHELL_BATCH_LABEL_MAX];
    int label_count;
    FILE *batch_file;
    /** setlocal scopes opened by this frame, unwound when it returns. */
    int setlocal_depth;
} shell_batch_frame_t;

/**
 * How a batch frame was asked to stop.
 *
 * `exit /b` unwinds exactly one frame; a bare `exit` inside nested batch
 * files unwinds all of them, matching COMMAND.COM.
 */
typedef enum {
    BATCH_STOP_NONE = 0,   /**< Keep executing. */
    BATCH_STOP_FRAME,      /**< Leave the current batch file only. */
    BATCH_STOP_ALL,        /**< Leave every nested batch file. */
} batch_stop_mode_t;

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/* Command pipeline hooks registered by command_init() */
static batch_command_ops_t s_command_ops;

/* Environment variables (RAM-only, not persisted across boots) */
static shell_env_var_t s_shell_env_vars[SHELL_ENV_VAR_MAX];

/* ---- Aliases (DOSKEY-style macros, RAM-only until /save) ---- */

/** One RAM-only alias slot. */
typedef struct {
    bool used;
    char name[SHELL_ALIAS_NAME_BYTES];
    char value[SHELL_ALIAS_VALUE_BYTES];
} shell_alias_t;

static shell_alias_t s_aliases[SHELL_ALIAS_MAX];

/* Batch execution state */
static shell_batch_frame_t *s_active_batch_frame;
static int s_errorlevel;
static char s_goto_label[SHELL_COMMAND_BYTES];
static bool s_goto_pending;
static bool s_goto_eof;            /* goto :eof — jump to end of current frame */
static batch_stop_mode_t s_stop_mode;
static bool s_default_echo = true;   /* CONFIG.SYS ECHO ON|OFF sets this; batch frames inherit it */

/**
 * setlocal environment scope stack.
 *
 * Each entry is a full snapshot of the variable table taken when `setlocal`
 * ran. `endlocal` restores the top snapshot, and any scope a batch file
 * leaves open is unwound automatically when that frame returns — exactly the
 * COMMAND.COM contract. Snapshots are heap-allocated because the table is
 * large enough that a static stack would waste RAM on a board that mostly
 * never uses setlocal.
 */
static shell_env_var_t *s_setlocal_stack[SHELL_SETLOCAL_DEPTH_MAX];
static int s_setlocal_depth;

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static void shell_env_print_all(void);
static bool shell_setlocal_push(void);
static bool shell_setlocal_pop(void);
static void shell_setlocal_unwind_to(int depth);
static bool shell_is_label(const char *line);
static void shell_extract_label_name(const char *line, char *name, size_t name_size);
static long shell_find_label_pos(shell_batch_frame_t *frame, const char *label_name);
static void shell_scan_batch_labels(shell_batch_frame_t *frame, FILE *file);
static void shell_execute_for_loop(shell_batch_frame_t *frame, char *command_line);
static void batch_run_nested(char *command);

/* ========================================================================
 * COMMAND MODULE HOOKS
 * ======================================================================== */

void batch_register_command_ops(const batch_command_ops_t *ops)
{
    if (ops == NULL) {
        memset(&s_command_ops, 0, sizeof(s_command_ops));
        return;
    }

    s_command_ops = *ops;
}

/**
 * Run one nested command line through the full command pipeline.
 *
 * Every nested context (`if` bodies, `for` bodies, pipe stages, batch lines)
 * goes through here so it inherits variable expansion and redirection. The
 * hook is NULL-checked so the batch engine degrades gracefully if it is used
 * before command_init() registers the table.
 */
static void batch_run_nested(char *command)
{
    if (command == NULL) {
        return;
    }

    if (s_command_ops.execute_command == NULL) {
        shell_transcript_append_text("batch: command pipeline is not available yet\n");
        shell_record_warningf("batch", "Nested execution attempted before command_init()");
        return;
    }

    s_command_ops.execute_command(command);
}

/* ========================================================================
 * ENVIRONMENT VARIABLES AND PATH
 * ======================================================================== */

static void shell_env_normalize_name(const char *name, char *output, size_t output_size)
{
    size_t index = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    if (name == NULL) {
        return;
    }

    while (name[index] != '\0' && index + 1 < output_size) {
        output[index] = (char)toupper((unsigned char)name[index]);
        index++;
    }
    output[index] = '\0';
}

static bool shell_env_name_is_valid(const char *name)
{
    size_t index;

    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (index = 0; name[index] != '\0'; index++) {
        if (!(isalnum((unsigned char)name[index]) || name[index] == '_')) {
            return false;
        }
    }

    return true;
}

static shell_env_var_t *shell_env_find_slot(const char *name)
{
    char normalized[SHELL_ENV_NAME_BYTES];
    size_t index;

    shell_env_normalize_name(name, normalized, sizeof(normalized));
    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (s_shell_env_vars[index].used && strcmp(s_shell_env_vars[index].name, normalized) == 0) {
            return &s_shell_env_vars[index];
        }
    }

    return NULL;
}

static shell_env_var_t *shell_env_find_free_slot(void)
{
    size_t index;

    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (!s_shell_env_vars[index].used) {
            return &s_shell_env_vars[index];
        }
    }

    return NULL;
}

const char *shell_env_get(const char *name)
{
    shell_env_var_t *slot = shell_env_find_slot(name);

    return slot != NULL ? slot->value : NULL;
}

esp_err_t shell_env_set(const char *name, const char *value)
{
    char normalized[SHELL_ENV_NAME_BYTES];
    shell_env_var_t *slot;

    shell_env_normalize_name(name, normalized, sizeof(normalized));
    if (!shell_env_name_is_valid(normalized)) {
        return ESP_ERR_INVALID_ARG;
    }

    slot = shell_env_find_slot(normalized);
    if (value == NULL || value[0] == '\0') {
        if (slot != NULL) {
            memset(slot, 0, sizeof(*slot));
        }
        return ESP_OK;
    }

    if (slot == NULL) {
        slot = shell_env_find_free_slot();
        if (slot == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    slot->used = true;
    snprintf(slot->name, sizeof(slot->name), "%s", normalized);
    snprintf(slot->value, sizeof(slot->value), "%s", value);
    return ESP_OK;
}

static void shell_env_print_all(void)
{
    size_t index;
    bool any = false;

    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (!s_shell_env_vars[index].used) {
            continue;
        }
        shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n", s_shell_env_vars[index].name, s_shell_env_vars[index].value);
        any = true;
    }

    if (!any) {
        shell_print_muted("No environment variables defined");
    }
}

/* ========================================================================
 * ALIASES (alias / unalias, DOSKEY-style macros)
 * ========================================================================
 * A small RAM-only macro table. When a command is typed at the prompt, the
 * leading word is expanded to the alias value before parsing (DOSKEY
 * behaviour), so `alias ll=dir /s` makes `ll` run `dir /s`. Aliases are NOT
 * expanded inside batch files, so an alias can never shadow a batch verb.
 *
 * Persistence: `alias /save` writes `alias name="value"` lines to the SD
 * profile (P4_CONFIG_ALIAS_PROFILE); boot.c auto-runs that batch file after
 * CONFIG.SYS, and `alias /load` reloads it manually.
 */

static void shell_alias_normalize_name(const char *name, char *output, size_t output_size)
{
    size_t index = 0;

    if (output == NULL || output_size == 0) {
        return;
    }
    output[0] = '\0';
    if (name == NULL) {
        return;
    }
    while (name[index] != '\0' && index + 1 < output_size) {
        output[index] = (char)toupper((unsigned char)name[index]);
        index++;
    }
    output[index] = '\0';
}

static bool shell_alias_name_is_valid(const char *name)
{
    size_t index;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (index = 0; name[index] != '\0'; index++) {
        if (!(isalnum((unsigned char)name[index]) || name[index] == '_')) {
            return false;
        }
    }
    return true;
}

static shell_alias_t *shell_alias_find_slot(const char *name)
{
    char normalized[SHELL_ALIAS_NAME_BYTES];
    size_t index;

    shell_alias_normalize_name(name, normalized, sizeof(normalized));
    for (index = 0; index < SHELL_ALIAS_MAX; index++) {
        if (s_aliases[index].used && strcmp(s_aliases[index].name, normalized) == 0) {
            return &s_aliases[index];
        }
    }
    return NULL;
}

const char *shell_alias_get(const char *name)
{
    shell_alias_t *slot = shell_alias_find_slot(name);

    return slot != NULL ? slot->value : NULL;
}

esp_err_t shell_alias_set(const char *name, const char *value)
{
    char normalized[SHELL_ALIAS_NAME_BYTES];
    shell_alias_t *slot;

    shell_alias_normalize_name(name, normalized, sizeof(normalized));
    if (!shell_alias_name_is_valid(normalized)) {
        return ESP_ERR_INVALID_ARG;
    }

    slot = shell_alias_find_slot(normalized);
    if (value == NULL || value[0] == '\0') {
        if (slot != NULL) {
            memset(slot, 0, sizeof(*slot));
        }
        return ESP_OK;
    }

    if (slot == NULL) {
        size_t index;

        for (index = 0; index < SHELL_ALIAS_MAX; index++) {
            if (!s_aliases[index].used) {
                slot = &s_aliases[index];
                break;
            }
        }
        if (slot == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    slot->used = true;
    snprintf(slot->name, sizeof(slot->name), "%s", normalized);
    snprintf(slot->value, sizeof(slot->value), "%s", value);
    return ESP_OK;
}

int shell_alias_count(void)
{
    size_t index;
    int count = 0;

    for (index = 0; index < SHELL_ALIAS_MAX; index++) {
        if (s_aliases[index].used) {
            count++;
        }
    }
    return count;
}

bool shell_alias_get_by_index(int index, char *name_out, size_t name_size,
                              char *value_out, size_t value_size)
{
    size_t cursor = 0;
    int seen = 0;

    if (name_out == NULL || value_out == NULL) {
        return false;
    }
    for (cursor = 0; cursor < SHELL_ALIAS_MAX; cursor++) {
        if (!s_aliases[cursor].used) {
            continue;
        }
        if (seen == index) {
            snprintf(name_out, name_size, "%s", s_aliases[cursor].name);
            snprintf(value_out, value_size, "%s", s_aliases[cursor].value);
            return true;
        }
        seen++;
    }
    return false;
}

bool batch_alias_expand_command(const char *command, char *out, size_t out_size)
{
    char first_word[SHELL_ALIAS_NAME_BYTES];
    const char *cursor;
    const char *rest;
    size_t first_len = 0;
    size_t value_len;
    size_t rest_len;
    const char *value;
    size_t pos = 0;

    if (command == NULL || out == NULL || out_size == 0) {
        return false;
    }
    /* DOSKEY macros expand only at the interactive prompt, never inside a
     * batch file, so an alias cannot shadow a batch verb. */
    if (s_active_batch_frame != NULL) {
        return false;
    }

    cursor = command;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
           first_len + 1 < sizeof(first_word)) {
        first_word[first_len++] = *cursor;
        cursor++;
    }
    first_word[first_len] = '\0';
    if (first_len == 0) {
        return false;
    }

    value = shell_alias_get(first_word);
    if (value == NULL) {
        return false;
    }

    rest = cursor;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    value_len = strlen(value);
    rest_len = strlen(rest);

    if (value_len + (rest_len > 0 ? rest_len + 1 : 0) + 1 > out_size) {
        return false;
    }

    memcpy(out, value, value_len);
    pos = value_len;
    if (rest_len > 0) {
        out[pos++] = ' ';
        memcpy(out + pos, rest, rest_len);
        pos += rest_len;
    }
    out[pos] = '\0';
    return true;
}

/** Write the alias table to the given resolved SD path as `alias` lines. */
static esp_err_t shell_alias_save_to(const char *resolved_path)
{
    shell_sd_session_t session;
    char tmp[SHELL_SD_PATH_BYTES + 8];
    FILE *file = NULL;
    int index;

    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", resolved_path);

    {
        uint64_t needed = (uint64_t)SHELL_ALIAS_MAX *
                          (SHELL_ALIAS_NAME_BYTES + SHELL_ALIAS_VALUE_BYTES + 16) + 256;
        uint64_t reclaim = storage_get_file_size(resolved_path);
        if (!storage_check_free_space(needed, reclaim, "alias save")) {
            shell_sd_end(&session, "alias");
            return ESP_ERR_INVALID_STATE;
        }
    }

    file = fopen(tmp, "w");
    if (file == NULL) {
        shell_sd_end(&session, "alias");
        return ESP_ERR_NOT_FOUND;
    }

    for (index = 0; index < SHELL_ALIAS_MAX; index++) {
        if (!s_aliases[index].used) {
            continue;
        }
        /* Values containing a double quote cannot round-trip through the
         * batch quoting used by the profile; skip them rather than write a
         * line that would load wrong. */
        if (strchr(s_aliases[index].value, '"') != NULL) {
            shell_print_warning("alias: skipped %s (value contains a double quote)",
                                s_aliases[index].name);
            continue;
        }
        fprintf(file, "alias %s=\"%s\"\n", s_aliases[index].name, s_aliases[index].value);
    }

    if (fflush(file) != 0 || fclose(file) != 0) {
        remove(tmp);
        shell_sd_end(&session, "alias");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* FATFS f_rename refuses to overwrite an existing target: remove it first. */
    if (rename(tmp, resolved_path) != 0) {
        remove(resolved_path);
        if (rename(tmp, resolved_path) != 0) {
            remove(tmp);
            shell_sd_end(&session, "alias");
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    shell_sd_end(&session, "alias");
    return ESP_OK;
}

/** Resolve an alias-profile path (argument or the configured default). */
static esp_err_t shell_alias_resolve_profile(const char *arg, char *out, size_t out_size)
{
    if (arg != NULL && arg[0] != '\0') {
        return shell_fs_resolve_path(arg, out, out_size);
    }
    return shell_fs_resolve_path(SHELL_ALIAS_PROFILE, out, out_size);
}

void shell_command_alias(int argc, char **argv)
{
    int index;

    if (argc == 1) {
        int slot;
        bool any = false;

        if (shell_alias_count() == 0) {
            shell_print_muted("No aliases defined");
            return;
        }
        for (slot = 0; slot < SHELL_ALIAS_MAX; slot++) {
            if (!s_aliases[slot].used) {
                continue;
            }
            shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                          s_aliases[slot].name, s_aliases[slot].value);
            any = true;
        }
        if (!any) {
            shell_print_muted("No aliases defined");
        }
        return;
    }

    /* Flags. */
    if (argv[1][0] == '/') {
        if (shell_text_equals_ignore_case(argv[1], "/save")) {
            char resolved[SHELL_SD_PATH_BYTES];
            const char *file_arg = (argc >= 3) ? argv[2] : NULL;
            esp_err_t error;

            if (argc > 3) {
                shell_print_usage("Usage: alias /save [file]");
                return;
            }
            if (shell_alias_resolve_profile(file_arg, resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("alias: invalid profile path");
                return;
            }
            error = shell_alias_save_to(resolved);
            if (error != ESP_OK) {
                shell_print_error("alias: could not save the profile (%s)", esp_err_to_name(error));
                return;
            }
            shell_print_ok("alias: saved %d alias(es) to %s", shell_alias_count(), resolved);
            return;
        }

        if (shell_text_equals_ignore_case(argv[1], "/load")) {
            char resolved[SHELL_SD_PATH_BYTES];
            const char *file_arg = (argc >= 3) ? argv[2] : NULL;
            esp_err_t error;

            if (argc > 3) {
                shell_print_usage("Usage: alias /load [file]");
                return;
            }
            if (shell_alias_resolve_profile(file_arg, resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("alias: invalid profile path");
                return;
            }
            /* The profile is a batch file of `alias` lines; run it through the
             * batch pipeline. Alias expansion is suppressed inside it, so the
             * `alias` commands define the table. */
            error = shell_execute_batch_file(resolved, 0, NULL);
            if (error != ESP_OK) {
                shell_print_error("alias: could not load the profile (%s)", esp_err_to_name(error));
                return;
            }
            shell_print_ok("alias: loaded %d alias(es) from %s", shell_alias_count(), resolved);
            return;
        }

        if (shell_text_equals_ignore_case(argv[1], "/clear")) {
            if (argc != 2) {
                shell_print_usage("Usage: alias /clear");
                return;
            }
            for (index = 0; index < SHELL_ALIAS_MAX; index++) {
                memset(&s_aliases[index], 0, sizeof(s_aliases[index]));
            }
            shell_print_ok("alias: all aliases cleared");
            return;
        }

        shell_print_error("alias: unknown option %s", argv[1]);
        shell_print_usage("Usage: alias [name[=value]] | alias /save [/load] [file] | alias /clear");
        return;
    }

    /* `alias name` or `alias name=value`. The value may be quoted with spaces. */
    {
        char *equals = strchr(argv[1], '=');

        if (equals == NULL) {
            const char *value = shell_alias_get(argv[1]);
            if (value == NULL) {
                shell_print_error("alias: %s is not defined", argv[1]);
                return;
            }
            shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                          argv[1], value);
            return;
        }

        *equals = '\0';
        if (shell_alias_set(argv[1], equals + 1) != ESP_OK) {
            shell_print_error("alias: invalid alias name or the table is full");
            return;
        }
        shell_print_ok("alias: %s set", argv[1]);
    }
}

void shell_command_unalias(int argc, char **argv)
{
    if (argc != 2) {
        shell_print_usage("Usage: unalias <name>");
        return;
    }
    (void)shell_alias_set(argv[1], "");
    shell_print_ok("unalias: %s removed", argv[1]);
}

/* ========================================================================
 * SETLOCAL / ENDLOCAL ENVIRONMENT SCOPING
 * ========================================================================
 * `setlocal` snapshots the whole variable table; `endlocal` restores it.
 * Snapshotting the table wholesale is the honest implementation here: the
 * table is a fixed-size array, so a copy is a single memcpy and restoring it
 * correctly reverts creations, modifications, and deletions in one step.
 */

/**
 * Push a snapshot of the environment onto the setlocal stack.
 * @return true on success, false when the stack is full or out of memory.
 */
static bool shell_setlocal_push(void)
{
    shell_env_var_t *snapshot;

    if (s_setlocal_depth >= SHELL_SETLOCAL_DEPTH_MAX) {
        return false;
    }

    snapshot = malloc(sizeof(s_shell_env_vars));
    if (snapshot == NULL) {
        return false;
    }

    memcpy(snapshot, s_shell_env_vars, sizeof(s_shell_env_vars));
    s_setlocal_stack[s_setlocal_depth++] = snapshot;
    return true;
}

/**
 * Restore and release the most recent setlocal snapshot.
 * @return true when a scope was popped, false when none was open.
 */
static bool shell_setlocal_pop(void)
{
    shell_env_var_t *snapshot;

    if (s_setlocal_depth <= 0) {
        return false;
    }

    snapshot = s_setlocal_stack[--s_setlocal_depth];
    s_setlocal_stack[s_setlocal_depth] = NULL;

    if (snapshot == NULL) {
        return false;
    }

    memcpy(s_shell_env_vars, snapshot, sizeof(s_shell_env_vars));
    free(snapshot);
    return true;
}

/**
 * Unwind the setlocal stack down to @p depth.
 *
 * Called when a batch frame returns so a file that opened a scope and never
 * called `endlocal` cannot leak its changes into the caller, and so the
 * snapshot allocations are always released.
 */
static void shell_setlocal_unwind_to(int depth)
{
    if (depth < 0) {
        depth = 0;
    }

    while (s_setlocal_depth > depth) {
        (void)shell_setlocal_pop();
    }
}

/**
 * Build the `%*` expansion: every frame argument from `%1` onward, joined
 * with spaces. `%0` is the script name and is excluded, matching DOS.
 *
 * The scratch buffer is static because it is only ever consumed by the
 * caller of this helper before the next call can overwrite it, and the
 * command path is single-threaded. Sized by the existing batch line limit.
 */
static const char *shell_batch_all_args_string(void)
{
    static char all_args[SHELL_BATCH_LINE_BYTES];
    int index;

    all_args[0] = '\0';

    if (s_active_batch_frame == NULL || s_active_batch_frame->argc <= 1) {
        return all_args;
    }

    for (index = 1; index < s_active_batch_frame->argc; index++) {
        if (index > 1) {
            strncat(all_args, " ", sizeof(all_args) - strlen(all_args) - 1);
        }
        strncat(all_args, s_active_batch_frame->args[index],
                sizeof(all_args) - strlen(all_args) - 1);
    }

    return all_args;
}

/**
 * Expand `%VAR%` and batch argument references.
 *
 * Quote and escape aware, following the shell's documented rules:
 *   - Inside `'...'` nothing expands; the run is fully literal.
 *   - Inside `"..."` expansion still applies, matching COMMAND.COM.
 *   - `^%` suppresses expansion for that one percent sign.
 *
 * Batch arguments follow COMMAND.COM: `%0` is the script name (args[0]),
 * `%1`..`%9` are the caller's arguments (args[1]..args[9]), and `%*` is all
 * of them from `%1` onward joined with spaces. With no active batch frame all
 * three expand to empty.
 *
 * Quoting and escape markup is preserved here because the argument tokenizer
 * strips it later. Removing it now would let an expanded value that happens
 * to contain a space or an operator be re-parsed as syntax.
 */
void shell_expand_variables(const char *input, char *output, size_t output_size)
{
    size_t out_index = 0;
    shell_quote_state_t quote = SHELL_QUOTE_NONE;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    if (input == NULL) {
        return;
    }

    while (*input != '\0' && out_index + 1 < output_size) {
        /* A caret escape passes both characters through untouched, so `^%`
         * reaches the tokenizer as a literal percent sign. */
        if (*input == P4_CONFIG_ESCAPE_CHAR && quote != SHELL_QUOTE_SINGLE && input[1] != '\0') {
            output[out_index++] = *input++;
            if (out_index + 1 < output_size) {
                output[out_index++] = *input;
            }
            input++;
            continue;
        }

        /* Track quote runs so a single-quoted section stays literal. */
        if (*input == '"' && quote != SHELL_QUOTE_SINGLE) {
            quote = (quote == SHELL_QUOTE_DOUBLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_DOUBLE;
            output[out_index++] = *input++;
            continue;
        }

        if (*input == '\'' && quote != SHELL_QUOTE_DOUBLE) {
            quote = (quote == SHELL_QUOTE_SINGLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_SINGLE;
            output[out_index++] = *input++;
            continue;
        }

        if (quote == SHELL_QUOTE_SINGLE) {
            /* Fully literal run: copy without interpreting '%'. */
            output[out_index++] = *input++;
            continue;
        }

        if (*input == '%') {
            /* Batch arguments are a `%` plus exactly one character and are
             * recognized immediately wherever they appear, matching
             * COMMAND.COM: `%0` is the script name, `%1`..`%9` the caller's
             * arguments, `%*` all of them from `%1` onward. They never wait
             * for a closing `%`, so `p0=[%0] p1=[%1]` expands both. With no
             * active batch frame they expand to empty. */
            const char *replacement = NULL;
            const char *after = NULL;

            if (input[1] == '0') {
                replacement = (s_active_batch_frame != NULL && s_active_batch_frame->argc > 0)
                                  ? s_active_batch_frame->args[0]
                                  : "";
                after = input + 2;
            } else if (input[1] == '*') {
                replacement = (s_active_batch_frame != NULL) ? shell_batch_all_args_string() : "";
                after = input + 2;
            } else if (isdigit((unsigned char)input[1])) {
                if (s_active_batch_frame != NULL) {
                    int arg_index = input[1] - '0';
                    replacement = (arg_index >= 0 && arg_index < s_active_batch_frame->argc)
                                      ? s_active_batch_frame->args[arg_index]
                                      : "";
                } else {
                    /* No active batch frame: positional args are empty. */
                    replacement = "";
                }
                after = input + 2;
            } else {
                /* Environment variable `%VAR%` or a literal `%%`: needs the
                 * closing `%`. An unknown name is left untouched. */
                const char *end = strchr(input + 1, '%');
                if (end != NULL) {
                    size_t token_len = (size_t)(end - (input + 1));
                    char token[SHELL_ENV_NAME_BYTES];

                    if (token_len == 0) {
                        replacement = "%";
                    } else if (token_len < sizeof(token)) {
                        memcpy(token, input + 1, token_len);
                        token[token_len] = '\0';
                        replacement = shell_env_get(token);
                    }
                    after = end + 1;
                }
                /* No closing `%`: the percent is copied literally below. */
            }

            if (replacement != NULL && after != NULL) {
                while (*replacement != '\0' && out_index + 1 < output_size) {
                    output[out_index++] = *replacement++;
                }
                input = after;
                continue;
            }
        }

        output[out_index++] = *input++;
    }

    output[out_index] = '\0';
}

/* ========================================================================
 * ERRORLEVEL
 * ======================================================================== */

int batch_get_errorlevel(void)
{
    return s_errorlevel;
}

void batch_set_errorlevel(int level)
{
    s_errorlevel = level;
}

/* ========================================================================
 * ENVIRONMENT COMMANDS: set, path, echo
 * ======================================================================== */

/* ========================================================================
 * ARITHMETIC EXPRESSIONS (set /a)
 * ========================================================================
 * A recursive-descent evaluator over 32-bit signed integers, following the
 * COMMAND.COM operator set and precedence:
 *
 *   |        bitwise or            (lowest)
 *   ^        bitwise xor
 *   &        bitwise and
 *   << >>    shifts
 *   + -      additive
 *   * / %    multiplicative
 *   - ~ !    unary
 *   ( )      grouping              (highest)
 *
 * A bare identifier reads an environment variable, and an undefined variable
 * evaluates to 0 exactly as DOS does. Numbers accept decimal, `0x` hex, and
 * leading-zero octal.
 */

/** Parser state threaded through the expression grammar. */
typedef struct {
    const char *cursor;   /**< Current read position. */
    int depth;            /**< Parenthesis nesting depth. */
    bool failed;          /**< Set on the first syntax or maths error. */
    const char *message;  /**< Human-readable reason for the failure. */
} expr_parser_t;

static int32_t shell_expr_parse_or(expr_parser_t *parser);
static int32_t shell_expr_parse_compare(expr_parser_t *parser);
static int32_t shell_expr_parse_logical_and(expr_parser_t *parser);
static int32_t shell_expr_parse_logical(expr_parser_t *parser);

/** Record the first error and stop evaluating. */
static void shell_expr_fail(expr_parser_t *parser, const char *message)
{
    if (!parser->failed) {
        parser->failed = true;
        parser->message = message;
    }
}

static void shell_expr_skip_space(expr_parser_t *parser)
{
    while (isspace((unsigned char)*parser->cursor)) {
        parser->cursor++;
    }
}

/** Consume @p text when it is next, after whitespace. */
static bool shell_expr_accept(expr_parser_t *parser, const char *text)
{
    size_t length = strlen(text);

    shell_expr_skip_space(parser);
    if (strncmp(parser->cursor, text, length) != 0) {
        return false;
    }

    parser->cursor += length;
    return true;
}

/**
 * Parse a number, a variable reference, or a parenthesised subexpression.
 */
static int32_t shell_expr_parse_primary(expr_parser_t *parser)
{
    shell_expr_skip_space(parser);

    if (parser->failed) {
        return 0;
    }

    if (*parser->cursor == '(') {
        int32_t value;

        if (parser->depth >= P4_CONFIG_SET_EXPR_DEPTH_MAX) {
            shell_expr_fail(parser, "expression nests too deeply");
            return 0;
        }

        parser->cursor++;
        parser->depth++;
        value = shell_expr_parse_logical(parser);
        parser->depth--;

        if (!shell_expr_accept(parser, ")")) {
            shell_expr_fail(parser, "missing ')'");
        }
        return value;
    }

    /* Numeric literal: strtol handles decimal, 0x hex, and octal via base 0.
     * Out-of-range literals (e.g. 2147483648) are rejected rather than
     * silently clamped, so `-2147483648/-1` is reported as an error instead
     * of evaluating to a wrapped value. */
    if (isdigit((unsigned char)*parser->cursor)) {
        char *end = NULL;
        long value;

        errno = 0;
        value = strtol(parser->cursor, &end, 0);

        if (end == parser->cursor || errno == ERANGE) {
            shell_expr_fail(parser, "malformed number");
            return 0;
        }
        parser->cursor = end;
        return (int32_t)value;
    }

    /* Variable reference. An undefined name is 0, matching DOS. */
    if (isalpha((unsigned char)*parser->cursor) || *parser->cursor == '_') {
        char name[SHELL_ENV_NAME_BYTES];
        size_t length = 0;
        const char *value;

        while ((isalnum((unsigned char)*parser->cursor) || *parser->cursor == '_') &&
               length + 1 < sizeof(name)) {
            name[length++] = *parser->cursor++;
        }
        name[length] = '\0';

        value = shell_env_get(name);
        if (value == NULL) {
            return 0;
        }

        return (int32_t)strtol(value, NULL, 0);
    }

    shell_expr_fail(parser, "unexpected character");
    return 0;
}

/** Unary minus, plus, bitwise not, and logical not. */
static int32_t shell_expr_parse_unary(expr_parser_t *parser)
{
    shell_expr_skip_space(parser);

    if (parser->failed) {
        return 0;
    }

    if (*parser->cursor == '-') {
        parser->cursor++;
        return -shell_expr_parse_unary(parser);
    }

    if (*parser->cursor == '+') {
        parser->cursor++;
        return shell_expr_parse_unary(parser);
    }

    if (*parser->cursor == '~') {
        parser->cursor++;
        return ~shell_expr_parse_unary(parser);
    }

    if (*parser->cursor == '!') {
        parser->cursor++;
        return (shell_expr_parse_unary(parser) == 0) ? 1 : 0;
    }

    return shell_expr_parse_primary(parser);
}

static int32_t shell_expr_parse_multiplicative(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_unary(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (*parser->cursor == '*') {
            parser->cursor++;
            left = left * shell_expr_parse_unary(parser);
        } else if (*parser->cursor == '/') {
            int32_t right;

            parser->cursor++;
            right = shell_expr_parse_unary(parser);
            if (right == 0) {
                shell_expr_fail(parser, "divide by zero");
                return 0;
            }
            /* INT32_MIN / -1 overflows on two's-complement hardware. */
            if (left == INT32_MIN && right == -1) {
                shell_expr_fail(parser, "arithmetic overflow");
                return 0;
            }
            left = left / right;
        } else if (*parser->cursor == '%') {
            int32_t right;

            parser->cursor++;
            right = shell_expr_parse_unary(parser);
            if (right == 0) {
                shell_expr_fail(parser, "divide by zero");
                return 0;
            }
            if (left == INT32_MIN && right == -1) {
                shell_expr_fail(parser, "arithmetic overflow");
                return 0;
            }
            left = left % right;
        } else {
            break;
        }
    }

    return left;
}

static int32_t shell_expr_parse_additive(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_multiplicative(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (*parser->cursor == '+') {
            parser->cursor++;
            left = left + shell_expr_parse_multiplicative(parser);
        } else if (*parser->cursor == '-') {
            parser->cursor++;
            left = left - shell_expr_parse_multiplicative(parser);
        } else {
            break;
        }
    }

    return left;
}

static int32_t shell_expr_parse_shift(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_additive(parser);

    while (!parser->failed) {
        int32_t right;

        if (shell_expr_accept(parser, "<<")) {
            right = shell_expr_parse_additive(parser);
            /* A shift of 32 or more is undefined in C; clamp to 0. */
            left = (right >= 0 && right < 32) ? (int32_t)((uint32_t)left << right) : 0;
        } else if (shell_expr_accept(parser, ">>")) {
            right = shell_expr_parse_additive(parser);
            left = (right >= 0 && right < 32) ? (left >> right) : (left < 0 ? -1 : 0);
        } else {
            break;
        }
    }

    return left;
}

static int32_t shell_expr_parse_and(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_shift(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        /* A single '&' is bitwise and; '&&' is a command separator and must
         * never be consumed here. */
        if (*parser->cursor != '&' || parser->cursor[1] == '&') {
            break;
        }

        parser->cursor++;
        left = left & shell_expr_parse_shift(parser);
    }

    return left;
}

static int32_t shell_expr_parse_xor(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_and(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (*parser->cursor != '^') {
            break;
        }

        parser->cursor++;
        left = left ^ shell_expr_parse_and(parser);
    }

    return left;
}

static int32_t shell_expr_parse_or(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_xor(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        /* '||' is a command separator, so only a lone '|' is bitwise or. */
        if (*parser->cursor != '|' || parser->cursor[1] == '|') {
            break;
        }

        parser->cursor++;
        left = left | shell_expr_parse_xor(parser);
    }

    return left;
}

/**
 * Relational / equality operators: `==`, `!=`, `<`, `>`, `<=`, `>=`.
 *
 * Lower precedence than the bitwise operators, matching cmd.exe: each operand
 * is a full `|`/`^`/`&`/shift expression and the comparison yields 1 when the
 * relation holds, 0 otherwise. A single `<`/`>` is never confused with the
 * shift operators `<<`/`>>`, which a tighter precedence level consumes first.
 */
static int32_t shell_expr_parse_compare(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_or(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (shell_expr_accept(parser, "==")) {
            left = (left == shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, "!=")) {
            left = (left != shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, "<=")) {
            left = (left <= shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, ">=")) {
            left = (left >= shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, "<")) {
            left = (left < shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, ">")) {
            left = (left > shell_expr_parse_or(parser)) ? 1 : 0;
        } else {
            break;
        }
    }

    return left;
}

/**
 * Logical `&&` — binds tighter than `||`, matching cmd.exe.
 *
 * Returns 1/0. Both sides are always evaluated (no short-circuiting), so
 * `0 && 1/0` still reports a divide-by-zero error. The right operand is
 * parsed into a local before combining: a C `&&` would short-circuit and skip
 * consuming it, leaving a dangling tail that fails the expression.
 */
static int32_t shell_expr_parse_logical_and(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_compare(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (shell_expr_accept(parser, "&&")) {
            int32_t right = shell_expr_parse_compare(parser);
            left = ((left != 0) && (right != 0)) ? 1 : 0;
        } else {
            break;
        }
    }

    return left;
}

/**
 * Logical `||` — the lowest-precedence operator in the grammar.
 *
 * Returns 1/0, evaluating both sides (no short-circuiting), matching
 * cmd.exe. From the shell an expression using `&&`/`||` must be quoted: an
 * unquoted `&&`/`||` is a command-chain separator and is split before the
 * command ever runs.
 */
static int32_t shell_expr_parse_logical(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_logical_and(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (shell_expr_accept(parser, "||")) {
            int32_t right = shell_expr_parse_logical_and(parser);
            left = ((left != 0) || (right != 0)) ? 1 : 0;
        } else {
            break;
        }
    }

    return left;
}

bool shell_expr_evaluate(const char *expression, int32_t *result_out, const char **error_out)
{
    expr_parser_t parser;
    int32_t value;

    if (error_out != NULL) {
        *error_out = NULL;
    }
    if (result_out != NULL) {
        *result_out = 0;
    }

    if (expression == NULL) {
        if (error_out != NULL) {
            *error_out = "no expression";
        }
        return false;
    }

    parser.cursor = expression;
    parser.depth = 0;
    parser.failed = false;
    parser.message = NULL;

    value = shell_expr_parse_logical(&parser);

    if (!parser.failed) {
        shell_expr_skip_space(&parser);
        if (*parser.cursor != '\0') {
            shell_expr_fail(&parser, "trailing characters after the expression");
        }
    }

    if (parser.failed) {
        if (error_out != NULL) {
            *error_out = parser.message;
        }
        return false;
    }

    if (result_out != NULL) {
        *result_out = value;
    }

    return true;
}

/**
 * Locate the assignment `=` in a `set /a` statement.
 *
 * The statement is `NAME[OP]=<expression>`; the expression may itself contain
 * comparison operators (`==`, `!=`, `<=`, `>=`), so a plain strchr would split
 * on the first `=` of a comparison. This scans left to right and treats an `=`
 * as the assignment only when it is not part of a comparison operator:
 *   - `==`, `!=`, `<=`, `>=` are skipped (their `=` is comparison syntax);
 *   - `<<=` / `>>=` are compound shift assignments (their `=` IS the one);
 *   - anything else is the assignment (`=`, or compound `+= -= *= /= %= &= |= ^=`).
 *
 * @return Pointer to the assignment `=`, or NULL when the statement is a pure
 *         expression with no assignment (which `set /a` then evaluates and
 *         prints rather than storing).
 */
static char *shell_expr_find_assignment(char *statement)
{
    char *p;

    for (p = statement; *p != '\0'; p++) {
        if (*p != '=') {
            continue;
        }

        {
            char prev = (p > statement) ? p[-1] : '\0';
            char next = p[1];

            if (prev == '=' || prev == '!') {
                continue;   /* second of `==`, or the `=` of `!=` */
            }

            if (prev == '<' || prev == '>') {
                if (p > statement + 1 && p[-2] == prev) {
                    return p;   /* `<<=` or `>>=` : compound shift assignment */
                }
                continue;   /* `<=` or `>=` : comparison, not the assignment */
            }

            if (next == '=') {
                p++;        /* first of `==` : skip the pair */
                continue;
            }

            return p;       /* plain `=` or compound `+= -= *= /= %= &= |= ^=` */
        }
    }

    return NULL;
}

/**
 * `set /a` — evaluate an arithmetic expression and store the result.
 *
 * Usage: set /a NAME=<expression>
 *        set /a <expression>          (prints the result without storing)
 *
 * Compound assignment operators (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`,
 * `^=`, `<<=`, `>>=`) are supported, as in DOS. Expressions may also use the
 * comparison operators `==`, `!=`, `<`, `>`, `<=`, `>=` (1 when true, else 0)
 * and the logical operators `&&`, `||` (1/0), following cmd.exe precedence.
 */
static void shell_command_set_arithmetic(int argc, char **argv)
{
    char statement[SHELL_ENV_NAME_BYTES + SHELL_ENV_VALUE_BYTES];
    char name[SHELL_ENV_NAME_BYTES];
    char value_text[32];
    const char *error = NULL;
    char *equals;
    char *expression;
    int32_t result = 0;

    if (argc < 3) {
        shell_print_usage("Usage: set /a NAME=<expression>");
        shell_transcript_append_text("  Operators: + - * / % & | ^ ~ ! << >> == != < > <= >= && || ( )\n");
        s_errorlevel = 1;
        return;
    }

    /* Rejoin so an unquoted expression with spaces still parses. */
    shell_join_args(argv, 2, argc, statement, sizeof(statement));

    /* Find the assignment '=' that is not part of a comparison or a
     * compound operator's own character. */
    equals = shell_expr_find_assignment(statement);

    if (equals == NULL) {
        /* No assignment: evaluate and report, leaving the environment alone. */
        if (!shell_expr_evaluate(statement, &result, &error)) {
            shell_transcript_appendf("set: %s\n", error != NULL ? error : "invalid expression");
            s_errorlevel = 1;
            return;
        }

        shell_transcript_appendf_ansi(SH_NUM "%ld" SH_RST "\n", (long)result);
        s_errorlevel = 0;
        return;
    }

    /* Detect a compound assignment: the character before '=' is an operator. */
    {
        char op = '\0';
        char *name_end = equals;
        bool shift_op = false;

        if (name_end > statement) {
            char previous = name_end[-1];

            if (previous == '+' || previous == '-' || previous == '*' || previous == '/' ||
                previous == '%' || previous == '&' || previous == '|' || previous == '^') {
                op = previous;
                name_end--;
            } else if (previous == '<' || previous == '>') {
                /* '<<=' and '>>=' need both characters removed. */
                if (name_end - 1 > statement && name_end[-2] == previous) {
                    op = previous;
                    shift_op = true;
                    name_end -= 2;
                }
            }
        }

        *name_end = '\0';
        expression = equals + 1;

        snprintf(name, sizeof(name), "%s", shell_trim(statement));
        if (name[0] == '\0') {
            shell_transcript_append_text("set: /a needs a variable name before '='\n");
            s_errorlevel = 1;
            return;
        }

        if (!shell_expr_evaluate(expression, &result, &error)) {
            shell_transcript_appendf("set: %s\n", error != NULL ? error : "invalid expression");
            s_errorlevel = 1;
            return;
        }

        /* Apply the compound operator against the variable's current value. */
        if (op != '\0') {
            const char *current_text = shell_env_get(name);
            int32_t current = (current_text != NULL) ? (int32_t)strtol(current_text, NULL, 0) : 0;

            switch (op) {
            case '+': result = current + result; break;
            case '-': result = current - result; break;
            case '*': result = current * result; break;
            case '/':
                if (result == 0) {
                    shell_transcript_append_text("set: divide by zero\n");
                    s_errorlevel = 1;
                    return;
                }
                result = current / result;
                break;
            case '%':
                if (result == 0) {
                    shell_transcript_append_text("set: divide by zero\n");
                    s_errorlevel = 1;
                    return;
                }
                result = current % result;
                break;
            case '&': result = current & result; break;
            case '|': result = current | result; break;
            case '^': result = current ^ result; break;
            case '<':
                result = (shift_op && result >= 0 && result < 32)
                             ? (int32_t)((uint32_t)current << result) : 0;
                break;
            case '>':
                result = (shift_op && result >= 0 && result < 32)
                             ? (current >> result) : (current < 0 ? -1 : 0);
                break;
            default:
                break;
            }
        }
    }

    snprintf(value_text, sizeof(value_text), "%ld", (long)result);
    if (shell_env_set(name, value_text) != ESP_OK) {
        shell_print_error("set: invalid variable name or environment is full");
        s_errorlevel = 1;
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_NUM "%s" SH_RST "\n", name, value_text);
    s_errorlevel = 0;
}

/**
 * `set /p` — prompt the user and store the typed line in a variable.
 *
 * Usage: set /p NAME=<prompt text>
 *
 * Matches DOS: if the user submits an empty line the variable keeps its
 * previous value rather than being cleared.
 */
static void shell_command_set_prompt(int argc, char **argv)
{
    char statement[SHELL_ENV_NAME_BYTES + SHELL_ENV_VALUE_BYTES];
    char name[SHELL_ENV_NAME_BYTES];
    char input[P4_CONFIG_SET_PROMPT_INPUT_BYTES];
    char *equals;
    const char *prompt;

    if (argc < 3) {
        shell_print_usage("Usage: set /p NAME=<prompt text>");
        s_errorlevel = 1;
        return;
    }

    shell_join_args(argv, 2, argc, statement, sizeof(statement));

    equals = strchr(statement, '=');
    if (equals == NULL) {
        shell_transcript_append_text("set: /p needs NAME=<prompt text>\n");
        s_errorlevel = 1;
        return;
    }

    *equals = '\0';
    prompt = equals + 1;

    snprintf(name, sizeof(name), "%s", shell_trim(statement));
    if (name[0] == '\0') {
        shell_transcript_append_text("set: /p needs a variable name before '='\n");
        s_errorlevel = 1;
        return;
    }

    if (prompt[0] != '\0') {
        shell_transcript_appendf("%s", prompt);
    }

    if (!shell_read_line(input, sizeof(input), SHELL_KEY_WAIT_TIMEOUT_MS)) {
        /* Cancelled, timed out, or no key source. DOS leaves the variable
         * untouched in this case, so errorlevel reports the failure without
         * destroying an existing value. */
        shell_transcript_append_text("set: no input received, the variable is unchanged\n");
        s_errorlevel = 1;
        return;
    }

    if (input[0] == '\0') {
        /* An empty line leaves the variable as it was, matching DOS. */
        s_errorlevel = 1;
        return;
    }

    if (shell_env_set(name, input) != ESP_OK) {
        shell_print_error("set: invalid variable name or environment is full");
        s_errorlevel = 1;
        return;
    }

    s_errorlevel = 0;
}

void shell_command_set(int argc, char **argv)
{
    char *assignment = NULL;
    char *equals;

    if (argc == 1) {
        shell_env_print_all();
        return;
    }

    /* /a and /p change the meaning of the rest of the line, so they are
     * dispatched before the plain NAME=VALUE handling. */
    if (argv[1][0] == '/' && argv[1][2] == '\0') {
        char flag = (char)toupper((unsigned char)argv[1][1]);

        if (flag == 'A') {
            shell_command_set_arithmetic(argc, argv);
            return;
        }

        if (flag == 'P') {
            shell_command_set_prompt(argc, argv);
            return;
        }
    }

    /* The joined statement is env-sized and `set` runs on the recursive
     * batch path, so it is heap-allocated and freed on every exit. */
    assignment = malloc(SHELL_ENV_NAME_BYTES + SHELL_ENV_VALUE_BYTES);
    if (assignment == NULL) {
        shell_print_error("set: out of memory");
        return;
    }

    shell_join_args(argv, 1, argc, assignment, SHELL_ENV_NAME_BYTES + SHELL_ENV_VALUE_BYTES);
    equals = strchr(assignment, '=');
    if (equals == NULL) {
        const char *value = shell_env_get(assignment);
        if (value == NULL) {
            shell_print_warning("%s is not defined", assignment);
            free(assignment);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n", assignment, value);
        free(assignment);
        return;
    }

    *equals = '\0';
    equals++;
    if (shell_env_set(assignment, equals) != ESP_OK) {
        shell_print_error("set: invalid variable name or environment is full");
        free(assignment);
        return;
    }

    if (equals[0] == '\0') {
        shell_print_ok("Cleared %s", assignment);
    } else {
        shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n", assignment, equals);
    }
    free(assignment);
}

void shell_command_path(int argc, char **argv)
{
    const char *current;
    char *value = NULL;

    if (argc == 1) {
        current = shell_env_get("PATH");
        shell_transcript_appendf_ansi(SH_LBL "PATH" SH_RST "=" SH_PATH "%s" SH_RST "\n", current != NULL ? current : "");
        return;
    }

    /* The value is env-sized and `path` runs on the recursive batch path. */
    value = malloc(SHELL_ENV_VALUE_BYTES);
    if (value == NULL) {
        shell_print_error("path: out of memory");
        return;
    }

    shell_join_args(argv, 1, argc, value, SHELL_ENV_VALUE_BYTES);
    if (shell_env_set("PATH", value) != ESP_OK) {
        free(value);
        shell_print_error("path: failed to update PATH");
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "PATH" SH_RST "=" SH_PATH "%s" SH_RST "\n", value);
    free(value);
}

void shell_command_echo(int argc, char **argv)
{
    /* The joined text is line-sized and `echo` runs on the recursive batch
     * path (every batch line dispatches through it), so the buffer is
     * heap-allocated and freed on the single exit. */
    char *text = NULL;

    if (argc == 1) {
        shell_transcript_appendf_ansi(SH_LBL "ECHO is" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                 (s_active_batch_frame != NULL && !s_active_batch_frame->echo_enabled) ? "off" : "on");
        return;
    }

    if (s_active_batch_frame != NULL && argc == 2) {
        if (shell_text_equals_ignore_case(argv[1], "on")) {
            s_active_batch_frame->echo_enabled = true;
            return;
        }
        if (shell_text_equals_ignore_case(argv[1], "off")) {
            s_active_batch_frame->echo_enabled = false;
            return;
        }
    }

    text = malloc(SHELL_BATCH_LINE_BYTES);
    if (text == NULL) {
        shell_transcript_append_text("echo: out of memory\n");
        return;
    }

    shell_join_args(argv, 1, argc, text, SHELL_BATCH_LINE_BYTES);
    shell_transcript_appendf("%s\n", text);
    free(text);
}

/* ========================================================================
 * BATCH FILE RESOLUTION
 * ======================================================================== */

bool shell_resolve_batch_path(const char *command_name, char *resolved_path, size_t resolved_path_size)
{
    char *candidate = NULL;
    char *path_copy = NULL;
    char *dir_path = NULL;
    char *with_ext = NULL;
    char *entry;
    char *context = NULL;
    struct stat st;
    shell_sd_session_t session;
    esp_err_t error;
    bool found = false;
    const char *path_env = shell_env_get("PATH");

    if (command_name == NULL || command_name[0] == '\0' || resolved_path == NULL || resolved_path_size == 0) {
        return false;
    }

    /* The path buffers are SD-path sized, and this function runs on the
     * recursive batch path (a nested `call` dispatches through it, so it
     * stacks with the nested dispatch frames). They are heap-allocated and
     * released at the single exit, keeping the stack frame small. */
    candidate = malloc(SHELL_SD_PATH_BYTES);
    path_copy = malloc(SHELL_ENV_VALUE_BYTES);
    dir_path = malloc(SHELL_SD_PATH_BYTES);
    with_ext = malloc(SHELL_SD_PATH_BYTES);
    if (candidate == NULL || path_copy == NULL || dir_path == NULL || with_ext == NULL) {
        free(candidate);
        free(path_copy);
        free(dir_path);
        free(with_ext);
        return false;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        free(candidate);
        free(path_copy);
        free(dir_path);
        free(with_ext);
        return false;
    }

    error = shell_fs_resolve_path(command_name, candidate, SHELL_SD_PATH_BYTES);
    if (error == ESP_OK && shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
        snprintf(resolved_path, resolved_path_size, "%s", candidate);
        found = true;
        goto done;
    }

    if (!shell_path_has_extension(command_name, ".bat")) {
        snprintf(with_ext, SHELL_SD_PATH_BYTES, "%s.bat", command_name);
        error = shell_fs_resolve_path(with_ext, candidate, SHELL_SD_PATH_BYTES);
        if (error == ESP_OK && shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
            snprintf(resolved_path, resolved_path_size, "%s", candidate);
            found = true;
            goto done;
        }
    }

    if (path_env != NULL && path_env[0] != '\0' && !shell_path_has_directory_component(command_name)) {
        snprintf(path_copy, SHELL_ENV_VALUE_BYTES, "%s", path_env);
        entry = strtok_r(path_copy, ";", &context);
        while (entry != NULL) {
            if (shell_fs_resolve_path(entry, dir_path, SHELL_SD_PATH_BYTES) == ESP_OK) {
                int written = snprintf(candidate, SHELL_SD_PATH_BYTES, "%s/%s", dir_path, command_name);
                if (written > 0 && (size_t)written < SHELL_SD_PATH_BYTES &&
                    shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                    snprintf(resolved_path, resolved_path_size, "%s", candidate);
                    found = true;
                    goto done;
                }

                if (!shell_path_has_extension(command_name, ".bat")) {
                    written = snprintf(candidate, SHELL_SD_PATH_BYTES, "%s/%s.bat", dir_path, command_name);
                    if (written > 0 && (size_t)written < SHELL_SD_PATH_BYTES &&
                        shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                        snprintf(resolved_path, resolved_path_size, "%s", candidate);
                        found = true;
                        goto done;
                    }
                }
            }

            entry = strtok_r(NULL, ";", &context);
        }
    }

done:
    shell_sd_end(&session, "call");
    free(candidate);
    free(path_copy);
    free(dir_path);
    free(with_ext);
    return found;
}

/* ========================================================================
 * BATCH CONTROL FLOW: call, goto, shift, if
 * ======================================================================== */

void shell_command_call(int argc, char **argv)
{
    char batch_path[SHELL_SD_PATH_BYTES];

    if (argc < 2) {
        shell_print_usage("Usage: call <file.bat> [args]");
        return;
    }

    if (!shell_resolve_batch_path(argv[1], batch_path, sizeof(batch_path))) {
        shell_transcript_appendf("call: batch file not found %s\n", argv[1]);
        return;
    }

    shell_execute_batch_file(batch_path, argc - 2, &argv[2]);
}

void shell_command_goto(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: goto <label>");
        return;
    }
    if (s_active_batch_frame == NULL) {
        shell_transcript_append_text("goto: only valid inside batch files\n");
        return;
    }
    /* :eof is an implicit end-of-file label: jump to the end of the current
     * batch frame, unwinding any open setlocal scopes — exactly like reaching
     * the end of the file. Compared case-insensitively like every other
     * batch label, so `goto :EOF` works too. */
    if (strcasecmp(argv[1], ":eof") == 0 || strcasecmp(argv[1], "eof") == 0) {
        s_goto_eof = true;
        s_goto_pending = true;
        s_goto_label[0] = '\0';
        return;
    }
    snprintf(s_goto_label, sizeof(s_goto_label), ":%s", argv[1]);
    s_goto_pending = true;
}

void shell_command_shift(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_active_batch_frame == NULL) {
        shell_transcript_append_text("shift: only valid inside batch files\n");
        return;
    }
    if (s_active_batch_frame->argc <= 1) return;
    for (int i = 0; i < s_active_batch_frame->argc - 1; i++) {
        memmove(s_active_batch_frame->args[i], s_active_batch_frame->args[i + 1], SHELL_COMMAND_BYTES);
    }
    s_active_batch_frame->args[s_active_batch_frame->argc - 1][0] = '\0';
    s_active_batch_frame->argc--;
}

void shell_command_if(int argc, char **argv)
{
    /* Supports: if errorlevel N command, if exist file command, if NOT ...,
     * and the numeric keyword comparisons (EQU NEQ LSS LEQ GTR GEQ). */
    if (argc < 3) {
        shell_print_usage("Usage: if [/i] [not] errorlevel N cmd | if [/i] [not] exist file cmd | if [/i] [not] \"a\"==\"b\" cmd | if [not] a EQU|NEQ|LSS|LEQ|GTR|GEQ b cmd");
        return;
    }

    int arg_idx = 1;
    bool not_flag = false;
    bool ignore_case = false;

    /* Consume the optional `/i` (case-insensitive) and `not` flags in either
     * order: `if /i not X cmd`, `if not /i X cmd`, `if /i X cmd`, etc. */
    for (int i = 0; i < 2 && arg_idx < argc; i++) {
        if (shell_text_equals_ignore_case(argv[arg_idx], "/i")) {
            ignore_case = true;
            arg_idx++;
        } else if (shell_text_equals_ignore_case(argv[arg_idx], "not")) {
            not_flag = true;
            arg_idx++;
        } else {
            break;
        }
    }

    if (arg_idx >= argc) {
        shell_transcript_append_text("if: expected condition\n");
        return;
    }

    bool condition = false;

    if (shell_text_equals_ignore_case(argv[arg_idx], "errorlevel")) {
        arg_idx++;
        if (arg_idx >= argc) {
            shell_transcript_append_text("if: expected number after errorlevel\n");
            return;
        }
        int level = atoi(argv[arg_idx]);
        condition = (s_errorlevel >= level);
        arg_idx++;
    } else if (shell_text_equals_ignore_case(argv[arg_idx], "exist")) {
        arg_idx++;
        if (arg_idx >= argc) {
            shell_transcript_append_text("if: expected filename after exist\n");
            return;
        }
        {
            /* Resolve against the current directory and inside a guarded SD
             * session, the same as every other filesystem command. A bare
             * stat() on the raw argument fails for any relative path. The
             * path buffer is SD-path sized and `if` runs on the recursive
             * batch path, so it is heap-allocated. */
            char *resolved = malloc(SHELL_SD_PATH_BYTES);
            shell_sd_session_t session;
            struct stat st;

            condition = false;
            if (resolved == NULL) {
                shell_transcript_append_text("if: out of memory\n");
                return;
            }
            if (shell_fs_resolve_path(argv[arg_idx], resolved, SHELL_SD_PATH_BYTES) == ESP_OK &&
                shell_sd_begin(&session) == ESP_OK) {
                condition = (shell_sd_stat_path(resolved, &st) == ESP_OK);
                shell_sd_end(&session, "if exist");
            }
            free(resolved);
        }
        arg_idx++;
    } else {
        /* Numeric comparison via the cmd.exe keywords, or a string
         * comparison with `==`. The numeric form is
         * `if [not] [/i] <operand1> <OP> <operand2> <command>` where OP is
         * EQU, NEQ, LSS, LEQ, GTR or GEQ. Operands are parsed as decimal
         * integers; a non-numeric operand reads as 0, matching cmd.exe. */
        if (arg_idx + 2 < argc &&
            (shell_text_equals_ignore_case(argv[arg_idx + 1], "EQU") ||
             shell_text_equals_ignore_case(argv[arg_idx + 1], "NEQ") ||
             shell_text_equals_ignore_case(argv[arg_idx + 1], "LSS") ||
             shell_text_equals_ignore_case(argv[arg_idx + 1], "LEQ") ||
             shell_text_equals_ignore_case(argv[arg_idx + 1], "GTR") ||
             shell_text_equals_ignore_case(argv[arg_idx + 1], "GEQ"))) {
            const char *op = argv[arg_idx + 1];
            long left = strtol(argv[arg_idx], NULL, 10);
            long right = strtol(argv[arg_idx + 2], NULL, 10);

            if (shell_text_equals_ignore_case(op, "EQU")) {
                condition = (left == right);
            } else if (shell_text_equals_ignore_case(op, "NEQ")) {
                condition = (left != right);
            } else if (shell_text_equals_ignore_case(op, "LSS")) {
                condition = (left < right);
            } else if (shell_text_equals_ignore_case(op, "LEQ")) {
                condition = (left <= right);
            } else if (shell_text_equals_ignore_case(op, "GTR")) {
                condition = (left > right);
            } else {
                condition = (left >= right);
            }
            arg_idx += 3;
        } else {
            /* String comparison. The tokenizer already removed the quoting, so
             * `if "%VAR%"=="yes"` arrives as a single argument `<value>==yes`
             * and `if a == b` arrives as three. Handle both spellings.
             *
             * Quoting still matters to the user: it is what keeps an empty or
             * space-containing value from collapsing the comparison into a
             * malformed expression before it reaches here.
             *
             * `/i` selects case-insensitive comparison (strcasecmp). */
            int (*cmp)(const char *, const char *) = ignore_case ? strcasecmp : strcmp;
            char *eq = strstr(argv[arg_idx], "==");

            if (eq != NULL) {
                /* Joined form: left==right in one argument. */
                *eq = '\0';
                const char *left = argv[arg_idx];
                const char *right = eq + 2;
                condition = (cmp(left, right) == 0);
                arg_idx++;
            } else if (arg_idx + 2 < argc && strcmp(argv[arg_idx + 1], "==") == 0) {
                /* Spaced form: left == right as three arguments. */
                condition = (cmp(argv[arg_idx], argv[arg_idx + 2]) == 0);
                arg_idx += 3;
            } else if (arg_idx + 1 < argc && strncmp(argv[arg_idx + 1], "==", 2) == 0) {
                /* Half-spaced form: left ==right. */
                condition = (cmp(argv[arg_idx], argv[arg_idx + 1] + 2) == 0);
                arg_idx += 2;
            } else {
                shell_print_error("if: unsupported condition");
                return;
            }
        }
    }

    if (not_flag) condition = !condition;

    if (condition && arg_idx < argc) {
        /* Execute the rest of the line as a command. The joined command is
         * line-width and `if` re-enters the pipeline (recursive batch path),
         * so the buffer is heap-allocated and freed after the nested run. */
        char *cmd = malloc(SHELL_COMMAND_BYTES);

        if (cmd == NULL) {
            shell_transcript_append_text("if: out of memory\n");
            return;
        }
        shell_join_args(argv, arg_idx, argc, cmd, SHELL_COMMAND_BYTES);
        batch_run_nested(cmd);
        free(cmd);
    }
}

/* ========================================================================
 * BUILT-IN COMMANDS: pause, choice, setlocal, endlocal, exit
 * ======================================================================== */

void shell_command_pause(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* A headless board (no UART console, no USB keyboard) has no way to
     * deliver a key, so fall back to the bounded delay rather than stalling
     * a batch file until the wait times out. */
    if (!shell_key_input_available()) {
        shell_transcript_appendf_ansi(SH_MUTE "Press any key to continue . . . " SH_RST "\n");
        vTaskDelay(pdMS_TO_TICKS(SHELL_PAUSE_DELAY_MS));
        return;
    }

    shell_transcript_appendf_ansi(SH_MUTE "Press any key to continue . . . " SH_RST "\n");

    shell_key_wait_begin();
    if (!shell_wait_for_key(SHELL_KEY_WAIT_TIMEOUT_MS, NULL)) {
        shell_transcript_append_text("pause: timed out waiting for a key\n");
        shell_record_warningf("pause", "Timed out waiting for a keypress");
    }
    shell_key_wait_end();
}

void shell_command_choice(int argc, char **argv)
{
    char options[SHELL_COMMAND_BYTES];
    char *message = NULL;
    const char *default_key = NULL;
    bool show_list = true;
    bool case_sensitive = false;
    uint32_t timeout_ms = SHELL_KEY_WAIT_TIMEOUT_MS;
    bool has_timeout_default = false;
    size_t option_count;
    size_t index;
    int chosen = -1;

    snprintf(options, sizeof(options), "YN");

    /* The prompt text is line-sized and `choice` runs on the recursive batch
     * path, so it is heap-allocated and freed at the single exit. */
    message = malloc(SHELL_BATCH_LINE_BYTES);
    if (message == NULL) {
        shell_transcript_append_text("choice: out of memory\n");
        return;
    }
    message[0] = '\0';

    /* Parse the DOS switch set. Anything that is not a switch becomes the
     * prompt text, joined with spaces. */
    for (int arg = 1; arg < argc; arg++) {
        const char *token = argv[arg];

        if (token[0] == '/' && token[1] != '\0') {
            char flag = (char)toupper((unsigned char)token[1]);
            const char *value = token + 2;

            if (*value == ':' || *value == '=') {
                value++;
            }

            switch (flag) {
            case 'C':
                if (*value == '\0') {
                    shell_transcript_append_text("choice: /C needs a key list, for example /C:YNC\n");
                    goto done;
                }
                snprintf(options, sizeof(options), "%s", value);
                continue;
            case 'N':
                show_list = false;
                continue;
            case 'S':
                case_sensitive = true;
                continue;
            case 'T': {
                /* /T:c,secs — default to key c after secs seconds. */
                const char *comma = strchr(value, ',');

                if (value[0] == '\0' || comma == NULL || comma == value) {
                    shell_transcript_append_text("choice: /T needs a key and seconds, for example /T:Y,10\n");
                    goto done;
                }

                {
                    static char timeout_key[2];
                    long seconds = strtol(comma + 1, NULL, 10);

                    timeout_key[0] = value[0];
                    timeout_key[1] = '\0';
                    default_key = timeout_key;
                    has_timeout_default = true;

                    if (seconds > 0) {
                        timeout_ms = (uint32_t)seconds * 1000U;
                    }
                }
                continue;
            }
            default:
                shell_print_error("choice: unknown option %s", token);
                shell_print_usage("Usage: choice [/C:list] [/N] [/T:c,secs] [/S] [text]");
                goto done;
            }
        }

        if (message[0] != '\0') {
            strncat(message, " ", SHELL_BATCH_LINE_BYTES - strlen(message) - 1);
        }
        strncat(message, token, SHELL_BATCH_LINE_BYTES - strlen(message) - 1);
    }

    option_count = strlen(options);
    if (option_count == 0) {
        shell_transcript_append_text("choice: the key list is empty\n");
        goto done;
    }

    /* Render the prompt in DOS form: "text [Y,N]?" */
    if (message[0] != '\0') {
        shell_transcript_appendf("%s ", message);
    }

    if (show_list) {
        shell_transcript_append_text("[");
        for (index = 0; index < option_count; index++) {
            shell_transcript_appendf("%s%c", index > 0 ? "," : "", options[index]);
        }
        shell_transcript_append_text("]? ");
    }

    if (!shell_key_input_available()) {
        /* No interactive source: honor an explicit /T default, otherwise
         * take the first key, and say so instead of pretending to wait. */
        chosen = 0;
        if (has_timeout_default && default_key != NULL) {
            for (index = 0; index < option_count; index++) {
                bool match = case_sensitive
                                 ? (options[index] == default_key[0])
                                 : (toupper((unsigned char)options[index]) ==
                                    toupper((unsigned char)default_key[0]));
                if (match) {
                    chosen = (int)index;
                    break;
                }
            }
        }

        shell_transcript_appendf("%c (no interactive input, using the default)\n", options[chosen]);
        s_errorlevel = chosen + 1;
        goto done;
    }

    shell_key_wait_begin();
    while (chosen < 0) {
        char key = '\0';

        if (!shell_wait_for_key(timeout_ms, &key)) {
            break;
        }

        for (index = 0; index < option_count; index++) {
            bool match = case_sensitive
                             ? (options[index] == key)
                             : (toupper((unsigned char)options[index]) == toupper((unsigned char)key));
            if (match) {
                chosen = (int)index;
                break;
            }
        }

        /* An unmatched key is ignored, exactly like DOS CHOICE. */
    }
    shell_key_wait_end();

    if (chosen < 0) {
        /* Timed out: fall back to the /T key when one was given, else the
         * first key in the list. */
        chosen = 0;
        if (has_timeout_default && default_key != NULL) {
            for (index = 0; index < option_count; index++) {
                bool match = case_sensitive
                                 ? (options[index] == default_key[0])
                                 : (toupper((unsigned char)options[index]) ==
                                    toupper((unsigned char)default_key[0]));
                if (match) {
                    chosen = (int)index;
                    break;
                }
            }
        }
        shell_transcript_appendf("%c (timed out)\n", options[chosen]);
    } else {
        shell_transcript_appendf("%c\n", options[chosen]);
    }

    /* DOS reports the 1-based index of the chosen key in errorlevel. */
    s_errorlevel = chosen + 1;

done:
    free(message);
}

void shell_command_setlocal(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!shell_setlocal_push()) {
        shell_print_error("setlocal: cannot nest deeper than %d scopes",
                                 SHELL_SETLOCAL_DEPTH_MAX);
        shell_record_warningf("setlocal", "setlocal nesting limit reached");
        s_errorlevel = 1;
        return;
    }

    /* Track the scope against the running frame so an unmatched setlocal is
     * unwound automatically when the batch file returns. */
    if (s_active_batch_frame != NULL) {
        s_active_batch_frame->setlocal_depth++;
    }
}

void shell_command_endlocal(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!shell_setlocal_pop()) {
        shell_transcript_append_text("endlocal: no matching setlocal is open\n");
        shell_record_warningf("endlocal", "endlocal without a matching setlocal");
        s_errorlevel = 1;
        return;
    }

    if (s_active_batch_frame != NULL && s_active_batch_frame->setlocal_depth > 0) {
        s_active_batch_frame->setlocal_depth--;
    }
}

void shell_command_exit(int argc, char **argv)
{
    bool frame_only = false;
    int code_index = 1;
    int code;

    /* `exit /b [code]` leaves only the current batch file. */
    if (argc >= 2 && argv[1][0] == '/' && toupper((unsigned char)argv[1][1]) == 'B' && argv[1][2] == '\0') {
        frame_only = true;
        code_index = 2;
    }

    code = (argc > code_index) ? atoi(argv[code_index]) : s_errorlevel;

    if (s_active_batch_frame == NULL) {
        if (frame_only) {
            shell_transcript_append_text("exit: /b is only valid inside batch files\n");
            return;
        }
        shell_transcript_append_text("exit: use reboot to restart the board\n");
        return;
    }

    s_errorlevel = code;
    s_stop_mode = frame_only ? BATCH_STOP_FRAME : BATCH_STOP_ALL;

    /* A pending goto would otherwise be re-evaluated after this line. */
    s_goto_pending = false;
    s_goto_label[0] = '\0';

    shell_transcript_appendf("exit: leaving %s (errorlevel %d)\n",
                             frame_only ? "this batch file" : "all batch files",
                             code);
}

/* ========================================================================
 * PIPE SUPPORT
 * ========================================================================
 * DOS-style pipeline: cmd1 | cmd2 | cmd3
 *
 * Like COMMAND.COM, each stage is spooled through a temporary file on the SD
 * card rather than a live stream — the shell runs one command at a time on a
 * single worker task, so there is no second process to stream into. Each
 * stage writes to its own spool file, and the next stage reads it through the
 * storage input-redirection slot, which is the same slot the `<` operator
 * uses. That makes `type f | find "x" | sort` behave correctly and keeps a
 * single code path in the text-processing commands.
 *
 * Every spool file is removed on every exit path, including a stage failure.
 */

/** Build the spool path for pipeline stage @p stage. */
static void shell_pipe_spool_path(int stage, char *output, size_t output_size)
{
    snprintf(output, output_size, "%s/_pipe%d.tmp", BSP_SD_MOUNT_POINT, stage);
}

void shell_execute_pipe(char *command)
{
    char *stages[SHELL_PIPE_STAGE_MAX];
    char spool[SHELL_PIPE_STAGE_MAX][64];
    int stage_count = 0;
    char *cursor;
    char *segment_start;
    int index;

    if (command == NULL) {
        return;
    }

    /* Split on '|' that is neither quoted nor caret-escaped, so a pipe
     * character inside "..." / '...' or written as ^| is treated as data.
     * The scanner lives in the shell core and is shared with redirection
     * parsing and command chaining. */
    segment_start = command;
    cursor = command;
    while (true) {
        char *pipe_pos = shell_find_unquoted_char(cursor, '|');

        if (pipe_pos == NULL) {
            break;
        }

        if (stage_count >= SHELL_PIPE_STAGE_MAX) {
            shell_transcript_appendf("pipe: at most %d stages are supported\n", SHELL_PIPE_STAGE_MAX);
            return;
        }

        *pipe_pos = '\0';
        stages[stage_count++] = shell_trim(segment_start);
        segment_start = pipe_pos + 1;
        cursor = segment_start;
    }

    if (stage_count == 0) {
        /* No unquoted separator after all: run the line as an ordinary
         * command so a quoted '|' cannot be lost. */
        batch_run_nested(command);
        return;
    }

    if (stage_count >= SHELL_PIPE_STAGE_MAX) {
        shell_transcript_appendf("pipe: at most %d stages are supported\n", SHELL_PIPE_STAGE_MAX);
        return;
    }
    stages[stage_count++] = shell_trim(segment_start);

    for (index = 0; index < stage_count; index++) {
        if (stages[index] == NULL || stages[index][0] == '\0') {
            shell_print_error("pipe: invalid pipe syntax, a stage is empty");
            return;
        }
    }

    /* Every stage but the last spools its output for the next one. */
    for (index = 0; index < stage_count; index++) {
        bool is_last = (index + 1 == stage_count);

        spool[index][0] = '\0';

        if (index > 0) {
            /* Hand the previous stage's spool file to this stage. The
             * text-processing commands pick it up when no filename argument
             * was supplied. */
            storage_set_input_redirect(spool[index - 1]);
        }

        if (is_last) {
            batch_run_nested(stages[index]);
        } else {
            char redirected[SHELL_COMMAND_BYTES * 2];

            shell_pipe_spool_path(index, spool[index], sizeof(spool[index]));
            snprintf(redirected, sizeof(redirected), "%s > %s", stages[index], spool[index]);
            batch_run_nested(redirected);

            /* Give the FATFS write cache a moment to settle before the next
             * stage opens the file for reading. */
            vTaskDelay(pdMS_TO_TICKS(SHELL_PIPE_SETTLE_DELAY_MS));
        }

        /* The redirection slot belongs to one stage only. */
        storage_clear_input_redirect();

        if (s_stop_mode != BATCH_STOP_NONE || s_goto_pending) {
            break;
        }
    }

    /* Remove every spool file that was actually created. */
    for (index = 0; index < stage_count; index++) {
        if (spool[index][0] != '\0') {
            (void)unlink(spool[index]);
        }
    }
}

/* ========================================================================
 * BATCH ENGINE: FILE EXECUTION, LABELS, FOR LOOPS
 * ======================================================================== */

esp_err_t shell_execute_batch_file(const char *path, int argc, char **argv)
{
    /* The frame carries the argument copies and the label table, which
     * together are far larger than the command worker task's stack can hold
     * across nested calls. Allocating it on the heap keeps this recursive
     * function's stack frame small and bounded regardless of nesting depth. */
    shell_batch_frame_t *frame = NULL;
    FILE *file = NULL;
    char *line = NULL;
    shell_sd_session_t session;
    esp_err_t error;
    int setlocal_depth_on_entry;
    int depth;
    int index;

    depth = (s_active_batch_frame != NULL) ? s_active_batch_frame->depth + 1 : 1;
    if (depth > SHELL_BATCH_DEPTH_MAX) {
        shell_transcript_append_text("call: maximum batch nesting depth reached\n");
        return ESP_ERR_INVALID_STATE;
    }

    /* Both the frame and the read buffer are heap-allocated. This function
     * recurses once per nesting level and each level also re-enters the
     * command pipeline, so keeping its stack frame small is what makes
     * SHELL_BATCH_DEPTH_MAX levels fit in the worker task's stack. */
    frame = calloc(1, sizeof(*frame));
    line = malloc(SHELL_BATCH_LINE_BYTES);
    if (frame == NULL || line == NULL) {
        free(frame);
        free(line);
        shell_transcript_append_text("call: out of memory starting the batch frame\n");
        shell_record_errorf("call", ESP_ERR_NO_MEM, "Out of memory allocating a batch frame");
        return ESP_ERR_NO_MEM;
    }

    frame->echo_enabled = s_default_echo;
    /* Classic DOS: %0 is the script name (path as invoked), and the caller's
     * arguments begin at %1. Store the path in args[0] and shift the passed
     * arguments up by one slot so argv[0] -> args[1], etc. */
    frame->argc = MIN(argc + 1, SHELL_BATCH_ARGS_MAX);
    snprintf(frame->args[0], sizeof(frame->args[0]), "%s", path);
    for (index = 0; index < argc && index + 1 < SHELL_BATCH_ARGS_MAX; index++) {
        snprintf(frame->args[index + 1], sizeof(frame->args[index + 1]), "%s", argv[index]);
    }
    frame->depth = depth;
    frame->parent = s_active_batch_frame;
    frame->label_count = 0;
    frame->batch_file = NULL;
    frame->setlocal_depth = 0;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("call: SD card not present - insert and retry");
        free(frame);
        free(line);
        return error;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "call");
        free(frame);
        free(line);
        return ESP_ERR_NOT_FOUND;
    }

    frame->batch_file = file;

    /* args[0] already holds the script path; the caller's arguments were
     * copied into args[1..] above, so no further copying is needed here. */

    /* Scan for labels first */
    shell_scan_batch_labels(frame, file);

    /* Remember the caller's setlocal depth so any scope this file opens and
     * forgets to close is unwound when the frame returns. */
    setlocal_depth_on_entry = s_setlocal_depth;

    s_active_batch_frame = frame;

    while (fgets(line, SHELL_BATCH_LINE_BYTES, file) != NULL) {
        char *trimmed = shell_trim(line);
        bool suppress_echo = false;
        size_t line_len;
        int continuations = 0;

        line_len = strlen(trimmed);
        while (line_len > 0 && (trimmed[line_len - 1] == '\n' || trimmed[line_len - 1] == '\r')) {
            trimmed[--line_len] = '\0';
        }

        /* Line continuation: a trailing caret joins the next physical line.
         * The caret is consumed and the lines are concatenated, so a long
         * command can be split for readability the way DOS allows.
         *
         * A doubled `^^` at the end is an escaped literal caret, not a
         * continuation, so it is left alone. The chain is bounded by
         * P4_CONFIG_LINE_CONTINUATION_MAX so a malformed file cannot loop. */
        while (line_len > 0 && trimmed[line_len - 1] == P4_CONFIG_ESCAPE_CHAR &&
               continuations < P4_CONFIG_LINE_CONTINUATION_MAX) {
            char *next_start;
            size_t next_len;
            size_t available;

            /* Count the run of trailing carets. An even count means they are
             * escaped pairs and this line does not continue. */
            {
                size_t carets = 0;
                size_t scan = line_len;

                while (scan > 0 && trimmed[scan - 1] == P4_CONFIG_ESCAPE_CHAR) {
                    carets++;
                    scan--;
                }
                if ((carets % 2) == 0) {
                    break;
                }
            }

            /* Drop the continuation caret. */
            trimmed[--line_len] = '\0';

            /* Append the next physical line into the space left in the
             * buffer, which is bounded by SHELL_BATCH_LINE_BYTES. */
            next_start = trimmed + line_len;
            available = SHELL_BATCH_LINE_BYTES - (size_t)(next_start - line) - 1;
            if (available == 0) {
                shell_transcript_append_text("call: continued line is too long\n");
                shell_record_warningf("call", "Continued batch line exceeded %d bytes",
                                      SHELL_BATCH_LINE_BYTES);
                break;
            }

            if (fgets(next_start, (int)available + 1, file) == NULL) {
                /* A trailing caret on the final line has nothing to join. */
                break;
            }

            next_len = strlen(next_start);
            while (next_len > 0 &&
                   (next_start[next_len - 1] == '\n' || next_start[next_len - 1] == '\r')) {
                next_start[--next_len] = '\0';
            }

            line_len += next_len;
            continuations++;
        }

        if (continuations >= P4_CONFIG_LINE_CONTINUATION_MAX) {
            shell_transcript_appendf("call: stopped after %d line continuations\n",
                                     P4_CONFIG_LINE_CONTINUATION_MAX);
            shell_record_warningf("call", "Line continuation limit reached");
        }

        if (trimmed[0] == '@') {
            suppress_echo = true;
            trimmed = shell_trim(trimmed + 1);
        }

        if (trimmed[0] == '\0') {
            continue;
        }

        /* Check for label - skip execution */
        if (shell_is_label(trimmed)) {
            continue;
        }

        /* Check for for loop */
        if (strncasecmp(trimmed, "for ", 4) == 0) {
            shell_execute_for_loop(frame, trimmed);
            if (s_stop_mode != BATCH_STOP_NONE || s_goto_pending) break;
            continue;
        }

        if (!suppress_echo && frame->echo_enabled) {
            shell_transcript_appendf_ansi(SH_PROMPT "%s" SH_RST SH_DESC "%s" SH_RST "\n", SHELL_PROMPT, trimmed);
        }

        batch_run_nested(trimmed);

        /* `exit` or `exit /b` asked this file to stop. */
        if (s_stop_mode != BATCH_STOP_NONE) {
            break;
        }

        /* `goto :eof` jumps to the end of the current batch frame. */
        if (s_goto_eof) {
            s_goto_eof = false;
            s_goto_pending = false;
            break;
        }

        /* Check for goto or call :label that may have changed execution flow */
        if (s_goto_pending) {
            long label_pos = shell_find_label_pos(frame, s_goto_label + 1);
            if (label_pos >= 0) {
                fseek(file, label_pos, SEEK_SET);
                s_goto_pending = false;
                s_goto_label[0] = '\0';
                /* Continue from the label position - the label line will be skipped */
                continue;
            } else {
                shell_transcript_appendf("goto: label not found: %s\n", s_goto_label + 1);
                s_goto_pending = false;
                s_goto_label[0] = '\0';
            }
        }
    }

    s_active_batch_frame = frame->parent;

    /* Discard any setlocal scope this file left open, restoring the caller's
     * environment and releasing the snapshots. */
    shell_setlocal_unwind_to(setlocal_depth_on_entry);

    /* A pending `goto` / `goto :eof` belongs to this frame only: labels are
     * resolved against this file's label table. Once the frame returns the
     * flags are stale and must not leak into the caller, which would
     * otherwise break out of its own line loop. `exit` already clears them;
     * this also covers a goto issued from inside a `for` or `if` body that
     * ended the loop early. */
    s_goto_pending = false;
    s_goto_eof = false;
    s_goto_label[0] = '\0';

    /* `exit /b` stops only this file; the caller keeps running. A bare
     * `exit` keeps unwinding until no batch frame is left. */
    if (s_stop_mode == BATCH_STOP_FRAME) {
        s_stop_mode = BATCH_STOP_NONE;
    } else if (s_stop_mode == BATCH_STOP_ALL && frame->parent == NULL) {
        s_stop_mode = BATCH_STOP_NONE;
    }

    fclose(file);
    shell_sd_end(&session, "call");
    free(frame);
    free(line);
    return ESP_OK;
}

/* Helper: check if a line is a label (starts with :) */
static bool shell_is_label(const char *line)
{
    if (line == NULL) return false;
    while (*line && isspace((unsigned char)*line)) line++;
    return *line == ':';
}

/* Helper: extract label name from a label line */
static void shell_extract_label_name(const char *line, char *name, size_t name_size)
{
    if (line == NULL || name == NULL || name_size == 0) return;
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == ':') line++;
    size_t i = 0;
    while (*line && !isspace((unsigned char)*line) && i + 1 < name_size) {
        name[i++] = *line++;
    }
    name[i] = '\0';
}

/* Helper: find a label in the current batch frame */
static long shell_find_label_pos(shell_batch_frame_t *frame, const char *label_name)
{
    if (frame == NULL || label_name == NULL) return -1;
    for (int i = 0; i < frame->label_count; i++) {
        if (strcasecmp(frame->labels[i].name, label_name) == 0) {
            return frame->labels[i].file_pos;
        }
    }
    return -1;
}

/* Helper: scan batch file for labels and build label table */
static void shell_scan_batch_labels(shell_batch_frame_t *frame, FILE *file)
{
    if (frame == NULL || file == NULL) return;

    long current_pos = ftell(file);
    long line_pos = 0;
    bool previous_continues = false;

    /* The read buffer is line-sized, so it lives on the heap: this function
     * runs from shell_execute_batch_file(), which is on the recursive batch
     * path, and a line-sized stack buffer there would stack up across nesting
     * levels. */
    char *line = malloc(SHELL_BATCH_LINE_BYTES);
    if (line == NULL) {
        return;
    }

    rewind(file);
    frame->label_count = 0;

    /* Tracks whether the previous physical line ended with an unescaped
     * continuation caret. A line that is the tail of a continuation is part
     * of the command above it, so a ':' at its start is data, not a label. */
    while (fgets(line, SHELL_BATCH_LINE_BYTES, file) != NULL) {
        line_pos = ftell(file);
        char *trimmed = shell_trim(line);
        bool this_continues;
        size_t length;

        /* Determine whether THIS line continues, using the same odd-caret
         * rule the executor applies, so both agree on line boundaries. */
        length = strlen(trimmed);
        while (length > 0 && (trimmed[length - 1] == '\n' || trimmed[length - 1] == '\r')) {
            trimmed[--length] = '\0';
        }

        {
            size_t carets = 0;
            size_t scan = length;

            while (scan > 0 && trimmed[scan - 1] == P4_CONFIG_ESCAPE_CHAR) {
                carets++;
                scan--;
            }
            this_continues = ((carets % 2) == 1);
        }

        if (!previous_continues && shell_is_label(trimmed)) {
            if (frame->label_count < SHELL_BATCH_LABEL_MAX) {
                shell_extract_label_name(trimmed, frame->labels[frame->label_count].name,
                                         sizeof(frame->labels[frame->label_count].name));
                frame->labels[frame->label_count].file_pos = line_pos - strlen(line);
                frame->label_count++;
            }
        }

        previous_continues = this_continues;
    }

    fseek(file, current_pos, SEEK_SET);
    free(line);
}

/* Helper: substitute a `for` variable with a value in the body and run it. */
static void shell_for_substitute_and_run(const char *do_command, char var_name, const char *value)
{
    /* The expanded body is line-sized and this helper runs from the
     * recursive batch path (for-loop body re-enters the command pipeline),
     * so the buffer is heap-allocated and freed after the nested command
     * returns. */
    const size_t cmd_size = SHELL_BATCH_LINE_BYTES * 2;
    char *expanded_cmd = malloc(cmd_size);
    const char *src;
    char *dst;
    size_t remaining;

    if (expanded_cmd == NULL) {
        shell_transcript_append_text("for: out of memory expanding the loop body\n");
        return;
    }

    src = do_command;
    dst = expanded_cmd;
    remaining = cmd_size - 1;

    while (*src != '\0' && remaining > 0) {
        /* In a batch file the loop variable is written `%%var` (the doubled
         * percent is the batch-file escape for a literal `%`). Accept that
         * form first, then the single `%var` for tolerance, and substitute
         * the current value for either. */
        if (*src == '%' && *(src + 1) == '%' && *(src + 2) == var_name) {
            const char *replacement = value;
            while (*replacement != '\0' && remaining > 0) {
                *dst++ = *replacement++;
                remaining--;
            }
            src += 3;
        } else if (*src == '%' && *(src + 1) == var_name) {
            const char *replacement = value;
            while (*replacement != '\0' && remaining > 0) {
                *dst++ = *replacement++;
                remaining--;
            }
            src += 2;
        } else {
            *dst++ = *src++;
            remaining--;
        }
    }
    *dst = '\0';

    batch_run_nested(expanded_cmd);
    free(expanded_cmd);
}

/* Helper: execute a for loop command */
static void shell_execute_for_loop(shell_batch_frame_t *frame, char *command_line)
{
    char *set_str = NULL;
    char *for_ptr;
    char *do_command;
    char var_name;

    (void)frame;

    /* Parse: for %%var in (set) do command */
    for_ptr = strstr(command_line, "for ");
    if (for_ptr == NULL) return;

    /* Skip "for " */
    for_ptr += 4;
    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    /* Check for %%var */
    if (*for_ptr != '%' || *(for_ptr + 1) != '%') return;
    for_ptr += 2;

    var_name = *for_ptr;
    if (!isalpha((unsigned char)var_name)) return;
    for_ptr++;
    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    /* Expect "in" */
    if (strncasecmp(for_ptr, "in", 2) != 0) return;
    for_ptr += 2;
    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    /* Expect "(" */
    if (*for_ptr != '(') return;
    for_ptr++;

    /* Parse the set - everything until ")". The set is line-sized and this
     * function sits on the recursive batch path, so it is heap-allocated and
     * freed on every exit. */
    set_str = malloc(SHELL_BATCH_LINE_BYTES);
    if (set_str == NULL) {
        shell_transcript_append_text("for: out of memory parsing the set\n");
        return;
    }
    {
        char *set_ptr = set_str;
        int paren_depth = 1;

        while (*for_ptr && paren_depth > 0 && set_ptr - set_str < SHELL_BATCH_LINE_BYTES - 1) {
            if (*for_ptr == '(') paren_depth++;
            else if (*for_ptr == ')') paren_depth--;
            if (paren_depth > 0) {
                *set_ptr++ = *for_ptr;
            }
            for_ptr++;
        }
        *set_ptr = '\0';
    }

    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    /* Expect "do" */
    if (strncasecmp(for_ptr, "do", 2) != 0) {
        free(set_str);
        return;
    }
    for_ptr += 2;
    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    if (*for_ptr == '\0') {
        free(set_str);
        return;
    }

    do_command = shell_trim(for_ptr);
    if (*do_command == '\0') {
        free(set_str);
        return;
    }

    /* Wildcard set: if the set contains `*` or `?`, expand it against the
     * filesystem and iterate over matching paths. Otherwise fall through to
     * the classic in-line token set. */
    if (strchr(set_str, '*') != NULL || strchr(set_str, '?') != NULL) {
        char **tokens = NULL;
        int count = 0;
        esp_err_t error = storage_expand_wildcard(set_str, &tokens, &count);

        if (error != ESP_OK) {
            shell_print_warning("for: could not expand %s (%s)", set_str, esp_err_to_name(error));
            shell_record_warningf("for", "Wildcard expansion failed for %s", set_str);
            free(set_str);
            return;
        }

        for (int i = 0; i < count; i++) {
            shell_for_substitute_and_run(do_command, var_name, tokens[i]);
            if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                break;
            }
        }

        storage_free_wildcard_expansion(tokens, count);
        free(set_str);
        return;
    }

    /* Now iterate over the set */
    {
        char *save_ptr = NULL;
        char *token = strtok_r(set_str, " \t", &save_ptr);

        while (token != NULL) {
            shell_for_substitute_and_run(do_command, var_name, token);

            /* Check for goto/exit conditions that end the loop early */
            if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                break;
            }

            token = strtok_r(NULL, " \t", &save_ptr);
        }
    }

    free(set_str);
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void batch_init(void)
{
    if (s_initialized) {
        return;
    }

    memset(s_shell_env_vars, 0, sizeof(s_shell_env_vars));
    (void)shell_env_set("PATH", "sd:/");
    memset(s_aliases, 0, sizeof(s_aliases));

    s_active_batch_frame = NULL;
    s_errorlevel = 0;
    s_goto_label[0] = '\0';
    s_goto_pending = false;
    s_stop_mode = BATCH_STOP_NONE;

    /* Release any setlocal snapshot left over from a previous init. */
    shell_setlocal_unwind_to(0);
    memset(s_setlocal_stack, 0, sizeof(s_setlocal_stack));
    s_setlocal_depth = 0;

    s_initialized = true;
    ESP_LOGI(BATCH_TAG, "Batch module initialized");
}

bool batch_is_initialized(void)
{
    return s_initialized;
}

void batch_set_default_echo(bool enabled)
{
    s_default_echo = enabled;
}

void batch_boot_execute_command(const char *command)
{
    char *command_copy;
    size_t command_len;

    if (command == NULL || command[0] == '\0') {
        return;
    }
    if (s_command_ops.execute_command == NULL) {
        return;
    }

    /* The pipeline takes a mutable buffer, and the caller's string may be
     * read-only storage. Copy it on the heap (this may run at boot, outside
     * the worker task's command path) and release it when the command
     * returns. */
    command_len = strlen(command);
    command_copy = malloc(command_len + 1);
    if (command_copy == NULL) {
        shell_transcript_append_text("shell: out of memory executing a boot command\n");
        return;
    }

    snprintf(command_copy, command_len + 1, "%s", command);
    s_command_ops.execute_command(command_copy);
    free(command_copy);
}

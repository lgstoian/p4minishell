/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
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
#include "filetype.h"
#include "shell.h"
#include "markdown.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "clock.h"
#include "esp_random.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
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
#define SHELL_BIND_MAX                    P4_CONFIG_BIND_MAX
#define SHELL_BIND_VALUE_BYTES            P4_CONFIG_BIND_VALUE_BYTES
#define SHELL_BIND_PROFILE                P4_CONFIG_BIND_PROFILE
#define SHELL_MACRO_BYTES                 P4_CONFIG_MACRO_BYTES
#define SHELL_MACRO_DEFAULT_FILE          P4_CONFIG_MACRO_DEFAULT_FILE
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

/**
 * One `call :label` scope running inside a batch frame.
 *
 * A labelled subroutine gets its own argument set (the caller's `%1..%9` are
 * replaced) and `shift` affects only that subroutine, so the caller's
 * arguments are saved on entry and restored on return. A label may call
 * another label, so these scopes nest.
 */
typedef struct {
    long resume_pos;                     /**< Position to resume at on return. */
    int argc;                            /**< Saved caller argument count. */
    char (*args)[SHELL_COMMAND_BYTES];   /**< Saved caller argument copies. */
} shell_label_call_t;

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
    /** Whole-file RAM image when the file fit P4_CONFIG_BATCH_FILE_MAX_BYTES
     *  (NULL = stream from batch_file). Executing from RAM keeps `goto`-heavy
     *  loops off the SD card entirely. */
    char *ram;
    size_t ram_len;
    size_t ram_pos;
    /** setlocal scopes opened by this frame, unwound when it returns. */
    int setlocal_depth;
    /** Nested `call :label` scopes (`call_resume_pos` / argument saves). */
    shell_label_call_t label_calls[SHELL_BATCH_DEPTH_MAX + 2];
    int label_call_depth;
    /** True when this frame entered app mode (`appmode on`); the saved screen
     *  is restored automatically when the frame returns. */
    bool app_mode;
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

/* Environment variables (RAM-only, not persisted across boots). The slot
 * array is heap-allocated (PSRAM first) in batch_init(): a static table large
 * enough for the reference apps (~22 vars each) costs too much internal DIRAM
 * at boot, which starved the command-worker stack. */
#define SHELL_ENV_BYTES (sizeof(shell_env_var_t) * SHELL_ENV_VAR_MAX)
static shell_env_var_t *s_shell_env_vars;

/* Shared tables (env vars, aliases) are DOS-global across workers: `set`
 * on one task is visible to the other, which is exactly the cheap IPC games
 * need. Mutations take this lock so two setters can never corrupt a slot;
 * readers stay lock-free (a value changing mid-read is transient, and every
 * consumer copies it out immediately). Snapshot copies (setlocal) and full
 * iterations (print/save/clear) also take it. */
static SemaphoreHandle_t s_shared_lock = NULL;

static void batch_shared_lock_init(void)
{
    if (s_shared_lock == NULL) {
        s_shared_lock = xSemaphoreCreateMutex();
    }
}

static void batch_shared_take(void)
{
    batch_shared_lock_init();
    if (s_shared_lock != NULL) {
        xSemaphoreTake(s_shared_lock, portMAX_DELAY);
    }
}

static void batch_shared_give(void)
{
    if (s_shared_lock != NULL) {
        xSemaphoreGive(s_shared_lock);
    }
}

/* ---- Aliases (DOSKEY-style macros, RAM-only until /save) ---- */

/** One RAM-only alias slot. */
typedef struct {
    bool used;
    char name[SHELL_ALIAS_NAME_BYTES];
    char value[SHELL_ALIAS_VALUE_BYTES];
} shell_alias_t;

static shell_alias_t s_aliases[SHELL_ALIAS_MAX];

/* ---- Key binds (F1..F12 command lines, RAM-only until /save) ---- */

/** USB HID codes for F1..F12 (standard HID usage table). */
#define SHELL_BIND_F1_CODE 0x3A
#define SHELL_BIND_F12_CODE 0x45

/** Ctrl+letter chord codes: 0x80 | HID letter (a=0x04..z=0x1D). */
#define SHELL_BIND_CHORD_FLAG 0x80
#define SHELL_BIND_LETTER_MIN 0x04
#define SHELL_BIND_LETTER_MAX 0x1D
/** Reserved chord: ^C is the foreground break, never bindable. */
#define SHELL_BIND_CHORD_C 0x06

/** USB HID Ctrl modifier bits (mirrored from components/usb/usb.h;
 *  batch cannot include usb.h, same as shell.c). */
#define SHELL_BIND_MOD_LCTRL 0x01
#define SHELL_BIND_MOD_RCTRL 0x10

/** True for bindable F-key and chord codes (^C excluded). */
static bool shell_bind_code_valid(uint8_t code)
{
    if (code >= SHELL_BIND_F1_CODE && code <= SHELL_BIND_F12_CODE) {
        return true;
    }
    if ((code & SHELL_BIND_CHORD_FLAG) != 0) {
        uint8_t hid = code & (uint8_t)~SHELL_BIND_CHORD_FLAG;
        return hid >= SHELL_BIND_LETTER_MIN && hid <= SHELL_BIND_LETTER_MAX &&
               hid != SHELL_BIND_CHORD_C;
    }
    return false;
}

/** One bound function key. */
typedef struct {
    bool used;
    uint8_t key;
    char value[SHELL_BIND_VALUE_BYTES];
} shell_bind_t;

static shell_bind_t s_binds[SHELL_BIND_MAX];

/* Batch execution state (per worker task).
 *
 * The main worker owns slot 0; `start` background tasks own the rest.
 * Everything a batch run mutates while executing — frame stack, errorlevel,
 * goto state, stop mode, setlocal scopes — lives here so two workers never
 * share them. Shared DOS-style state (env table, aliases, cwd) stays global.
 * Code below keeps using the historical `s_*` names: they are macros into
 * the current task's slot, so the ~140 use sites needed no edits. Unknown
 * tasks (unit tests, init) fall back to slot 0. */
typedef struct {
    shell_batch_frame_t *active_frame;
    int errorlevel;
    char goto_label[SHELL_COMMAND_BYTES];
    bool goto_pending;
    bool goto_eof;
    batch_stop_mode_t stop_mode;
    shell_env_var_t *setlocal_stack[SHELL_SETLOCAL_DEPTH_MAX];
    int setlocal_depth;
    /* Delayed-expansion flag (`!VAR!`) with one saved value per open scope,
     * so `endlocal` (and the frame-return unwind) restores exactly what the
     * matching `setlocal` saw — the COMMAND.COM contract. */
    bool delayed_expansion;
    bool delayed_stack[SHELL_SETLOCAL_DEPTH_MAX];
    TaskHandle_t task;      /* owning bg task, NULL for slot 0 */
    bool in_use;            /* bg slot claimed by a live task */
    bool kill_requested;    /* taskkill: unwind at the next batch line */
    char all_args[SHELL_BATCH_LINE_BYTES]; /* `%*` scratch (per task) */
} batch_task_ctx_t;

static batch_task_ctx_t s_batch_ctx[1 + P4_CONFIG_BG_TASKS];

static batch_task_ctx_t *batch_ctx_current(void)
{
    TaskHandle_t me;
    int i;

    /* xTaskGetCurrentTaskHandle is safe during init (returns NULL). */
    me = xTaskGetCurrentTaskHandle();
    if (me != NULL) {
        for (i = 1; i < 1 + P4_CONFIG_BG_TASKS; i++) {
            if (s_batch_ctx[i].in_use && s_batch_ctx[i].task == me) {
                return &s_batch_ctx[i];
            }
        }
    }
    return &s_batch_ctx[0];
}

#define s_active_batch_frame (batch_ctx_current()->active_frame)
#define s_errorlevel         (batch_ctx_current()->errorlevel)
#define s_goto_label         (batch_ctx_current()->goto_label)
#define s_goto_pending       (batch_ctx_current()->goto_pending)
#define s_goto_eof           (batch_ctx_current()->goto_eof)
#define s_stop_mode          (batch_ctx_current()->stop_mode)
#define s_setlocal_stack     (batch_ctx_current()->setlocal_stack)
#define s_setlocal_depth     (batch_ctx_current()->setlocal_depth)
#define s_delayed_expansion  (batch_ctx_current()->delayed_expansion)
#define s_delayed_stack      (batch_ctx_current()->delayed_stack)
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
static void shell_execute_for_loop_body(shell_batch_frame_t *frame, char *command_line);
static char *shell_frame_fgets(shell_batch_frame_t *frame, char *buf, int size);
static long shell_frame_tell(shell_batch_frame_t *frame);
static void shell_frame_seek(shell_batch_frame_t *frame, long pos);
static void shell_frame_load(shell_batch_frame_t *frame, FILE *file);
static void batch_run_nested(char *command);
static esp_err_t shell_batch_run_internal(const char *path, const char *start_label,
                                          int argc, char **argv);

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

    /* Alphanumerics plus underscore, plus `[`/`]` so batch apps can keep
     * indexed arrays (`set SPR[3]=...` / `%SPR[3]%`). Brackets are only
     * name characters here — the value side is untouched — so existing
     * names keep working and no new storage is introduced. */
    for (index = 0; name[index] != '\0'; index++) {
        if (!(isalnum((unsigned char)name[index]) || name[index] == '_' ||
              name[index] == '[' || name[index] == ']')) {
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
    /* Shared table, two workers: a returned pointer stays valid only until
     * the next set (callers copy it out immediately — established pattern).
     * The lookup itself is lock-free; mutation takes the env lock. */
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

    batch_shared_take();
    slot = shell_env_find_slot(normalized);
    if (value == NULL || value[0] == '\0') {
        if (slot != NULL) {
            memset(slot, 0, sizeof(*slot));
        }
        batch_shared_give();
        return ESP_OK;
    }

    if (slot == NULL) {
        slot = shell_env_find_free_slot();
        if (slot == NULL) {
            batch_shared_give();
            return ESP_ERR_NO_MEM;
        }
    }

    slot->used = true;
    snprintf(slot->name, sizeof(slot->name), "%s", normalized);
    snprintf(slot->value, sizeof(slot->value), "%s", value);
    batch_shared_give();
    return ESP_OK;
}

static void shell_env_print_all(void)
{
    size_t index;
    bool any = false;

    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        /* Copy out under lock, print after: never hold the shared lock
         * across transcript calls. */
        bool used = false;
        char name[SHELL_ENV_NAME_BYTES];
        char value[SHELL_ENV_VALUE_BYTES];
        batch_shared_take();
        used = s_shell_env_vars[index].used;
        if (used) {
            snprintf(name, sizeof(name), "%s", s_shell_env_vars[index].name);
            snprintf(value, sizeof(value), "%s", s_shell_env_vars[index].value);
        }
        batch_shared_give();
        if (!used) {
            continue;
        }
        shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n", name, value);
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
    esp_err_t result = ESP_OK;

    shell_alias_normalize_name(name, normalized, sizeof(normalized));
    if (!shell_alias_name_is_valid(normalized)) {
        return ESP_ERR_INVALID_ARG;
    }

    batch_shared_take();
    slot = shell_alias_find_slot(normalized);
    if (value == NULL || value[0] == '\0') {
        if (slot != NULL) {
            memset(slot, 0, sizeof(*slot));
        }
        batch_shared_give();
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
            result = ESP_ERR_NO_MEM;
        }
    }

    if (result == ESP_OK) {
        slot->used = true;
        snprintf(slot->name, sizeof(slot->name), "%s", normalized);
        snprintf(slot->value, sizeof(slot->value), "%s", value);
    }
    batch_shared_give();
    return result;
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
        bool used;
        char name[SHELL_ALIAS_NAME_BYTES];
        char value[SHELL_ALIAS_VALUE_BYTES];

        /* Copy out under lock, print/write after: never hold the shared
         * lock across transcript or filesystem calls. */
        batch_shared_take();
        used = s_aliases[index].used;
        if (used) {
            snprintf(name, sizeof(name), "%s", s_aliases[index].name);
            snprintf(value, sizeof(value), "%s", s_aliases[index].value);
        }
        batch_shared_give();
        if (!used) {
            continue;
        }
        /* Values containing a double quote cannot round-trip through the
         * batch quoting used by the profile; skip them rather than write a
         * line that would load wrong. */
        if (strchr(value, '"') != NULL) {
            shell_print_warning("alias: skipped %s (value contains a double quote)",
                                name);
            continue;
        }
        fprintf(file, "alias %s=\"%s\"\n", name, value);
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
            bool used = false;
            char name[SHELL_ALIAS_NAME_BYTES];
            char value[SHELL_ALIAS_VALUE_BYTES];
            batch_shared_take();
            used = s_aliases[slot].used;
            if (used) {
                snprintf(name, sizeof(name), "%s", s_aliases[slot].name);
                snprintf(value, sizeof(value), "%s", s_aliases[slot].value);
            }
            batch_shared_give();
            if (!used) {
                continue;
            }
            shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                          name, value);
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
            batch_shared_take();
            for (index = 0; index < SHELL_ALIAS_MAX; index++) {
                memset(&s_aliases[index], 0, sizeof(s_aliases[index]));
            }
            batch_shared_give();
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
        /* The value is the text after `=`. Unquoted multi-word values are
         * joined from the remaining tokens (like `echo`/`set`) instead of
         * silently dropping everything after the first word. */
        {
            char *value = malloc(SHELL_ALIAS_VALUE_BYTES);
            bool truncated = false;

            if (value == NULL) {
                shell_print_error("alias: out of memory");
                return;
            }
            snprintf(value, SHELL_ALIAS_VALUE_BYTES, "%s", equals + 1);
            for (index = 2; index < argc; index++) {
                size_t used = strlen(value);

                if (used + 1 >= SHELL_ALIAS_VALUE_BYTES) {
                    truncated = true;
                    break;
                }
                value[used++] = ' ';
                value[used] = '\0';
                strncat(value, argv[index], SHELL_ALIAS_VALUE_BYTES - used - 1);
            }
            if (shell_alias_set(argv[1], value) != ESP_OK) {
                free(value);
                shell_print_error("alias: invalid alias name or the table is full");
                return;
            }
            if (truncated) {
                shell_print_warning("alias: value truncated to %d bytes",
                                    SHELL_ALIAS_VALUE_BYTES - 1);
            }
            free(value);
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
 * KEY BINDS (bind F1..F12, USB function keys)
 * ========================================================================
 * A small RAM-only table mapping USB HID function keys to command lines.
 * A bound key fires its line onto the command worker while the prompt is
 * idle (the shell checks the table in the non-printable-key path, after
 * the modal and key-wait guards, and submits via execute_command_async).
 *
 * Persistence mirrors aliases: `bind /save` writes `bind Fx <line>` lines
 * to the SD profile (P4_CONFIG_BIND_PROFILE); boot.c auto-runs that batch
 * file after the alias profile, and `bind /load` reloads it manually.
 */

/** Parse a key name (F1..F12 or ^<letter>, case-insensitive). */
static bool shell_bind_parse_key(const char *name, uint8_t *code_out)
{
    long number;
    char *end = NULL;

    if (name == NULL || code_out == NULL) {
        return false;
    }
    /* Chord form first: ^<letter>. ^C parses (so the verb can name the
     * reservation); shell_bind_code_valid() still rejects it for storage. */
    if (name[0] == '^' && name[1] != '\0' && name[2] == '\0' &&
        isalpha((unsigned char)name[1])) {
        uint8_t hid = (uint8_t)(SHELL_BIND_LETTER_MIN +
                                (tolower((unsigned char)name[1]) - 'a'));
        *code_out = (uint8_t)(SHELL_BIND_CHORD_FLAG | hid);
        return true;
    }
    if ((name[0] != 'F' && name[0] != 'f') || name[1] == '\0' || strlen(name) > 3) {
        return false;
    }
    number = strtol(name + 1, &end, 10);
    if (end == name + 1 || *end != '\0' || number < 1 || number > 12) {
        return false;
    }
    *code_out = (uint8_t)(SHELL_BIND_F1_CODE + number - 1);
    return true;
}

/** Format a code back to its name (F1..F12, ^X for chords). */
static void shell_bind_format_key(uint8_t code, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    if ((code & SHELL_BIND_CHORD_FLAG) != 0) {
        uint8_t hid = code & (uint8_t)~SHELL_BIND_CHORD_FLAG;
        if (hid >= SHELL_BIND_LETTER_MIN && hid <= SHELL_BIND_LETTER_MAX) {
            snprintf(out, out_size, "^%c", (char)('A' + (hid - SHELL_BIND_LETTER_MIN)));
            return;
        }
    }
    if (code < SHELL_BIND_F1_CODE || code > SHELL_BIND_F12_CODE) {
        snprintf(out, out_size, "0x%02X", code);
        return;
    }
    snprintf(out, out_size, "F%u", (unsigned)(code - SHELL_BIND_F1_CODE + 1));
}

static shell_bind_t *shell_bind_find_slot(uint8_t code)
{
    size_t index;
    for (index = 0; index < SHELL_BIND_MAX; index++) {
        if (s_binds[index].used && s_binds[index].key == code) {
            return &s_binds[index];
        }
    }
    return NULL;
}

esp_err_t shell_bind_set(uint8_t code, const char *line)
{
    shell_bind_t *slot;

    if (!shell_bind_code_valid(code)) {
        return ESP_ERR_INVALID_ARG;
    }
    batch_shared_take();
    slot = shell_bind_find_slot(code);
    if (line == NULL || line[0] == '\0') {
        if (slot != NULL) {
            memset(slot, 0, sizeof(*slot));
        }
        batch_shared_give();
        return ESP_OK;
    }
    if (slot == NULL) {
        size_t index;
        for (index = 0; index < SHELL_BIND_MAX; index++) {
            if (!s_binds[index].used) {
                slot = &s_binds[index];
                break;
            }
        }
        if (slot == NULL) {
            batch_shared_give();
            return ESP_ERR_NO_MEM;
        }
    }
    slot->used = true;
    slot->key = code;
    snprintf(slot->value, sizeof(slot->value), "%s", line);
    batch_shared_give();
    return ESP_OK;
}

bool shell_bind_lookup_fkey(uint8_t code, char *out, size_t out_size)
{
    shell_bind_t *slot;
    bool found = false;

    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (!shell_bind_code_valid(code)) {
        return false;
    }
    batch_shared_take();
    slot = shell_bind_find_slot(code);
    if (slot != NULL) {
        snprintf(out, out_size, "%s", slot->value);
        found = true;
    }
    batch_shared_give();
    return found;
}

bool shell_bind_lookup_chord(uint8_t key_code, uint8_t modifiers,
                             char *out, size_t out_size)
{
    shell_bind_t *slot;
    uint8_t code;
    bool found = false;

    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    /* Ctrl held with a letter key; ^C never matches (foreground break). */
    if ((modifiers & (SHELL_BIND_MOD_LCTRL | SHELL_BIND_MOD_RCTRL)) == 0 ||
        key_code < SHELL_BIND_LETTER_MIN || key_code > SHELL_BIND_LETTER_MAX ||
        key_code == SHELL_BIND_CHORD_C) {
        return false;
    }
    code = (uint8_t)(SHELL_BIND_CHORD_FLAG | key_code);
    batch_shared_take();
    slot = shell_bind_find_slot(code);
    if (slot != NULL) {
        snprintf(out, out_size, "%s", slot->value);
        found = true;
    }
    batch_shared_give();
    return found;
}

int shell_bind_count(void)
{
    size_t index;
    int count = 0;

    for (index = 0; index < SHELL_BIND_MAX; index++) {
        if (s_binds[index].used) {
            count++;
        }
    }
    return count;
}

bool shell_bind_get_by_index(int index, uint8_t *code_out, char *value_out, size_t value_size)
{
    size_t cursor = 0;
    int seen = 0;

    if (code_out == NULL || value_out == NULL) {
        return false;
    }
    for (cursor = 0; cursor < SHELL_BIND_MAX; cursor++) {
        if (!s_binds[cursor].used) {
            continue;
        }
        if (seen == index) {
            *code_out = s_binds[cursor].key;
            snprintf(value_out, value_size, "%s", s_binds[cursor].value);
            return true;
        }
        seen++;
    }
    return false;
}

/** Write the bind table to the given resolved SD path as `bind` lines. */
static esp_err_t shell_bind_save_to(const char *resolved_path)
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
        uint64_t needed = (uint64_t)SHELL_BIND_MAX *
                          (SHELL_BIND_VALUE_BYTES + 16) + 256;
        uint64_t reclaim = storage_get_file_size(resolved_path);
        if (!storage_check_free_space(needed, reclaim, "bind save")) {
            shell_sd_end(&session, "bind");
            return ESP_ERR_INVALID_STATE;
        }
    }
    file = fopen(tmp, "w");
    if (file == NULL) {
        shell_sd_end(&session, "bind");
        return ESP_ERR_NOT_FOUND;
    }
    for (index = 0; index < SHELL_BIND_MAX; index++) {
        bool used;
        uint8_t code = 0;
        char value[SHELL_BIND_VALUE_BYTES];
        char keyname[8];

        /* Copy out under lock, print/write after: never hold the shared
         * lock across transcript or filesystem calls. */
        batch_shared_take();
        used = s_binds[index].used;
        if (used) {
            code = s_binds[index].key;
            snprintf(value, sizeof(value), "%s", s_binds[index].value);
        }
        batch_shared_give();
        if (!used) {
            continue;
        }
        /* Values containing a double quote cannot round-trip through the
         * batch quoting used by the profile; skip them rather than write a
         * line that would load wrong (same policy as aliases). */
        if (strchr(value, '"') != NULL) {
            shell_bind_format_key(code, keyname, sizeof(keyname));
            shell_print_warning("bind: skipped %s (line contains a double quote)",
                                keyname);
            continue;
        }
        shell_bind_format_key(code, keyname, sizeof(keyname));
        fprintf(file, "bind %s %s\n", keyname, value);
    }
    if (fflush(file) != 0 || fclose(file) != 0) {
        remove(tmp);
        shell_sd_end(&session, "bind");
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* FATFS f_rename refuses to overwrite an existing target: remove it first. */
    if (rename(tmp, resolved_path) != 0) {
        remove(resolved_path);
        if (rename(tmp, resolved_path) != 0) {
            remove(tmp);
            shell_sd_end(&session, "bind");
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    shell_sd_end(&session, "bind");
    return ESP_OK;
}

/** Resolve a bind-profile path (argument or the configured default). */
static esp_err_t shell_bind_resolve_profile(const char *arg, char *out, size_t out_size)
{
    if (arg != NULL && arg[0] != '\0') {
        return shell_fs_resolve_path(arg, out, out_size);
    }
    return shell_fs_resolve_path(SHELL_BIND_PROFILE, out, out_size);
}

/** Join argv[from..argc) with single spaces into a heap line. */
static char *shell_bind_join_line(int argc, char **argv, int from)
{
    size_t cap = SHELL_COMMAND_BYTES;
    size_t used = 0;
    char *buf;
    int i;

    buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    buf[0] = '\0';
    for (i = from; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (used > 0) {
            if (used + 1 >= cap) {
                break;
            }
            buf[used++] = ' ';
        }
        if (used + len >= cap) {
            len = cap - used - 1;
        }
        memcpy(buf + used, argv[i], len);
        used += len;
        buf[used] = '\0';
    }
    return buf;
}

void shell_command_bind(int argc, char **argv)
{
    if (argc == 1) {
        int slot;
        bool any = false;

        if (shell_bind_count() == 0) {
            shell_print_muted("No keys bound (bind F1 <line> or bind ^G <line> to bind one)");
            return;
        }
        for (slot = 0; slot < SHELL_BIND_MAX; slot++) {
            bool used = false;
            uint8_t code = 0;
            char value[SHELL_BIND_VALUE_BYTES];
            char keyname[8];
            batch_shared_take();
            used = s_binds[slot].used;
            if (used) {
                code = s_binds[slot].key;
                snprintf(value, sizeof(value), "%s", s_binds[slot].value);
            }
            batch_shared_give();
            if (!used) {
                continue;
            }
            shell_bind_format_key(code, keyname, sizeof(keyname));
            shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                          keyname, value);
            any = true;
        }
        if (!any) {
            shell_print_muted("No keys bound (bind F1 <line> or bind ^G <line> to bind one)");
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
                shell_print_usage("Usage: bind /save [file]");
                return;
            }
            if (shell_bind_resolve_profile(file_arg, resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("bind: invalid profile path");
                return;
            }
            error = shell_bind_save_to(resolved);
            if (error != ESP_OK) {
                shell_print_error("bind: could not save the profile (%s)", esp_err_to_name(error));
                return;
            }
            shell_print_ok("bind: saved %d bind(s) to %s", shell_bind_count(), resolved);
            return;
        }
        if (shell_text_equals_ignore_case(argv[1], "/load")) {
            char resolved[SHELL_SD_PATH_BYTES];
            const char *file_arg = (argc >= 3) ? argv[2] : NULL;
            esp_err_t error;

            if (argc > 3) {
                shell_print_usage("Usage: bind /load [file]");
                return;
            }
            if (shell_bind_resolve_profile(file_arg, resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("bind: invalid profile path");
                return;
            }
            error = shell_execute_batch_file(resolved, 0, NULL);
            if (error != ESP_OK) {
                shell_print_error("bind: could not load the profile (%s)", esp_err_to_name(error));
                return;
            }
            shell_print_ok("bind: loaded %d bind(s) from %s", shell_bind_count(), resolved);
            return;
        }
        if (shell_text_equals_ignore_case(argv[1], "/clear")) {
            int index;
            if (argc != 2) {
                shell_print_usage("Usage: bind /clear");
                return;
            }
            batch_shared_take();
            for (index = 0; index < SHELL_BIND_MAX; index++) {
                memset(&s_binds[index], 0, sizeof(s_binds[index]));
            }
            batch_shared_give();
            shell_print_ok("bind: all binds cleared");
            return;
        }
        shell_print_error("bind: unknown option %s", argv[1]);
        shell_print_usage("Usage: bind [F1..F12|^A..^Z <line>] | bind unbind <F1..F12|^A..^Z> | bind /save [/load] [file] | bind /clear");
        return;
    }

    /* `bind unbind <key>` clears one slot (^C is never bound: reserved). */
    if (shell_text_equals_ignore_case(argv[1], "unbind")) {
        uint8_t code;
        if (argc != 3 || !shell_bind_parse_key(argv[2], &code)) {
            shell_print_usage("Usage: bind unbind <F1..F12|^A..^Z>");
            return;
        }
        if (code == (uint8_t)(SHELL_BIND_CHORD_FLAG | SHELL_BIND_CHORD_C)) {
            shell_print_error("bind: ^C is reserved for break and cannot be bound");
            return;
        }
        (void)shell_bind_set(code, "");
        shell_print_ok("bind: %s cleared", argv[2]);
        return;
    }

    /* `bind <key> <line...>` binds the rest of the line. */
    {
        uint8_t code;
        char *line;

        if (argc < 3 || !shell_bind_parse_key(argv[1], &code)) {
            shell_print_usage("Usage: bind [F1..F12|^A..^Z <line>] | bind unbind <F1..F12|^A..^Z> | bind /save [/load] [file] | bind /clear");
            return;
        }
        if (code == (uint8_t)(SHELL_BIND_CHORD_FLAG | SHELL_BIND_CHORD_C)) {
            shell_print_error("bind: ^C is reserved for break and cannot be bound");
            return;
        }
        line = shell_bind_join_line(argc, argv, 2);
        if (line == NULL) {
            shell_print_error("bind: out of memory");
            return;
        }
        if (shell_bind_set(code, line) != ESP_OK) {
            free(line);
            shell_print_error("bind: the table is full");
            return;
        }
        free(line);
        shell_print_ok("bind: %s set", argv[1]);
    }
}

/* ========================================================================
 * MACRO RECORDER (macro record/stop/play/status)
 * ========================================================================
 * Captures submitted command lines into a heap buffer for later replay as
 * a batch file. The hook (batch_macro_record_line) runs in the async
 * submit path, so both typed surfaces and touch-tap actions are captured
 * while `macro ...` control lines are never recorded. Short lock holds
 * only (never across transcript or filesystem calls).
 */

typedef struct {
    bool active;
    bool overflowed;
    char file[SHELL_SD_PATH_BYTES];
    char *buf;
    size_t used;
    size_t cap;
} shell_macro_t;

static shell_macro_t s_macro;

void batch_macro_record_line(const char *line)
{
    const char *p;
    size_t len;

    if (line == NULL) {
        return;
    }
    batch_shared_take();
    if (!s_macro.active || s_macro.buf == NULL) {
        batch_shared_give();
        return;
    }
    p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' ||
        (strncasecmp(p, "macro", 5) == 0 &&
         (p[5] == '\0' || isspace((unsigned char)p[5])))) {
        batch_shared_give();
        return;
    }
    len = strlen(line);
    if (s_macro.used + len + 1 >= s_macro.cap) {
        /* Buffer full: auto-stop with the overflow mark (reported by stop
         * and status on the worker, where printing is safe). */
        s_macro.active = false;
        s_macro.overflowed = true;
        batch_shared_give();
        return;
    }
    memcpy(s_macro.buf + s_macro.used, line, len);
    s_macro.used += len;
    s_macro.buf[s_macro.used++] = '\n';
    s_macro.buf[s_macro.used] = '\0';
    batch_shared_give();
}

void batch_macro_status(batch_macro_status_t *out)
{
    if (out == NULL) {
        return;
    }
    batch_shared_take();
    out->active = s_macro.active;
    out->overflowed = s_macro.overflowed;
    out->used = s_macro.used;
    out->capacity = s_macro.cap;
    snprintf(out->file, sizeof(out->file), "%s", s_macro.file);
    batch_shared_give();
}

const char *batch_macro_text(void)
{
    /* Single-word read; the buffer is NUL-terminated under the lock at
     * every mutation, so a lock-free peek is safe for tests and status. */
    if (!s_macro.active && s_macro.buf == NULL) {
        return "";
    }
    return (s_macro.buf != NULL) ? s_macro.buf : "";
}

void batch_macro_discard(void)
{
    batch_shared_take();
    if (s_macro.buf != NULL) {
        free(s_macro.buf);
        s_macro.buf = NULL;
    }
    memset(&s_macro, 0, sizeof(s_macro));
    batch_shared_give();
}

void shell_command_macro(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: macro record [file] | macro stop | macro play <file> | macro status");
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "record")) {
        char resolved[SHELL_SD_PATH_BYTES];
        char *buf;
        const char *file_arg = (argc >= 3) ? argv[2] : SHELL_MACRO_DEFAULT_FILE;

        if (argc > 3) {
            shell_print_usage("Usage: macro record [file]");
            return;
        }
        batch_shared_take();
        if (s_macro.active) {
            batch_shared_give();
            shell_print_error("macro: already recording (macro stop first)");
            return;
        }
        batch_shared_give();
        if (shell_fs_resolve_path(file_arg, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("macro: invalid path %s", file_arg);
            return;
        }
        buf = malloc(SHELL_MACRO_BYTES);
        if (buf == NULL) {
            shell_print_error("macro: out of memory");
            return;
        }
        batch_shared_take();
        if (s_macro.active) {
            /* Won a race with another recorder start; keep the first. */
            batch_shared_give();
            free(buf);
            shell_print_error("macro: already recording (macro stop first)");
            return;
        }
        memset(&s_macro, 0, sizeof(s_macro));
        s_macro.active = true;
        snprintf(s_macro.file, sizeof(s_macro.file), "%s", resolved);
        s_macro.buf = buf;
        s_macro.used = 0;
        s_macro.cap = SHELL_MACRO_BYTES;
        s_macro.buf[0] = '\0';
        batch_shared_give();
        shell_print_ok("macro: recording to %s", resolved);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "stop")) {
        char file[SHELL_SD_PATH_BYTES];
        char *buf;
        size_t used;
        bool overflowed;
        int lines = 0;

        if (argc != 2) {
            shell_print_usage("Usage: macro stop");
            return;
        }
        batch_shared_take();
        if (!s_macro.active && s_macro.buf == NULL) {
            batch_shared_give();
            shell_print_error("macro: not recording");
            return;
        }
        snprintf(file, sizeof(file), "%s", s_macro.file);
        buf = s_macro.buf;
        used = s_macro.used;
        overflowed = s_macro.overflowed;
        memset(&s_macro, 0, sizeof(s_macro));
        batch_shared_give();
        if (buf == NULL || used == 0) {
            free(buf);
            shell_print_muted("macro: nothing recorded");
            return;
        }
        for (size_t i = 0; i < used; i++) {
            if (buf[i] == '\n') {
                lines++;
            }
        }
        if (storage_write_text_file(file, buf) != ESP_OK) {
            free(buf);
            shell_print_error("macro: cannot write %s", file);
            return;
        }
        free(buf);
        if (overflowed) {
            shell_print_warning("macro: buffer filled and recording auto-stopped; %d line(s) -> %s",
                                lines, file);
            return;
        }
        shell_print_ok("macro: %d line(s) -> %s", lines, file);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "play")) {
        char resolved[SHELL_SD_PATH_BYTES];

        if (argc != 3) {
            shell_print_usage("Usage: macro play <file>");
            return;
        }
        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("macro: invalid path %s", argv[2]);
            return;
        }
        if (shell_execute_batch_file(resolved, 0, NULL) != ESP_OK) {
            shell_print_error("macro: cannot play %s", resolved);
            return;
        }
        shell_print_ok("macro: played %s", resolved);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "status")) {
        batch_macro_status_t st;

        if (argc != 2) {
            shell_print_usage("Usage: macro status");
            return;
        }
        batch_macro_status(&st);
        if (st.active) {
            shell_transcript_appendf_ansi(SH_LBL "macro" SH_RST "=" SH_VAL "recording" SH_RST "  "
                                          SH_LBL "file" SH_RST "=" SH_VAL "%s" SH_RST "  "
                                          SH_LBL "used" SH_RST "=" SH_NUM "%u/%u" SH_RST "%s\n",
                                          st.file, (unsigned)st.used, (unsigned)st.capacity,
                                          st.overflowed ? "  (overflowed)" : "");
        } else if (st.overflowed) {
            shell_transcript_appendf_ansi(SH_LBL "macro" SH_RST "=" SH_VAL "stopped (overflow)" SH_RST "  "
                                          SH_LBL "file" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                          st.file);
        } else {
            shell_print_muted("macro: idle (macro record [file] to start)");
        }
        return;
    }
    shell_print_usage("Usage: macro record [file] | macro stop | macro play <file> | macro status");
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

    snapshot = malloc(SHELL_ENV_BYTES);
    if (snapshot == NULL) {
        return false;
    }

    batch_shared_take();
    memcpy(snapshot, s_shell_env_vars, SHELL_ENV_BYTES);
    batch_shared_give();
    s_delayed_stack[s_setlocal_depth] = s_delayed_expansion;
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
    s_delayed_expansion = s_delayed_stack[s_setlocal_depth];

    if (snapshot == NULL) {
        return false;
    }

    batch_shared_take();
    memcpy(s_shell_env_vars, snapshot, SHELL_ENV_BYTES);
    batch_shared_give();
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
 * The scratch buffer lives in the current task's ctx (not a file static)
 * so two workers can expand `%*` concurrently without clobbering each
 * other. Still only consumed by the caller before its next call.
 */
static const char *shell_batch_all_args_string(void)
{
    batch_task_ctx_t *ctx = batch_ctx_current();
    char *all_args = ctx->all_args;
    int index;

    all_args[0] = '\0';

    if (s_active_batch_frame == NULL || s_active_batch_frame->argc <= 1) {
        return all_args;
    }

    for (index = 1; index < s_active_batch_frame->argc; index++) {
        if (index > 1) {
            strncat(all_args, " ", sizeof(ctx->all_args) - strlen(all_args) - 1);
        }
        strncat(all_args, s_active_batch_frame->args[index],
                sizeof(ctx->all_args) - strlen(all_args) - 1);
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
/**
 * Apply cmd.exe `%~` argument modifiers to a raw batch argument value.
 *
 * Pure string surgery: strips one pair of surrounding double quotes, then
 * applies the `f`/`d`/`p`/`n`/`x` modifiers documented in batch.h. The two
 * path-sized locals stay far below the worker-task stack budget (the command
 * worker runs 32 KB; this helper's frame is under 1 KB).
 */
void shell_arg_apply_modifiers(const char *value, const char *mods,
                               char *out, size_t out_size)
{
    char work[SHELL_SD_PATH_BYTES];
    char full[SHELL_SD_PATH_BYTES];
    bool want_f = false, want_d = false, want_p = false;
    bool want_n = false, want_x = false;
    const char *m;
    const char *base;
    const char *slash;
    const char *name;
    const char *dot;
    size_t pos = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (value == NULL) {
        return;
    }
    if (mods == NULL) {
        mods = "";
    }

    for (m = mods; *m != '\0'; m++) {
        switch (tolower((unsigned char)*m)) {
            case 'f': want_f = true; break;
            case 'd': want_d = true; break;
            case 'p': want_p = true; break;
            case 'n': want_n = true; break;
            case 'x': want_x = true; break;
            case 's': break; /* No short names on FATFS; accept and ignore. */
            default: break;  /* Validated by the caller; ignore here. */
        }
    }

    /* Strip one pair of surrounding double quotes (cmd `%~` behavior). */
    {
        size_t vlen = strlen(value);
        const char *src = value;
        size_t slen = vlen;

        if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
            src = value + 1;
            slen = vlen - 2;
        }
        if (slen >= sizeof(work)) {
            slen = sizeof(work) - 1;
        }
        memcpy(work, src, slen);
        work[slen] = '\0';
    }

    base = work;
    if (want_f) {
        if (shell_fs_resolve_path(work, full, sizeof(full)) == ESP_OK) {
            base = full;
        }
    }

    if (!want_d && !want_p && !want_n && !want_x) {
        snprintf(out, out_size, "%s", base);
        return;
    }

    slash = strrchr(base, '/');
    name = (slash != NULL) ? slash + 1 : base;
    dot = strrchr(name, '.');
    if (dot == name) {
        dot = NULL; /* A leading dot is not an extension. */
    }

    /* `d` (drive) contributes nothing: FATFS has no drive letters. */
    if (want_p && slash != NULL) {
        size_t dir_len = (size_t)(slash - base) + 1;

        if (dir_len > out_size - pos - 1) {
            dir_len = out_size - pos - 1;
        }
        memcpy(out + pos, base, dir_len);
        pos += dir_len;
    }
    if (want_n) {
        size_t nlen = (dot != NULL) ? (size_t)(dot - name) : strlen(name);

        if (nlen > out_size - pos - 1) {
            nlen = out_size - pos - 1;
        }
        memcpy(out + pos, name, nlen);
        pos += nlen;
    }
    if (want_x && dot != NULL) {
        size_t xlen = strlen(dot);

        if (xlen > out_size - pos - 1) {
            xlen = out_size - pos - 1;
        }
        memcpy(out + pos, dot, xlen);
        pos += xlen;
    }
    out[pos] = '\0';
}

void shell_expand_substring(const char *value, int start, bool has_len, int len,
                            char *out, size_t out_size)
{
    size_t vlen;
    size_t first;
    size_t count;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (value == NULL) {
        return;
    }
    vlen = strlen(value);

    /* Negative start counts back from the end (cmd.exe parity). */
    if (start < 0) {
        long from_end = (long)vlen + (long)start;
        first = (from_end > 0) ? (size_t)from_end : 0;
    } else {
        first = (size_t)start;
    }
    if (first > vlen) {
        return;
    }
    count = vlen - first;
    if (has_len) {
        if (len < 0) {
            /* Negative length drops that many characters from the end. */
            long keep = (long)count + (long)len;
            count = (keep > 0) ? (size_t)keep : 0;
        } else {
            count = ((size_t)len < count) ? (size_t)len : count;
        }
    }
    if (count > out_size - 1) {
        count = out_size - 1;
    }
    memcpy(out, value + first, count);
    out[count] = '\0';
}

void shell_expand_replace(const char *value, const char *old, const char *new_str,
                          char *out, size_t out_size)
{
    size_t old_len;
    size_t new_len;
    size_t used = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (value == NULL) {
        return;
    }
    if (old == NULL || *old == '\0') {
        snprintf(out, out_size, "%s", value);
        return;
    }
    if (new_str == NULL) {
        new_str = "";
    }
    old_len = strlen(old);
    new_len = strlen(new_str);

    while (*value != '\0' && used + 1 < out_size) {
        /* Case-insensitive match (cmd.exe parity: replacement ignores case). */
        bool match = true;

        for (size_t i = 0; i < old_len; i++) {
            if (tolower((unsigned char)value[i]) != tolower((unsigned char)old[i])) {
                match = false;
                break;
            }
            if (value[i] == '\0') {
                match = false;
                break;
            }
        }
        if (match) {
            size_t room = out_size - 1 - used;
            size_t take = (new_len < room) ? new_len : room;

            memcpy(out + used, new_str, take);
            used += take;
            value += old_len;
        } else {
            out[used++] = *value++;
        }
    }
    out[used] = '\0';
}

/**
 * Resolve a bare `%NAME%` token to its text (ERRORLEVEL, the DOS dynamic
 * variables, or the environment table). The single shared implementation
 * behind plain expansion and the `:` string forms, so `%TIME%` and
 * `%TIME:~0,2%` can never disagree.
 */
static const char *shell_expand_named_value(const char *token, char *errorlevel_buf,
                                            char *pseudo_buf)
{
    if (shell_text_equals_ignore_case(token, "ERRORLEVEL")) {
        snprintf(errorlevel_buf, 16, "%d", s_errorlevel);
        return errorlevel_buf;
    }
    if (shell_text_equals_ignore_case(token, "DATE") ||
        shell_text_equals_ignore_case(token, "TIME") ||
        shell_text_equals_ignore_case(token, "RANDOM") ||
        shell_text_equals_ignore_case(token, "CD")) {
        /* DOS/cmd-style dynamic variables. They always win over a user
         * variable of the same name. */
        if (shell_text_equals_ignore_case(token, "RANDOM")) {
            /* cmd.exe %RANDOM% is 0..32767. */
            snprintf(pseudo_buf, 40, "%d", (int)(esp_random() & 0x7FFF));
            return pseudo_buf;
        }
        if (shell_text_equals_ignore_case(token, "CD")) {
            const char *cwd = shell_get_cwd();
            return (cwd != NULL) ? cwd : "";
        }
        {
            struct tm lt = time_get_local();
            if (shell_text_equals_ignore_case(token, "DATE")) {
                snprintf(pseudo_buf, 40, "%02d-%02d-%04d",
                         lt.tm_mon + 1, lt.tm_mday, lt.tm_year + 1900);
            } else {
                snprintf(pseudo_buf, 40, "%02d:%02d:%02d",
                         lt.tm_hour, lt.tm_min, lt.tm_sec);
            }
            return pseudo_buf;
        }
    }
    {
        const char *value = shell_env_get(token);
        if (value == NULL) {
            /* cmd.exe parity: an undefined variable expands to the empty
             * string, so the DOS `if "%var%"==""` idiom works. */
            return "";
        }
        return value;
    }
}

/**
 * Resolve one `%...%` / `!...!` token (without the delimiters) to its text:
 * plain names via shell_expand_named_value(), `NAME:~start[,len]` via
 * shell_expand_substring(), `NAME:old=new` via shell_expand_replace().
 * Scratch comes from the caller (@p pseudo_buf 40 bytes, @p strbuf value
 * sized); the result points at one of the scratches, the environment slot,
 * or a literal. The single shared implementation behind both delimiter
 * forms, so `%TIME:~0,2%` and `!TIME:~0,2!` can never disagree.
 */
static const char *shell_expand_token_text(char *token, char *errorlevel_buf,
                                           char *pseudo_buf, char *strbuf,
                                           size_t strbuf_size)
{
    char *colon = strchr(token, ':');

    if (colon == NULL) {
        return shell_expand_named_value(token, errorlevel_buf, pseudo_buf);
    }
    if (colon[1] == '~') {
        /* `NAME:~start[,len]`: slice a substring. */
        const char *base = NULL;
        const char *spec = colon + 2;
        char *num_end = NULL;
        long start;
        bool has_len = false;
        long len = 0;

        *colon = '\0';
        base = shell_expand_named_value(token, errorlevel_buf, pseudo_buf);
        start = strtol(spec, &num_end, 10);
        if (num_end == spec) {
            strbuf[0] = '\0';
        } else {
            spec = num_end;
            while (*spec == ' ') {
                spec++;
            }
            if (*spec == ',') {
                spec++;
                len = strtol(spec, &num_end, 10);
                if (num_end == spec) {
                    strbuf[0] = '\0';
                    spec = NULL;
                } else {
                    has_len = true;
                    spec = num_end;
                }
            }
            if (spec != NULL) {
                while (*spec == ' ') {
                    spec++;
                }
                if (*spec != '\0') {
                    strbuf[0] = '\0';
                    spec = NULL;
                }
            }
            if (spec != NULL) {
                shell_expand_substring(base, (int)start, has_len,
                                       (int)len, strbuf, strbuf_size);
            }
        }
        return strbuf;
    }
    /* `NAME:old=new`: replace every occurrence. */
    {
        const char *base = NULL;
        char *equals;

        *colon = '\0';
        base = shell_expand_named_value(token, errorlevel_buf, pseudo_buf);
        equals = strchr(colon + 1, '=');
        if (equals == NULL) {
            strbuf[0] = '\0';
        } else {
            *equals = '\0';
            shell_expand_replace(base, colon + 1, equals + 1,
                                 strbuf, strbuf_size);
        }
        return strbuf;
    }
}

void shell_expand_variables(const char *input, char *output, size_t output_size)
{
    size_t out_index = 0;
    shell_quote_state_t quote = SHELL_QUOTE_NONE;
    char errorlevel_buf[16];   /* %ERRORLEVEL% renders here; copied immediately */
    char modbuf[SHELL_SD_PATH_BYTES]; /* %~ modifiers render here; copied immediately */
    char strbuf[SHELL_ENV_VALUE_BYTES]; /* %VAR:...% / !VAR...! forms render here */

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
            } else if (input[1] == '~') {
                /* cmd.exe `%~[modifiers]N`: modifier letters f/d/p/n/x/s
                 * followed by exactly one digit. Anything else stays literal,
                 * so a stray `%~` can never inject an argument. The result is
                 * rendered into modbuf (see shell_arg_apply_modifiers) and
                 * copied immediately, like %ERRORLEVEL% above. */
                const char *mp = input + 2;
                char modstr[8];
                size_t modlen = 0;
                bool mods_ok = true;

                while (*mp != '\0' && !isdigit((unsigned char)*mp)) {
                    char c = (char)tolower((unsigned char)*mp);

                    if (c == 'f' || c == 'd' || c == 'p' ||
                        c == 'n' || c == 'x' || c == 's') {
                        if (modlen + 1 < sizeof(modstr)) {
                            modstr[modlen++] = c;
                        } else {
                            mods_ok = false;
                            break;
                        }
                    } else {
                        mods_ok = false;
                        break;
                    }
                    mp++;
                }
                modstr[modlen] = '\0';
                if (mods_ok && isdigit((unsigned char)*mp)) {
                    int arg_index = *mp - '0';
                    const char *raw = "";

                    if (s_active_batch_frame != NULL && arg_index >= 0 &&
                        arg_index < s_active_batch_frame->argc) {
                        raw = s_active_batch_frame->args[arg_index];
                    }
                    shell_arg_apply_modifiers(raw, modstr, modbuf, sizeof(modbuf));
                    replacement = modbuf;
                    after = mp + 1;
                }
                /* else: no valid digit follows — the `%` is copied literally below. */
            } else {
                /* Environment variable `%VAR%` (or a literal `%%`), plus the
                 * DOS string forms `%VAR:~start[,len]%` and `%VAR:old=new%`:
                 * needs the closing `%`. An undefined name expands to the
                 * empty string (cmd.exe parity), so the DOS `if "%var%"=="`
                 * idiom works. `%ERRORLEVEL%` expands to the current
                 * errorlevel as a decimal string. */
                const char *end = strchr(input + 1, '%');
                if (end != NULL) {
                    size_t token_len = (size_t)(end - (input + 1));
                    char token[SHELL_ENV_NAME_BYTES];

                    if (token_len == 0) {
                        replacement = "%";
                    } else if (token_len < sizeof(token)) {
                        char pseudo_buf[40];

                        memcpy(token, input + 1, token_len);
                        token[token_len] = '\0';
                        replacement = shell_expand_token_text(token, errorlevel_buf,
                                                              pseudo_buf, strbuf,
                                                              sizeof(strbuf));
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

        /* Delayed expansion `!VAR!` (cmd.exe parity): only when a
         * `setlocal enabledelayedexpansion` scope is open. Same token forms
         * as `%VAR%` through the shared helper; a missing closing `!` (or a
         * disabled scope) copies the `!` literally. The caret handler above
         * already consumed `^!` pairs, so they never reach here. */
        if (s_delayed_expansion && *input == '!') {
            const char *replacement = NULL;
            const char *after = NULL;
            const char *end = strchr(input + 1, '!');

            if (end != NULL) {
                size_t token_len = (size_t)(end - (input + 1));
                char token[SHELL_ENV_NAME_BYTES];

                if (token_len > 0 && token_len < sizeof(token)) {
                    char pseudo_buf[40];

                    memcpy(token, input + 1, token_len);
                    token[token_len] = '\0';
                    replacement = shell_expand_token_text(token, errorlevel_buf,
                                                          pseudo_buf, strbuf,
                                                          sizeof(strbuf));
                } else if (token_len == 0) {
                    replacement = "!";
                }
                after = end + 1;
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
 * BACKGROUND TASK SLOTS (`start` pool)
 * ======================================================================== */

static SemaphoreHandle_t s_bg_lock = NULL;

static void batch_bg_lock_init(void)
{
    if (s_bg_lock == NULL) {
        s_bg_lock = xSemaphoreCreateMutex();
    }
}

int batch_bg_alloc(void)
{
    int i;
    int slot = -1;

    batch_bg_lock_init();
    if (s_bg_lock != NULL) {
        xSemaphoreTake(s_bg_lock, portMAX_DELAY);
    }
    for (i = 1; i < 1 + P4_CONFIG_BG_TASKS; i++) {
        if (!s_batch_ctx[i].in_use) {
            s_batch_ctx[i].in_use = true;
            s_batch_ctx[i].task = NULL;
            s_batch_ctx[i].kill_requested = false;
            /* Fresh execution state (BSS may hold a previous run). */
            s_batch_ctx[i].active_frame = NULL;
            s_batch_ctx[i].errorlevel = 0;
            s_batch_ctx[i].goto_label[0] = '\0';
            s_batch_ctx[i].goto_pending = false;
            s_batch_ctx[i].goto_eof = false;
            s_batch_ctx[i].stop_mode = BATCH_STOP_NONE;
            s_batch_ctx[i].setlocal_depth = 0;
            slot = i;
            break;
        }
    }
    if (s_bg_lock != NULL) {
        xSemaphoreGive(s_bg_lock);
    }
    return slot;
}

void batch_bg_bind(int slot)
{
    if (slot < 1 || slot >= 1 + P4_CONFIG_BG_TASKS) {
        return;
    }
    s_batch_ctx[slot].task = xTaskGetCurrentTaskHandle();
}

void batch_bg_release(int slot)
{
    if (slot < 1 || slot >= 1 + P4_CONFIG_BG_TASKS) {
        return;
    }
    batch_bg_lock_init();
    if (s_bg_lock != NULL) {
        xSemaphoreTake(s_bg_lock, portMAX_DELAY);
    }
    s_batch_ctx[slot].in_use = false;
    s_batch_ctx[slot].task = NULL;
    s_batch_ctx[slot].kill_requested = false;
    if (s_bg_lock != NULL) {
        xSemaphoreGive(s_bg_lock);
    }
}

void batch_bg_request_kill(int slot)
{
    if (slot < 1 || slot >= 1 + P4_CONFIG_BG_TASKS) {
        return;
    }
    s_batch_ctx[slot].kill_requested = true;
}

bool batch_bg_kill_requested(void)
{
    return batch_ctx_current()->kill_requested;
}

bool batch_bg_is_background(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    int i;

    if (me == NULL) {
        return false;
    }
    for (i = 1; i < 1 + P4_CONFIG_BG_TASKS; i++) {
        if (s_batch_ctx[i].in_use && s_batch_ctx[i].task == me) {
            return true;
        }
    }
    return false;
}

/* ========================================================================
 * ENVIRONMENT COMMANDS: set, path, echo
 * ======================================================================== */

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
    if (argv[1][0] == '/' && argv[1][1] != '\0' && argv[1][2] == '\0') {
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

/* Forward declaration (defined below shell_command_echo). */
static void shell_echo_emit_rendered(const char *text);

void shell_command_echo(int argc, char **argv)
{
    /* The joined text can be a full interactive command line (up to
     * P4_CONFIG_COMMAND_BYTES), so the buffer is command-sized and
     * heap-allocated — `echo` runs on the recursive batch path (every batch
     * line dispatches through it), and a fixed stack buffer of this size
     * would overflow the worker task's stack when nested. */
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

    text = malloc(SHELL_COMMAND_BYTES);
    if (text == NULL) {
        shell_transcript_append_text("echo: out of memory\n");
        return;
    }

    /* `echo /raw ...` bypasses Markdown auto-render (documented). */
    if (argc > 2 && shell_text_equals_ignore_case(argv[1], "/raw")) {
        shell_join_args(argv, 2, argc, text, SHELL_COMMAND_BYTES);
        shell_transcript_appendf("%s\n", text);
        free(text);
        return;
    }
    shell_join_args(argv, 1, argc, text, SHELL_COMMAND_BYTES);
    shell_echo_emit_rendered(text);
    free(text);
}

/** Emit echo output with per-line Markdown auto-render (unless off or the
 * line came from `echo /raw`). Rendered lines go through the ANSI path so
 * spans and serial terminals style them; untouched lines stay verbatim. */
static void shell_echo_emit_rendered(const char *text)
{
    if (text == NULL) {
        return;
    }
    if (!markdown_get_auto()) {
        shell_transcript_appendf("%s\n", text);
        return;
    }
    {
        char *rendered = malloc(SHELL_COMMAND_BYTES * 2);
        const char *p = text;
        if (rendered == NULL) {
            shell_transcript_appendf("%s\n", text);
            return;
        }
        /* Echo text is one logical line, but split defensively: any embedded
         * newline renders per line so a block can never leak across. */
        while (*p != '\0') {
            const char *e = p;
            while (*e != '\0' && *e != '\n' && *e != '\r') {
                e++;
            }
            {
                size_t linelen = (size_t)(e - p);
                /* Line-sized copy: echo text itself is command-sized, so a
                 * line never exceeds the buffer. */
                char *line = malloc(linelen + 1);
                if (line == NULL) {
                    break;
                }
                memcpy(line, p, linelen);
                line[linelen] = '\0';
                if (markdown_render_line(line, rendered, SHELL_COMMAND_BYTES * 2)) {
                    shell_transcript_append_ansi(rendered);
                } else {
                    shell_transcript_appendf("%s\n", line);
                }
                free(line);
            }
            while (*e == '\n' || *e == '\r') {
                e++;
            }
            p = e;
        }
        free(rendered);
    }
}

void shell_command_echo_text(char *line)
{
    char *p;
    char *text;

    if (line == NULL) {
        return;
    }

    /* Skip the leading "echo" word (case-insensitive) and the space after it. */
    p = line + 4;
    if (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    p = shell_trim(p);

    /* Bare `echo` reports the current batch-echo state. */
    if (*p == '\0') {
        shell_transcript_appendf_ansi(SH_LBL "ECHO is" SH_RST " " SH_VAL "%s" SH_RST "\n",
                 (s_active_batch_frame != NULL && !s_active_batch_frame->echo_enabled) ? "off" : "on");
        return;
    }

    /* `echo on` / `echo off` toggle the batch echo state. Checked on the raw
     * value (before caret-unescaping) so `echo ^on` prints "on" literally
     * instead of toggling the flag. */
    if (s_active_batch_frame != NULL &&
        (shell_text_equals_ignore_case(p, "on") || shell_text_equals_ignore_case(p, "off"))) {
        s_active_batch_frame->echo_enabled = shell_text_equals_ignore_case(p, "on");
        return;
    }

    /* The remainder is printed verbatim, but caret escapes are consumed
     * (`echo a^&b` -> `a&b`), matching DOS. Quotes are preserved. It can be a
     * full interactive command line (up to P4_CONFIG_COMMAND_BYTES), and
     * `echo` runs on the recursive batch path, so the copy is heap-allocated. */
    shell_unescape_carets_in_place(p);
    /* `echo /raw ...` bypasses Markdown auto-render (documented). */
    if (shell_text_equals_ignore_case(p, "/raw") ||
        strncasecmp(p, "/raw ", 5) == 0) {
        const char *raw = p + 4;
        if (*raw != '\0') {
            raw++;
        }
        text = malloc(SHELL_COMMAND_BYTES);
        if (text == NULL) {
            shell_transcript_append_text("echo: out of memory\n");
            return;
        }
        snprintf(text, SHELL_COMMAND_BYTES, "%s", raw);
        shell_transcript_appendf("%s\n", text);
        free(text);
        return;
    }
    text = malloc(SHELL_COMMAND_BYTES);
    if (text == NULL) {
        shell_transcript_append_text("echo: out of memory\n");
        return;
    }
    snprintf(text, SHELL_COMMAND_BYTES, "%s", p);
    shell_echo_emit_rendered(text);
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

    if (!filetype_is_executable(filetype_of(command_name))) {
        static const char *const script_exts[] = { ".bat", ".cmd" };
        size_t e;

        /* DOS parity: an extensionless name also probes the sibling script
         * type, so .cmd files run exactly like .bat ones. */
        for (e = 0; e < 2 && !found; e++) {
            snprintf(with_ext, SHELL_SD_PATH_BYTES, "%s%s", command_name, script_exts[e]);
            error = shell_fs_resolve_path(with_ext, candidate, SHELL_SD_PATH_BYTES);
            if (error == ESP_OK && shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                snprintf(resolved_path, resolved_path_size, "%s", candidate);
                found = true;
                goto done;
            }
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

                if (!filetype_is_executable(filetype_of(command_name))) {
                    static const char *const script_exts[] = { ".bat", ".cmd" };
                    size_t e;

                    for (e = 0; e < 2 && !found; e++) {
                        written = snprintf(candidate, SHELL_SD_PATH_BYTES, "%s/%s%s", dir_path, command_name, script_exts[e]);
                        if (written > 0 && (size_t)written < SHELL_SD_PATH_BYTES &&
                            shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                            snprintf(resolved_path, resolved_path_size, "%s", candidate);
                            found = true;
                            goto done;
                        }
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

/** True while the frame is executing inside a `call :label` block. */
static bool shell_in_label_call(const shell_batch_frame_t *frame)
{
    return frame != NULL && frame->label_call_depth > 0;
}

/**
 * Enter a `call :label` block: save the caller's arguments so they can be
 * restored on return, then bind the subroutine's own argument set. `%0` stays
 * the script name; the caller's arguments (`argv[0..argc-1]`) become
 * `%1..%9`. Returns false when the scope stack is full or allocation fails.
 */
static bool shell_label_call_enter(shell_batch_frame_t *frame, long resume_pos,
                                   int argc, char **argv)
{
    shell_label_call_t *slot;
    int index;

    if (frame == NULL ||
        frame->label_call_depth >= (int)(sizeof(frame->label_calls) / sizeof(frame->label_calls[0]))) {
        return false;
    }

    slot = &frame->label_calls[frame->label_call_depth];
    slot->args = malloc(sizeof(*slot->args) * SHELL_BATCH_ARGS_MAX);
    if (slot->args == NULL) {
        return false;
    }
    memcpy(slot->args, frame->args, sizeof(*slot->args) * SHELL_BATCH_ARGS_MAX);
    slot->argc = frame->argc;
    slot->resume_pos = resume_pos;
    frame->label_call_depth++;

    frame->argc = MIN(argc + 1, SHELL_BATCH_ARGS_MAX);
    for (index = 0; index < argc && index + 1 < SHELL_BATCH_ARGS_MAX; index++) {
        snprintf(frame->args[index + 1], SHELL_COMMAND_BYTES, "%s", argv[index]);
    }
    for (index = argc + 1; index < SHELL_BATCH_ARGS_MAX; index++) {
        frame->args[index][0] = '\0';
    }
    return true;
}

/**
 * Leave the innermost `call :label` block, restoring the caller's arguments.
 * Returns the position to resume at, or -1 when no scope is active.
 */
static long shell_label_call_leave(shell_batch_frame_t *frame)
{
    shell_label_call_t *slot;
    long resume;

    if (!shell_in_label_call(frame)) {
        return -1;
    }
    frame->label_call_depth--;
    slot = &frame->label_calls[frame->label_call_depth];
    resume = slot->resume_pos;
    memcpy(frame->args, slot->args, sizeof(*slot->args) * SHELL_BATCH_ARGS_MAX);
    frame->argc = slot->argc;
    free(slot->args);
    slot->args = NULL;
    return resume;
}

/** Store a jump target normalised (leading `:` stripped) and length-capped to
 *  the same width the label scanner stores, so a long target can still match a
 *  truncated label-table name instead of never being found. */
static void shell_store_goto_target(const char *label)
{
    if (label != NULL && label[0] == ':') {
        label++;
    }
    snprintf(s_goto_label, sizeof(s_goto_label), "%.*s",
             (int)(SHELL_BATCH_LABEL_BYTES - 1), (label != NULL) ? label : "");
}

/**
 * Shared by `call :label`, `gosub :label`, and `on … gosub`/`on … call`:
 * verify the target exists in the current frame, push a `call :label` scope
 * with the given arguments, and arm the jump. Prints the cmd.exe-parity error
 * and sets ERRORLEVEL 1 on failure (a missing label is fatal to the jump, not
 * to the frame, for the `call`-family verbs). `verb` names the caller for the
 * diagnostics.
 */
static bool shell_local_subroutine_begin(const char *verb, const char *label,
                                         int argc, char **argv)
{
    shell_batch_frame_t *frame = s_active_batch_frame;

    if (frame == NULL) {
        shell_transcript_appendf("%s: :label is only valid inside batch files\n", verb);
        batch_set_errorlevel(1);
        return false;
    }
    if (shell_find_label_pos(frame, label) < 0) {
        shell_transcript_appendf("The system cannot find the batch label specified - %s\n", label);
        batch_set_errorlevel(1);
        return false;
    }
    if (!shell_label_call_enter(frame, shell_frame_tell(frame), argc, argv)) {
        shell_print_error("%s: too many nested label calls", verb);
        batch_set_errorlevel(1);
        return false;
    }
    shell_store_goto_target(label);
    s_goto_pending = true;
    return true;
}

/**
 * Shared by `call` and `gosub`: run `file.bat::routine [args]` in an isolated
 * scope that starts at the routine label. Returns false when @p argv[1] is not
 * the `file::routine` form (so the caller can handle other forms).
 */
static bool shell_library_routine_call(const char *verb, int argc, char **argv)
{
    const char *double_colon = strstr(argv[1], "::");
    char lib_path[SHELL_SD_PATH_BYTES];
    char batch_path[SHELL_SD_PATH_BYTES];
    char routine[SHELL_BATCH_LABEL_BYTES];
    size_t path_len;

    if (double_colon == NULL) {
        return false;
    }
    path_len = (size_t)(double_colon - argv[1]);
    if (path_len == 0 || path_len >= sizeof(lib_path) ||
        snprintf(routine, sizeof(routine), "%s", double_colon + 2) >= (int)sizeof(routine) ||
        routine[0] == '\0') {
        shell_print_usage("Usage: %s <file.bat>::<routine> [args]", verb);
        batch_set_errorlevel(2);
        return true;
    }
    memcpy(lib_path, argv[1], path_len);
    lib_path[path_len] = '\0';

    if (!shell_resolve_batch_path(lib_path, batch_path, sizeof(batch_path))) {
        shell_transcript_appendf("%s: library not found %s\n", verb, lib_path);
        batch_set_errorlevel(1);
        return true;
    }
    shell_batch_run_internal(batch_path, routine, argc - 2, &argv[2]);
    return true;
}

void shell_command_call(int argc, char **argv)
{
    char batch_path[SHELL_SD_PATH_BYTES];

    if (argc < 2) {
        shell_print_usage("Usage: call <file.bat> [args] | call <file.bat>::<routine> [args] | call :label [args]");
        batch_set_errorlevel(2);
        return;
    }

    /* `call <file.bat>::<routine> [args]` invokes a labelled routine from a
     * shared library of batch routines: the external file is loaded and
     * execution starts at `:routine`, isolated from the caller's variables
     * (the routine's scope is unwound when it returns). */
    if (shell_library_routine_call("call", argc, argv)) {
        return;
    }

    /* `call :label` runs a labelled block in the current batch file as a
     * subroutine: the block runs until `return` / `exit /b` / `goto :eof` (or
     * end of file) and then execution resumes at the line after the call. */
    if (argv[1][0] == ':') {
        (void)shell_local_subroutine_begin("call", argv[1] + 1, argc - 2, &argv[2]);
        return;
    }

    if (!shell_resolve_batch_path(argv[1], batch_path, sizeof(batch_path))) {
        shell_transcript_appendf("call: batch file not found %s\n", argv[1]);
        batch_set_errorlevel(1);
        return;
    }

    shell_execute_batch_file(batch_path, argc - 2, &argv[2]);
}

/** `gosub` — BASIC-named sibling of `call :label` / `call file::routine`. */
void shell_command_gosub(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: gosub :label [args] | gosub <file.bat>::<routine> [args]");
        batch_set_errorlevel(2);
        return;
    }
    if (shell_library_routine_call("gosub", argc, argv)) {
        return;
    }
    if (argv[1][0] == ':') {
        (void)shell_local_subroutine_begin("gosub", argv[1] + 1, argc - 2, &argv[2]);
        return;
    }
    shell_print_usage("Usage: gosub :label [args] | gosub <file.bat>::<routine> [args]");
    batch_set_errorlevel(2);
}

/**
 * `return [code]` — BASIC-named sibling of `goto :eof`. Inside a `call :label`
 * / `gosub` scope it returns to the caller; at the top level of a batch file it
 * ends the frame (the documented `RETURN -> goto :eof` mapping). An optional
 * code sets ERRORLEVEL first.
 */
void shell_command_return(int argc, char **argv)
{
    if (s_active_batch_frame == NULL) {
        shell_transcript_append_text("return: only valid inside batch files\n");
        batch_set_errorlevel(1);
        return;
    }
    if (argc >= 2) {
        batch_set_errorlevel(atoi(argv[1]));
    }
    s_goto_eof = true;
    s_goto_pending = true;
    s_goto_label[0] = '\0';
}

/**
 * Parse `on <expr> goto|gosub|call <label>[,<label>...]` into @p expr (the
 * expression tokens joined by spaces) and @p targets (the raw target list).
 * Sets @p is_gosub_out for the `gosub`/`call` form. Pure; returns false on a
 * malformed command (missing expression or targets). Shared by the executor and
 * the unit tests (via `batch_on_parse`).
 */
static bool shell_on_parse(int argc, char **argv, char *expr, size_t expr_size,
                           bool *is_gosub_out, char *targets, size_t targets_size)
{
    int kw = -1;
    bool is_gosub = false;
    int i;

    if (argv == NULL || expr == NULL || targets == NULL || expr_size == 0 || targets_size == 0) {
        return false;
    }
    for (i = 1; i < argc; i++) {
        if (shell_text_equals_ignore_case(argv[i], "goto") ||
            shell_text_equals_ignore_case(argv[i], "gosub") ||
            shell_text_equals_ignore_case(argv[i], "call")) {
            kw = i;
            is_gosub = !shell_text_equals_ignore_case(argv[i], "goto");
            break;
        }
    }
    if (kw < 2 || kw + 1 >= argc) {
        return false;
    }
    expr[0] = '\0';
    for (i = 1; i < kw; i++) {
        snprintf(expr + strlen(expr), expr_size - strlen(expr), "%s%s",
                 (i > 1) ? " " : "", argv[i]);
    }
    targets[0] = '\0';
    for (i = kw + 1; i < argc; i++) {
        snprintf(targets + strlen(targets), targets_size - strlen(targets), "%s%s",
                 (i > kw + 1) ? " " : "", argv[i]);
    }
    if (is_gosub_out != NULL) {
        *is_gosub_out = is_gosub;
    }
    return true;
}

/**
 * Select the 1-based @p index entry of a comma-separated @p targets list,
 * splitting @p targets in place (commas and surrounding whitespace). Returns a
 * pointer to the entry, or NULL when @p index is out of range (BASIC: fall
 * through). Pure; only touches the caller's buffer.
 */
static const char *shell_on_select(char *targets, int index)
{
    char *seg = targets;
    char *selected = NULL;
    int count = 0;

    while (seg != NULL) {
        char *comma = strchr(seg, ',');
        char *s = seg;
        char *end;

        if (comma != NULL) {
            *comma = '\0';
        }
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        end = s + strlen(s);
        while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        count++;
        if (count == index) {
            selected = s;
        }
        seg = (comma != NULL) ? comma + 1 : NULL;
    }
    if (index < 1 || index > count) {
        return NULL;
    }
    return selected;
}

void shell_command_on(int argc, char **argv)
{
    char expr[SHELL_COMMAND_BYTES];
    char targets[SHELL_COMMAND_BYTES];
    const char *err = NULL;
    const char *selected;
    int32_t index = 0;
    bool is_gosub = false;

    if (s_active_batch_frame == NULL) {
        shell_transcript_append_text("on: only valid inside batch files\n");
        batch_set_errorlevel(1);
        return;
    }
    if (!shell_on_parse(argc, argv, expr, sizeof(expr), &is_gosub, targets, sizeof(targets))) {
        shell_print_usage("Usage: on <expr> goto|gosub <label>[,<label>...]");
        batch_set_errorlevel(2);
        return;
    }
    if (!shell_expr_evaluate(expr, &index, &err)) {
        shell_transcript_appendf("on: %s\n", (err != NULL) ? err : "invalid expression");
        batch_set_errorlevel(1);
        return;
    }
    selected = shell_on_select(targets, (int)index);
    if (selected == NULL) {
        /* Out of range: fall through without jumping (BASIC). */
        batch_set_errorlevel(0);
        return;
    }
    if (selected[0] == ':') {
        selected++;
    }
    if (selected[0] == '\0') {
        shell_print_error("on: empty dispatch target");
        batch_set_errorlevel(1);
        return;
    }
    if (is_gosub) {
        (void)shell_local_subroutine_begin("on", selected, 0, NULL);
    } else {
        shell_store_goto_target(selected);
        s_goto_pending = true;
    }
}

/**
 * `switch <value> <match>:<label> [...] [/d:<label>]` — string-match dispatch
 * to a batch label (the string twin of `on ... goto`, which dispatches on a
 * 1-based number). The first case-insensitive match jumps; otherwise the
 * `/d:` default jumps; otherwise the line falls through with ERRORLEVEL 1.
 * A missing label behaves exactly like `goto` to a missing label (the frame
 * aborts at the jump). Only valid inside batch files.
 */
void shell_command_switch(int argc, char **argv)
{
    const char *def = NULL;
    const char *target = NULL;
    char **cases = NULL;
    int ncases = 0;
    int i;

    if (s_active_batch_frame == NULL) {
        shell_transcript_append_text("switch: only valid inside batch files\n");
        batch_set_errorlevel(1);
        return;
    }
    if (argc < 3) {
        shell_print_usage("Usage: switch <value> <match>:<label> [...] [/d:<label>]");
        batch_set_errorlevel(2);
        return;
    }
    /* Split off the /d: default first so it never competes as a case (a match
     * containing a colon splits on its LAST colon, so `C:\\x:lbl` works). */
    cases = malloc(sizeof(char *) * (size_t)(argc - 2));
    if (cases == NULL) {
        shell_transcript_append_text("switch: out of memory\n");
        batch_set_errorlevel(1);
        return;
    }
    for (i = 2; i < argc; i++) {
        if (strncasecmp(argv[i], "/d:", 3) == 0 && argv[i][3] != '\0') {
            def = argv[i] + 3;
        } else {
            cases[ncases++] = argv[i];
        }
    }
    target = shell_switch_select(argv[1], cases, ncases);
    if (target == NULL) {
        target = def;
    }
    free(cases);
    if (target == NULL) {
        batch_set_errorlevel(1);
        return;
    }
    if (target[0] == '\0') {
        shell_print_error("switch: empty dispatch target");
        batch_set_errorlevel(1);
        return;
    }
    /* Same jump machinery as `goto` (leading `:` accepted, length-capped, the
     * executor aborts the frame on a missing label). */
    shell_store_goto_target(target);
    s_goto_pending = true;
    batch_set_errorlevel(0);
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
    /* Normalize the label: accept both `goto skip` and `goto :skip` (the
     * documented form), length-capped to the label-table width. */
    shell_store_goto_target(argv[1]);
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
    } else if (shell_text_equals_ignore_case(argv[arg_idx], "defined")) {
        arg_idx++;
        if (arg_idx >= argc) {
            shell_transcript_append_text("if: expected variable name after defined\n");
            return;
        }
        condition = (shell_env_get(argv[arg_idx]) != NULL);
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

    if (arg_idx < argc) {
        /* Execute the command part. This may be a bare command (the whole
         * remainder) or the parenthesized block form
         * `(true-cmd) else (false-cmd)`. The joined text is line-width and
         * `if` re-enters the pipeline (recursive batch path), so the buffer is
         * heap-allocated and freed after the nested run. */
        char *cmd = malloc(SHELL_COMMAND_BYTES);
        char *true_cmd = NULL;
        char *false_cmd = NULL;
        bool paren_form = false;
        char *run = NULL;

        if (cmd == NULL) {
            shell_transcript_append_text("if: out of memory\n");
            return;
        }
        shell_join_args(argv, arg_idx, argc, cmd, SHELL_COMMAND_BYTES);

        {
            char *p = cmd;

            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '(') {
                /* Balanced-paren scan: extract (true-cmd), then an optional
                 * `else (false-cmd)`. */
                int depth = 0;
                char *q;
                char *close_true = NULL;

                paren_form = true;
                true_cmd = p + 1;
                for (q = true_cmd; *q != '\0'; q++) {
                    if (*q == '(') {
                        depth++;
                    } else if (*q == ')') {
                        if (depth == 0) {
                            close_true = q;
                            break;
                        }
                        depth--;
                    }
                }
                if (close_true != NULL) {
                    *close_true = '\0';
                    q = close_true + 1;
                    while (*q == ' ' || *q == '\t') {
                        q++;
                    }
                    if (strncmp(q, "else", 4) == 0 &&
                        (q[4] == '\0' || isspace((unsigned char)q[4]))) {
                        char *s2;

                        q += 4;
                        while (*q == ' ' || *q == '\t') {
                            q++;
                        }
                        if (*q == '(') {
                            char *scan = q + 1;

                            s2 = scan;
                            depth = 0;
                            for (; *scan != '\0'; scan++) {
                                if (*scan == '(') {
                                    depth++;
                                } else if (*scan == ')') {
                                    if (depth == 0) {
                                        *scan = '\0';
                                        break;
                                    }
                                    depth--;
                                }
                            }
                            false_cmd = s2;
                        }
                    }
                }
            }
        }

        if (paren_form) {
            run = condition ? true_cmd : false_cmd;
        } else if (condition) {
            run = cmd;
        }

        if (run != NULL && run[0] != '\0') {
            batch_run_nested(run);
        }
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

    /* Arm the key wait BEFORE the prompt is printed. The prompt text is queued
     * for the UART console task, so a key typed as soon as the prompt appears
     * can reach the console reader before shell_key_wait_begin() would have
     * run if it came after the print - and would then be dispatched as a
     * command instead of answering the wait. */
    shell_key_wait_begin();
    shell_transcript_appendf_ansi(SH_MUTE "Press any key to continue . . . " SH_RST "\n");

    if (!shell_wait_for_key(SHELL_KEY_WAIT_TIMEOUT_MS, NULL)) {
        shell_transcript_append_text("pause: timed out waiting for a key\n");
        shell_record_warningf("pause", "Timed out waiting for a keypress");
    }
    shell_key_wait_end();
}

void shell_command_choice(int argc, char **argv)
{
    /* The key list holds only single-char keys (/C:YNC), so it is a small
     * stack buffer — never command-sized. */
    char options[P4_CONFIG_CHOICE_KEY_MAX];
    char *message = NULL;
    const char *default_key = NULL;
    char timeout_key[2] = {'\0', '\0'}; /* /T default key (stack: two tasks
                                         * may parse choice concurrently) */
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

    /* Arm the key wait BEFORE the prompt is rendered so a key typed as soon
     * as the prompt appears answers the wait instead of being dispatched as a
     * command (the prompt text reaches the console reader asynchronously). */
    shell_key_wait_begin();

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

/** Delayed `!VAR!` expansion for the current task (see `setlocal`). */
bool shell_delayed_expansion_enabled(void)
{
    return s_delayed_expansion;
}

/** Test hook: set the delayed-expansion flag directly. */
void shell_set_delayed_expansion(bool enabled)
{
    s_delayed_expansion = enabled;
}

void shell_command_setlocal(int argc, char **argv)
{
    bool want_delayed = s_delayed_expansion;
    bool want_set = false;

    /* Optional switches (cmd.exe parity): `enabledelayedexpansion` /
     * `disabledelayedexpansion` flip the `!VAR!` flag inside the new scope
     * (restored by `endlocal`); `enableextensions` / `disableextensions` are
     * accepted and ignored (extensions are always on here) so portable
     * scripts parse. Anything else is a usage error. */
    for (int i = 1; i < argc; i++) {
        if (shell_text_equals_ignore_case(argv[i], "enabledelayedexpansion")) {
            want_delayed = true;
            want_set = true;
        } else if (shell_text_equals_ignore_case(argv[i], "disabledelayedexpansion")) {
            want_delayed = false;
            want_set = true;
        } else if (shell_text_equals_ignore_case(argv[i], "enableextensions") ||
                   shell_text_equals_ignore_case(argv[i], "disableextensions")) {
            continue;
        } else {
            shell_print_usage("Usage: setlocal [enabledelayedexpansion|disabledelayedexpansion]");
            s_errorlevel = 2;
            return;
        }
    }

    if (!shell_setlocal_push()) {
        shell_print_error("setlocal: cannot nest deeper than %d scopes",
                                 SHELL_SETLOCAL_DEPTH_MAX);
        shell_record_warningf("setlocal", "setlocal nesting limit reached");
        s_errorlevel = 1;
        return;
    }
    if (want_set) {
        s_delayed_expansion = want_delayed;
    }

    /* Track the scope against the running frame so an unmatched setlocal is
     * unwound automatically when the batch file returns. */
    if (s_active_batch_frame != NULL) {
        s_active_batch_frame->setlocal_depth++;
    }
    s_errorlevel = 0;
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
 * PROCESS ABSTRACTION (proc)
 * ========================================================================
 * A batch file runs as a "process" on a worker task: it has an
 * argument frame (%0..%9 / %*), its own environment scope (setlocal), the
 * caller's cwd, an exit code (errorlevel, expandable as %ERRORLEVEL%), and
 * stdin/stdout through the pipe and redirection layer. `proc` is the
 * introspection view of that process stack, so a batch file can report (or
 * branch on) its own depth, name, arguments, echo state, and exit code, and
 * a shell user can see which scripts are nested and what the pipe input
 * source is. Each worker task (`start` background tasks included) sees its
 * own process stack.
 */

/** Render a frame's caller arguments (args[1]..) joined with spaces. Uses
 * the per-task `%*` scratch (same buffer, same lifetime rules). */
static const char *shell_frame_args_string(const shell_batch_frame_t *frame)
{
    char *all_args = batch_ctx_current()->all_args;
    int index;

    all_args[0] = '\0';
    if (frame == NULL || frame->argc <= 1) {
        return all_args;
    }
    for (index = 1; index < frame->argc; index++) {
        if (index > 1) {
            strncat(all_args, " ", SHELL_BATCH_LINE_BYTES - strlen(all_args) - 1);
        }
        strncat(all_args, frame->args[index], SHELL_BATCH_LINE_BYTES - strlen(all_args) - 1);
    }
    return all_args;
}

/**
 * `proc` — introspect the active batch process stack.
 *
 *   proc               list every nested batch process (script, depth, args, echo)
 *   proc /args         print the current process's %* (all arguments from %1)
 *   proc /name         print the current script name (%0)
 *   proc /depth        print the current nesting depth
 *   proc /errorlevel   print the current exit code
 *   proc /echo         print the current batch echo state
 *   proc /stdin        print the active input source (a pipe stage's spool file
 *                      or a `< file`), or "none"
 */
void shell_command_proc(int argc, char **argv)
{
    shell_batch_frame_t *frame = s_active_batch_frame;

    if (frame == NULL) {
        shell_print_muted("proc: no batch process is running");
        batch_set_errorlevel(0);
        return;
    }

    if (argc >= 2) {
        if (shell_text_equals_ignore_case(argv[1], "/args")) {
            shell_transcript_appendf_ansi(SH_LBL "proc.args" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                          shell_frame_args_string(frame));
        } else if (shell_text_equals_ignore_case(argv[1], "/name")) {
            shell_transcript_appendf_ansi(SH_LBL "proc.name" SH_RST "=" SH_PATH "%s" SH_RST "\n",
                                          frame->args[0]);
        } else if (shell_text_equals_ignore_case(argv[1], "/depth")) {
            shell_print_field_num("proc.depth", frame->depth);
        } else if (shell_text_equals_ignore_case(argv[1], "/errorlevel")) {
            shell_print_field_num("proc.errorlevel", s_errorlevel);
        } else if (shell_text_equals_ignore_case(argv[1], "/echo")) {
            shell_transcript_appendf_ansi(SH_LBL "proc.echo" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                          frame->echo_enabled ? "on" : "off");
        } else if (shell_text_equals_ignore_case(argv[1], "/stdin")) {
            const char *source = storage_get_input_redirect();

            if (source != NULL && source[0] != '\0') {
                shell_transcript_appendf_ansi(SH_LBL "proc.stdin" SH_RST "=" SH_PATH "%s" SH_RST "\n",
                                              source);
            } else {
                shell_transcript_appendf_ansi(SH_LBL "proc.stdin" SH_RST "=" SH_MUTE "none" SH_RST "\n");
            }
        } else if (shell_text_equals_ignore_case(argv[1], "/labels")) {
            int li;

            shell_print_field_num("proc.labels", frame->label_count);
            for (li = 0; li < frame->label_count; li++) {
                shell_transcript_appendf_ansi("  " SH_NUM "[%ld]" SH_RST " " SH_PATH "%s" SH_RST "\n",
                                              frame->labels[li].file_pos, frame->labels[li].name);
            }
        } else if (shell_text_equals_ignore_case(argv[1], "/goto")) {
            if (s_goto_pending && s_goto_eof) {
                shell_print_field("proc.goto", "eof");
            } else if (s_goto_pending) {
                shell_print_field("proc.goto", "%s", s_goto_label);
            } else {
                shell_transcript_appendf_ansi(SH_LBL "proc.goto" SH_RST "=" SH_MUTE "none" SH_RST "\n");
            }
        } else {
            shell_print_usage("Usage: proc [/args | /name | /depth | /errorlevel | /echo | /stdin | /labels | /goto]");
            batch_set_errorlevel(2);
            return;
        }
        batch_set_errorlevel(0);
        return;
    }

    /* Full process-stack view, caller first. */
    {
        shell_batch_frame_t *stack[SHELL_BATCH_DEPTH_MAX];
        int count = 0;
        int index;

        for (shell_batch_frame_t *walk = frame; walk != NULL && count < SHELL_BATCH_DEPTH_MAX; walk = walk->parent) {
            stack[count++] = walk;
        }

        shell_print_heading("Batch process stack");
        shell_print_field_num("proc.depth", frame->depth);
        shell_transcript_appendf_ansi(SH_LBL "proc.echo" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                      frame->echo_enabled ? "on" : "off");

        for (index = count - 1; index >= 0; index--) {
            shell_batch_frame_t *entry = stack[index];

            shell_transcript_appendf_ansi("  " SH_NUM "[%d]" SH_RST " " SH_PATH "%s" SH_RST,
                                          entry->depth, entry->args[0]);
            if (entry->argc > 1) {
                shell_transcript_appendf_ansi("  args=" SH_VAL "%s" SH_RST,
                                              shell_frame_args_string(entry));
            }
            shell_transcript_append_text("\n");
        }
        batch_set_errorlevel(0);
    }
}

/* ========================================================================
 * PERSISTENT STATE: ini + temp (all storage on the SD card)
 * ========================================================================
 * DOS-like apps kept state in environment variables, temporary files, and
 * simple `KEY=VALUE` INI files. `ini` reads/writes those files (and imports/
 * exports the environment), `temp` manages SD-backed temporary files. The
 * file mechanics live once in components/storage (storage_ini.c) and are
 * shared with applib, so nothing is duplicated.
 */

static bool shell_ini_list_cb(const char *key, const char *value, void *ctx)
{
    (void)ctx;
    shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n", key, value);
    return true;
}

static bool shell_ini_load_cb(const char *key, const char *value, void *ctx)
{
    (void)ctx;
    (void)shell_env_set(key, value);
    return true;
}

/** Export the whole environment table into @p buf as KEY=VALUE lines. */
static bool shell_ini_export_env(char *buf, size_t cap)
{
    buf[0] = '\0';
    for (size_t index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (s_shell_env_vars[index].used &&
            !storage_ini_upsert(buf, cap, s_shell_env_vars[index].name,
                                s_shell_env_vars[index].value)) {
            return false;
        }
    }
    return true;
}

/* ---- Shared file helpers used by both `ini` and `appconfig` ---- */
/* Each prints its own error and returns 0 on success, 1 on an I/O error. */

static int shell_ini_cmd_list(const char *path)
{
    esp_err_t error = storage_ini_file_foreach(path, shell_ini_list_cb, NULL);

    if (error != ESP_OK) {
        shell_print_error("ini: cannot read %s (%s)", path, esp_err_to_name(error));
        return 1;
    }
    return 0;
}

static int shell_ini_cmd_get(const char *path, const char *key)
{
    char value[SHELL_ENV_VALUE_BYTES];
    esp_err_t error = storage_ini_file_get(path, key, value, sizeof(value));

    if (error != ESP_OK) {
        shell_print_error("ini: %s not found in %s", key, path);
        return 1;
    }
    shell_transcript_appendf_ansi(SH_VAL "%s" SH_RST "\n", value);
    return 0;
}

static int shell_ini_cmd_set(const char *path, const char *key, const char *value)
{
    esp_err_t error = storage_ini_file_set(path, key, value);

    if (error != ESP_OK) {
        shell_print_error("ini: could not write %s (%s)", path, esp_err_to_name(error));
        return 1;
    }
    return 0;
}

static int shell_ini_cmd_del(const char *path, const char *key)
{
    esp_err_t error = storage_ini_file_delete(path, key);

    if (error != ESP_OK) {
        shell_print_error("ini: %s not found in %s", key, path);
        return 1;
    }
    return 0;
}

/**
 * `ini` — persistent state in simple KEY=VALUE files on the SD card.
 *
 *   ini list <file>           list every KEY=VALUE line
 *   ini get <file> <key>      print the value of <key>
 *   ini set <file> <key> <value>  create or update <key>
 *   ini del <file> <key>      remove <key> (alias: delete)
 *   ini load <file>           import every KEY=VALUE into the environment
 *   ini save <file>           export the whole environment to <file>
 */
void shell_command_ini(int argc, char **argv)
{
    int result = 0;

    if (argc < 3) {
        shell_print_usage("Usage: ini <list|get|set|del|load|save> <file> [key] [value]");
        batch_set_errorlevel(2);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        if (argc != 3) {
            shell_print_usage("Usage: ini list <file>");
            batch_set_errorlevel(2);
            return;
        }
        result = shell_ini_cmd_list(argv[2]);
    } else if (shell_text_equals_ignore_case(argv[1], "get")) {
        if (argc != 4) {
            shell_print_usage("Usage: ini get <file> <key>");
            batch_set_errorlevel(2);
            return;
        }
        result = shell_ini_cmd_get(argv[2], argv[3]);
    } else if (shell_text_equals_ignore_case(argv[1], "set")) {
        char *value;

        if (argc < 5) {
            shell_print_usage("Usage: ini set <file> <key> <value>");
            batch_set_errorlevel(2);
            return;
        }
        value = malloc(SHELL_ENV_VALUE_BYTES);
        if (value == NULL) {
            shell_print_error("ini: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        shell_join_args(argv, 4, argc, value, SHELL_ENV_VALUE_BYTES);
        result = shell_ini_cmd_set(argv[2], argv[3], value);
        free(value);
    } else if (shell_text_equals_ignore_case(argv[1], "del") ||
               shell_text_equals_ignore_case(argv[1], "delete")) {
        if (argc != 4) {
            shell_print_usage("Usage: ini del <file> <key>");
            batch_set_errorlevel(2);
            return;
        }
        result = shell_ini_cmd_del(argv[2], argv[3]);
    } else if (shell_text_equals_ignore_case(argv[1], "load")) {
        esp_err_t error;

        if (argc != 3) {
            shell_print_usage("Usage: ini load <file>");
            batch_set_errorlevel(2);
            return;
        }
        error = storage_ini_file_foreach(argv[2], shell_ini_load_cb, NULL);
        if (error != ESP_OK) {
            shell_print_error("ini: cannot load %s (%s)", argv[2], esp_err_to_name(error));
            batch_set_errorlevel(1);
            return;
        }
    } else if (shell_text_equals_ignore_case(argv[1], "save")) {
        char *text;
        esp_err_t error;

        if (argc != 3) {
            shell_print_usage("Usage: ini save <file>");
            batch_set_errorlevel(2);
            return;
        }
        text = malloc(P4_CONFIG_INI_MAX_BYTES + 1);
        if (text == NULL) {
            shell_print_error("ini: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        if (!shell_ini_export_env(text, P4_CONFIG_INI_MAX_BYTES + 1)) {
            shell_print_error("ini: environment too large to export");
            free(text);
            batch_set_errorlevel(1);
            return;
        }
        error = storage_write_text_file(argv[2], text);
        free(text);
        if (error != ESP_OK) {
            shell_print_error("ini: could not save %s (%s)", argv[2], esp_err_to_name(error));
            batch_set_errorlevel(1);
            return;
        }
    } else {
        shell_print_usage("Usage: ini <list|get|set|del|load|save> <file> [key] [value]");
        batch_set_errorlevel(2);
        return;
    }

    batch_set_errorlevel(result);
}

/**
 * `appconfig` — per-app settings without hand-rolling file parsing.
 *
 * Batch apps get a namespaced `KEY=VALUE` settings file on the SD card
 * (`sd:/APPS/<APP>.INI`) that they read and write through this command; the
 * file mechanics are the shared `ini`/storage core, so no app parses files.
 *
 *   appconfig <app>                list every setting in <APP>.INI
 *   appconfig <app> path           print the settings file path
 *   appconfig <app> get <key>      print the value of <key>
 *   appconfig <app> set <key> <value>  create or update <key>
 *   appconfig <app> del <key>      remove <key> (alias: delete)
 */
void shell_command_appconfig(int argc, char **argv)
{
    char path[P4_CONFIG_SD_PATH_BYTES];
    const char *app;
    int result = 0;

    if (argc < 2) {
        shell_print_usage("Usage: appconfig <app> [path|list|get|set|del] [key] [value]");
        batch_set_errorlevel(2);
        return;
    }
    app = argv[1];

    /* The app name becomes part of a filename: reject separators and dots
     * that could escape the APPS directory. */
    if (app[0] == '\0' || strchr(app, '/') != NULL || strchr(app, '\\') != NULL ||
        strcmp(app, ".") == 0 || strcmp(app, "..") == 0) {
        shell_print_error("appconfig: invalid app name %s", app);
        batch_set_errorlevel(2);
        return;
    }
    snprintf(path, sizeof(path), "%s/APPS/%s.INI", BSP_SD_MOUNT_POINT, app);

    if (argc == 2) {
        result = shell_ini_cmd_list(path);
    } else if (shell_text_equals_ignore_case(argv[2], "list")) {
        if (argc != 3) {
            shell_print_usage("Usage: appconfig <app> list");
            batch_set_errorlevel(2);
            return;
        }
        result = shell_ini_cmd_list(path);
    } else if (shell_text_equals_ignore_case(argv[2], "path")) {
        if (argc != 3) {
            shell_print_usage("Usage: appconfig <app> path");
            batch_set_errorlevel(2);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "appconfig.path" SH_RST "=" SH_PATH "%s" SH_RST "\n", path);
    } else if (shell_text_equals_ignore_case(argv[2], "get")) {
        if (argc != 4) {
            shell_print_usage("Usage: appconfig <app> get <key>");
            batch_set_errorlevel(2);
            return;
        }
        result = shell_ini_cmd_get(path, argv[3]);
    } else if (shell_text_equals_ignore_case(argv[2], "set")) {
        char *value;

        if (argc < 5) {
            shell_print_usage("Usage: appconfig <app> set <key> <value>");
            batch_set_errorlevel(2);
            return;
        }
        value = malloc(SHELL_ENV_VALUE_BYTES);
        if (value == NULL) {
            shell_print_error("appconfig: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        shell_join_args(argv, 4, argc, value, SHELL_ENV_VALUE_BYTES);
        result = shell_ini_cmd_set(path, argv[3], value);
        free(value);
    } else if (shell_text_equals_ignore_case(argv[2], "del") ||
               shell_text_equals_ignore_case(argv[2], "delete")) {
        if (argc != 4) {
            shell_print_usage("Usage: appconfig <app> del <key>");
            batch_set_errorlevel(2);
            return;
        }
        result = shell_ini_cmd_del(path, argv[3]);
    } else {
        shell_print_usage("Usage: appconfig <app> [path|list|get|set|del] [key] [value]");
        batch_set_errorlevel(2);
        return;
    }

    batch_set_errorlevel(result);
}

/**
 * `temp` — SD-backed temporary files.
 *
 *   temp             print the temp directory (sd:/tmp)
 *   temp new [ext]   create a unique temp file and print its path
 *   temp clean       delete every temp file
 */
void shell_command_temp(int argc, char **argv)
{
    if (argc == 1) {
        char dir[P4_CONFIG_SD_PATH_BYTES];

        snprintf(dir, sizeof(dir), "%s/%s", BSP_SD_MOUNT_POINT, P4_CONFIG_TEMP_DIR_NAME);
        shell_transcript_appendf_ansi(SH_LBL "temp.dir" SH_RST "=" SH_PATH "%s" SH_RST "\n", dir);
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "new")) {
        char path[P4_CONFIG_SD_PATH_BYTES];
        const char *ext = (argc >= 3) ? argv[2] : NULL;
        esp_err_t error = storage_temp_path(path, sizeof(path), ext);

        if (error != ESP_OK) {
            shell_print_error("temp: could not create a temp file (%s)", esp_err_to_name(error));
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "temp.path" SH_RST "=" SH_PATH "%s" SH_RST "\n", path);
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "clean")) {
        esp_err_t error = storage_temp_cleanup();

        if (error != ESP_OK) {
            shell_print_error("temp: could not clean %s (%s)",
                              P4_CONFIG_TEMP_DIR_NAME, esp_err_to_name(error));
            batch_set_errorlevel(1);
            return;
        }
        shell_print_ok("temp: cleaned the SD temp directory");
        batch_set_errorlevel(0);
        return;
    }

    shell_print_usage("Usage: temp [new [ext] | clean]");
    batch_set_errorlevel(2);
}

/* ========================================================================
 * MENU / FORM PRIMITIVES (ansi + menu; CHOICE + ANSI was the DOS way)
 * ========================================================================
 * Interactive apps were built from CHOICE (single-key selection) plus ANSI
 * escape codes (colors, bold, reverse video). `ansi` lets a batch script emit
 * arbitrary SGR codes for the text that follows, and `menu` renders a numbered
 * form and reads a numeric choice — both rendered in the shell transcript
 * (the display area) and readable from touch, USB keyboard, or serial.
 */

/**
 * `ansi <sgr-codes> [text...]` — emit text styled with the given SGR codes.
 *
 * The codes are the `ESC[<codes>m` parameters (digits and semicolons): `7`
 * reverse video, `1` bold, `31` red, `90` muted, `0` reset, etc. With text the
 * text is wrapped in `ESC[<codes>m text ESC[0m` and rendered in the transcript
 * (and UART); without text only the codes are emitted (for terminal effects).
 */
void shell_command_ansi(int argc, char **argv)
{
    const char *arg;
    char *styled;
    size_t styled_size;

    if (argc < 2) {
        shell_print_usage("Usage: ansi <sgr-codes|@spec> [text...]");
        batch_set_errorlevel(2);
        return;
    }
    arg = argv[1];

    /* The styled line is command-sized and this runs on the recursive batch
     * path, so it is heap-allocated and freed on every exit. */
    styled_size = (argc >= 3 ? SHELL_COMMAND_BYTES : 0) + 64;
    styled = malloc(styled_size);
    if (styled == NULL) {
        shell_print_error("ansi: out of memory");
        batch_set_errorlevel(1);
        return;
    }

    /* Handle @-specifiers (cursor/screen control) */
    if (arg[0] == '@') {
        const char *spec = arg + 1;

        if (strncmp(spec, "POS:", 4) == 0) {
            /* @POS:row;col */
            snprintf(styled, styled_size, "\x1b[%sH", spec + 4);
        } else if (strncmp(spec, "CLEAR", 5) == 0) {
            /* @CLEAR[=mode] - default 2 (entire screen) */
            if (spec[5] == '=') {
                snprintf(styled, styled_size, "\x1b[%sJ", spec + 6);
            } else {
                snprintf(styled, styled_size, "\x1b[2J");
            }
        } else if (strcmp(spec, "SAVE") == 0) {
            /* @SAVE - save cursor position */
            snprintf(styled, styled_size, "\x1b[s");
        } else if (strcmp(spec, "RESTORE") == 0) {
            /* @RESTORE - restore cursor position */
            snprintf(styled, styled_size, "\x1b[u");
        } else if (strcmp(spec, "ALTON") == 0) {
            /* @ALTON - enter alternate screen buffer */
            snprintf(styled, styled_size, "\x1b[?1049h");
        } else if (strcmp(spec, "ALTOFF") == 0) {
            /* @ALTOFF - exit alternate screen buffer */
            snprintf(styled, styled_size, "\x1b[?1049l");
        } else if (strcmp(spec, "CURSON") == 0) {
            /* @CURSON - show cursor */
            snprintf(styled, styled_size, "\x1b[?25h");
        } else if (strcmp(spec, "CURSOFF") == 0) {
            /* @CURSOFF - hide cursor */
            snprintf(styled, styled_size, "\x1b[?25l");
        } else if (strncmp(spec, "SCROLL:", 7) == 0) {
            /* @SCROLL:n - scroll up n lines */
            snprintf(styled, styled_size, "\x1b[%sS", spec + 7);
        } else if (strncmp(spec, "FG256:", 6) == 0) {
            /* @FG256:n - 256-color foreground */
            snprintf(styled, styled_size, "\x1b[38;5;%sm", spec + 6);
        } else if (strncmp(spec, "BG256:", 6) == 0) {
            /* @BG256:n - 256-color background */
            snprintf(styled, styled_size, "\x1b[48;5;%sm", spec + 6);
        } else if (strncmp(spec, "FGRGB:", 6) == 0) {
            /* @FGRGB:r;g;b - 24-bit truecolor foreground */
            snprintf(styled, styled_size, "\x1b[38;2;%sm", spec + 6);
        } else if (strncmp(spec, "BGRGB:", 6) == 0) {
            /* @BGRGB:r;g;b - 24-bit truecolor background */
            snprintf(styled, styled_size, "\x1b[48;2;%sm", spec + 6);
        } else {
            shell_print_error("ansi: unknown @-specifier '%s'", arg);
            free(styled);
            batch_set_errorlevel(2);
            return;
        }

        if (argc >= 3) {
            char *text = malloc(SHELL_COMMAND_BYTES);
            if (text == NULL) {
                free(styled);
                shell_print_error("ansi: out of memory");
                batch_set_errorlevel(1);
                return;
            }
            shell_join_args(argv, 2, argc, text, SHELL_COMMAND_BYTES);
            strncat(styled, text, styled_size - strlen(styled) - 1);
            free(text);
        }

        shell_transcript_append_ansi(styled);
        free(styled);
        batch_set_errorlevel(0);
        return;
    }

    /* Original SGR code path */
    const char *codes_arg = arg;
    char codes[64];
    const char *p;
    size_t codes_len;

    codes_len = strlen(codes_arg);
    if (codes_len >= sizeof(codes)) {
        shell_print_error("ansi: SGR codes too long");
        free(styled);
        batch_set_errorlevel(2);
        return;
    }
    snprintf(codes, sizeof(codes), "%s", codes_arg);
    if (codes_len > 0 && codes[codes_len - 1] == 'm') {
        codes[--codes_len] = '\0';
    }
    for (p = codes; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p) && *p != ';') {
            shell_print_error("ansi: invalid SGR codes '%s'", argv[1]);
            free(styled);
            batch_set_errorlevel(2);
            return;
        }
    }
    if (codes[0] == '\0') {
        shell_print_error("ansi: invalid SGR codes '%s'", argv[1]);
        free(styled);
        batch_set_errorlevel(2);
        return;
    }

    if (argc >= 3) {
        char *text = malloc(SHELL_COMMAND_BYTES);

        if (text == NULL) {
            free(styled);
            shell_print_error("ansi: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        shell_join_args(argv, 2, argc, text, SHELL_COMMAND_BYTES);
        snprintf(styled, styled_size, "\x1b[%sm%s\x1b[0m", codes, text);
        free(text);
    } else {
        snprintf(styled, styled_size, "\x1b[%sm", codes);
    }

    shell_transcript_append_ansi(styled);
    free(styled);
    batch_set_errorlevel(0);
}

/**
 * `menu <item> [item...]` — render a numbered menu in the transcript and read
 * a numeric choice. ERRORLEVEL becomes the 1-based index of the chosen item
 * (0 on cancel, timeout, or an invalid entry), so a batch app branches on it.
 */
void shell_command_menu(int argc, char **argv)
{
    char input[P4_CONFIG_SET_PROMPT_INPUT_BYTES];
    int count = argc - 1;
    int choice;
    int index;

    if (argc < 2) {
        shell_print_usage("Usage: menu <item> [item...]");
        batch_set_errorlevel(2);
        return;
    }

    shell_transcript_appendf_ansi(SH_SUBHEAD "Menu" SH_RST "\n");
    for (index = 0; index < count; index++) {
        shell_transcript_appendf_ansi("  " SH_NUM "%d." SH_RST " %s\n", index + 1, argv[index + 1]);
    }
    shell_transcript_appendf_ansi(SH_LBL "Enter choice (1-%d): " SH_RST, count);

    if (!shell_read_line(input, sizeof(input), SHELL_KEY_WAIT_TIMEOUT_MS)) {
        shell_transcript_append_text("menu: cancelled or timed out\n");
        batch_set_errorlevel(0);
        return;
    }

    choice = atoi(input);
    if (choice < 1 || choice > count) {
        shell_transcript_append_text("menu: invalid choice\n");
        batch_set_errorlevel(0);
        return;
    }
    batch_set_errorlevel(choice);
}

/* ========================================================================
 * NOTIFY (notify)
 * ========================================================================
 * Shows a short message in the header notification area. A dash as the only
 * argument clears the current notification immediately.
 */

void shell_command_notify(int argc, char **argv)
{
    uint32_t timeout_ms = P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS;
    int text_start = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncasecmp(argv[i], "/t:", 3) == 0 && strlen(argv[i]) > 3) {
            int secs = atoi(argv[i] + 3);
            timeout_ms = (secs > 0) ? (uint32_t)secs * 1000u : 0u;
            text_start++;
        }
    }

    if (text_start >= argc) {
        shell_print_usage("Usage: notify [/t:secs] <text>");
        batch_set_errorlevel(2);
        return;
    }

    if (text_start == argc - 1 && strcmp(argv[text_start], "-") == 0) {
        shell_header_notify("", 0);
    } else {
        char text[P4_CONFIG_HEADER_NOTIFICATION_BYTES];
        shell_join_args(argv, text_start, argc, text, sizeof(text));
        shell_header_notify(text, timeout_ms);
    }
    batch_set_errorlevel(0);
}

/* ========================================================================
 * APP MODE (appmode)
 * ========================================================================
 * A clean way for a batch app to take over the shell: the current screen
 * (transcript) is saved, optionally the shell input widgets are hidden for a
 * full-screen app surface, and on exit — including an automatic cleanup when
 * the batch file that entered app mode returns via `exit /b` / `goto :eof` /
 * EOF — the saved screen is restored. The mechanics live once in the shell
 * core (shell_screen_* / shell_app_mode_*), shared with the applib
 * `app_mode_enter`/`app_mode_exit`.
 */

static char *s_appmode_saved = NULL;
static bool s_appmode_active = false;

/** Restore the screen and leave full-screen (idempotent). */
static void shell_appmode_restore(void)
{
    if (!s_appmode_active) {
        return;
    }
    shell_app_mode_exit();
    shell_screen_restore(s_appmode_saved);
    shell_screen_discard(s_appmode_saved);
    s_appmode_saved = NULL;
    s_appmode_active = false;
}

/**
 * `appmode` — enter/exit app mode (save/restore screen, optional full-screen).
 *
 *   appmode            show whether app mode is active
 *   appmode status     show whether app mode is active
 *   appmode on [/full] [/clear]  save the screen; hide the shell input widgets
 *                          for a full-screen surface; optionally clear it
 *   appmode off        restore the saved screen and leave full-screen
 */
void shell_command_appmode(int argc, char **argv)
{
    bool full = false;
    bool clear = false;

    if (argc == 1 || (argc == 2 && shell_text_equals_ignore_case(argv[1], "status"))) {
        shell_transcript_appendf_ansi(SH_LBL "appmode" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                      s_appmode_active ? "on" : "off");
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "on")) {
        if (batch_bg_is_background()) {
            shell_print_error("appmode: not available to background tasks");
            batch_set_errorlevel(1);
            return;
        }
        if (s_appmode_active) {
            shell_print_error("appmode: already active");
            batch_set_errorlevel(1);
            return;
        }
        for (int index = 2; index < argc; index++) {
            if (shell_text_equals_ignore_case(argv[index], "/full")) {
                full = true;
            } else if (shell_text_equals_ignore_case(argv[index], "/clear")) {
                clear = true;
            } else {
                shell_print_usage("Usage: appmode on [/full] [/clear] | appmode off | appmode status");
                batch_set_errorlevel(2);
                return;
            }
        }

        s_appmode_saved = shell_screen_save();
        shell_app_mode_enter(full);
        if (clear) {
            shell_transcript_reset();
        }
        s_appmode_active = true;
        if (s_active_batch_frame != NULL) {
            s_active_batch_frame->app_mode = true;
        }
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "off")) {
        if (batch_bg_is_background()) {
            shell_print_error("appmode: not available to background tasks");
            batch_set_errorlevel(1);
            return;
        }
        shell_appmode_restore();
        batch_set_errorlevel(0);
        return;
    }

    shell_print_usage("Usage: appmode on [/full] [/clear] | appmode off | appmode status");
    batch_set_errorlevel(2);
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

/** Build the spool path for pipeline stage @p stage. Per-task names so two
 * workers piping at once never share a spool file. */
static void shell_pipe_spool_path(int stage, char *output, size_t output_size)
{
    int slot = (int)(batch_ctx_current() - s_batch_ctx);
    snprintf(output, output_size, "%s/_pipe%d_%d.tmp", BSP_SD_MOUNT_POINT, slot, stage);
}

void shell_execute_pipe(char *command)
{
    char *stages[SHELL_PIPE_STAGE_MAX];
    char spool[SHELL_PIPE_STAGE_MAX][64];
    /* A redirected stage can carry a full 4096-byte command plus " > " and
     * the spool path. The buffer is command-sized, and shell_execute_pipe
     * sits on the recursive batch path, so it is heap-allocated and freed at
     * the single exit (a 2x command-sized stack local here would overflow the
     * worker task when a pipe appears in a nested batch file). */
    char *redirected = NULL;
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

    redirected = malloc(SHELL_COMMAND_BYTES + 128);
    if (redirected == NULL) {
        shell_transcript_append_text("pipe: out of memory redirecting a stage\n");
        shell_record_errorf("pipe", ESP_ERR_NO_MEM, "Out of memory buffering a pipeline stage");
        return;
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
            shell_pipe_spool_path(index, spool[index], sizeof(spool[index]));
            snprintf(redirected, SHELL_COMMAND_BYTES + 128, "%s > %s", stages[index], spool[index]);
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

    free(redirected);
}

/* ========================================================================
 * BATCH ENGINE: FILE EXECUTION, LABELS, FOR LOOPS
 * ======================================================================== */

/**
 * Run one batch frame from @p path. When @p start_label is non-NULL the frame
 * begins at that `:routine` in the file (a shared-library call made by
 * `call <file.bat>::<routine>`), with an automatic environment scope so the
 * routine's temporary variables are isolated from the caller (variable
 * isolation beyond `setlocal`). The frame ends at `exit /b`, `goto :eof`, or
 * end of file and returns to the caller like any nested call.
 */
static esp_err_t shell_batch_run_internal(const char *path, const char *start_label,
                                          int argc, char **argv)
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
    frame->ram = NULL;
    frame->ram_len = 0;
    frame->ram_pos = 0;
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

    /* Load the whole file into RAM when it fits, so the line loop and every
     * `goto`/`call` seek run from memory rather than the SD card. Behavior is
     * identical (the reader mirrors fgets); larger files stream as before. */
    shell_frame_load(frame, file);
    if (frame->ram != NULL) {
        fclose(file);
        file = NULL;
        frame->batch_file = NULL;
    }

    /* args[0] already holds the script path; the caller's arguments were
     * copied into args[1..] above, so no further copying is needed here. */

    /* Scan for labels first */
    shell_scan_batch_labels(frame, file);

    /* Remember the caller's setlocal depth so any scope this file opens and
     * forgets to close is unwound when the frame returns. */
    setlocal_depth_on_entry = s_setlocal_depth;

    /* A library call (`call file.bat::routine`) starts at a labelled routine
     * in an external file. Seek to the label so the frame loop begins there;
     * the routine ends at `exit /b` / `goto :eof` / EOF and returns to the
     * caller like any nested call. The pushed environment scope isolates the
     * routine's temporary variables from the caller ("beyond setlocal": the
     * caller does not have to write setlocal/endlocal); the frame-return
     * unwinding below restores the caller's environment. */
    if (start_label != NULL && start_label[0] != '\0') {
        long label_pos = shell_find_label_pos(frame, start_label);

        if (label_pos < 0) {
            shell_transcript_appendf("call: library routine not found: %s\n", start_label);
            if (file != NULL) fclose(file);
            if (frame->ram != NULL) free(frame->ram);
            shell_sd_end(&session, "call");
            free(frame);
            free(line);
            return ESP_ERR_NOT_FOUND;
        }
        shell_frame_seek(frame, label_pos);
        (void)shell_setlocal_push();
    }

    s_active_batch_frame = frame;
    shell_set_batch_active(true);

    while (true) {
        char *trimmed;
        bool suppress_echo = false;
        size_t line_len;
        int continuations = 0;

        /* Cooperative stop for `taskkill`: a background batch unwinds at
         * the next line boundary (frames unwind normally below). */
        if (batch_bg_kill_requested()) {
            s_stop_mode = BATCH_STOP_ALL;
            break;
        }

        /* Foreground break (Ctrl+C / Stop button): the same unwind, with a
         * `^C` echo. Consume-on-fire so one press prints exactly once. */
        if (shell_abort_requested()) {
            shell_mark_foreground_break();
            shell_clear_abort();
            shell_transcript_append_text("^C\n");
            s_stop_mode = BATCH_STOP_ALL;
            break;
        }

        if (shell_frame_fgets(frame, line, SHELL_BATCH_LINE_BYTES) == NULL) {
            /* End of file. Inside a called `:label` block this returns to the
             * caller; otherwise the batch frame ends here. */
            if (shell_in_label_call(frame)) {
                shell_frame_seek(frame, shell_label_call_leave(frame));
                continue;
            }
            break;
        }

        trimmed = shell_trim(line);

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

            if (shell_frame_fgets(frame, next_start, (int)available + 1) == NULL) {
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

        /* `exit` or `exit /b` asked this file to stop. A frame-stop inside a
         * called `:label` block returns to the caller instead. */
        if (s_stop_mode == BATCH_STOP_FRAME && shell_in_label_call(frame)) {
            s_stop_mode = BATCH_STOP_NONE;
            s_goto_pending = false;
            s_goto_eof = false;
            shell_frame_seek(frame, shell_label_call_leave(frame));
            continue;
        }
        if (s_stop_mode != BATCH_STOP_NONE) {
            break;
        }

        /* `goto :eof` jumps to the end of the current batch frame. Inside a
         * called `:label` block it returns to the caller instead. */
        if (s_goto_eof) {
            s_goto_eof = false;
            s_goto_pending = false;
            if (shell_in_label_call(frame)) {
                shell_frame_seek(frame, shell_label_call_leave(frame));
                continue;
            }
            break;
        }

        /* Check for goto or call :label that may have changed execution flow */
        if (s_goto_pending) {
            long label_pos = shell_find_label_pos(frame, s_goto_label);
            if (label_pos >= 0) {
                shell_frame_seek(frame, label_pos);
                s_goto_pending = false;
                s_goto_label[0] = '\0';
                /* Continue from the label position - the label line will be skipped */
                continue;
            } else {
                /* cmd.exe parity: a `goto` to a missing label prints this and
                 * aborts the current batch file. The `call`-family verbs
                 * already validate the target before arming, so only a `goto`
                 * (or `on … goto`) can reach here. */
                shell_transcript_appendf("The system cannot find the batch label specified - %s\n",
                                         s_goto_label);
                batch_set_errorlevel(1);
                s_goto_pending = false;
                s_goto_label[0] = '\0';
                break;
            }
        }
    }

    s_active_batch_frame = frame->parent;
    shell_set_batch_active(frame->parent != NULL);

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

    /* Cleanup on exit: if this frame entered app mode (`appmode on`) and did
     * not already restore it, restore the saved screen and leave full-screen,
     * so a batch app that ends via `exit /b` / `goto :eof` / EOF never leaves
     * the shell in app mode. */
    if (frame->app_mode) {
        shell_appmode_restore();
        frame->app_mode = false;
    }

    /* Any `call :label` scope left open (a subroutine that never returned)
     * owns a heap copy of the caller's arguments; release it with the frame. */
    while (shell_in_label_call(frame)) {
        (void)shell_label_call_leave(frame);
    }

    if (file != NULL) fclose(file);
    if (frame->ram != NULL) free(frame->ram);
    shell_sd_end(&session, "call");
    free(frame);
    free(line);
    return ESP_OK;
}

/* Public entry point: run a batch file from its first line. */
esp_err_t shell_execute_batch_file(const char *path, int argc, char **argv)
{
    return shell_batch_run_internal(path, NULL, argc, argv);
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

/* ---- Test-only wrappers for the pure control-flow helpers ----
 * These expose the exact parser the executor uses so the unit suite can cover
 * it without duplicating logic. */
bool batch_label_is_line(const char *line)
{
    return shell_is_label(line);
}

void batch_label_extract(const char *line, char *name, size_t name_size)
{
    shell_extract_label_name(line, name, name_size);
}

bool batch_on_parse(int argc, char **argv, char *expr, size_t expr_size,
                    bool *is_gosub_out, char *targets, size_t targets_size)
{
    return shell_on_parse(argc, argv, expr, expr_size, is_gosub_out, targets, targets_size);
}

const char *batch_on_select(char *targets, int index)
{
    return shell_on_select(targets, index);
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

/* ---- Frame input: RAM image with a streaming fallback ----
 * A batch file at or below P4_CONFIG_BATCH_FILE_MAX_BYTES is read once into a
 * PSRAM buffer (shell_frame_load), so the line loop and every `goto`/`call`
 * seek run from memory instead of hitting the SD card mid-loop. The reader
 * mirrors fgets() byte-for-byte (stop after size-1 bytes, a newline, or EOF)
 * and tell/seek are byte offsets, so behavior matches the streaming path. */
static char *shell_frame_fgets(shell_batch_frame_t *frame, char *buf, int size)
{
    size_t i = 0;

    if (frame == NULL || buf == NULL || size <= 0) return NULL;
    if (frame->ram != NULL) {
        while (i < (size_t)(size - 1) && frame->ram_pos < frame->ram_len) {
            char c = frame->ram[frame->ram_pos++];
            buf[i++] = c;
            if (c == '\n') break;
        }
        buf[i] = '\0';
        return (i > 0) ? buf : NULL;
    }
    return (frame->batch_file != NULL) ? fgets(buf, size, frame->batch_file) : NULL;
}

static long shell_frame_tell(shell_batch_frame_t *frame)
{
    if (frame == NULL) return 0;
    if (frame->ram != NULL) return (long)frame->ram_pos;
    return frame->batch_file != NULL ? ftell(frame->batch_file) : 0;
}

static void shell_frame_seek(shell_batch_frame_t *frame, long pos)
{
    if (frame == NULL) return;
    if (frame->ram != NULL) {
        if (pos < 0) pos = 0;
        if ((size_t)pos > frame->ram_len) pos = (long)frame->ram_len;
        frame->ram_pos = (size_t)pos;
        return;
    }
    if (frame->batch_file != NULL) {
        fseek(frame->batch_file, pos, SEEK_SET);
    }
}

/** Read the whole file into frame->ram when it fits the cap; otherwise leave
 *  frame->ram NULL so the caller streams from the FILE. */
static void shell_frame_load(shell_batch_frame_t *frame, FILE *file)
{
    long size;

    if (frame == NULL) return;
    frame->ram = NULL;
    frame->ram_len = 0;
    frame->ram_pos = 0;
    if (file == NULL) return;
    if (fseek(file, 0, SEEK_END) != 0) return;
    size = ftell(file);
    if (size <= 0 || (unsigned long)size > (unsigned long)P4_CONFIG_BATCH_FILE_MAX_BYTES) {
        fseek(file, 0, SEEK_SET);
        return;
    }
    frame->ram = p4heap_alloc_psram((size_t)size + 1);
    if (frame->ram == NULL) {
        fseek(file, 0, SEEK_SET);
        return;
    }
    fseek(file, 0, SEEK_SET);
    if (fread(frame->ram, 1, (size_t)size, file) != (size_t)size) {
        free(frame->ram);
        frame->ram = NULL;
        fseek(file, 0, SEEK_SET);
        return;
    }
    frame->ram[size] = '\0';
    frame->ram_len = (size_t)size;
    frame->ram_pos = 0;
}

/* Helper: scan batch file for labels and build label table */
static void shell_scan_batch_labels(shell_batch_frame_t *frame, FILE *file)
{
    if (frame == NULL) return;
    long current_pos = shell_frame_tell(frame);
    long line_pos = 0;
    bool previous_continues = false;
    /* Set on the first label past the table so the overflow warns exactly
     * once per file instead of once per extra label. */
    bool warned = false;

    /* The read buffer is line-sized, so it lives on the heap: this function
     * runs from shell_execute_batch_file(), which is on the recursive batch
     * path, and a line-sized stack buffer there would stack up across nesting
     * levels. */
    char *line = malloc(SHELL_BATCH_LINE_BYTES);
    if (line == NULL) {
        return;
    }

    shell_frame_seek(frame, 0);
    frame->label_count = 0;

    /* Tracks whether the previous physical line ended with an unescaped
     * continuation caret. A line that is the tail of a continuation is part
     * of the command above it, so a ':' at its start is data, not a label. */
    while (shell_frame_fgets(frame, line, SHELL_BATCH_LINE_BYTES) != NULL) {
        line_pos = shell_frame_tell(frame);
        {
            /* Raw byte count read by fgets (including the trailing newline),
             * captured before shell_trim() shortens the buffer so the label
             * file position points at the line START, not a byte into it. */
            size_t raw_len = strlen(line);
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
                    frame->labels[frame->label_count].file_pos = line_pos - (long)raw_len;
                    frame->label_count++;
                } else if (!warned) {
                    /* The table is full: extra goto targets would vanish
                     * silently (a 33-label game loses its quit path with no
                     * hint), so warn loudly, once per file. */
                    warned = true;
                    shell_print_warning("batch: too many labels (max %d); extra labels ignored",
                                        SHELL_BATCH_LABEL_MAX);
                    shell_record_warningf("batch", "Label table full (%d); extra labels ignored",
                                          SHELL_BATCH_LABEL_MAX);
                }
            }

            previous_continues = this_continues;
        }
    }

    shell_frame_seek(frame, current_pos);
    free(line);
}

/* ========================================================================
 * FOR LOOP EXECUTION
 * ======================================================================== */

/**
 * Internal token-split cap for `for /f` source lines. The `tokens=` spec is
 * bounded separately by P4_CONFIG_FORF_TOKEN_MAX; this only bounds how many
 * leading tokens are recorded, so a `*` capture (which slices the original
 * line from an offset) is still correct for longer lines.
 */
#define SHELL_FORF_SPLIT_MAX 32

/** One `for` variable binding substituted into a loop body. */
typedef struct {
    char var;             /**< Loop-variable letter (a..z). */
    const char *text;     /**< Replacement text (may be "", never NULL). */
    size_t len;           /**< Bytes of @p text to copy. */
} shell_for_binding_t;

/**
 * Substitute every `%%var` / `%var` occurrence in the body with its binding
 * and run the result through the command pipeline. Runs from the recursive
 * batch path, so the expanded body is heap-allocated.
 */
static void shell_for_substitute_and_run_bindings(const char *do_command,
                                                  const shell_for_binding_t *bindings,
                                                  int binding_count)
{
    const size_t cmd_size = SHELL_BATCH_LINE_BYTES * 2;
    char *expanded_cmd = malloc(cmd_size);
    const char *src;
    char *dst;
    size_t remaining;

    /* Foreground break: skip this body and unwind. Every `for` form funnels
     * through here, so one check covers classic, wildcard, and /f loops;
     * the per-iteration callers break on the stop mode they already test. */
    if (shell_abort_requested()) {
        shell_mark_foreground_break();
        shell_clear_abort();
        shell_transcript_append_text("^C\n");
        s_stop_mode = BATCH_STOP_ALL;
        return;
    }

    if (expanded_cmd == NULL) {
        shell_transcript_append_text("for: out of memory expanding the loop body\n");
        return;
    }

    src = do_command;
    dst = expanded_cmd;
    remaining = cmd_size - 1;

    while (*src != '\0' && remaining > 0) {
        bool replaced = false;

        for (int b = 0; b < binding_count; b++) {
            char var = bindings[b].var;

            /* Batch files write `%%var` (the doubled percent is the batch-file
             * escape for a literal `%`); the prompt writes `%var`. */
            if (*src == '%' && *(src + 1) == '%' && *(src + 2) == var) {
                for (size_t i = 0; i < bindings[b].len && remaining > 0; i++) {
                    *dst++ = bindings[b].text[i];
                    remaining--;
                }
                src += 3;
                replaced = true;
                break;
            }
            if (*src == '%' && *(src + 1) == var) {
                for (size_t i = 0; i < bindings[b].len && remaining > 0; i++) {
                    *dst++ = bindings[b].text[i];
                    remaining--;
                }
                src += 2;
                replaced = true;
                break;
            }
        }

        if (!replaced) {
            *dst++ = *src++;
            remaining--;
        }
    }
    *dst = '\0';

    batch_run_nested(expanded_cmd);
    free(expanded_cmd);
}

/** Single-binding wrapper used by the classic token / wildcard `for`. */
static void shell_for_substitute_and_run(const char *do_command, char var_name, const char *value)
{
    shell_for_binding_t binding;

    binding.var = var_name;
    binding.text = value != NULL ? value : "";
    binding.len = value != NULL ? strlen(value) : 0;
    shell_for_substitute_and_run_bindings(do_command, &binding, 1);
}

/* `for /A` prefix collector: copy every variable name shaped `PREFIX[...]`
 * (case-insensitive prefix, non-empty index) into @p names. Slot (creation)
 * order, which is the only stable order the table has. Pure over the shared
 * table (copy-out discipline: names are copied, values are re-read live per
 * iteration so a body that stores stays coherent). */
int shell_fora_collect(const char *prefix, char names[][SHELL_ENV_NAME_BYTES], int max)
{
    size_t prefix_len;
    size_t index = 0;
    int count = 0;

    if (prefix == NULL || prefix[0] == '\0' || names == NULL || max <= 0) {
        return 0;
    }
    prefix_len = strlen(prefix);
    batch_shared_take();
    for (index = 0; index < SHELL_ENV_VAR_MAX && count < max; index++) {
        const char *name;
        size_t name_len;
        const char *open;
        const char *close;

        if (!s_shell_env_vars[index].used) {
            continue;
        }
        name = s_shell_env_vars[index].name;
        name_len = strlen(name);
        if (name_len <= prefix_len + 2) {
            continue;
        }
        if (strncasecmp(name, prefix, prefix_len) != 0) {
            continue;
        }
        open = name + prefix_len;
        if (*open != '[') {
            continue;
        }
        close = name + name_len - 1;
        if (*close != ']' || close == open + 1) {
            continue;
        }
        snprintf(names[count], SHELL_ENV_NAME_BYTES, "%s", name);
        count++;
    }
    batch_shared_give();
    return count;
}

/* `switch` case matcher (pure, unit-tested): first case-insensitive match
 * wins. Each case is `match:label`, split on the LAST colon so a match
 * containing `:` (such as a path) survives. Entries without a colon never
 * match. Returns the label (possibly empty), or NULL when nothing matches. */
const char *shell_switch_select(const char *value, char **cases, int count)
{
    int i;

    if (value == NULL || cases == NULL || count <= 0) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        const char *arg = cases[i];
        const char *split;
        size_t match_len;

        if (arg == NULL) {
            continue;
        }
        split = strrchr(arg, ':');
        if (split == NULL || split == arg) {
            continue;
        }
        match_len = (size_t)(split - arg);
        if (strlen(value) == match_len && strncasecmp(value, arg, match_len) == 0) {
            return split + 1;
        }
    }
    return NULL;
}

/* `for /L` (start,step,end) set parser: three comma-separated integers with
 * optional whitespace, nothing else. Pure so the unit tests drive it
 * directly; the loop body below reuses the classic substitution runner. */
bool shell_forl_parse(const char *set, int32_t *start_out, int32_t *step_out,
                      int32_t *end_out)
{
    long values[3];
    const char *cursor;
    int index;

    if (set == NULL || start_out == NULL || step_out == NULL || end_out == NULL) {
        return false;
    }
    cursor = set;
    for (index = 0; index < 3; index++) {
        char *num_end = NULL;
        long value;

        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            return false;
        }
        value = strtol(cursor, &num_end, 10);
        if (num_end == cursor) {
            return false;
        }
        if (value < INT32_MIN || value > INT32_MAX) {
            return false;
        }
        values[index] = value;
        cursor = num_end;
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (index < 2) {
            if (*cursor != ',') {
                return false;
            }
            cursor++;
        }
    }
    if (*cursor != '\0') {
        return false;
    }
    if (values[1] == 0) {
        return false;
    }
    *start_out = (int32_t)values[0];
    *step_out = (int32_t)values[1];
    *end_out = (int32_t)values[2];
    return true;
}

/* ========================================================================
 * `for /f` — FILE-LINE LOOPS
 * ======================================================================== */

void shell_forf_options_default(shell_forf_options_t *opts)
{
    if (opts == NULL) {
        return;
    }
    snprintf(opts->delims, sizeof(opts->delims), " \t");
    opts->token_list[0] = 1;
    opts->token_count = 1;
    opts->skip = 0;
    opts->eol = '\0';
    opts->star = false;
    opts->usebackq = false;
}

/** Parse a bounded decimal integer (digits only). */
static bool shell_forf_parse_num(const char *text, size_t len, int *out)
{
    int value = 0;

    if (len == 0 || len > 6) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    *out = value;
    return true;
}

/** Parse a `tokens=` list: `N`, `N-M`, and `*` items separated by commas. */
static bool shell_forf_parse_tokens(const char *value, size_t vlen, shell_forf_options_t *opts)
{
    size_t pos = 0;

    if (vlen == 0) {
        return true;   /* empty `tokens=` falls back to the default below */
    }

    while (pos < vlen) {
        size_t start = pos;

        while (pos < vlen && value[pos] != ',') {
            pos++;
        }
        {
            size_t ilen = pos - start;

            if (ilen == 0) {
                return false;   /* empty item such as `tokens=1,,2` */
            }
            if (ilen == 1 && value[start] == '*') {
                opts->star = true;
            } else {
                int lo;
                int hi;
                const char *dash = memchr(value + start, '-', ilen);

                if (dash != NULL) {
                    size_t lo_len = (size_t)(dash - (value + start));

                    if (!shell_forf_parse_num(value + start, lo_len, &lo) ||
                        !shell_forf_parse_num(dash + 1, ilen - lo_len - 1, &hi)) {
                        return false;
                    }
                } else {
                    if (!shell_forf_parse_num(value + start, ilen, &lo)) {
                        return false;
                    }
                    hi = lo;
                }
                if (lo < 1 || hi < lo) {
                    return false;
                }
                for (int i = lo; i <= hi; i++) {
                    if (opts->token_count >= P4_CONFIG_FORF_TOKEN_MAX) {
                        return false;   /* too many requested tokens */
                    }
                    opts->token_list[opts->token_count++] = i;
                }
            }
        }
        if (pos < vlen) {
            pos++;   /* consume ',' */
        }
    }
    return true;
}

bool shell_forf_parse_options(const char *text, size_t len, shell_forf_options_t *opts)
{
    size_t pos = 0;

    if (text == NULL || opts == NULL) {
        return false;
    }
    shell_forf_options_default(opts);

    while (pos < len) {
        size_t start;
        size_t wlen;
        const char *eq;
        size_t klen;
        const char *value;
        size_t vlen;

        while (pos < len && (text[pos] == ' ' || text[pos] == '\t')) {
            pos++;
        }
        if (pos >= len) {
            break;
        }

        start = pos;
        while (pos < len && text[pos] != ' ' && text[pos] != '\t') {
            pos++;
        }
        wlen = pos - start;

        /* `usebackq` is accepted for DOS parity; the quoted-command source is
         * not supported on this shell, so it is a no-op. */
        if (wlen == 8 && strncasecmp(text + start, "usebackq", 8) == 0) {
            opts->usebackq = true;
            continue;
        }

        eq = memchr(text + start, '=', wlen);
        if (eq == NULL) {
            return false;   /* bare word that is not an option */
        }
        klen = (size_t)(eq - (text + start));
        value = eq + 1;
        vlen = wlen - klen - 1;

        if (klen == 6 && strncasecmp(text + start, "delims", 6) == 0) {
            size_t n = vlen < sizeof(opts->delims) - 1 ? vlen : sizeof(opts->delims) - 1;

            memcpy(opts->delims, value, n);
            opts->delims[n] = '\0';
        } else if (klen == 6 && strncasecmp(text + start, "tokens", 6) == 0) {
            /* A `tokens=` option replaces the default token 1 rather than
             * appending to it. */
            opts->token_count = 0;
            opts->star = false;
            if (!shell_forf_parse_tokens(value, vlen, opts)) {
                return false;
            }
        } else if (klen == 4 && strncasecmp(text + start, "skip", 4) == 0) {
            if (!shell_forf_parse_num(value, vlen, &opts->skip)) {
                return false;
            }
        } else if (klen == 3 && strncasecmp(text + start, "eol", 3) == 0) {
            opts->eol = (vlen > 0) ? *value : '\0';
        } else {
            return false;   /* unknown option */
        }
    }

    /* `tokens=` with no usable entry (or no `tokens=` at all) defaults to 1. */
    if (opts->token_count == 0 && !opts->star) {
        opts->token_list[0] = 1;
        opts->token_count = 1;
    }
    return true;
}

int shell_forf_split_line(const char *line, const char *delims,
                          shell_forf_tok_t *tokens, int max_tokens)
{
    const char *p = line;
    int count = 0;

    if (line == NULL || tokens == NULL || max_tokens <= 0) {
        return 0;
    }

    /* An empty delims set means "no delimiters": the whole line is one token,
     * matching DOS `delims=` with no characters. */
    if (delims == NULL || delims[0] == '\0') {
        if (line[0] != '\0') {
            tokens[0].start = line;
            tokens[0].len = strlen(line);
            return 1;
        }
        return 0;
    }

    while (*p != '\0') {
        while (*p != '\0' && strchr(delims, *p) != NULL) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        {
            const char *start = p;

            while (*p != '\0' && strchr(delims, *p) == NULL) {
                p++;
            }
            if (count < max_tokens) {
                tokens[count].start = start;
                tokens[count].len = (size_t)(p - start);
                count++;
            }
        }
    }
    return count;
}

/**
 * Process one `for /f` source line: apply eol/skip filters, split on delims,
 * bind the requested token indices to consecutive loop-variable letters (with
 * `*` capturing the rest of the line), and run the substituted body.
 */
static void shell_forf_process_line(char *line, const char *do_command, char var_name,
                                    const shell_forf_options_t *opts)
{
    shell_forf_tok_t tokens[SHELL_FORF_SPLIT_MAX];
    shell_for_binding_t bindings[P4_CONFIG_FORF_TOKEN_MAX + 1];
    int count;
    int binding_count = 0;
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }

    if (opts->eol != 0 && line[0] == opts->eol) {
        return;
    }

    count = shell_forf_split_line(line, opts->delims, tokens, SHELL_FORF_SPLIT_MAX);
    if (count == 0) {
        return;
    }

    for (int i = 0; i < opts->token_count &&
                    binding_count < (int)(sizeof(bindings) / sizeof(bindings[0])); i++) {
        int idx = opts->token_list[i];
        shell_for_binding_t *b = &bindings[binding_count++];

        b->var = (char)(var_name + i);
        if (idx >= 1 && idx <= count) {
            b->text = tokens[idx - 1].start;
            b->len = tokens[idx - 1].len;
        } else {
            b->text = "";
            b->len = 0;
        }
    }

    /* `tokens=...*` binds the remainder after the last explicit index. The
     * split keeps offsets into the line, so the slice is correct even when
     * the line holds more tokens than the split cap recorded. */
    if (opts->star && binding_count < (int)(sizeof(bindings) / sizeof(bindings[0]))) {
        int rest0 = (opts->token_count > 0) ? opts->token_list[opts->token_count - 1] : 0;
        shell_for_binding_t *b = &bindings[binding_count];

        b->var = (char)(var_name + binding_count);
        if (rest0 >= 0 && rest0 < count) {
            const char *rest = tokens[rest0].start;
            size_t rlen = strlen(rest);

            while (rlen > 0 && strchr(opts->delims, rest[rlen - 1]) != NULL) {
                rlen--;
            }
            b->text = rest;
            b->len = rlen;
        } else {
            b->text = "";
            b->len = 0;
        }
        binding_count++;
    }

    shell_for_substitute_and_run_bindings(do_command, bindings, binding_count);
}

/**
 * Iterate the lines of one `for /f` source file, honoring the skip/eol options
 * and breaking on goto/exit conditions exactly like the token loop.
 */
static void shell_forf_run_source(const char *spec, const char *do_command,
                                  char var_name, const shell_forf_options_t *opts)
{
    char *resolved = malloc(SHELL_SD_PATH_BYTES);
    char *line = malloc(SHELL_BATCH_LINE_BYTES);
    FILE *file = NULL;
    int line_count = 0;

    if (resolved == NULL || line == NULL) {
        free(resolved);
        free(line);
        shell_transcript_append_text("for /f: out of memory\n");
        return;
    }

    if (shell_fs_resolve_path(spec, resolved, SHELL_SD_PATH_BYTES) != ESP_OK) {
        free(resolved);
        free(line);
        shell_print_warning("for /f: invalid path %s", spec);
        return;
    }

    file = fopen(resolved, "r");
    free(resolved);
    if (file == NULL) {
        free(line);
        shell_print_warning("for /f: cannot open %s", spec);
        return;
    }

    while (fgets(line, SHELL_BATCH_LINE_BYTES, file) != NULL) {
        line_count++;
        if (line_count <= opts->skip) {
            continue;
        }
        if (line_count > P4_CONFIG_FORF_LINE_MAX) {
            break;
        }
        shell_forf_process_line(line, do_command, var_name, opts);
        if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
            break;
        }
    }

    fclose(file);
    free(line);
}

/**
 * Detect the `for /f ... in ('command')` command form (pure, no SD or
 * pipeline access). A single-quoted set is always the command form; with
 * @p usebackq a backquoted set is too. Surrounding whitespace is ignored.
 * On success the inner command text (without the quotes) is written to
 * @p inner_out.
 */
bool shell_forf_is_command_set(const char *set_str, bool usebackq,
                               char *inner_out, size_t inner_size)
{
    const char *s;
    size_t slen;
    char open;
    char close;
    size_t inner_len;

    if (inner_out == NULL || inner_size == 0) {
        return false;
    }
    inner_out[0] = '\0';
    if (set_str == NULL) {
        return false;
    }

    s = set_str;
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    slen = strlen(s);
    while (slen > 0 && isspace((unsigned char)s[slen - 1])) {
        slen--;
    }
    if (slen < 2) {
        return false;
    }

    open = s[0];
    close = s[slen - 1];
    if (open == '\'' && close == '\'') {
        /* Single-quoted command: always the command form. */
    } else if (usebackq && open == '`' && close == '`') {
        /* usebackq backquote command form. */
    } else {
        return false;
    }

    inner_len = slen - 2;
    if (inner_len >= inner_size) {
        inner_len = inner_size - 1;
    }
    memcpy(inner_out, s + 1, inner_len);
    inner_out[inner_len] = '\0';
    return true;
}

/**
 * Run a `for /f` loop over the captured output of a command.
 *
 * `for /f ... %%v in ('command') do ...` runs the inner command through the
 * full pipeline and iterates its stdout line by line — the cmd.exe mechanism
 * for parsing command output. The capture is the same re-entrant
 * output-redirection capture the `>`/`>>` layer uses, so an inner command
 * with its own redirect nests correctly; like pipe stages, the inner output
 * also remains visible on the transcript. Skip/eol/delims/tokens handling is
 * the shared per-line processor, so file and command forms cannot disagree.
 * Every buffer is heap-allocated: this runs on the recursive batch path.
 */
static void shell_forf_run_command(const char *inner, const char *do_command,
                                   char var_name, const shell_forf_options_t *opts)
{
    char *cmd_copy = NULL;
    const char *captured = NULL;
    size_t cap_len = 0;
    bool truncated = false;
    char *text = NULL;
    char *line = NULL;
    int line_count = 0;

    if (inner == NULL || inner[0] == '\0') {
        shell_print_warning("for /f: empty command");
        return;
    }

    cmd_copy = strdup(inner);
    if (cmd_copy == NULL) {
        shell_transcript_append_text("for /f: out of memory\n");
        return;
    }

    shell_redirect_capture_begin();
    batch_run_nested(cmd_copy);
    free(cmd_copy);
    shell_redirect_capture_end();

    captured = shell_redirect_capture_get(&cap_len);
    truncated = shell_redirect_capture_was_truncated();
    if (captured == NULL || cap_len == 0) {
        shell_redirect_capture_reset();
        return;
    }

    text = malloc(cap_len + 1);
    line = malloc(SHELL_BATCH_LINE_BYTES);
    if (text == NULL || line == NULL) {
        free(text);
        free(line);
        shell_redirect_capture_reset();
        shell_transcript_append_text("for /f: out of memory\n");
        return;
    }
    memcpy(text, captured, cap_len);
    text[cap_len] = '\0';
    shell_redirect_capture_reset();
    if (truncated) {
        shell_print_warning("for /f: command output exceeded %d bytes and was truncated",
                            P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES);
    }

    /* Walk the captured output one raw line at a time. Skip counts raw lines
     * (like the file form's fgets loop); eol/delims/tokens go through the
     * shared processor. A final unterminated chunk still counts as a line. */
    {
        char *cursor = text;

        while (*cursor != '\0') {
            size_t chunk = strcspn(cursor, "\n");
            size_t copy_len = chunk;

            if (copy_len >= (size_t)SHELL_BATCH_LINE_BYTES) {
                copy_len = (size_t)SHELL_BATCH_LINE_BYTES - 1;
            }
            memcpy(line, cursor, copy_len);
            line[copy_len] = '\0';
            cursor += chunk;
            if (*cursor == '\n') {
                cursor++;
            }
            line_count++;
            if (line_count <= opts->skip) {
                continue;
            }
            if (line_count > P4_CONFIG_FORF_LINE_MAX) {
                break;
            }
            shell_forf_process_line(line, do_command, var_name, opts);
            if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                break;
            }
        }
    }

    free(text);
    free(line);
}

/** Drive a `for /f` loop over its file-set (wildcards, names, or `<>` input). */
static void shell_execute_for_f_loop(const char *set_str, const char *do_command,
                                     char var_name, const shell_forf_options_t *opts)
{
    /* Command form first: `for /f ... %%v in ('command') do ...` (or
     * backquotes with `usebackq`) runs the inner command and iterates its
     * output. File, wildcard, and `<`/pipe-input forms fall through below. */
    {
        char *inner = malloc(SHELL_COMMAND_BYTES);

        if (inner == NULL) {
            shell_transcript_append_text("for /f: out of memory\n");
            return;
        }
        if (shell_forf_is_command_set(set_str, opts->usebackq, inner, SHELL_COMMAND_BYTES)) {
            shell_forf_run_command(inner, do_command, var_name, opts);
            free(inner);
            return;
        }
        free(inner);
    }

    if (set_str[0] == '\0') {
        /* `for /f %%v in () do ...` (or `< file` / a pipe stage): read the
         * active input-redirection source. The slot belongs to this command
         * and is cleared after dispatch. */
        char *resolved = malloc(SHELL_SD_PATH_BYTES);

        if (resolved == NULL) {
            shell_transcript_append_text("for /f: out of memory\n");
            return;
        }
        if (storage_resolve_input_source(NULL, resolved, SHELL_SD_PATH_BYTES) == ESP_OK) {
            shell_forf_run_source(resolved, do_command, var_name, opts);
        } else {
            shell_print_warning("for /f: no input file and no input redirection active");
        }
        free(resolved);
        return;
    }

    {
        char *set_copy = strdup(set_str);
        char *save_ptr = NULL;
        char *token;

        if (set_copy == NULL) {
            shell_transcript_append_text("for /f: out of memory\n");
            return;
        }

        token = strtok_r(set_copy, " \t", &save_ptr);
        while (token != NULL) {
            if (strchr(token, '*') != NULL || strchr(token, '?') != NULL) {
                char **files = NULL;
                int count = 0;
                esp_err_t error = storage_expand_wildcard(token, &files, &count);

                if (error != ESP_OK) {
                    shell_print_warning("for /f: could not expand %s (%s)", token, esp_err_to_name(error));
                } else {
                    for (int i = 0; i < count; i++) {
                        shell_forf_run_source(files[i], do_command, var_name, opts);
                        if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                            break;
                        }
                    }
                    storage_free_wildcard_expansion(files, count);
                }
            } else {
                shell_forf_run_source(token, do_command, var_name, opts);
            }

            if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                break;
            }
            token = strtok_r(NULL, " \t", &save_ptr);
        }
        free(set_copy);
    }
}

/* Helper: execute a for loop command */
static void shell_execute_for_loop(shell_batch_frame_t *frame, char *command_line)
{
    /* Defer transcript label repaints across the WHOLE loop. Each iteration
     * re-enters the command pipeline, which runs its own defer window, so
     * without this the O(buffer) LVGL span rebuild happened once per
     * iteration. One rebuild per loop instead (nested windows are safe:
     * shell_transcript_defer_begin/end are depth-counted and only the
     * outermost end flushes). */
    shell_transcript_defer_begin();
    shell_execute_for_loop_body(frame, command_line);
    shell_transcript_defer_end();
}

static void shell_execute_for_loop_body(shell_batch_frame_t *frame, char *command_line)
{
    char *set_str = NULL;
    char *for_ptr;
    char *do_command;
    char var_name;
    shell_forf_options_t forf_opts;
    bool for_f = false;
    bool for_l = false;
    bool for_a = false;
    bool for_d = false;
    bool for_r = false;
    char for_root[SHELL_SD_PATH_BYTES];

    (void)frame;
    for_root[0] = '\0';

    /* Parse: for [/f "options" | /L | /A | /D | /R [path]] %%var in (set) do command */
    for_ptr = strstr(command_line, "for ");
    if (for_ptr == NULL) return;

    /* Skip "for " */
    for_ptr += 4;
    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    /* `for /L` — numeric counting loop (cmd.exe parity). */
    if (*for_ptr == '/' && (for_ptr[1] == 'l' || for_ptr[1] == 'L') &&
        (for_ptr[2] == '\0' || isspace((unsigned char)for_ptr[2]))) {
        for_l = true;
        for_ptr += 2;
        while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;
    }

    /* `for /A` — associative-array iteration over one `PREFIX[...]`. */
    if (!for_l && *for_ptr == '/' &&
        (for_ptr[1] == 'a' || for_ptr[1] == 'A') &&
        (for_ptr[2] == '\0' || isspace((unsigned char)for_ptr[2]))) {
        for_a = true;
        for_ptr += 2;
        while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;
    }

    /* `for /D` — directory names instead of file names (cmd.exe parity). */
    if (!for_l && !for_a && *for_ptr == '/' &&
        (for_ptr[1] == 'd' || for_ptr[1] == 'D') &&
        (for_ptr[2] == '\0' || isspace((unsigned char)for_ptr[2]))) {
        for_d = true;
        for_ptr += 2;
        while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;
    }

    /* `for /R [path]` — recursive file walk (cmd.exe parity). The optional
     * root comes between the flag and the loop variable. */
    if (!for_l && !for_a && !for_d && *for_ptr == '/' &&
        (for_ptr[1] == 'r' || for_ptr[1] == 'R') &&
        (for_ptr[2] == '\0' || isspace((unsigned char)for_ptr[2]))) {
        char *path_end;

        for_r = true;
        for_ptr += 2;
        while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;
        /* A `%var` next means no root was given (the walk starts at the
         * cwd); anything else up to whitespace is the root. */
        if (*for_ptr != '%' && *for_ptr != '\0') {
            path_end = for_ptr;
            while (*path_end != '\0' && !isspace((unsigned char)*path_end)) {
                path_end++;
            }
            if ((size_t)(path_end - for_ptr) < sizeof(for_root)) {
                memcpy(for_root, for_ptr, (size_t)(path_end - for_ptr));
                for_root[path_end - for_ptr] = '\0';
            }
            for_ptr = path_end;
            while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;
        }
    }

    /* `for /f` — file-line loops. The DOS options are one quoted string
     * ("delims=, tokens=1,2"); the shell tokenizer has stripped the quotes and
     * the rejoin has left them as space-separated words, so the options region
     * runs from here up to the loop variable. */
    if (*for_ptr == '/' && (for_ptr[1] == 'f' || for_ptr[1] == 'F') &&
        (for_ptr[2] == '\0' || isspace((unsigned char)for_ptr[2]))) {
        char *pct;
        size_t opt_len;

        for_f = true;
        for_ptr += 2;
        while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

        /* The interactive tokenizer strips the quotes from the DOS options
         * string and the rejoin leaves space-separated words; the batch path
         * passes the raw line, so the surrounding double quotes survive here.
         * Drop a leading quote and trim a trailing quote/space so both paths
         * feed the parser identical text. */
        if (*for_ptr == '"') {
            for_ptr++;
        }
        pct = strchr(for_ptr, '%');
        opt_len = pct != NULL ? (size_t)(pct - for_ptr) : strlen(for_ptr);
        while (opt_len > 0 && (for_ptr[opt_len - 1] == '"' ||
                               isspace((unsigned char)for_ptr[opt_len - 1]))) {
            opt_len--;
        }
        if (!shell_forf_parse_options(for_ptr, opt_len, &forf_opts)) {
            shell_transcript_append_text("for /f: malformed options\n");
            return;
        }

        /* Advance past the options to the loop variable; the check below
         * rejects a missing `%var` (pct == NULL leaves for_ptr where it is). */
        if (pct != NULL) {
            for_ptr = pct;
        }
    }

    /* Loop variable. Batch files write `%%var` (the doubled percent is the
     * batch-file escape for a literal `%`); the interactive prompt writes
     * `%var`. Accept both. */
    if (*for_ptr != '%') return;
    for_ptr++;
    if (*for_ptr == '%') {
        for_ptr++;
    }

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

    /* `for /f` iterates file lines; the classic form iterates tokens or
     * wildcard paths. */
    if (for_f) {
        shell_execute_for_f_loop(set_str, do_command, var_name, &forf_opts);
        free(set_str);
        return;
    }

    /* `for /R` walks the tree for files matching the set pattern (an empty
     * set matches everything, cmd.exe parity). */
    if (for_r) {
        char *pattern = shell_trim(set_str);
        char **tokens = NULL;
        int count = 0;
        esp_err_t error;

        if (*pattern == '\0') {
            pattern = "*";
        }
        error = storage_expand_recursive(for_root[0] != '\0' ? for_root : ".",
                                         pattern, &tokens, &count);
        free(set_str);
        set_str = NULL;
        if (error != ESP_OK) {
            shell_print_warning("for /R: could not walk %s (%s)",
                                for_root[0] != '\0' ? for_root : ".",
                                esp_err_to_name(error));
            shell_record_warningf("for", "Recursive expansion failed");
            return;
        }
        for (int i = 0; i < count; i++) {
            shell_for_substitute_and_run(do_command, var_name, tokens[i]);
            if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                break;
            }
        }
        storage_free_wildcard_expansion(tokens, count);
        return;
    }

    /* `for /L` counts from start to end inclusive (a negative step counts
     * down). Values are bound to the loop variable as decimal text through
     * the classic substitution runner, so `%i`/`%%i` work per iteration. */
    if (for_l) {
        int32_t start = 0;
        int32_t step = 0;
        int32_t end = 0;
        int64_t value;
        int iterations = 0;
        char numbuf[12];

        if (!shell_forl_parse(set_str, &start, &step, &end)) {
            shell_transcript_append_text("for /L: needs (start,step,end) with a nonzero step\n");
            free(set_str);
            return;
        }
        free(set_str);
        set_str = NULL;
        for (value = start;
             (step > 0) ? (value <= (int64_t)end) : (value >= (int64_t)end);
             value += step) {
            if (iterations++ >= P4_CONFIG_FORL_ITER_MAX) {
                shell_transcript_append_text("for /L: iteration cap reached, stopping\n");
                break;
            }
            snprintf(numbuf, sizeof(numbuf), "%lld", (long long)value);
            shell_for_substitute_and_run(do_command, var_name, numbuf);
            if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                break;
            }
        }
        return;
    }

    /* `for /A` iterates one `PREFIX[...]`: `%%k` binds the index text and
     * the next letter (`%%l`) the live value, mirroring the `for /f`
     * consecutive-letter convention. Values read live per iteration, so a
     * body that stores stays coherent. */
    if (for_a) {
        /* The set names the prefix; surrounding whitespace is trimmed. */
        char *prefix = shell_trim(set_str);
        char (*names)[SHELL_ENV_NAME_BYTES] = NULL;
        int collected = 0;

        if (*prefix == '\0') {
            shell_transcript_append_text("for /A: needs a PREFIX in (prefix)\n");
            free(set_str);
            return;
        }
        names = malloc(sizeof(*names) * SHELL_ENV_VAR_MAX);
        if (names == NULL) {
            shell_transcript_append_text("for /A: out of memory\n");
            free(set_str);
            return;
        }
        collected = shell_fora_collect(prefix, names, SHELL_ENV_VAR_MAX);
        free(set_str);
        set_str = NULL;
        for (int i = 0; i < collected; i++) {
            /* Index text lives between the brackets of the stored name. */
            char *open = strchr(names[i], '[');
            char *close = strrchr(names[i], ']');
            char index[SHELL_ENV_NAME_BYTES];
            size_t index_len;
            const char *value;
            shell_for_binding_t bindings[2];

            if (open == NULL || close == NULL || close <= open + 1) {
                continue;
            }
            index_len = (size_t)(close - (open + 1));
            if (index_len >= sizeof(index)) {
                index_len = sizeof(index) - 1;
            }
            memcpy(index, open + 1, index_len);
            index[index_len] = '\0';
            value = shell_env_get(names[i]);
            bindings[0].var = var_name;
            bindings[0].text = index;
            bindings[0].len = index_len;
            bindings[1].var = (char)(var_name + 1);
            bindings[1].text = value != NULL ? value : "";
            bindings[1].len = value != NULL ? strlen(value) : 0;
            shell_for_substitute_and_run_bindings(do_command, bindings, 2);
            if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                break;
            }
        }
        free(names);
        return;
    }

    /* `for /D` matches directory names: wildcard tokens expand through the
     * dirs-only matcher, literal tokens pass through untouched (cmd.exe
     * parity — the set is enumerated, not verified). */
    if (for_d) {
        char *save_ptr = NULL;
        char *token = strtok_r(set_str, " \t", &save_ptr);

        while (token != NULL) {
            if (strchr(token, '*') != NULL || strchr(token, '?') != NULL) {
                char **tokens = NULL;
                int count = 0;

                if (storage_expand_dirs(token, &tokens, &count) == ESP_OK) {
                    for (int i = 0; i < count; i++) {
                        shell_for_substitute_and_run(do_command, var_name, tokens[i]);
                        if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                            break;
                        }
                    }
                    storage_free_wildcard_expansion(tokens, count);
                } else {
                    shell_print_warning("for /D: could not expand %s", token);
                }
            } else {
                shell_for_substitute_and_run(do_command, var_name, token);
            }
            if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                break;
            }
            token = strtok_r(NULL, " \t", &save_ptr);
        }
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

/**
 * Interactive `for` entry point: `for %var in (set) do command` at the shell
 * prompt. Runs outside a batch frame; the loop body re-enters the pipeline via
 * batch_run_nested, so variables, pipes, and redirection all work per-iteration.
 */
void shell_command_for(int argc, char **argv)
{
    char *command_line = malloc(SHELL_COMMAND_BYTES);

    if (command_line == NULL) {
        shell_transcript_append_text("for: out of memory\n");
        return;
    }
    shell_join_args(argv, 0, argc, command_line, SHELL_COMMAND_BYTES);
    shell_execute_for_loop(NULL, command_line);
    free(command_line);
}

/* Translate the DOS numeric-comparison keywords (`if` parity: EQU NEQ LSS
 * LEQ GTR GEQ) in a `while` condition to the `set /a` operators. Keywords
 * must be standalone whitespace-separated words; anything else (including a
 * bare `<`, which the pipeline already claimed as input redirection) is left
 * alone. Pure, so the unit tests drive it directly. */
void shell_while_translate_keywords(const char *cond, char *out, size_t out_size)
{
    const char *cursor;
    size_t used = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (cond == NULL) {
        return;
    }
    cursor = cond;
    while (*cursor != '\0') {
        const char *end;
        size_t len;
        const char *op = NULL;

        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        end = cursor;
        while (*end != '\0' && *end != ' ' && *end != '\t') {
            end++;
        }
        len = (size_t)(end - cursor);
        if (len == 3 && (strncasecmp(cursor, "EQU", 3) == 0)) {
            op = "==";
        } else if (len == 3 && (strncasecmp(cursor, "NEQ", 3) == 0)) {
            op = "!=";
        } else if (len == 3 && (strncasecmp(cursor, "LSS", 3) == 0)) {
            op = "<";
        } else if (len == 3 && (strncasecmp(cursor, "LEQ", 3) == 0)) {
            op = "<=";
        } else if (len == 3 && (strncasecmp(cursor, "GTR", 3) == 0)) {
            op = ">";
        } else if (len == 3 && (strncasecmp(cursor, "GEQ", 3) == 0)) {
            op = ">=";
        }
        if (used > 0 && used + 1 < out_size) {
            out[used++] = ' ';
        }
        if (op != NULL) {
            size_t oplen = strlen(op);
            if (used + oplen < out_size) {
                memcpy(out + used, op, oplen);
                used += oplen;
            }
        } else {
            size_t take = len;
            if (used + take >= out_size) {
                take = out_size - 1 - used;
            }
            memcpy(out + used, cursor, take);
            used += take;
        }
        out[used] = '\0';
        cursor = end;
    }
}

/* Split a `while` line into its condition and body at the first unquoted
 * `do` surrounded by whitespace (quote/caret rules match the shared
 * scanner: `"..."` groups, `'...'` is literal, `^c` escapes one char). */
static bool shell_while_split(char *line, char **cond_out, char **body_out)
{
    shell_quote_state_t quote = SHELL_QUOTE_NONE;
    char *cursor = line;

    while (*cursor != '\0') {
        if (*cursor == P4_CONFIG_ESCAPE_CHAR && quote != SHELL_QUOTE_SINGLE &&
            cursor[1] != '\0') {
            cursor += 2;
            continue;
        }
        if (*cursor == '"' && quote != SHELL_QUOTE_SINGLE) {
            quote = (quote == SHELL_QUOTE_DOUBLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_DOUBLE;
            cursor++;
            continue;
        }
        if (*cursor == '\'' && quote != SHELL_QUOTE_DOUBLE) {
            quote = (quote == SHELL_QUOTE_SINGLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_SINGLE;
            cursor++;
            continue;
        }
        if (quote == SHELL_QUOTE_NONE &&
            (cursor == line || isspace((unsigned char)cursor[-1])) &&
            (strncasecmp(cursor, "do", 2) == 0) &&
            (cursor[2] == '\0' || isspace((unsigned char)cursor[2]))) {
            *cursor = '\0';
            *cond_out = shell_trim(line);
            *body_out = shell_trim(cursor + 2);
            return true;
        }
        cursor++;
    }
    return false;
}

void shell_command_while(int argc, char **argv)
{
    char *command_line = NULL;
    char *rest = NULL;
    char *cond = NULL;
    char *body = NULL;
    int iterations = 0;

    command_line = malloc(SHELL_COMMAND_BYTES);
    if (command_line == NULL) {
        shell_transcript_append_text("while: out of memory\n");
        batch_set_errorlevel(1);
        return;
    }
    shell_join_args(argv, 0, argc, command_line, SHELL_COMMAND_BYTES);
    rest = strstr(command_line, "while ");
    if (rest == NULL) {
        shell_print_usage("Usage: while <expr> do <command>");
        batch_set_errorlevel(2);
        free(command_line);
        return;
    }
    rest += 6;
    while (*rest && isspace((unsigned char)*rest)) {
        rest++;
    }
    if (!shell_while_split(rest, &cond, &body) || *cond == '\0' || *body == '\0') {
        shell_print_usage("Usage: while <expr> do <command>");
        batch_set_errorlevel(2);
        free(command_line);
        return;
    }

    /* Per-iteration live values: the pipeline expanded `%VAR%` once before
     * dispatch, so the condition and body re-expand every pass from their
     * pristine text. A batch file writes `while %%n%% LSS 3 do ...` (the
     * pipeline turns `%%` into `%`, each pass resolves the live value); bare
     * names (`while n LSS 3 do ...`) read the table directly through the
     * evaluator. DOS keywords (`LSS`, `EQU`, ...) read like `if` and dodge
     * the `<` input-redirection trap: the pipeline claims a bare `<` before
     * dispatch, so `while %n%<3` can never arrive intact. All scratch is
     * heap-allocated (line-sized, recursive batch path). */
    {
        char *cond_live = malloc(SHELL_BATCH_LINE_BYTES);
        char *cond_eval = malloc(SHELL_BATCH_LINE_BYTES);

        if (cond_live == NULL || cond_eval == NULL) {
            shell_transcript_append_text("while: out of memory\n");
            batch_set_errorlevel(1);
            free(cond_live);
            free(cond_eval);
            free(command_line);
            return;
        }

        /* One repaint per loop (same reasoning as the `for` wrapper): each
         * body re-enters the pipeline with its own defer window. */
        shell_transcript_defer_begin();
        for (iterations = 0; iterations < P4_CONFIG_WHILE_ITER_MAX; iterations++) {
            /* Both scratch buffers are re-filled every pass: the pipeline may
             * mutate the line it runs, so every pass starts from pristine
             * text and the current environment. */
            char *run = malloc(SHELL_BATCH_LINE_BYTES * 2);
            int32_t check = 0;
            const char *error = NULL;

            if (run == NULL) {
                shell_transcript_append_text("while: out of memory\n");
                batch_set_errorlevel(1);
                break;
            }
            shell_expand_variables(cond, cond_live, SHELL_BATCH_LINE_BYTES);
            shell_while_translate_keywords(cond_live, cond_eval, SHELL_BATCH_LINE_BYTES);
            if (!shell_expr_evaluate(cond_eval, &check, &error)) {
                shell_transcript_appendf("while: %s\n", error != NULL ? error : "invalid expression");
                free(run);
                batch_set_errorlevel(1);
                break;
            }
            if (check == 0) {
                free(run);
                batch_set_errorlevel(0);
                break;
            }
            shell_expand_variables(body, run, SHELL_BATCH_LINE_BYTES * 2);
            batch_run_nested(run);
            free(run);
            if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
                batch_set_errorlevel(0);
                break;
            }
            if (iterations + 1 >= P4_CONFIG_WHILE_ITER_MAX) {
                shell_transcript_append_text("while: iteration cap reached, stopping\n");
                batch_set_errorlevel(1);
                break;
            }
        }
        shell_transcript_defer_end();
        free(cond_live);
        free(cond_eval);
    }
    free(command_line);
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void batch_init(void)
{
    if (s_initialized) {
        return;
    }

    /* Allocate the environment slot table off the internal bank (PSRAM
     * first): the table must not consume boot internal RAM ahead of the
     * command-worker stack. A failed allocation leaves the table NULL and
     * env lookups/sets degrade to no-ops. */
    if (s_shell_env_vars == NULL) {
        s_shell_env_vars = (shell_env_var_t *)p4heap_alloc_psram(SHELL_ENV_BYTES);
        if (s_shell_env_vars == NULL) {
            s_shell_env_vars = (shell_env_var_t *)p4heap_alloc_any(SHELL_ENV_BYTES);
        }
    }
    if (s_shell_env_vars == NULL) {
        return;
    }
    memset(s_shell_env_vars, 0, SHELL_ENV_BYTES);
    (void)shell_env_set("PATH", "sd:/");
    memset(s_aliases, 0, sizeof(s_aliases));

    s_active_batch_frame = NULL;
    shell_set_batch_active(false);
    s_errorlevel = 0;
    s_goto_label[0] = '\0';
    s_goto_pending = false;
    s_stop_mode = BATCH_STOP_NONE;

    /* Release any setlocal snapshot left over from a previous init. */
    shell_setlocal_unwind_to(0);
    memset(s_setlocal_stack, 0, sizeof(s_setlocal_stack));
    memset(s_delayed_stack, 0, sizeof(s_delayed_stack));
    s_setlocal_depth = 0;
    s_delayed_expansion = false;

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

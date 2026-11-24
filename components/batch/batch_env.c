/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_env.c
 * @brief Hardware-free batch environment core: variable table, per-worker
 *        context, `start` slots connected to it, and `%VAR%` expansion.
 *
 * This is the half of the batch engine that is pure state and string
 * surgery: the RAM-only variable table with its budget classes, the
 * setlocal snapshot view, the virtual (`%CD%`, `%SERIAL%`, ...) resolvers,
 * the per-worker execution context with the `start` slot pool and its
 * job-private env overlay, the `%VAR%`/`!VAR!` expansion engine
 * (substring/replace/named forms and the `%~` argument modifiers), the
 * batch argument (`%0..%9`, `%*`) expansion and the errorlevel accessors.
 *
 * Everything else stays in batch.c: the batch-file interpreter, the pipe
 * operator, the batch verbs, the alias/bind/macro tables and every console
 * entry point (`set`, `path`, `if`, `for`, ...). batch.c reaches the objects
 * here through batch_env_internal.h, so each object has exactly one
 * definition and the locking/ordering of the original single file is
 * unchanged: the shared tables still use one recursive mutex, the snapshot
 * and print paths still copy out under the lock, and every buffer keeps its
 * configured size.
 *
 * The split makes the environment core a candidate for the host unit-test
 * lane (test/host), which compiles this same source natively - never a copy.
 *
 * @module components/batch
 * @layer batch
 * @domain core
 * @provides RAM-only env table, budget classes, per-worker ctx + bg slots, %VAR% expansion, errorlevel
 * @status stable
 * @test test/main/test_calc.c
 * @hard-rule pure state - no LVGL, display, USB or board BSP in this file
 */

#include "batch.h"
#include "batch_env_internal.h"
#include "error_strings.h"
#include "shell.h"
#include "storage_path.h"
#include "clock.h"
#include "board_identity.h"
#include "p4minishell_config.h"
#include "esp_random.h"
#include "p4heap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Config aliases (same names and values as batch.c) */
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_ENV_NAME_BYTES            P4_CONFIG_ENV_NAME_BYTES
#define SHELL_ENV_VALUE_BYTES           P4_CONFIG_ENV_VALUE_BYTES
#define SHELL_ALIAS_NAME_BYTES          P4_CONFIG_ALIAS_NAME_BYTES
#define SHELL_BATCH_LINE_BYTES          P4_CONFIG_BATCH_LINE_BYTES
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES

/* ========================================================================
 * SHARED OBJECTS
 * ======================================================================== */

/** Command pipeline hooks registered by command_init() (used by
 *  batch_run_nested() below and by batch_boot_execute_command() in batch.c). */
batch_command_ops_t s_command_ops;

/* Cross-module slot-release hooks, installed by batch_init() (batch.c links
 * sync.c and svc.c). NULL on the host unit-test lane, where those modules are
 * not part of the link and there is no table to clean up. */
static const batch_bg_cleanup_ops_t *s_bg_cleanup_ops;

void batch_register_bg_cleanup_ops(const batch_bg_cleanup_ops_t *ops)
{
    s_bg_cleanup_ops = ops;
}

/* ========================================================================
 * ENVIRONMENT CORE
 * ======================================================================== */

/* Environment variables (RAM-only, not persisted across boots). The slot
 * pool is heap-allocated (PSRAM first) in batch_init(): a static table large
 * enough for the reference apps (~22 vars each) costs too much internal DIRAM
 * at boot, which starved the command-worker stack.
 *
 * The pool is partitioned into classes, each with its own budget. Slot 0
 * starts the globals run, followed by frame-locals, arrays, and maps. A slot's
 * class_tag field records its class for routing and reporting. */
shell_env_var_t *s_shell_env_vars;

/* Class boundaries within the flat pool. */
size_t s_class_start[SHELL_CLASS_COUNT];
size_t s_class_budget[SHELL_CLASS_COUNT];

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

void batch_shared_take(void)
{
    batch_shared_lock_init();
    if (s_shared_lock != NULL) {
        xSemaphoreTake(s_shared_lock, portMAX_DELAY);
    }
}

void batch_shared_give(void)
{
    if (s_shared_lock != NULL) {
        xSemaphoreGive(s_shared_lock);
    }
}

static batch_task_ctx_t s_fg_ctx;
batch_task_ctx_t *s_bg_ctx = NULL;

batch_task_ctx_t *batch_ctx_current(void)
{
    TaskHandle_t me;
    int i;

    /* xTaskGetCurrentTaskHandle is safe during init (returns NULL). */
    me = xTaskGetCurrentTaskHandle();
    if (me != NULL && s_bg_ctx != NULL) {
        for (i = 0; i < (int)P4_CONFIG_BG_TASKS; i++) {
            if (s_bg_ctx[i].in_use && s_bg_ctx[i].task == me) {
                return &s_bg_ctx[i];
            }
        }
    }
    return &s_fg_ctx;
}

/** Slot index for a ctx pointer (0 = foreground). */
int batch_ctx_slot(const batch_task_ctx_t *ctx)
{
    if (ctx == NULL || ctx == &s_fg_ctx || s_bg_ctx == NULL) {
        return 0;
    }
    {
        long index = (long)(ctx - s_bg_ctx);

        if (index < 0 || index >= (long)P4_CONFIG_BG_TASKS) {
            return 0;
        }
        return (int)index + 1;
    }
}

/** Writable slot context: slot 0 is the foreground ctx, 1..N the pool.
 *  @return NULL for a bad slot (or when the pool array is missing). */
static batch_task_ctx_t *batch_ctx_at(int slot)
{
    if (slot == 0) {
        return &s_fg_ctx;
    }
    if (slot < 1 || slot > (int)P4_CONFIG_BG_TASKS || s_bg_ctx == NULL) {
        return NULL;
    }
    return &s_bg_ctx[slot - 1];
}

/**
 * Run one nested command line through the full command pipeline.
 *
 * Every nested context (`if` bodies, `for` bodies, pipe stages, batch lines)
 * goes through here so it inherits variable expansion and redirection. The
 * hook is NULL-checked so the batch engine degrades gracefully if it is used
 * before command_init() registers the table.
 */
void batch_run_nested(char *command)
{
    if (command == NULL) {
        return;
    }

    if (s_command_ops.execute_command == NULL) {
        shell_transcript_append_text("batch: command pipeline is not available yet\n");
        shell_record_warningf("batch", "Nested execution attempted before command_init()");
        return;
    }

    /* A CPU-bound script (a tight for/while loop, a gfx animation) keeps the
     * command worker running without ever blocking between lines, which
     * starves IDLE0 and trips the task watchdog mid-app. Yield at most once a
     * second (a 1-tick delay is the minimum that lets IDLE run); the throttle
     * is a shared, atomic tick read, so the worst case is one extra yield. */
    {
        static TickType_t s_last_yield_tick;
        TickType_t now = xTaskGetTickCount();

        if ((TickType_t)(now - s_last_yield_tick) >= pdMS_TO_TICKS(1000)) {
            s_last_yield_tick = now;
            vTaskDelay(1);
        }
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

/* Virtual / read-only variable names. Resolved on read from runtime
 * state or flash; NEVER stored in the named table, so they consume
 * zero RAM slots. shell_env_set() refuses them and the expansion
 * engine short-circuits them before the table lookup.
 * ERRCODE exposes the last specific error code (additive). */
#define SHELL_ENV_VIRTUAL_NAME_COUNT 11
static const char *const s_virtual_names[SHELL_ENV_VIRTUAL_NAME_COUNT] = {
    "CD", "DATE", "ERR", "ERRCODE", "ERRMSG", "ERRORLEVEL",
    "RANDOM", "TIME", "VERSION", "BOARD", "SERIAL",
};

/* %SERIAL% is MAC-derived on the board and documented "unknown" on the host;
 * the accessor owns the one-time read and its cache. */
static bool shell_env_name_is_virtual(const char *normalized)
{
    int i;
    for (i = 0; i < SHELL_ENV_VIRTUAL_NAME_COUNT; i++) {
        if (strcmp(normalized, s_virtual_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Resolve a virtual name into @p buf. Result valid until next call for the
 * same variable (callers copy out immediately). Returns false if not virtual. */
static bool shell_env_virtual_value(const char *normalized, char *buf, size_t buf_size)
{
    if (strcmp(normalized, "ERR") == 0 || strcmp(normalized, "ERRORLEVEL") == 0) {
        snprintf(buf, buf_size, "%d", s_errorlevel);
        return true;
    }
    if (strcmp(normalized, "ERRMSG") == 0) {
        const char *msg = "error";
        if (s_errorlevel == 0) {
            msg = "success";
        } else if (s_errorlevel == 2) {
            msg = "usage";
        }
        snprintf(buf, buf_size, "%s", msg);
        return true;
    }
    if (strcmp(normalized, "ERRCODE") == 0) {
        snprintf(buf, buf_size, "%d", s_last_errorcode);
        return true;
    }
    if (strcmp(normalized, "VERSION") == 0) {
        snprintf(buf, buf_size, "%s", P4_CONFIG_VERSION_STRING);
        return true;
    }
    if (strcmp(normalized, "BOARD") == 0) {
        snprintf(buf, buf_size, "%s", BOARD_CFG_NAME);
        return true;
    }
    if (strcmp(normalized, "SERIAL") == 0) {
        snprintf(buf, buf_size, "%s", board_identity_serial());
        return true;
    }
    return false;
}

static bool shell_env_name_is_valid(const char *name)
{
    size_t index;

    if (name == NULL || name[0] == '\0') {
        return false;
    }

    /* Alphanumerics plus underscore, plus `[`/`]` so batch apps can keep
     * indexed arrays (`set SPR[3]=...` / `%SPR[3]%`), and `#` for the hash
     * slot names the `map` core writes (`CFG#<hash>#<chain>`). These are only
     * name characters here — the value side is untouched — so existing
     * names keep working and no new storage is introduced. */
    for (index = 0; name[index] != '\0'; index++) {
        if (!(isalnum((unsigned char)name[index]) || name[index] == '_' ||
              name[index] == '[' || name[index] == ']' || name[index] == '#' ||
              name[index] == '.')) {
            return false;
        }
    }

    /* Purely-numeric names are batch arguments (%1..%9), handled by the
     * expansion engine. Storing them would waste a named slot. */
    {
        bool has_non_digit = false;
        for (index = 0; name[index] != '\0'; index++) {
            if (!isdigit((unsigned char)name[index])) {
                has_non_digit = true;
                break;
            }
        }
        if (!has_non_digit) {
            return false;
        }
    }

    return true;
}

/* True when @p normalized (already-uppercased) names an array element. */
static bool shell_env_name_is_array_normalized(const char *normalized)
{
    size_t len = strlen(normalized);
    return len >= 3 && normalized[len - 1] == ']' &&
           strchr(normalized, '[') != NULL;
}

/* True when @p name is an indexed array element (e.g. "SPR[3]"). */
bool shell_env_name_is_array(const char *name)
{
    char normalized[SHELL_ENV_NAME_BYTES];
    shell_env_normalize_name(name, normalized, sizeof(normalized));
    return shell_env_name_is_array_normalized(normalized);
}

/* True when @p class_tag has a non-zero budget. */
static bool shell_env_class_is_active(shell_env_class_t class_tag)
{
    if (class_tag >= SHELL_CLASS_COUNT) {
        return false;
    }
    return s_class_budget[class_tag] > 0;
}

/* Look up a variable slot by name across the whole pool. When @p class_tag
 * is non-negative, only scan that class. When negative, scan all classes. */
shell_env_var_t *shell_env_find_slot(const char *name)
{
    char normalized[SHELL_ENV_NAME_BYTES];
    size_t index;

    shell_env_normalize_name(name, normalized, sizeof(normalized));
    for (index = 0; index < SHELL_POOL_TOTAL; index++) {
        if (s_shell_env_vars[index].used &&
            s_shell_env_vars[index].class_tag < SHELL_CLASS_COUNT &&
            strcmp(s_shell_env_vars[index].name, normalized) == 0) {
            return &s_shell_env_vars[index];
        }
    }

    return NULL;
}

/* Find a free slot within a specific class. Returns NULL when the class
 * budget is exhausted. */
static shell_env_var_t *shell_env_find_free_slot_class(shell_env_class_t class_tag)
{
    size_t index;

    if (class_tag >= SHELL_CLASS_COUNT || !shell_env_class_is_active(class_tag)) {
        return NULL;
    }
    for (index = s_class_start[class_tag]; index < s_class_start[class_tag] + s_class_budget[class_tag]; index++) {
        if (!s_shell_env_vars[index].used) {
            return &s_shell_env_vars[index];
        }
    }
    return NULL;
}

/* Determine the appropriate class for a new variable being set. Frame-local
 * scope (call/gosub active) routes to SHELL_CLASS_FRAME_LOCAL. Array-element
 * names route to SHELL_CLASS_ARRAY. Everything else is a global. The first
 * existing slot found for the name wins (no re-classification on update). */
shell_env_class_t shell_env_route_class(const char *normalized)
{
    if (shell_env_name_is_array_normalized(normalized)) {
        if (shell_env_class_is_active(SHELL_CLASS_ARRAY)) {
            return SHELL_CLASS_ARRAY;
        }
        return SHELL_CLASS_GLOBAL;  /* arrays disabled — fall back to global */
    }
    /* Frame-local: only when we are inside a call/gosub scope. */
    {
        shell_batch_frame_t *frame = batch_ctx_current()->active_frame;
        if (frame != NULL && frame->label_call_depth > 0 &&
            shell_env_class_is_active(SHELL_CLASS_FRAME_LOCAL)) {
            return SHELL_CLASS_FRAME_LOCAL;
        }
    }
    return SHELL_CLASS_GLOBAL;
}

/* Count in-use slots per class for the `slots` report. */
size_t shell_env_class_used(shell_env_class_t class_tag)
{
    size_t index, count = 0;

    if (class_tag >= SHELL_CLASS_COUNT) {
        return 0;
    }
    for (index = s_class_start[class_tag]; index < s_class_start[class_tag] + s_class_budget[class_tag]; index++) {
        if (s_shell_env_vars[index].used) {
            count++;
        }
    }
    return count;
}

/**
 * Enumerate the one slot table by a name prefix (the shared primitive behind
 * the map key collector). No new storage or matcher: names are copied out
 * under the same lock the array collector uses, and values stay live because
 * callers re-read them with shell_env_get().
 */
int shell_env_collect_prefix(const char *prefix,
                             char names[][P4_CONFIG_ENV_NAME_BYTES], int max)
{
    size_t prefix_len;
    size_t index;
    int count = 0;

    if (prefix == NULL || prefix[0] == '\0' || names == NULL || max <= 0) {
        return 0;
    }
    prefix_len = strlen(prefix);
    batch_shared_take();
    for (index = 0; index < SHELL_POOL_TOTAL && count < max; index++) {
        const char *name;

        if (!s_shell_env_vars[index].used) {
            continue;
        }
        name = s_shell_env_vars[index].name;
        if (strlen(name) < prefix_len ||
            strncasecmp(name, prefix, prefix_len) != 0) {
            continue;
        }
        snprintf(names[count], P4_CONFIG_ENV_NAME_BYTES, "%s", name);
        count++;
    }
    /* A background job's private slots live in its overlay, not the pool. */
    {
        shell_env_var_t *overlay = batch_bg_overlay(false);

        if (overlay != NULL) {
            for (index = 0; index < P4_CONFIG_BG_ENV_SLOTS && count < max; index++) {
                const char *name;
                int seen = 0;

                if (!overlay[index].used) {
                    continue;
                }
                name = overlay[index].name;
                if (strlen(name) < prefix_len ||
                    strncasecmp(name, prefix, prefix_len) != 0) {
                    continue;
                }
                for (int k = 0; k < count; k++) {
                    if (strcasecmp(names[k], name) == 0) {
                        seen = 1;
                        break;
                    }
                }
                if (seen) {
                    continue;
                }
                snprintf(names[count], P4_CONFIG_ENV_NAME_BYTES, "%s", name);
                count++;
            }
        }
    }
    batch_shared_give();
    return count;
}

const char *shell_env_get(const char *name)
{
    /* Shared table, two workers: a returned pointer stays valid only until
     * the next set (callers copy it out immediately — established pattern).
     * The lookup itself is lock-free; mutation takes the env lock. A
     * background task reads its job-private overlay first (an empty overlay
     * value shadows the global, i.e. reads cleared). */
    char normalized[SHELL_ENV_NAME_BYTES];
    shell_env_var_t *overlay;
    size_t index;
    shell_env_var_t *slot;

    shell_env_normalize_name(name, normalized, sizeof(normalized));

    /* Virtual / read-only names: computed on read, never stored. Checked
     * before the table so they always win over a user slot of the same
     * name and consume zero RAM. */
    if (shell_env_name_is_virtual(normalized)) {
        if (strcmp(normalized, "CD") == 0) {
            /* cwd lives in a storage-owned static; return it directly. */
            const char *cwd = shell_get_cwd();
            return cwd != NULL ? cwd : "";
        }
        {
            char virt_buf[40];
            if (shell_env_virtual_value(normalized, virt_buf, sizeof(virt_buf))) {
                /* Copy into a per-name static so the returned pointer stays
                 * valid until the next lookup (established copy-out discipline). */
                static char s_virtual_result[40];
                snprintf(s_virtual_result, sizeof(s_virtual_result), "%s", virt_buf);
                return s_virtual_result;
            }
        }
    }

    overlay = batch_bg_overlay(false);
    if (overlay != NULL) {
        for (index = 0; index < P4_CONFIG_BG_ENV_SLOTS; index++) {
            if (overlay[index].used && strcmp(overlay[index].name, normalized) == 0) {
                return overlay[index].value;
            }
        }
    }
    slot = shell_env_find_slot(name);

    return slot != NULL ? slot->value : NULL;
}

/** Lazily allocate (when @p create) the calling task's env overlay.
 *  @return The overlay table, or NULL for the foreground / when the
 *  allocation fails. The overlay is single-owner (its own task), so no
 *  lock is needed beyond the one the caller already holds. */
shell_env_var_t *batch_bg_overlay(bool create)
{
    batch_task_ctx_t *ctx = batch_ctx_current();
    int slot = batch_ctx_slot(ctx);

    if (slot < 1 || slot >= 1 + P4_CONFIG_BG_TASKS) {
        return NULL;
    }
    if (ctx->bg_env == NULL && create) {
        ctx->bg_env = p4heap_calloc_any(P4_CONFIG_BG_ENV_SLOTS, sizeof(shell_env_var_t));
    }
    return ctx->bg_env;
}

esp_err_t shell_env_set(const char *name, const char *value)
{
    char normalized[SHELL_ENV_NAME_BYTES];
    shell_env_var_t *slot;

    shell_env_normalize_name(name, normalized, sizeof(normalized));

    /* Virtual / read-only names: computed on read, never stored. Refuse to
     * store them so they consume zero RAM slots. */
    if (shell_env_name_is_virtual(normalized)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!shell_env_name_is_valid(normalized)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Process isolation: a background task writes overlay-only, so `set`
     * in a job can never mutate the foreground table. */
    if (batch_bg_current_slot() > 0) {
        size_t index;
        shell_env_var_t *overlay;

        batch_shared_take();
        overlay = batch_bg_overlay(true);
        if (overlay == NULL) {
            batch_shared_give();
            return ESP_ERR_NO_MEM;
        }
        slot = NULL;
        for (index = 0; index < P4_CONFIG_BG_ENV_SLOTS; index++) {
            if (overlay[index].used && strcmp(overlay[index].name, normalized) == 0) {
                slot = &overlay[index];
                break;
            }
        }
        if (value == NULL || value[0] == '\0') {
            /* Shadow-clear: an empty overlay entry hides the global rather
             * than removing it (removal would un-hide it). */
            if (slot == NULL) {
                for (index = 0; index < P4_CONFIG_BG_ENV_SLOTS; index++) {
                    if (!overlay[index].used) {
                        slot = &overlay[index];
                        break;
                    }
                }
                if (slot == NULL) {
                    batch_shared_give();
                    return ESP_ERR_NO_MEM;
                }
                slot->used = true;
                snprintf(slot->name, sizeof(slot->name), "%s", normalized);
            }
            slot->value[0] = '\0';
            batch_shared_give();
            return ESP_OK;
        }
        if (slot == NULL) {
            for (index = 0; index < P4_CONFIG_BG_ENV_SLOTS; index++) {
                if (!overlay[index].used) {
                    slot = &overlay[index];
                    break;
                }
            }
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
        shell_env_class_t routed = shell_env_route_class(normalized);
        slot = shell_env_find_free_slot_class(routed);
        if (slot == NULL) {
            batch_shared_give();
            return ESP_ERR_NO_MEM;
        }
        slot->class_tag = (uint8_t)routed;
    }

    slot->used = true;
    snprintf(slot->name, sizeof(slot->name), "%s", normalized);
    snprintf(slot->value, sizeof(slot->value), "%s", value);
    batch_shared_give();
    return ESP_OK;
}

/* Effective value of @p normalized for the calling task (overlay wins;
 * empty overlay value = shadow-cleared, still defined). Copies into
 * @p value_out. @return true when the name is defined (even as empty). */
bool shell_env_effective(const char *normalized, char *value_out, size_t value_size)
{
    const char *live = shell_env_get(normalized);

    if (live == NULL) {
        return false;
    }
    snprintf(value_out, value_size, "%s", live);
    return true;
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
    if (level == 0) {
        s_last_errorcode = ERR_BASE_OK;
    } else if (s_last_errorcode == ERR_BASE_OK || s_last_errorcode == ERR_BASE_ERROR) {
        s_last_errorcode = (level == 2) ? ERR_BASE_USAGE : ERR_BASE_ERROR;
    }
}

void batch_set_errorcode(int code)
{
    s_last_errorcode = code;
}

int batch_get_errorcode(void)
{
    return s_last_errorcode;
}

/* ========================================================================
 * BATCH ARGUMENTS AND VARIABLE EXPANSION
 * ========================================================================
 * `%0..%9`, `%*`, `%~` modifiers, `%VAR%` (with `:~start,len` and `:old=new`)
 * and the delayed `!VAR!` form. Quote/caret tracking is the shell's, so an
 * expanded value can never be re-parsed as syntax.
 */

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
        if (shell_text_equals_ignore_case(token, "RANDOM") ||
            strncasecmp(token, "RANDOM(", 7) == 0) {
            /* cmd.exe %RANDOM% is 0..32767. %RANDOM(min,max)% is bounded. */
            int r = (int)(esp_random() & 0x7FFF);
            if (token[6] == '(') {
                const char *inner = token + 7;
                const char *comma = strchr(inner, ',');
                if (comma != NULL) {
                    int lo = (int)strtol(inner, NULL, 10);
                    int hi = (int)strtol(comma + 1, NULL, 10);
                    if (hi > lo) {
                        int range = hi - lo + 1;
                        /* Use rejection sampling to avoid modulo bias */
                        int limit = (0x7FFF / range) * range;
                        while (r >= limit) {
                            r = (int)(esp_random() & 0x7FFF);
                        }
                        r = lo + (r % range);
                    }
                }
            }
            snprintf(pseudo_buf, 40, "%d", r);
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
    /* Virtual / read-only names: computed on read, never stored in the slot
     * table, so they consume zero RAM. %ERR% is the current errorlevel, the
     * short form of %ERRORLEVEL%. %ERRCODE% is the last specific error
     * code (from error_strings.h). %VERSION%/%BOARD% come from flash (compile-time
     * config). %SERIAL% is the MAC-derived chip serial, lazily filled once. */
    if (shell_text_equals_ignore_case(token, "ERR")) {
        snprintf(errorlevel_buf, 16, "%d", s_errorlevel);
        return errorlevel_buf;
    }
    if (shell_text_equals_ignore_case(token, "ERRMSG")) {
        const char *msg = "error";
        if (s_errorlevel == 0) {
            msg = "success";
        } else if (s_errorlevel == 2) {
            msg = "usage";
        }
        snprintf(pseudo_buf, 40, "%s", msg);
        return pseudo_buf;
    }
    if (shell_text_equals_ignore_case(token, "ERRCODE")) {
        snprintf(pseudo_buf, 40, "%d", s_last_errorcode);
        return pseudo_buf;
    }
    if (shell_text_equals_ignore_case(token, "VERSION")) {
        snprintf(pseudo_buf, 40, "%s", P4_CONFIG_VERSION_STRING);
        return pseudo_buf;
    }
    if (shell_text_equals_ignore_case(token, "BOARD")) {
        snprintf(pseudo_buf, 40, "%s", BOARD_CFG_NAME);
        return pseudo_buf;
    }
    if (shell_text_equals_ignore_case(token, "SERIAL")) {
        snprintf(pseudo_buf, 40, "%s", board_identity_serial());
        return pseudo_buf;
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

/** Is @p len characters at @p s a legal `%NAME%` token?
 *
 * cmd.exe names never contain whitespace, quotes or parentheses. Refusing
 * such a token is what keeps an interactive `for %f in (A) do echo %f`
 * intact: the first `%` pairs with the one in the body, and the "name"
 * between them is a sentence, not a variable. Treating it as one silently
 * deleted the loop variable and the whole command did nothing. `=` and `:`
 * stay legal because `%VAR:old=new%` is a real token. */
static bool shell_expand_name_like(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (isspace(c) || c == '"' || c == '\'' || c == '(' || c == ')') {
            return false;
        }
    }
    return len > 0;
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
                    } else if (token_len < sizeof(token) &&
                               !shell_expand_name_like(input + 1, token_len)) {
                        /* A sentence between two `%`: not a name (see above),
                         * so the percent stays literal and scanning resumes
                         * right after it. */
                        token_len = 0;
                    } else if (token_len < sizeof(token)) {
                        char pseudo_buf[40];

                        memcpy(token, input + 1, token_len);
                        token[token_len] = '\0';
                        replacement = shell_expand_token_text(token, errorlevel_buf,
                                                              pseudo_buf, strbuf,
                                                              sizeof(strbuf));
                    }
                    if (replacement != NULL) {
                        after = end + 1;   /* consumed the closing percent */
                    }
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
                    char expanded[SHELL_ENV_NAME_BYTES];

                    memcpy(token, input + 1, token_len);
                    token[token_len] = '\0';
                    /* cmd.exe expands `%VAR%`/`%1` before the delayed `!VAR!`
                     * pass, so a nested form such as `!ITEM[%1]!` must resolve
                     * its percent reference first (or the lookup uses the
                     * literal `%1` and never matches). */
                    shell_expand_variables(token, expanded, sizeof(expanded));
                    replacement = shell_expand_token_text(expanded, errorlevel_buf,
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
        batch_task_ctx_t *ctx = batch_ctx_at(i);

        if (ctx == NULL || ctx->in_use) {
            continue;
        }
        {
            ctx->in_use = true;
            ctx->task = NULL;
            ctx->kill_requested = false;
            /* Fresh execution state (PSRAM may hold a previous run). */
            ctx->active_frame = NULL;
            ctx->errorlevel = 0;
            ctx->goto_label[0] = '\0';
            ctx->goto_pending = false;
            ctx->goto_eof = false;
            ctx->stop_mode = BATCH_STOP_NONE;
            ctx->setlocal_depth = 0;
            /* Fresh isolation state: no overlay yet (lazy), no deadline. */
            ctx->bg_env = NULL;
            ctx->bg_deadline_ms = 0;
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
    batch_task_ctx_t *ctx = batch_ctx_at(slot);

    if (ctx == NULL) {
        return;
    }
    ctx->task = xTaskGetCurrentTaskHandle();
}

void batch_bg_release(int slot)
{
    batch_task_ctx_t *ctx = batch_ctx_at(slot);

    if (ctx == NULL) {
        return;
    }
    batch_bg_lock_init();
    if (s_bg_lock != NULL) {
        xSemaphoreTake(s_bg_lock, portMAX_DELAY);
    }
    ctx->in_use = false;
    ctx->task = NULL;
    ctx->kill_requested = false;
    ctx->bg_deadline_ms = 0;
    /* Isolation teardown: the job's private environment dies here, and
     * whatever himem blocks it still holds are freed (locks do not survive
     * job exit — nothing a job allocated outlives it). Service and mutex
     * ownership die here too (the supervisor reaps the service next). */
    p4heap_free(ctx->bg_env);
    ctx->bg_env = NULL;
    himem_free_owned(slot);
    if (s_bg_cleanup_ops != NULL) {
        if (s_bg_cleanup_ops->release_sync != NULL) {
            s_bg_cleanup_ops->release_sync(slot);
        }
        if (s_bg_cleanup_ops->release_service != NULL) {
            s_bg_cleanup_ops->release_service(slot);
        }
    }
    if (s_bg_lock != NULL) {
        xSemaphoreGive(s_bg_lock);
    }
}

int batch_bg_current_slot(void)
{
    return batch_ctx_slot(batch_ctx_current());
}

void batch_bg_set_deadline(int slot, uint32_t deadline_ms)
{
    batch_task_ctx_t *ctx = batch_ctx_at(slot);

    if (ctx == NULL) {
        return;
    }
    batch_bg_lock_init();
    if (s_bg_lock != NULL) {
        xSemaphoreTake(s_bg_lock, portMAX_DELAY);
    }
    ctx->bg_deadline_ms = deadline_ms;
    if (s_bg_lock != NULL) {
        xSemaphoreGive(s_bg_lock);
    }
}

size_t batch_bg_overlay_bytes(int slot)
{
    batch_task_ctx_t *ctx = batch_ctx_at(slot);
    size_t bytes = 0;
    int index;

    if (ctx == NULL || ctx->bg_env == NULL) {
        return 0;
    }
    for (index = 0; index < (int)P4_CONFIG_BG_ENV_SLOTS; index++) {
        if (ctx->bg_env[index].used) {
            bytes += strlen(ctx->bg_env[index].name) +
                     strlen(ctx->bg_env[index].value) + 2;
        }
    }
    return bytes;
}

uint32_t batch_bg_deadline_ms(int slot)
{
    batch_task_ctx_t *ctx = batch_ctx_at(slot);

    return (ctx == NULL) ? 0 : ctx->bg_deadline_ms;
}

bool batch_bg_slot_in_use(int slot)
{
    batch_task_ctx_t *ctx = batch_ctx_at(slot);

    return (ctx != NULL) && ctx->in_use;
}

void batch_bg_poll_timeout(void)
{
    batch_task_ctx_t *ctx = batch_ctx_current();
    int slot = batch_ctx_slot(ctx);
    uint32_t now;

    if (slot < 1 || slot >= 1 + P4_CONFIG_BG_TASKS) {
        return;
    }
    if (ctx->bg_deadline_ms == 0 || ctx->kill_requested) {
        return;
    }
    now = xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;
    /* Wrap-safe: the deadline is at most ~a day out, far below 2^31 ms. */
    if ((int32_t)(now - ctx->bg_deadline_ms) >= 0) {
        ctx->kill_requested = true;
    }
}

void batch_bg_request_kill(int slot)
{
    batch_task_ctx_t *ctx = batch_ctx_at(slot);

    if (ctx == NULL) {
        return;
    }
    ctx->kill_requested = true;
}

bool batch_bg_kill_requested(void)
{
    return batch_ctx_current()->kill_requested;
}

bool batch_bg_is_background(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    int i;

    if (me == NULL || s_bg_ctx == NULL) {
        return false;
    }
    for (i = 0; i < (int)P4_CONFIG_BG_TASKS; i++) {
        if (s_bg_ctx[i].in_use && s_bg_ctx[i].task == me) {
            return true;
        }
    }
    return false;
}

/* ========================================================================
 * DELAYED EXPANSION (`!VAR!`)
 * ========================================================================
 * The flag lives in the per-worker context, and every `setlocal` scope keeps
 * its own saved value so `endlocal` restores exactly what it saw.
 */

bool shell_delayed_expansion_enabled(void)
{
    return s_delayed_expansion;
}

/** Test hook: set the delayed-expansion flag directly. */
void shell_set_delayed_expansion(bool enabled)
{
    s_delayed_expansion = enabled;
}

/* ========================================================================
 * POOL LIFECYCLE
 * ======================================================================== */

bool batch_env_pool_init(void)
{
    /* Compute class boundaries within the flat pool. Each class owns a
     * contiguous range of slots. A class with budget 0 is skipped. */
    {
        s_class_start[0] = 0;
        s_class_budget[0] = P4_CONFIG_ENV_VAR_MAX;
        s_class_start[1] = s_class_budget[0];
        s_class_budget[1] = P4_CONFIG_ENV_FRAME_LOCAL_SLOTS;
        s_class_start[2] = s_class_start[1] + s_class_budget[1];
        s_class_budget[2] = P4_CONFIG_ENV_ARRAY_SLOTS;
        s_class_start[3] = s_class_start[2] + s_class_budget[2];
        s_class_budget[3] = P4_CONFIG_ENV_MAP_SLOTS;
    }

    /* Allocate the environment slot pool off the internal bank (PSRAM
     * first): the pool must not consume boot internal RAM ahead of the
     * command-worker stack. A failed allocation leaves the pool NULL and
     * env lookups/sets degrade to no-ops. */
    if (s_shell_env_vars == NULL) {
        s_shell_env_vars = (shell_env_var_t *)p4heap_alloc_psram(SHELL_POOL_BYTES);
        if (s_shell_env_vars == NULL) {
            s_shell_env_vars = (shell_env_var_t *)p4heap_alloc_any(SHELL_POOL_BYTES);
        }
    }
    /* The background ctx pool (~10 KB per slot) likewise lives in PSRAM:
     * BSS growth here directly eats the boot internal-RAM window that the
     * command worker (32 KB contiguous) and the Wi-Fi task (12 KB) need.
     * A failed allocation leaves `start` with no slots (clean refusal). */
    if (s_bg_ctx == NULL) {
        s_bg_ctx = (batch_task_ctx_t *)p4heap_calloc_psram(P4_CONFIG_BG_TASKS,
                                                           sizeof(*s_bg_ctx));
    }
    if (s_shell_env_vars == NULL) {
        return false;
    }
    memset(s_shell_env_vars, 0, SHELL_POOL_BYTES);
    return true;
}

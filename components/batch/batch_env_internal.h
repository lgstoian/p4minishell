/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_env_internal.h
 * @brief Private seam between the batch engine (batch.c) and its
 *        hardware-free environment core (batch_env.c).
 *
 * batch_env.c owns the state that needs no display, no SD card and no board
 * BSP: the RAM-only variable table with its budget classes, the per-worker
 * batch execution context, the `start` slot pool, `%VAR%`/`!VAR!` expansion
 * and the errorlevel machinery. batch.c keeps every console/command entry
 * point (`set`, `path`, `if`, `for`, aliases, batch-file execution, ...) and
 * reaches the shared objects through this header, so each object is defined
 * exactly once.
 *
 * The header also carries the `s_*` accessor macros: they are the historical
 * names for the current task's context fields, and both translation units
 * spell the same state the same way.
 *
 * The header is private to components/batch; no other component includes it.
 *
 * @module components/batch
 * @layer batch
 * @domain core
 * @provides internal declarations for state owned by batch_env.c
 * @status stable
 * @test test/main/test_calc.c
 * @hard-rule declarations only - no storage, display, USB or board BSP here
 */

#ifndef P4MINISHELL_BATCH_ENV_INTERNAL_H
#define P4MINISHELL_BATCH_ENV_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "batch.h"
#include "p4minishell_config.h"

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * SHARED TYPES
 * ========================================================================
 * The variable slot and the per-worker context are read (and, for the
 * setlocal snapshot, copied wholesale) by batch.c, so their layout is part
 * of this seam. Sizes stay the configured ones: enlarging a slot here would
 * change the pool footprint at boot.
 */

/** One RAM-only environment variable slot. */
typedef struct {
    bool used;
    uint8_t class_tag;            /**< SHELL_CLASS_* budget class. */
    char name[P4_CONFIG_ENV_NAME_BYTES];
    char value[P4_CONFIG_ENV_VALUE_BYTES];
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
    char name[P4_CONFIG_BATCH_LABEL_BYTES];
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
    char (*args)[P4_CONFIG_COMMAND_BYTES]; /**< Saved caller argument copies. */
} shell_label_call_t;

/** Execution context for one nested batch file. */
typedef struct shell_batch_frame {
    bool echo_enabled;
    int argc;
    int depth;
    char args[P4_CONFIG_BATCH_ARGS_MAX][P4_CONFIG_COMMAND_BYTES];
    struct shell_batch_frame *parent;
    shell_batch_label_t labels[P4_CONFIG_BATCH_LABEL_MAX];
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
    /** Frame-local slots already used when this batch file started. Slots
     *  created while it runs are released when the file returns, so a long
     *  sweep of apps cannot exhaust the 32-slot pool. */
    uint32_t frame_slots_on_entry;
    /** Nested `call :label` scopes (`call_resume_pos` / argument saves). */
    shell_label_call_t label_calls[P4_CONFIG_BATCH_DEPTH_MAX + 2];
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

/* Batch execution state (per worker task).
 *
 * The main worker owns slot 0; `start` background tasks own the rest.
 * Everything a batch run mutates while executing — frame stack, errorlevel,
 * goto state, stop mode, setlocal scopes — lives here so two workers never
 * share them. The environment table is global, but a background task writes
 * through its lazily-allocated job-private overlay (`bg_env`) and reads
 * overlay-first, so `set` in a job can never leak into the foreground
 * (process isolation); the overlay dies with the job. Aliases and cwd stay
 * global (shared services, like the filesystem).
 * Code below keeps using the historical `s_*` names: they are macros into
 * the current task's slot, so the ~140 use sites needed no edits. Unknown
 * tasks (unit tests, init) fall back to slot 0.
 *
 * Memory placement matters: one ctx is ~10 KB (command-size goto/label
 * scratch), so only slot 0 lives in BSS — the pool slots live in PSRAM
 * (allocated at batch_init). BSS growth here directly eats the boot
 * internal-RAM window that the command worker (32 KB contiguous) and the
 * Wi-Fi task (12 KB) need. */
typedef struct {
    shell_batch_frame_t *active_frame;
    int errorlevel;
    int last_errorcode;       /**< Last specific error code (ERR_*), 0 = none. */
    char goto_label[P4_CONFIG_COMMAND_BYTES];
    bool goto_pending;
    bool goto_eof;
    /* >0 while a for/while body is executing: `call :label`/`gosub :label`
     * inside must run inline, because a goto set from a loop body is only
     * processed by the enclosing frame after the loop returns. */
    int loop_body_depth;
    batch_stop_mode_t stop_mode;
    shell_env_var_t *setlocal_stack[P4_CONFIG_SETLOCAL_DEPTH_MAX];
    int setlocal_depth;
    /* Delayed-expansion flag (`!VAR!`) with one saved value per open scope,
     * so `endlocal` (and the frame-return unwind) restores exactly what the
     * matching `setlocal` saw — the COMMAND.COM contract. */
    bool delayed_expansion;
    bool delayed_stack[P4_CONFIG_SETLOCAL_DEPTH_MAX];
    TaskHandle_t task;      /* owning bg task, NULL for slot 0 */
    bool in_use;            /* bg slot claimed by a live task */
    bool kill_requested;    /* taskkill: unwind at the next batch line */
    char all_args[P4_CONFIG_BATCH_LINE_BYTES]; /* `%*` scratch (per task) */
    /* Job-private environment overlay (`set` writes land here, reads check
     * here first). NULL until the job's first `set` (dynamic: foreground
     * and read-only jobs cost nothing); freed at job exit. An entry with an
     * empty value shadows (clears) the same-named global. `setlocal` scopes
     * snapshot the global table only — the overlay is untouched by
     * endlocal and dies with the job. */
    shell_env_var_t *bg_env;
    /* Auto-kill deadline (`start /t:secs`, tick ms, 0 = none), enforced by
     * batch_bg_poll_timeout() at the cooperative poll points. */
    uint32_t bg_deadline_ms;
    /* break / continue request flags (set by the verbs, cleared by loops). */
    bool break_requested;
    bool continue_requested;
} batch_task_ctx_t;

/* ========================================================================
 * SHARED OBJECTS (defined once, in batch_env.c)
 * ======================================================================== */

/** Flat variable pool (heap, PSRAM-first; allocated by batch_env_pool_init()). */
extern shell_env_var_t *s_shell_env_vars;

/** Class boundaries within the flat pool (filled by batch_env_pool_init()). */
extern size_t s_class_start[SHELL_CLASS_COUNT];
extern size_t s_class_budget[SHELL_CLASS_COUNT];

/** Background `start` slots; NULL until batch_env_pool_init() allocates them. */
extern batch_task_ctx_t *s_bg_ctx;

/** Command pipeline hooks (written by batch_register_command_ops()). */
extern batch_command_ops_t s_command_ops;

/** Total slots in one pool, and its byte size. */
#define SHELL_POOL_TOTAL (P4_CONFIG_ENV_VAR_MAX + P4_CONFIG_ENV_FRAME_LOCAL_SLOTS + \
                           P4_CONFIG_ENV_ARRAY_SLOTS + P4_CONFIG_ENV_MAP_SLOTS)
#define SHELL_POOL_BYTES ((size_t)sizeof(shell_env_var_t) * (size_t)SHELL_POOL_TOTAL)

/* ========================================================================
 * SHARED HELPERS
 * ======================================================================== */

/**
 * Compute the class boundaries, allocate the variable pool and the `start`
 * slot pool, and clear the pool.
 *
 * Called once from `batch_init()`. @return false when the variable pool
 * could not be allocated (env lookups then degrade to no-ops, exactly as
 * before the split).
 */
bool batch_env_pool_init(void);

/** The calling task's context (foreground slot 0 for unknown tasks). */
batch_task_ctx_t *batch_ctx_current(void);

/** Slot index for a ctx pointer (0 = foreground, 1..N the `start` pool). */
int batch_ctx_slot(const batch_task_ctx_t *ctx);

/**
 * Run one nested command line through the full command pipeline.
 *
 * Shared with batch.c (the `if`/loop/pipe bodies expand and dispatch through
 * it); the hook is NULL-checked so a pre-command_init() call degrades
 * gracefully.
 */
void batch_run_nested(char *command);

/** Take/release the table lock guarding the shared env/alias tables. */
void batch_shared_take(void);
void batch_shared_give(void);

/** Find an in-use slot by (any) name; NULL when undefined. */
shell_env_var_t *shell_env_find_slot(const char *name);

/** Effective value for the calling task (own overlay wins over global). */
bool shell_env_effective(const char *normalized, char *value_out, size_t value_size);

/** The calling task's private overlay table (NULL for the foreground). */
shell_env_var_t *batch_bg_overlay(bool create);

/** Count in-use slots in one class (for the `slots` report). */
size_t shell_env_class_used(shell_env_class_t class_tag);
/** Used-slot bitmap for the frame-local class (one bit per slot). */
uint32_t shell_env_frame_local_used_mask(void);
/** Release frame-local slots that are used now but were free in @p before. */
void shell_env_release_new_frame_locals(uint32_t before);

/** Budget class a new variable name routes to (frame-local/array/global). */
shell_env_class_t shell_env_route_class(const char *normalized);

/* ========================================================================
 * CURRENT-TASK FIELD ALIASES
 * ========================================================================
 * The batch engine predates its split and spells the running task's state
 * with `s_` names. They are macros, so the ~140 use sites stay unchanged and
 * both halves of the engine see the same field.
 */
#define s_active_batch_frame (batch_ctx_current()->active_frame)
#define s_errorlevel         (batch_ctx_current()->errorlevel)
#define s_last_errorcode     (batch_ctx_current()->last_errorcode)
#define s_goto_label         (batch_ctx_current()->goto_label)
#define s_goto_pending       (batch_ctx_current()->goto_pending)
#define s_goto_eof           (batch_ctx_current()->goto_eof)
#define s_loop_body_depth    (batch_ctx_current()->loop_body_depth)
#define s_stop_mode          (batch_ctx_current()->stop_mode)
#define s_setlocal_stack     (batch_ctx_current()->setlocal_stack)
#define s_setlocal_depth     (batch_ctx_current()->setlocal_depth)
#define s_delayed_expansion  (batch_ctx_current()->delayed_expansion)
#define s_delayed_stack      (batch_ctx_current()->delayed_stack)
#define s_break_requested    (batch_ctx_current()->break_requested)
#define s_continue_requested (batch_ctx_current()->continue_requested)

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_BATCH_ENV_INTERNAL_H */

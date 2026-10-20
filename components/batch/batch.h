/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch.h
 * @brief Batch engine, environment variables, and batch language commands.
 *
 * Owns everything the `.bat` interpreter needs:
 *   - Batch file execution with nesting, `@` echo suppression, and `rem`/`::`
 *   - `:label` scanning, `goto`, and `call :label` targets
 *   - `for %%var in (set) do command` loops
 *   - The `|` pipe operator
 *   - RAM-only environment variables and PATH (class-tagged slot pool:
 *     globals, frame-locals, arrays, maps — each with its own budget)
 *   - Variable expansion: `%VAR%`, `%0`, `%1`..`%9`, `%*`
 *   - Errorlevel tracking
 *   - The batch language commands: `set`, `path`, `echo`, `call`, `if`,
 *     `goto`, `shift`, `pause`, `choice`, `setlocal`, `endlocal`, `exit`,
 *     `slots`
 *
 * Architecture:
 *   Dependencies flow one way: `command` -> `batch` -> `storage` -> `shell`.
 *   Nested execution contexts must re-enter the full command pipeline so they
 *   inherit variable expansion and output redirection. Because that pipeline
 *   lives in `components/command/`, this module reaches it through the
 *   registered `batch_command_ops_t` table instead of an include-time
 *   dependency, mirroring the `shell_command_ops_t` pattern.
 */

#ifndef P4MINISHELL_BATCH_H
#define P4MINISHELL_BATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * COMMAND MODULE HOOKS
 * ========================================================================
 * The batch engine needs the full command pipeline (expansion, redirection,
 * dispatch) for every nested line it runs. Routing it through a registered
 * operations table keeps the dependency one-way: command.c depends on
 * batch.c, never the reverse.
 */

typedef struct {
    /** Execute a command line with variable expansion and redirection. */
    void (*execute_command)(char *command);
} batch_command_ops_t;

/**
 * Register the command module's operations table.
 * Called once by `command_init()` during boot. Passing NULL clears the table,
 * after which nested batch execution degrades gracefully rather than
 * dereferencing a null pointer.
 */
void batch_register_command_ops(const batch_command_ops_t *ops);

/* ========================================================================
 * SLOT POOL CLASSES (spillover tier)
 * ========================================================================
 * The single environment slot table is split into named classes, each with
 * its own budget and its own exhaustion error. A runaway array can no
 * longer starve scalar variables, and an app that uses no maps pays nothing
 * for them.
 */

/** Class tags for per-class slot budgeting. */
typedef enum {
    SHELL_CLASS_GLOBAL = 0,       /**< set, set /a, set /p scalars. */
    SHELL_CLASS_FRAME_LOCAL,      /**< call/gosub temporaries. */
    SHELL_CLASS_ARRAY,            /**< NAME[i] element slab. */
    SHELL_CLASS_MAP,              /**< hash slab (reserved). */
    SHELL_CLASS_COUNT             /**< number of classes (always last). */
} shell_env_class_t;

/** Per-class budget descriptor for the `slots` report. */
typedef struct {
    shell_env_class_t class_tag;  /**< class identifier. */
    const char *name;             /**< human-readable class name. */
    size_t budget;                /**< configured slot budget. */
    size_t used;                  /**< slots currently in use. */
} shell_env_class_info_t;

/**
 * Get per-class budget and usage info for the `slots` report.
 * @param info  Output array of SHELL_CLASS_COUNT entries.
 * @return The number of classes written.
 */
int shell_env_class_info(shell_env_class_info_t *info);

/**
 * Count existing NAME[i] element slots for a dimensioned array.
 * Scans the pool for slots whose name starts with "NAME[".
 * @param name  Array base name (e.g. "SPR").
 * @return Number of NAME[i] slots currently allocated.
 */
int shell_env_array_count(const char *name);

/**
 * True when @p name is an indexed array element ("NAME[i]").
 * Pure — usable from batch_expr.c and calc.c.
 */
bool shell_env_name_is_array(const char *name);

/**
 * `slots` — per-class budget report.
 *   slots              show all classes (used / budget)
 */
void shell_command_slots(int argc, char **argv);

/**
 * `dim` — bounds-checked array dimensioning with C pointer-like semantics.
 *
 *   dim SPR 8 0                 allocate SPR[0..7], default 0
 *   dim MAT 3 4 0               allocate 3x4 matrix (12 elements)
 *   dim /len SPR                -> length
 *   dim /info SPR               -> metadata (dims, length)
 *   dim /push SPR 42            append at first free slot
 *   dim /pop SPR [TARGET]       remove last, optionally store in TARGET
 *   dim /shift SPR [TARGET]     remove first, shift left
 *   dim /unshift SPR 42         insert at beginning, shift right
 *   dim /ins SPR 3 99           insert at index, shift right
 *   dim /rem SPR 3              remove at index, shift left
 *   dim /slice SPR 2 4 COPY     copy 4 elements into COPY
 *   dim /copy SRC DST           duplicate array (with dims)
 *   dim /fill SPR VAL [S C]     fill elements with value
 *   dim /reverse SPR            reverse in place
 *   dim /sort SPR [/n] [/d]     sort in place (numeric, descending)
 *   dim /find SPR VAL [IDX]     find value, return index or -1
 *   dim /sum SPR DEST           sum of numeric elements
 *   dim /avg SPR DEST           average of numeric elements
 *   dim /min SPR DEST           minimum numeric element
 *   dim /max SPR DEST           maximum numeric element
 *   dim /cmp ARR1 ARR2          compare arrays (EL 0=same, 1=diff)
 *   dim /ref PTR ARR            make PTR reference ARR (pointer)
 *   dim /get PTR IDX [C] DEST   read through pointer (1D or 2D)
 *   dim /set PTR IDX [C] VAL    write through pointer (1D or 2D)
 *   dim /del SPR                free all slots
 */
void shell_command_dim(int argc, char **argv);

/**
 * `str` — string operation suite.
 *
 *   str /len VAR                 length of VAR
 *   str /sub VAR START LEN DEST  substring
 *   str /find VAR NEEDLE [IDX]   index or -1
 *   str /upper VAR DEST
 *   str /lower VAR DEST
 *   str /trim VAR DEST
 *   str /rep VAR OLD NEW DEST    replace all
 *   str /split VAR DELIM DEST    split into DEST[0..n-1]
 *   str /join VAR DELIM DEST     join DEST with DELIM
 *   str /fmt DEST "f" args...    %1..%9 substitution
 */
void shell_command_str(int argc, char **argv);

/**
 * `map` — associative key->value store.
 *
 *   map /set CFG host "p4mini"     set key
 *   map /get CFG host DEST         get key into DEST
 *   map /has CFG host              errorlevel 0/1
 *   map /del CFG host              remove key
 *   map /keys CFG KEYS             emit keys into KEYS[0..n-1]
 *   map /size CFG                  number of entries
 *   map /clear CFG                 remove all entries
 */
void shell_command_map(int argc, char **argv);

/**
 * `trap` — structured error handling.
 *
 *   trap :label                    install handler for this frame
 *   trap /on errorlevel|exit|int   event mask (reserved)
 *   trap /raise CODE "message"     manual raise
 *   trap /clear                    remove handler
 *
 * Engine hooks (called from the dispatcher on errorlevel):
 *   trap_handler_installed()
 *   trap_handler_label()
 *   trap_handler_pop()
 *   trap_set_message(msg)
 */
void shell_command_trap(int argc, char **argv);
bool trap_handler_installed(void);
const char *trap_handler_label(void);
void trap_handler_pop(void);
void trap_set_message(const char *msg);
void trap_init(void);

/* ========================================================================
 * ENVIRONMENT VARIABLES AND EXPANSION
 * ======================================================================== */

/**
 * Look up an environment variable by name (case-insensitive).
 * A background task reads its job-private overlay first, then the global
 * table; the foreground reads the global table only.
 * @return Pointer to the stored value, or NULL when the name is not defined.
 */
const char *shell_env_get(const char *name);

/**
 * Create, update, or clear an environment variable.
 * Passing an empty or NULL value clears the slot. A background task writes
 * into its job-private overlay (lazily allocated, bounded by
 * P4_CONFIG_BG_ENV_SLOTS, discarded at job exit) so it can never mutate the
 * foreground environment; aliases stay global by design.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a malformed name, or
 *         ESP_ERR_NO_MEM when all slots are in use.
 */
esp_err_t shell_env_set(const char *name, const char *value);

/**
 * Expand `%VAR%`, `%0` (script name), `%1`..`%9`, and `%*` (all arguments)
 * into @p output. An undefined `%VAR%` expands to the empty string (cmd.exe
 * parity), and `%%` yields a literal `%`. DOS string forms are supported:
 * `%VAR:~start[,len]%` slices a substring (negative counts from the end) and
 * `%VAR:old=new%` replaces every (case-insensitive) occurrence of @p old.
 */
void shell_expand_variables(const char *input, char *output, size_t output_size);

/**
 * Slice a `%VAR:~start[,len]%` substring (pure, unit-tested).
 *
 * @param value  The variable's value (never NULL).
 * @param start  First kept character; negative counts back from the end.
 * @param has_len  Whether @p len is present.
 * @param len    Kept characters; negative drops that many from the end.
 * @param out    Destination (always NUL-terminated).
 */
void shell_expand_substring(const char *value, int start, bool has_len, int len,
                            char *out, size_t out_size);

/**
 * Apply a `%VAR:old=new%` replacement (pure, unit-tested): every
 * case-insensitive occurrence of @p old in @p value becomes @p new_str.
 * An empty @p old copies @p value unchanged. Always NUL-terminates @p out.
 */
void shell_expand_replace(const char *value, const char *old, const char *new_str,
                          char *out, size_t out_size);

/**
 * Delayed-expansion flag for the current task (`!VAR!`, cmd.exe parity).
 * Set by `setlocal enabledelayedexpansion|disabledelayedexpansion`, saved and
 * restored with every setlocal scope (including the automatic unwind).
 */
bool shell_delayed_expansion_enabled(void);
void shell_set_delayed_expansion(bool enabled);

/* ========================================================================
 * ALIASES (alias / unalias, DOSKEY-style macros)
 * ======================================================================== */

/**
 * Look up an alias by name (case-insensitive).
 * @return Pointer to the stored value, or NULL when the name is not defined.
 */
const char *shell_alias_get(const char *name);

/**
 * Create, update, or clear an alias. Passing an empty or NULL value clears the
 * slot.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a malformed name, or
 *         ESP_ERR_NO_MEM when all slots are in use.
 */
esp_err_t shell_alias_set(const char *name, const char *value);

/** Number of aliases currently defined. */
int shell_alias_count(void);

/**
 * Copy an alias by table index.
 * @return true when the index is valid and the output pointers were filled.
 */
bool shell_alias_get_by_index(int index, char *name_out, size_t name_size,
                              char *value_out, size_t value_size);

/**
 * Expand the leading command word as a DOSKEY-style macro, writing the result
 * to @p out. Expansion happens only at the interactive prompt — never inside
 * a batch file — so an alias cannot shadow a batch verb.
 *
 * @return true when the command was rewritten, false when the first word is
 *         not an alias, the result would not fit, or a batch file is active.
 */
bool batch_alias_expand_command(const char *command, char *out, size_t out_size);

/**
 * `alias` — list, query, set, or clear aliases, and persist them.
 *
 *   alias                 list every alias
 *   alias name            show one alias
 *   alias name=value      set an alias
 *   alias name=           clear one alias
 *   alias /clear          clear all aliases
 *   alias /save [file]    write the table to the SD profile (default
 *                         P4_CONFIG_ALIAS_PROFILE)
 *   alias /load [file]    run the SD profile (batch of `alias` lines)
 */
void shell_command_alias(int argc, char **argv);

/** `unalias name` — remove one alias (shorthand for `alias name=`). */
void shell_command_unalias(int argc, char **argv);

/* ========================================================================
 * KEY BINDS (bind F1..F12 + Ctrl+letter chords, USB keys)
 * ========================================================================
 * Chord codes are 0x80 | HID letter (a=0x04..z=0x1D); ^C (0x86) is reserved
 * for the foreground break and can never be bound.
 */

/**
 * Bind, rebind, or clear a key. Codes are USB HID F1..F12 (0x3A..0x45) or
 * chord codes (0x80 | HID letter). Passing an empty or NULL line clears
 * the slot.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a non-bindable code, or
 *         ESP_ERR_NO_MEM when all slots are in use.
 */
esp_err_t shell_bind_set(uint8_t code, const char *line);

/**
 * Copy the bound line for an F-key code into @p out.
 * @return true when the key is bound.
 */
bool shell_bind_lookup_fkey(uint8_t code, char *out, size_t out_size);

/**
 * Copy the bound line for a Ctrl+letter chord into @p out. Only fires for
 * Ctrl held with a letter key (a..z); ^C never matches (reserved).
 * @return true when the chord is bound.
 */
bool shell_bind_lookup_chord(uint8_t key_code, uint8_t modifiers,
                             char *out, size_t out_size);

/** Number of keys currently bound. */
int shell_bind_count(void);

/**
 * Copy a bind by table index.
 * @return true when the index is valid and the outputs were filled.
 */
bool shell_bind_get_by_index(int index, uint8_t *code_out, char *value_out,
                             size_t value_size);

/**
 * `bind` — list, set, or clear key bindings, and persist them.
 *
 *   bind                        list every bind
 *   bind F5 <line...>           bind a function key to a command line
 *   bind ^G <line...>           bind a Ctrl+letter chord (^C is reserved)
 *   bind unbind F5|^G           clear one bind
 *   bind /clear                 clear all binds
 *   bind /save [file]           write the table to the SD profile (default
 *                               P4_CONFIG_BIND_PROFILE)
 *   bind /load [file]           run the SD profile (batch of `bind` lines)
 */
void shell_command_bind(int argc, char **argv);

/* ========================================================================
 * MACRO RECORDER (95LX-style command capture)
 * ========================================================================
 * `macro record` captures every submitted command line into a RAM buffer;
 * `macro stop` writes it as a .bat file; `macro play` runs such a file via
 * the `call` machinery. The recorder hook lives in the async submit path,
 * so both typed surfaces (LVGL input line, serial console) and touch-tap
 * actions are captured; `macro ...` control lines are never recorded.
 */

/**
 * Append a submitted line to the active recording (no-op when idle).
 * Overlong input truncates the buffer and auto-stops with an overflow mark
 * reported by `macro stop`/`status`.
 */
void batch_macro_record_line(const char *line);

/** Recording state for `macro status` and the unit tests. */
typedef struct {
    bool active;                 /**< A recording is in progress. */
    bool overflowed;             /**< The buffer filled and recording stopped. */
    size_t used;                 /**< Bytes captured (excluding NUL). */
    size_t capacity;             /**< Buffer capacity in bytes. */
    char file[P4_CONFIG_SD_PATH_BYTES]; /**< Target .bat (resolved at record). */
} batch_macro_status_t;

/** Snapshot the recorder state (all out-pointers may be NULL). */
void batch_macro_status(batch_macro_status_t *out);

/** Read-only view of the captured text ("" when idle/empty). Unit tests. */
const char *batch_macro_text(void);

/** Stop and discard the recording without writing. Unit tests. */
void batch_macro_discard(void);

/**
 * `macro` — record/stop/play/status command macros.
 *
 *   macro record [file]         capture submitted lines (default MACRO.BAT)
 *   macro stop                  write the capture as a .bat file
 *   macro play <file>           run a captured file via `call`
 *   macro status                show recording state
 */
void shell_command_macro(int argc, char **argv);

/* ========================================================================
 * ERRORLEVEL
 * ======================================================================== */

/** Get the current errorlevel (0 = success). */
int batch_get_errorlevel(void);

/** Set the current errorlevel. */
void batch_set_errorlevel(int level);

/** Set a specific error code (ERR_* from error_strings.h). */
void batch_set_errorcode(int code);

/** Get the most recent specific error code. */
int batch_get_errorcode(void);

/* ========================================================================
 * BACKGROUND TASK SLOTS (`start` pool)
 * ======================================================================== */

/** Claim a free background ctx slot (>= 0) or -1 when the pool is full.
 * The caller stores its task handle via batch_bg_bind() once created. */
int batch_bg_alloc(void);

/** Bind a task handle to a claimed slot (called once by the new task). */
void batch_bg_bind(int slot);

/** Release a slot (called when the bg task exits). */
void batch_bg_release(int slot);

/** Request cooperative stop of a slot (checked per batch line). */
void batch_bg_request_kill(int slot);

/** True when the current task's slot has a pending kill request. */
bool batch_bg_kill_requested(void);

/** Slot index of the calling task (0 = foreground/main worker, N = bg slot
 *  N). Unknown tasks (unit tests, init) report 0. */
int batch_bg_current_slot(void);

/** Set an auto-kill deadline for a slot (`start /t:secs`; 0 clears it). The
 *  deadline is enforced cooperatively at the same poll points as `taskkill`
 *  (batch line loop + `delay` chunks), so a timed-out job unwinds exactly
 *  like a killed one. */
void batch_bg_set_deadline(int slot, uint32_t deadline_ms);

/** Enforce the current task's auto-kill deadline (call at every cooperative
 *  poll point alongside `batch_bg_kill_requested()`). */
void batch_bg_poll_timeout(void);

/** True when the current task runs on a background slot. */
bool batch_bg_is_background(void);

/** Bytes held by a slot's private environment overlay (name + value +
 *  separators of every set entry; 0 when the slot has no overlay). Slot 0
 *  (foreground) has no overlay and always reports 0. Used by the task
 *  manager's per-job memory accounting. */
size_t batch_bg_overlay_bytes(int slot);

/** A slot's auto-kill deadline in tick ms (0 = none). Read-only accessor
 *  for the task manager's job listing. */
uint32_t batch_bg_deadline_ms(int slot);

/** True when a background slot is claimed (1..P4_CONFIG_BG_TASKS). */
bool batch_bg_slot_in_use(int slot);

/* ========================================================================
 * BATCH ENGINE
 * ======================================================================== */

/**
 * Resolve a command name to a batch file on the SD card.
 * Tries the literal name, then the name with `.bat` appended, then each
 * `;`-separated PATH entry with both forms.
 *
 * @return true when @p resolved_path holds an existing regular file.
 */
bool shell_resolve_batch_path(const char *command_name, char *resolved_path, size_t resolved_path_size);

/**
 * Execute a batch file, pushing a new frame onto the batch stack.
 *
 * @param path  Absolute VFS path to the `.bat` file.
 * @param argc  Argument count made available as `%1`..`%9`.
 * @param argv  Argument values (copied into the frame).
 * @return ESP_OK on completion, ESP_ERR_INVALID_STATE when the nesting limit
 *         is reached, ESP_ERR_NOT_FOUND when the file cannot be opened, or
 *         the SD mount error.
 */
esp_err_t shell_execute_batch_file(const char *path, int argc, char **argv);

/**
 * Execute a `cmd1 | cmd2` pipeline. The first stage is redirected into a
 * temporary file on SD, then the second stage runs.
 */
void shell_execute_pipe(char *command);

/* ========================================================================
 * BATCH LANGUAGE COMMANDS
 * ======================================================================== */

/**
 * Evaluate a DOS `set /a` arithmetic expression over 32-bit signed integers.
 *
 * Supports `+ - * / %`, bitwise `& | ^ ~`, logical `! && ||`, shifts `<< >>`,
 * the comparison operators `== != < > <= >=` (1 when true, 0 otherwise),
 * unary minus, and parentheses, with cmd.exe precedence. A bare identifier
 * reads an environment variable; an undefined name evaluates to 0, as in DOS.
 * Numbers accept decimal, `0x` hex, and leading-zero octal.
 *
 * @param expression  Expression text.
 * @param result_out  Receives the value. May be NULL.
 * @param error_out   Receives a static reason string on failure. May be NULL.
 * @return true on success, false on a syntax error or divide by zero.
 */
bool shell_expr_evaluate(const char *expression, int32_t *result_out, const char **error_out);

/**
 * `set /a` helper (implemented in batch_expr.c): evaluate an arithmetic
 * statement with optional `NAME=` / `NAME[OP]=` assignment.
 */
void shell_command_set_arithmetic(int argc, char **argv);

/**
 * `set /p` helper (implemented in batch_expr.c): prompt and store a line.
 */
void shell_command_set_prompt(int argc, char **argv);

/**
 * `set` — show all variables, query one, or assign `NAME=value`.
 *
 * Also handles the two DOS sub-forms:
 *   set /a NAME=<expression>   Evaluate arithmetic and store the result
 *   set /p NAME=<prompt>       Prompt the user and store the typed line
 */
void shell_command_set(int argc, char **argv);

/** `path` — show or replace the PATH variable. */
void shell_command_path(int argc, char **argv);

/** `echo` — print text, or toggle batch echo with `on`/`off`. */
void shell_command_echo(int argc, char **argv);

/** DOS glued-echo detector: `echo.`, `echo/`, `echo(`, `echo:` (any case).
 *  cmd.exe treats the first `.`/`/`/`(`/`:` glued to `echo` as a text
 *  separator, so the word remainder is the text (`echo.` = blank line,
 *  `echo.Hello` = `Hello`, `echo.on` = literal `on`). Plain `echo` (exact
 *  word) is NOT glued and returns false. On success writes the separator to
 *  @p sep_out and the word remainder (possibly "") to @p rest_out (both
 *  NULL-checked). Pure and unit-tested; the single echo renderer below owns
 *  the printing. */
bool shell_command_echo_glued(const char *word, char *sep_out, const char **rest_out);

/**
 * Echo the remainder of a raw command line after the leading `echo` word.
 * Used by the dispatcher when an `echo` line has more arguments than the argv
 * capacity (so a full 4096-byte echo line is never truncated). Handles the
 * bare `echo` state report, batch `echo on`/`echo off`, and the DOS glued
 * separators, then prints the remainder verbatim.
 * @param line  The raw line, starting with `echo` (mutated by trimming).
 */
void shell_command_echo_text(char *line);

/** `call` — run another batch file with its own argument frame. */
void shell_command_call(int argc, char **argv);

/** `if` — conditional execution on errorlevel, file existence, or strings. */
void shell_command_if(int argc, char **argv);

/** `for` — loop over a token set or wildcard pattern: `for %v in (set) do cmd`. */
void shell_command_for(int argc, char **argv);

/**
 * Parse a `for /L` numeric set `(start,step,end)` (pure, unit-tested).
 * Whitespace around the three integers is allowed; @p step must be nonzero.
 *
 * @return true and the three values, or false for any malformed set
 *         (wrong arity, non-numeric text, a zero step, trailing garbage).
 */
bool shell_forl_parse(const char *set, int32_t *start_out, int32_t *step_out,
                      int32_t *end_out);

/**
 * `while` — re-run a body while a `set /a` expression is nonzero:
 * `while <expr> do <command>`. The expression uses the same evaluator (and
 * operators) as `set /a`, so `%n%<5`, `%x%==3`, or `1` (until ^C) all work.
 * Bounded by `P4_CONFIG_WHILE_ITER_MAX`; a foreground break unwinds the loop.
 * ERRORLEVEL: 0 done, 1 capped or condition error, 2 usage.
 */
void shell_command_while(int argc, char **argv);

/** `break` / `continue` — loop control inside `for` and `while` bodies. */
void shell_command_break(int argc, char **argv);
void shell_command_continue(int argc, char **argv);

/** `until` — negated `while`: run body while the expression is zero. */
void shell_command_until(int argc, char **argv);

/** Sort an array in-place (stable insertion sort). Used by `sort /a`. */
int shell_sort_array(const char *name, bool numeric, bool descending);

/**
 * Collect every variable name shaped `PREFIX[...]` (case-insensitive prefix,
 * non-empty index) into @p names (slot order). Pure over the shared table;
 * values are re-read live per iteration.
 *
 * @return The entry count (0 when nothing matches or the arguments are
 *         unusable).
 */
int shell_fora_collect(const char *prefix, char names[][P4_CONFIG_ENV_NAME_BYTES], int max);

/**
 * `switch` case matcher (pure, unit-tested): first case-insensitive match
 * wins. Each case is `match:label`, split on the LAST colon; entries without
 * a colon never match.
 *
 * @return The label (possibly empty), or NULL when nothing matches.
 */
const char *shell_switch_select(const char *value, char **cases, int count);

/**
 * Translate the DOS comparison keywords (EQU NEQ LSS LEQ GTR GEQ) in a
 * `while` condition to `set /a` operators (pure, unit-tested). Keywords must
 * be standalone whitespace-separated words; anything else passes through, and
 * @p out is always NUL-terminated.
 */
void shell_while_translate_keywords(const char *cond, char *out, size_t out_size);

/* ========================================================================
 * `for /f` FILE-LINE LOOPS (pure helpers, unit-tested in test/main)
 * ========================================================================
 * `for /f "eol=c skip=n delims=xyz tokens=a,b,m-n" %%v in (file-set) do cmd`
 * iterates over the lines of a file (or the active `< file` / pipe input).
 * The option parser and line splitter are pure so the test project can drive
 * them without SD hardware.
 */

/** One space-delimited token inside a `for /f` source line (non-mutating). */
typedef struct {
    const char *start;  /**< Points into the source line (not NUL-terminated). */
    size_t len;         /**< Token length in bytes. */
} shell_forf_tok_t;

/** Parsed `for /f` options. */
typedef struct {
    char delims[P4_CONFIG_FORF_DELIMS_BYTES];  /**< Delimiter set (default " \t"). */
    int token_list[P4_CONFIG_FORF_TOKEN_MAX];  /**< 1-based token indices to bind. */
    int token_count;                           /**< Entries in @p token_list. */
    int skip;                                  /**< Leading lines to skip. */
    char eol;                                  /**< Comment-line marker (0 = none). */
    bool star;                                 /**< `tokens=...*` captures the rest. */
    bool usebackq;                             /**< Selects the backquote command form. */
} shell_forf_options_t;

/** Fill @p opts with the DOS defaults: " \t" delims, tokens=1. */
void shell_forf_options_default(shell_forf_options_t *opts);

/**
 * Parse the space-separated `for /f` options text (`delims=, tokens=1,2`).
 * The shell tokenizer has already stripped the DOS quoting layer.
 *
 * @return true when every word is a known option; false leaves @p opts
 *         partially filled but stops at the first malformed word.
 */
bool shell_forf_parse_options(const char *text, size_t len, shell_forf_options_t *opts);

/**
 * Split a line into non-mutating tokens on any character of @p delims.
 * An empty @p delims string means "no delimiters" (the whole line is one
 * token, matching DOS `delims=`).
 *
 * @return The number of tokens found (may exceed @p max_tokens; extra tokens
 *         are not recorded, but a trailing `*` still captures them because the
 *         split keeps offsets into the original line).
 */
int shell_forf_split_line(const char *line, const char *delims,
                          shell_forf_tok_t *tokens, int max_tokens);

/**
 * Apply cmd.exe `%~` argument modifiers to a raw batch argument value.
 *
 * Pure string surgery (unit-tested, no SD or frame access): strips one pair
 * of surrounding double quotes, then applies `f` (resolve to a full path via
 * the shell cwd, falling back to the input when resolution fails), `d`
 * (drive — always empty on FATFS, there are no drive letters), `p`
 * (directory part with trailing `/`), `n` (base name without extension),
 * and `x` (extension with dot). Modifiers combine in canonical d/p/n/x
 * order (`%~dpnx1`); `s` is accepted and ignored (no short names on FATFS).
 * An empty @p mods string only dequotes. Always NUL-terminates @p out.
 */
void shell_arg_apply_modifiers(const char *value, const char *mods,
                               char *out, size_t out_size);

/**
 * Detect the `for /f ... in ('command')` command form (pure, unit-tested).
 *
 * A single-quoted set is always the command form; with @p usebackq a
 * backquoted set is too. Surrounding whitespace is ignored. On success the
 * inner command text (without the quotes) is written to @p inner_out.
 *
 * @return true when @p set_str is the command form.
 */
bool shell_forf_is_command_set(const char *set_str, bool usebackq,
                               char *inner_out, size_t inner_size);

/** `goto` — jump to a `:label` in the running batch file. A missing label
 *  prints the cmd.exe error, sets ERRORLEVEL 1, and aborts the current file. */
void shell_command_goto(int argc, char **argv);

/** `gosub` — BASIC-named sibling of `call :label` / `call <file.bat>::<routine>`.
 *  Enters a labelled subroutine that returns on `return` / `exit /b` /
 *  `goto :eof`. */
void shell_command_gosub(int argc, char **argv);

/** `return [code]` — return from a `gosub`/`call :label` scope; at the top
 *  level of a batch file it ends the frame (like `goto :eof`). */
void shell_command_return(int argc, char **argv);

/** `on <expr> goto|gosub|call <label>[,<label>...]` — BASIC computed dispatch.
 *  The 1-based expression result selects a target; out of range falls through. */
void shell_command_on(int argc, char **argv);

/** `switch <value> <match>:<label> [...] [/d:<label>]` — string-match dispatch
 *  to a batch label (the string twin of `on ... goto`). First case-insensitive
 *  match jumps, else the `/d:` default, else fallthrough with ERRORLEVEL 1. */
void shell_command_switch(int argc, char **argv);

/* Pure control-flow helpers exposed for unit tests (no duplicated parsing). */
bool batch_label_is_line(const char *line);
void batch_label_extract(const char *line, char *name, size_t name_size);
bool batch_on_parse(int argc, char **argv, char *expr, size_t expr_size,
                    bool *is_gosub_out, char *targets, size_t targets_size);
const char *batch_on_select(char *targets, int index);

/** `shift` — shift batch arguments left by one position. */
void shell_command_shift(int argc, char **argv);

/**
 * Parse the optional `shift /n` start index (DOS parity: slide `%n`..`%9`,
 * leaving `%0`..`%(n-1)` in place). @p arg is the candidate switch text,
 * @p frame_argc the current frame size (`args[0]` is the script name).
 * On success writes the slide origin (0..frame_argc-1) to @p start_out.
 * Pure and unit-tested; defined beside shell_command_shift in batch.c.
 */
bool shell_shift_parse_start(const char *arg, int frame_argc, int *start_out);

/**
 * `pause` — print the DOS prompt text and block until a key is pressed.
 * Falls back to a bounded delay when no interactive key source is attached.
 */
void shell_command_pause(int argc, char **argv);

/**
 * `choice` — print the option list and block until the user picks one.
 *
 * Usage: choice [/C:list] [/N] [/T:c,secs] [/S] [text]
 *   /C:list   Allowed keys (default `YN`)
 *   /N        Do not display the choice list
 *   /T:c,secs Default to key `c` after `secs` seconds
 *   /S        Case-sensitive key matching
 *
 * Sets errorlevel to the 1-based index of the chosen key, matching DOS.
 */
void shell_command_choice(int argc, char **argv);

/**
 * `setlocal` — push a copy of the environment so later changes are local.
 * Restored by `endlocal` or automatically when the batch file returns.
 */
void shell_command_setlocal(int argc, char **argv);

/** `endlocal` — pop the most recent `setlocal` scope, discarding its changes. */
void shell_command_endlocal(int argc, char **argv);

/**
 * `exit` — leave the current batch context and set the errorlevel.
 * `exit /b [code]` leaves only the current batch file; a bare `exit [code]`
 * inside nested batch files unwinds every level.
 */
void shell_command_exit(int argc, char **argv);

/**
 * `proc` — introspect the active batch process stack (the batch "process"
 * abstraction). Lists every nested batch file (script, depth, args, echo
 * state), and the `/args` `/name` `/depth` `/errorlevel` `/echo` `/stdin`
 * sub-forms report the current process's arguments, name, depth, exit code,
 * echo state, and active pipe/`<` input source. ERRORLEVEL: 0 ok, 2 usage.
 */
void shell_command_proc(int argc, char **argv);

/**
 * `ini` — persistent state in simple `KEY=VALUE` INI files on the SD card:
 * `ini list|get|set|del <file> [key] [value]` and `ini load|save <file>` to
 * import/export the environment (easy persistent state: env + INI files).
 * ERRORLEVEL: 0 ok, 1 io/missing, 2 usage.
 */
void shell_command_ini(int argc, char **argv);

/**
 * `appconfig` — per-app settings without hand-rolling file parsing: reads and
 * writes `sd:/APPS/<APP>.INI` for a named app, so batch apps get a namespaced
 * settings file through `appconfig <app> list|path|get|set|del`. ERRORLEVEL:
 * 0 ok, 1 io/missing, 2 usage.
 */
void shell_command_appconfig(int argc, char **argv);

/**
 * `temp` — SD-backed temporary files: `temp` shows the temp directory,
 * `temp new [ext]` creates a unique temp file and prints its path,
 * `temp clean` deletes every temp file. ERRORLEVEL: 0 ok, 1 io, 2 usage.
 */
void shell_command_temp(int argc, char **argv);

/**
 * `ansi <sgr-codes> [text...]` — menu/form primitive: emit text styled with
 * the given ANSI SGR codes (reverse video, bold, colors; `ESC[<codes>m text
 * ESC[0m`) into the transcript display. With no text, only the codes are
 * emitted. ERRORLEVEL: 0 ok, 1/2 error.
 */
void shell_command_ansi(int argc, char **argv);

/**
 * `menu <item> [item...]` — menu/form primitive: render a numbered menu in
 * the transcript and read a numeric choice; ERRORLEVEL is the 1-based index
 * of the chosen item (0 on cancel/timeout/invalid). Pairs with `choice`.
 */
void shell_command_menu(int argc, char **argv);

/**
 * `notify [options] <text>` — show the text in the header notification area.
 * `notify -` clears the current notification.
 *   /t:secs   Override the notification timeout in seconds.
 * ERRORLEVEL: 0 ok, 2 usage.
 */
void shell_command_notify(int argc, char **argv);

/* NOTE: the TUI/modal verbs (`draw`, `anchor`, `browse`, `dialog`, `list`,
 * `ask`, `browse_batch`, `view`, `hexview`, `color`, `locate`, `tui`) live in
 * components/command/tui_commands.c since v0.35.3 and are declared in
 * command.h. Only `appmode` below stays in the batch engine (frame cleanup).
 */

/**
 * `appmode` — enter/exit app mode (save/restore screen, optional full-screen).
 * `appmode on [/full] [/clear]` saves the transcript (and hides the shell
 * input widgets for a full-screen app surface); `appmode off` restores it;
 * when the batch file that entered app mode returns (`exit /b` / `goto :eof` /
 * EOF) the screen is restored automatically. ERRORLEVEL 0/1/2.
 */
void shell_command_appmode(int argc, char **argv);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/**
 * Reset the batch module state: environment variables, PATH default, the
 * active frame, errorlevel, and any pending goto. Call once during boot.
 */
void batch_init(void);

/** Check if the batch module is initialized. */
bool batch_is_initialized(void);

/**
 * Set the default batch-echo state that new batch frames inherit.
 *
 * CONFIG.SYS `ECHO ON|OFF` sets this before AUTOEXEC.BAT runs, so the
 * batch file sees the requested echo policy. Defaults to true (echo on).
 *
 * @param enabled  true for ECHO ON, false for ECHO OFF.
 */
void batch_set_default_echo(bool enabled);

/**
 * Execute a single command line through the full batch/command pipeline
 * (variable expansion, redirection, pipes, chaining). Used by the boot
 * CONFIG.SYS runner to apply hardware directives (rotate, brightness, volume,
 * etc.) without re-implementing their parsers.
 *
 * NULL-checked against the registered ops hook; a missing hook is a no-op.
 *
 * @param command  Command line to execute (e.g. "rotate 90").
 */
void batch_boot_execute_command(const char *command);

/* ========================================================================
 * COMMAND MODULE HOOKS, BACK END (background-job control for the supervisor)
 * ========================================================================
 * The service supervisor (svc.c, this component) decides WHAT should run;
 * the pool that runs it lives in components/command/. These hooks bridge
 * the gap the same way batch_command_ops_t does (registered once by
 * command_init(), always NULL-checked).
 */

typedef struct {
    /** Start @p line as a background job (same as `start`, no /wait).
     *  @return The 1-based slot, or -1 when the pool is full/unavailable. */
    int (*bg_start)(const char *line);
    /** True while the 1-based @p slot's job is still running. */
    bool (*bg_running)(int slot);
} batch_bg_ops_t;

/** Register the background-job control table (command_init, once). */
void batch_register_bg_ops(const batch_bg_ops_t *ops);

/* ========================================================================
 * EXTENDED MEMORY (himem — XMS-style handle blocks over PSRAM)
 * ========================================================================
 * DOS extended-memory tricks for batch apps: handle blocks above the
 * 64-variable environment ceiling, for staging bulk data without touching
 * the SD card. One handle table over p4heap PSRAM; blocks never move, so
 * locks guard against free (not relocation). Handles record their owner
 * task slot: a background job's blocks are freed automatically when the job
 * exits (no TSRs), and one job's holdings are capped by
 * P4_CONFIG_HIMEM_JOB_QUOTA_BYTES (foreground uncapped).
 */

 /** Opaque block size/owner snapshot for `himem info` (no pointers out). */
typedef struct {
    bool used;
    size_t size;
    int owner_slot;   /**< 0 = foreground, N = bg slot N. */
    bool locked;
} himem_handle_info_t;

/** Allocate a zeroed @p size-byte block for @p slot. @return handle 0..N-1,
 *  or -1 when the table is full, @p size exceeds P4_CONFIG_HIMEM_MAX_BYTES,
 *  the slot quota is hit, or PSRAM is out. */
int himem_alloc(size_t size, int slot);

/** Free handle @p h. The owner slot (or the foreground supervisor, slot 0)
 *  may free; locked handles refuse. @return ESP_OK or an error code. */
esp_err_t himem_free(int h, int slot);

/** Lock @p h against free (XMS lock guard). Returns the lock count, or -1
 *  for a bad handle / foreign owner. */
int himem_lock(int h, int slot);

/** Release one lock on @p h. Returns the remaining count, or -1 for a bad
 *  handle / foreign owner / unlocked block. */
int himem_unlock(int h, int slot);

/** Bounds-checked read accessor behind `himem crc` (the CRC primitive lives
 *  in the command layer, so the verb computes over this view instead of a
 *  second copy of the bytes). @return ESP_OK with @p ptr_out/@p len_out set,
 *  or an error for a bad range. The pointer is valid until free/resize. */
esp_err_t himem_peek(int h, size_t offset, size_t count,
                     const uint8_t **ptr_out, size_t *len_out);

/** Fill [@p offset, @p offset+@p count) with @p byte. @p count 0 = to end. */
esp_err_t himem_fill(int h, int slot, uint8_t byte, size_t offset, size_t count);

/** Snapshot one handle for `himem info` (index @p h, never out-of-range). */
himem_handle_info_t himem_describe(int h);

/** Bytes currently held by @p slot (quota accounting). */
size_t himem_slot_bytes(int slot);

/** Free every block owned by @p slot (job-exit cleanup; no leaks). */
void himem_free_owned(int slot);

/** Parse a byte count: decimal, `0x` hex, or `K`/`M` suffix (pure,
 *  unit-tested). @return Bytes, or 0 for empty/garbage/overflow. */
uint64_t himem_parse_size(const char *text);

/** Parse a decimal handle number (pure, unit-tested). @return Handle, or -1
 *  for empty/garbage/out-of-range. */
int himem_parse_handle(const char *text);

/* ========================================================================
 * BATCH SYNCHRONIZATION (lock/unlock named mutexes, signal/wait events)
 * ========================================================================
 * Real-concurrency coordination for background jobs: one mutex table and
 * one event table in the batch engine (never a second implementation per
 * caller). Entries are owner-tracked and released automatically when the
 * owning job exits, so a dead job can never wedge a mutex. Signals are
 * sticky until taken; `wait` auto-clears on take so one signal wakes
 * exactly one waiter.
 */

/** True for a usable sync name: non-empty, fits, `[A-Za-z0-9_-]+` (pure,
 *  unit-tested). Sync names are case-sensitive (they are not env vars). */
bool sync_name_ok(const char *name);

/** Take the named mutex (creating it bounded). @p timeout_ms 0 = forever.
 *  Chunked so kill/deadline/abort stay responsive. @return ESP_OK,
 *  ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM (table full), or ESP_ERR_INVALID_ARG. */
esp_err_t sync_mutex_lock(const char *name, uint32_t timeout_ms, int slot);

/** Release the named mutex (owner slot, or the foreground supervisor).
 *  @return ESP_OK or an error code. */
esp_err_t sync_mutex_unlock(const char *name, int slot);

/** Release every mutex owned by @p slot (job-exit cleanup). */
void sync_release_owned(int slot);

/** Set the named event (creating it bounded; sticky until taken). */
esp_err_t sync_event_signal(const char *name);

/** Wait for the named event, auto-clearing on take. @p timeout_ms 0 =
 *  forever (still kill/abort-responsive). @return ESP_OK, ESP_ERR_TIMEOUT,
 *  or an error code. */
esp_err_t sync_event_wait(const char *name, uint32_t timeout_ms, int slot);

/** `lock <name> [/t:secs]` — take a named mutex (EL 0/1/2). */
void shell_command_lock(int argc, char **argv);

/** `unlock <name>` — release a named mutex (EL 0/1/2). */
void shell_command_unlock(int argc, char **argv);

/** `signal <name>` — set a named event (EL 0/1/2). */
void shell_command_signal(int argc, char **argv);

/** `wait <name> [/t:secs]` — wait for a named event (EL 0/1/2). */
void shell_command_wait(int argc, char **argv);

/* ========================================================================
 * RESIDENT SERVICES (registry + supervisor — TSRs that survive)
 * ========================================================================
 * Named background lines persisted in sd:/SERVICES.INI through the shared
 * storage_ini core. `service install` writes the registry; the supervisor
 * (own FreeRTOS timer, P4_CONFIG_SVC_TICK_MS) re-starts what should run;
 * `batch_svc_boot()` re-starts autostart entries after AUTOEXEC.BAT.
 * Restart-on-unexpected-exit per the /restart flag (bounded per boot);
 * `service stop` is the admin off switch (never restarted, never counted).
 * `taskkill` of a service job counts as unexpected — it IS restarted.
 */

 /** One registered service (RAM mirror of the INI registry + runtime). */
typedef struct {
    bool used;
    char name[P4_CONFIG_SVC_NAME_BYTES];
    /* Service line in PSRAM (never BSS: COMMAND_BYTES * SVC_MAX of .bss
     * would eat the boot internal-RAM window the command worker needs). */
    char *line;
    bool restart;
    bool auto_start;
    bool supervised;    /**< Should be running now (start/boot/restart). */
    bool stopped;       /**< Admin-stopped via `service stop` (wins). */
    bool parked;        /**< Restart budget exhausted this boot. */
    int slot;           /**< Bg slot while running, 0 when down. */
    int restarts;       /**< Restarts this boot (flood guard). */
} svc_entry_t;

/** Snapshot one registry entry for `service list` (index order). */
svc_entry_t svc_describe(int index);

/** Registered service count. */
int svc_count(void);

/** Install (or replace) a service in the registry + RAM mirror. */
esp_err_t svc_install(const char *name, const char *line, bool restart, bool auto_start);

/** Remove a service (refused while running — stop it first). */
esp_err_t svc_remove(const char *name);

/** Start a registered service now (marks supervised). */
esp_err_t svc_start(const char *name);

/** Admin-stop a service (marks stopped; never restarted, never counted). */
esp_err_t svc_stop(const char *name);

/** Re-start autostart entries (boot hook, after AUTOEXEC.BAT). */
void batch_svc_boot(void);

/** Reset supervisor state (once, from batch_init). The supervisor timer is
 *  created lazily on first supervised service (never inside the boot heap
 *  window — its task stack would starve the command worker). */
void batch_svc_init(void);

/** Clear service slot references at job exit (called from
 *  batch_bg_release, same component). */
void svc_slot_released(int slot);

/** Supervisor tick: reap unexpected exits, restart per policy. Safe from
 *  any task; no-ops until the bg ops hook is registered. */
void batch_svc_tick(void);

/** `service install|remove|list|start|stop ...` (EL 0/1/2). */
void shell_command_service(int argc, char **argv);

/**
 * `himem` — the extended-memory verb: alloc/free/info/load/save/fill/crc
 * over the handle table above. Batch-callable (`himem alloc 65536 /v:H`
 * then `himem save %H% dump.bin`). ERRORLEVEL 0 ok / 1 failure / 2 usage.
 */
void shell_command_himem(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_BATCH_H */

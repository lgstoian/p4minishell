/**
 * @file batch.h
 * @brief Batch engine, environment variables, and batch language commands.
 *
 * Owns everything the `.bat` interpreter needs:
 *   - Batch file execution with nesting, `@` echo suppression, and `rem`/`::`
 *   - `:label` scanning, `goto`, and `call :label` targets
 *   - `for %%var in (set) do command` loops
 *   - The `|` pipe operator
 *   - RAM-only environment variables and PATH
 *   - Variable expansion: `%VAR%`, `%0`, `%1`..`%9`, `%*`
 *   - Errorlevel tracking
 *   - The batch language commands: `set`, `path`, `echo`, `call`, `if`,
 *     `goto`, `shift`, `pause`, `choice`, `setlocal`, `endlocal`, `exit`
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
#include "esp_err.h"

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
 * ENVIRONMENT VARIABLES AND EXPANSION
 * ======================================================================== */

/**
 * Look up an environment variable by name (case-insensitive).
 * @return Pointer to the stored value, or NULL when the name is not defined.
 */
const char *shell_env_get(const char *name);

/**
 * Create, update, or clear an environment variable.
 * Passing an empty or NULL value clears the slot.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a malformed name, or
 *         ESP_ERR_NO_MEM when all slots are in use.
 */
esp_err_t shell_env_set(const char *name, const char *value);

/**
 * Expand `%VAR%`, `%0` (script name), `%1`..`%9`, and `%*` (all arguments)
 * into @p output. Unknown names are left untouched, and `%%` yields `%`.
 */
void shell_expand_variables(const char *input, char *output, size_t output_size);

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
 * ERRORLEVEL
 * ======================================================================== */

/** Get the current errorlevel (0 = success). */
int batch_get_errorlevel(void);

/** Set the current errorlevel. */
void batch_set_errorlevel(int level);

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

/** `call` — run another batch file with its own argument frame. */
void shell_command_call(int argc, char **argv);

/** `if` — conditional execution on errorlevel, file existence, or strings. */
void shell_command_if(int argc, char **argv);

/** `goto` — jump to a `:label` in the running batch file. */
void shell_command_goto(int argc, char **argv);

/** `shift` — shift batch arguments left by one position. */
void shell_command_shift(int argc, char **argv);

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

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_BATCH_H */

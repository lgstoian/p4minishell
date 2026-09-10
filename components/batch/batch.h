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
 * into @p output. An undefined `%VAR%` expands to the empty string (cmd.exe
 * parity), and `%%` yields a literal `%`.
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

/** True when the current task runs on a background slot. */
bool batch_bg_is_background(void);

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

/**
 * Echo the remainder of a raw command line after the leading `echo` word.
 * Used by the dispatcher when an `echo` line has more arguments than the argv
 * capacity (so a full 4096-byte echo line is never truncated). Handles the
 * bare `echo` state report and batch `echo on`/`echo off` like
 * shell_command_echo(), then prints the remainder verbatim.
 * @param line  The raw line, starting with `echo` (mutated by trimming).
 */
void shell_command_echo_text(char *line);

/** `call` — run another batch file with its own argument frame. */
void shell_command_call(int argc, char **argv);

/** `if` — conditional execution on errorlevel, file existence, or strings. */
void shell_command_if(int argc, char **argv);

/** `for` — loop over a token set or wildcard pattern: `for %v in (set) do cmd`. */
void shell_command_for(int argc, char **argv);

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

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_BATCH_H */

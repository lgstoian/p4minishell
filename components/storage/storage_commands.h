/**
 * @file storage_commands.h
 * @brief DOS-style file command handlers for P4MiniShell.
 *
 * Declares the command entry points that operate on the SD filesystem:
 *   - Navigation and listing: `cd`/`chdir`, `dir`, `tree`
 *   - File manipulation: `copy`, `move`, `del`/`erase`, `ren`/`rename`,
 *     `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch`
 *   - Extended DOS tools: `attrib`, `label`, `xcopy`
 *   - Text utilities: `find`, `more`, `fc`, `sort`
 *   - SD command family: `sd info|ls|stat|cat|eject`
 *
 * Each handler owns its own argument validation and transcript output.
 * The dispatcher in `components/command/` calls these directly; the storage
 * primitives they build on live in `storage.h`.
 */

#ifndef P4MINISHELL_STORAGE_COMMANDS_H
#define P4MINISHELL_STORAGE_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared SHELL_* compat aliases (moved here in v0.35.6). */
/* Backward-compatibility aliases */
#define SHELL_SD_FATFS_DRIVE            P4_CONFIG_SD_FATFS_DRIVE
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_SD_LIST_LIMIT             P4_CONFIG_SD_LIST_LIMIT
#define SHELL_SD_CAT_DEFAULT_BYTES      P4_CONFIG_SD_CAT_DEFAULT_BYTES
#define SHELL_SD_CAT_MAX_BYTES          P4_CONFIG_SD_CAT_MAX_BYTES
#define SHELL_SD_IO_BUFFER_BYTES        P4_CONFIG_SD_IO_BUFFER_BYTES
#define SHELL_BATCH_LINE_BYTES          P4_CONFIG_BATCH_LINE_BYTES
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_LFN_BYTES                 P4_CONFIG_LFN_BYTES
#define SHELL_TEXT_LINE_BYTES           P4_CONFIG_TEXT_LINE_BYTES
#define SHELL_SORT_LINE_MAX             P4_CONFIG_SORT_LINE_MAX
#define SHELL_MORE_PAGE_LINES           P4_CONFIG_MORE_PAGE_LINES
#define SHELL_MORE_PAGE_DELAY_MS        P4_CONFIG_MORE_PAGE_DELAY_MS

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * NAVIGATION AND LISTING
 * ======================================================================== */

/** `cd` / `chdir` — print or change the current working directory. */
void shell_command_cd(int argc, char **argv);

/**
 * `dir` — list a directory, optionally filtered by a DOS wildcard pattern.
 *
 * Usage: dir [path|pattern] [/W] [/P] [/S] [/B] [/L] [/A[:]attrs] [/O[:]order]
 *   /W          Wide multi-column listing
 *   /P          Pause after each screenful
 *   /S          Recurse into subdirectories
 *   /B          Bare listing, names only
 *   /L          Lowercase names
 *   /A:attrs    Filter by attribute: D dirs, H hidden, S system, R read-only,
 *               A archive. Prefix a letter with `-` to exclude it.
 *   /O:order    Sort order: N name, S size, E extension, D date, G dirs first.
 *               Prefix with `-` to reverse.
 */
void shell_command_dir(int argc, char **argv);

/** `tree` — single-level directory outline marking subdirectories with `+`. */
void shell_command_tree(int argc, char **argv);

/**
 * Format a FAT date/time pair as `YYYY-MM-DD HH:MM` (shared by `dir` in
 * storage_nav.c and find discovery output in storage_text.c).
 */
void shell_dir_format_stamp(uint16_t fdate, uint16_t ftime, char *output, size_t output_size);

/* ========================================================================
 * FILE MANIPULATION
 * ======================================================================== */

/** `copy` — copy one file, or every wildcard match into a directory. */
void shell_command_copy(int argc, char **argv);

/** `move` — rename across directories, falling back to copy-then-delete. */
void shell_command_move(int argc, char **argv);

/**
 * `del` / `erase` — move a file or every wildcard match to the recycle bin,
 * or delete permanently.
 *
 * Usage: del [/s] [/p|/f|/permanent] <path|pattern>
 *   /s           Recurse into subdirectories (requires exact confirmation)
 *   /p|/f|/permanent   True delete, bypassing the recycle bin
 *
 * Returns an ERRORLEVEL: 0 success, 1 failure / cancelled, 2 usage.
 */
int shell_command_del(int argc, char **argv);

/**
 * `ren` / `rename` — rename a file within its own directory.
 * @param verb  The alias the user typed, used in usage and error output.
 */
void shell_command_rename(int argc, char **argv, const char *verb);

/** `md` / `mkdir` — create a directory. */
void shell_command_mkdir(int argc, char **argv);

/**
 * `rd` / `rmdir` — remove a directory.
 *
 * Usage: rd [/s] [/p|/f|/permanent] <path>
 *   /s           Remove a directory tree (moves it to the recycle bin, or
 *                deletes permanently with /p|/f; requires exact confirmation)
 * Returns an ERRORLEVEL: 0 success, 1 failure / cancelled, 2 usage.
 */
int shell_command_rmdir(int argc, char **argv);

/** `type` — print the contents of a text file. */
void shell_command_type_file(int argc, char **argv);

/**
 * `write` / `append` — write a line of text to a file.
 * @param append_mode  true for `append` (`ab`), false for `write` (`wb`).
 */
void shell_command_write_file(int argc, char **argv, bool append_mode);

/** `touch` — create a file if missing and refresh its timestamp. */
void shell_command_touch(int argc, char **argv);

/* ========================================================================
 * EXTENDED DOS TOOLS
 * ======================================================================== */

/** `attrib` — show or change FATFS R/H/S/A file attributes. */
void shell_command_attrib(int argc, char **argv);

/** `label` — read or set the FATFS volume label. */
void shell_command_label(int argc, char **argv);

/**
 * `xcopy` — copy a file, or a directory tree with the full DOS 6.x switch set.
 *
 * Usage: xcopy <source> <destination> [/S] [/E] [/I] [/Y|/-Y] [/D[:mm-dd-yyyy]]
 *        [/H] [/R] [/K] [/C] [/Q] [/T] [/F] [/L] [/A] [/M] [/U] [/P] [/W] [/N]
 * Returns an ERRORLEVEL: 0 success, 1 nothing copied / copy failed, 2 usage.
 */
int shell_command_xcopy(int argc, char **argv);

/* ========================================================================
 * VOLUME MANAGEMENT
 * ======================================================================== */

/**
 * `chkdsk` — check the FATFS volume and report its capacity.
 *
 * Usage: chkdsk [path] [/F]
 *   /F  Also walk every directory verifying that each entry is readable
 *
 * Reports allocation unit size, cluster counts, and used and free space.
 * The scan is read-only: this firmware never rewrites FAT structures, so a
 * problem is reported rather than silently "repaired".
 */
void shell_command_chkdsk(int argc, char **argv);

/**
 * `format` — reformat the SD card, destroying all data.
 *
 * Usage: format [/FS:FAT|FAT32] [/A:size] [/V:label] [/Q]
 *
 * Requires the exact confirmation word P4_CONFIG_FORMAT_CONFIRM_WORD typed
 * at the prompt before anything is written. FAT/FAT32 select size-appropriately
 * through the standard IDF format helper; exFAT is not available in this
 * firmware build and is reported honestly.
 */
int shell_command_format(int argc, char **argv);

/**
 * Collect the destructive-operation confirmation word from the interactive key
 * queue. Shared by `format`, `disk clean`, `trash purge`, and `config factory`.
 *
 * Refused outright when a batch file is active or no interactive key source is
 * available. Prints the warning + "Type YES to continue: " prompt and returns
 * true only when the exact word P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD is typed.
 */
bool shell_confirm_destructive(const char *operation, const char *warning, const char *detail);

/**
 * Parse a `/A:size` allocation-unit value (shared by `format` in
 * storage_disk.c and `disk format` in storage_fam.c).
 */
bool shell_parse_alloc_unit(const char *text, uint32_t *bytes_out);

/**
 * Validated format run shared by `format` and `disk format`.
 */
int shell_format_execute(const char *operation,
                         const char *fs_type,
                         const char *new_label,
                         uint32_t alloc_unit,
                         bool alloc_set);

/**
 * `undelete` / `restore` — restore a file or directory from the recycle bin.
 *
 * Usage: undelete <name|index> | restore <name|index>
 * Returns an ERRORLEVEL: 0 success, 1 failure, 2 usage.
 */
int shell_command_undelete(int argc, char **argv);

/**
 * `trash` / `recycle` — manage the recycle bin.
 *
 * Usage: trash [list] | trash info | trash restore <name|index> |
 *        trash purge <name|index> | trash empty
 * Returns an ERRORLEVEL: 0 success, 1 failure / cancelled, 2 usage.
 */
int shell_command_trash(int argc, char **argv);

/**
 * `disk` — diskpart-style physical-disk and partition management.
 *
 * Usage: disk list | detail | clean | create partition primary [size=N] |
 *        delete partition N | format [fs=...] [label=...] [au=...] [quick]
 *
 * Receives the original unsplit command text because the subcommands
 * re-tokenize it, mirroring the `sd` family.
 */
int shell_command_disk(char *command);

/* ========================================================================
 * TEXT UTILITIES
 * ======================================================================== */

/**
 * `find` — search a file for a literal substring.
 *
 * Usage: find <text> [file] [/I] [/N] [/C] [/V] — or the recursive
 * file-discovery mode. Returns an ERRORLEVEL: 0 match found, 1 none, 2 usage.
 */
int shell_command_find(int argc, char **argv);

/**
 * `more` — page a text file with a timed advance between pages.
 *
 * Returns an ERRORLEVEL: 0 completed, 1 error, 2 usage.
 */
int shell_command_more(int argc, char **argv);

/**
 * `fc` — compare two text files line by line.
 *
 * Returns an ERRORLEVEL: 0 identical, 1 differences, 2 usage.
 */
int shell_command_fc(int argc, char **argv);

/**
 * `sort` — print a bounded text file with its lines sorted.
 *
 * Usage: sort [file] [/R] [/I] [/U]. Returns an ERRORLEVEL: 0 ok, 1 error,
 * 2 usage.
 */
int shell_command_sort(int argc, char **argv);

/**
 * `findstr` — classic DOS text search with optional limited regular
 * expressions, case-sensitive by default.
 *
 * Usage: findstr [switches] <search...> [file...]
 * Switches: /R /C:"string" /I /N /V /X /E /B /L /S /M /F:file /G:file
 * Returns an ERRORLEVEL: 0 match found, 1 no match, 2 usage.
 */
int shell_command_findstr(int argc, char **argv);

/**
 * `comp` — classic DOS byte-for-byte file comparison.
 *
 * Usage: comp <file1> <file2> [/D] [/A] [/L] [/N=number] [/C]
 * Returns an ERRORLEVEL: 0 identical, 1 different, 2 usage.
 */
int shell_command_comp(int argc, char **argv);

/* ========================================================================
 * PURE HELPERS EXPOSED FOR UNIT TESTS
 * ========================================================================
 * These are implementation details of findstr / comp with no I/O, exposed so
 * test/ can exercise the matching and comparison logic directly.
 */

/**
 * Parse a find `/NEWER:`/`/OLDER:` date (shared by `find` in storage_text.c
 * and `xcopy /D` in storage_files.c).
 */
bool shell_find_parse_date(const char *text, uint16_t *fdate_out);

/**
 * Search for the limited DOS findstr regex pattern anywhere in text (unless
 * anchored with `^`). Returns the match start index, or -1; @p end_out
 * receives the index just past the match.
 */
int shell_fsre_search(const char *pattern, const char *text, bool icase,
                      int *end_out);

/**
 * Match one line against one findstr search string. @p is_regex selects the
 * regex engine; otherwise the string is literal. @p beg / @p end / @p whole
 * implement the /B /E /X switches.
 */
bool shell_findstr_match_line(const char *pattern, bool is_regex,
                              const char *line, bool icase,
                              bool beg, bool end, bool whole);

/**
 * Find the first differing byte between two buffers. Returns true and fills
 * @p pos, @p va, @p vb when a difference exists (including a length
 * difference, where the missing byte reads as 0x00).
 */
bool shell_comp_first_diff(const uint8_t *a, size_t an,
                           const uint8_t *b, size_t bn,
                           bool icase, size_t *pos, uint8_t *va, uint8_t *vb);

/* ========================================================================
 * SD COMMAND FAMILY
 * ======================================================================== */

/**
 * `sd` command family entry point.
 * Receives the original unsplit command text because the subcommands
 * re-tokenize it.
 */
void shell_command_sd(char *command);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_STORAGE_COMMANDS_H */

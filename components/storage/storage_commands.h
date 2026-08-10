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

/* ========================================================================
 * FILE MANIPULATION
 * ======================================================================== */

/** `copy` — copy one file, or every wildcard match into a directory. */
void shell_command_copy(int argc, char **argv);

/** `move` — rename across directories, falling back to copy-then-delete. */
void shell_command_move(int argc, char **argv);

/** `del` / `erase` — delete one file or every wildcard match. */
void shell_command_del(int argc, char **argv);

/**
 * `ren` / `rename` — rename a file within its own directory.
 * @param verb  The alias the user typed, used in usage and error output.
 */
void shell_command_rename(int argc, char **argv, const char *verb);

/** `md` / `mkdir` — create a directory. */
void shell_command_mkdir(int argc, char **argv);

/** `rd` / `rmdir` — remove an empty directory. */
void shell_command_rmdir(int argc, char **argv);

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

/** `xcopy` — copy a file, or a directory tree with `/S`. */
void shell_command_xcopy(int argc, char **argv);

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
void shell_command_format(int argc, char **argv);

/**
 * `disk` — diskpart-style physical-disk and partition management.
 *
 * Usage: disk list | detail | clean | create partition primary [size=N] |
 *        delete partition N | format [fs=...] [label=...] [au=...] [quick]
 *
 * Receives the original unsplit command text because the subcommands
 * re-tokenize it, mirroring the `sd` family.
 */
void shell_command_disk(char *command);

/* ========================================================================
 * TEXT UTILITIES
 * ======================================================================== */

/** `find` — search a file for a literal substring. */
void shell_command_find(int argc, char **argv);

/** `more` — page a text file with a timed advance between pages. */
void shell_command_more(int argc, char **argv);

/** `fc` — compare two text files line by line. */
void shell_command_fc(int argc, char **argv);

/** `sort` — print a bounded text file with its lines sorted. */
void shell_command_sort(int argc, char **argv);

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

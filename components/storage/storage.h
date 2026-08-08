/**
 * @file storage.h
 * @brief SD session management and filesystem services for P4MiniShell.
 *
 * Owns everything that sits between the shell commands and the SD card:
 *   - Guarded SD mount sessions (`shell_sd_begin()` / `shell_sd_end()`)
 *   - Persistent mount tracking and the explicit eject path
 *   - Path resolution for `sd:/`, `/sdcard/`, absolute, and relative inputs
 *   - VFS-to-FATFS path conversion and FRESULT-to-esp_err_t translation
 *   - Human-readable size formatting
 *   - The RAM-only current working directory
 *   - Shared file helpers used by the DOS file commands (copy, listing,
 *     text printing, target resolution, DOS wildcard matching)
 *   - Output redirection writes for the `>` and `>>` operators
 *
 * Architecture:
 *   Dependencies flow one way: `command` -> `batch` -> `storage` -> `shell`.
 *   This module never calls back into `batch` or `command`; the DOS file
 *   command handlers live alongside it in `storage_commands.c`.
 */

#ifndef P4MINISHELL_STORAGE_H
#define P4MINISHELL_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_vfs_fat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * SD SESSION
 * ======================================================================== */

/**
 * Guarded SD access session handle.
 *
 * Every SD-touching command opens one of these so a mount failure can never
 * leak mounted state or dereference missing card metadata.
 */
typedef struct {
    bool mounted_here;  /**< true when this session performed the mount. */
} shell_sd_session_t;

/**
 * Begin a guarded SD access session.
 *
 * The mount is persistent: `shell_sd_end()` deliberately does not unmount.
 * Only the explicit `sd eject` / `sdeject` command tears the mount down.
 *
 * @param session  Session handle to initialize (must not be NULL).
 * @return ESP_OK when the card is mounted and usable,
 *         ESP_ERR_INVALID_STATE when the card was explicitly ejected,
 *         or the ESP-IDF error reported by the BSP mount.
 */
esp_err_t shell_sd_begin(shell_sd_session_t *session);

/**
 * End a guarded SD access session.
 *
 * Kept as an explicit call so every command has a symmetric cleanup point
 * even though the mount itself is persistent.
 *
 * @param session    Session opened by shell_sd_begin().
 * @param operation  Command name, used for diagnostics.
 */
void shell_sd_end(shell_sd_session_t *session, const char *operation);

/** Report whether the SD card is currently mounted by the shell. */
bool storage_sd_is_mounted(void);

/**
 * Unmount the SD card for safe removal and report the result to the user.
 * Implements the `sd eject` / `sdeject` command.
 */
void shell_command_sd_eject(void);

/* ========================================================================
 * PATH RESOLUTION AND CONVERSION
 * ======================================================================== */

/**
 * Resolve an SD-rooted path without applying the current working directory.
 * Accepts `sd:/...`, `/sdcard/...`, absolute, and SD-root-relative inputs.
 */
esp_err_t shell_sd_resolve_path(const char *input, char *output, size_t output_size);

/**
 * Resolve a path relative to the shell current working directory, collapsing
 * `.` and `..` segments. Produces an absolute path under the SD mount point.
 */
esp_err_t shell_fs_resolve_path(const char *input, char *output, size_t output_size);

/**
 * Resolve a destination path that may be relative to the source file's
 * directory, which is the DOS behavior for `ren` and `move`.
 */
esp_err_t shell_resolve_target_from_source(const char *source_path,
                                           const char *target_input,
                                           char *target_path,
                                           size_t target_path_size);

/** stat() wrapper mapping errno onto esp_err_t. */
esp_err_t shell_sd_stat_path(const char *path, struct stat *st);

/** Translate a FATFS FRESULT into the closest esp_err_t. */
esp_err_t shell_sd_fresult_to_esp_err(FRESULT result);

/**
 * Convert a VFS path under the SD mount point to a raw FATFS path.
 *
 * The direct FATFS API is used for directory listings because it exposes long
 * file names (CONFIG_FATFS_LFN_HEAP with MAX_LFN=255) and the 8.3 alternate
 * name, which the POSIX dirent path does not surface.
 */
esp_err_t shell_sd_vfs_to_fatfs_path(const char *vfs_path, char *fatfs_path, size_t fatfs_path_size);

/** true when the path contains a `/` or `\` directory separator. */
bool shell_path_has_directory_component(const char *path);

/** Case-insensitive check for a trailing file extension such as ".bat". */
bool shell_path_has_extension(const char *path, const char *extension);

/* ========================================================================
 * FORMATTING AND MATCHING
 * ======================================================================== */

/** Format a byte count as B / KiB / MiB / GiB with one decimal place. */
void shell_sd_format_size(uint64_t size_bytes, char *output, size_t output_size);

/** Describe a stat entry as "dir", "file", "other", or "unknown". */
const char *shell_sd_entry_type(const struct stat *st);

/** DOS-style wildcard match supporting `*` and `?`, case-insensitive. */
bool shell_wildcard_match(const char *pattern, const char *name);

/* ========================================================================
 * CURRENT WORKING DIRECTORY
 * ======================================================================== */

/**
 * Get the shell current working directory as an absolute VFS path.
 * @return Static string owned by this module (do not free).
 */
const char *shell_get_cwd(void);

/**
 * Replace the shell current working directory.
 * @param absolute_path  Absolute VFS path under the SD mount point.
 */
void storage_set_cwd(const char *absolute_path);

/** Print the current working directory in DOS form (`\` for the root). */
void shell_fs_print_cwd(void);

/* ========================================================================
 * SHARED FILE OPERATIONS
 * ======================================================================== */

/** Copy one regular file, opening a guarded SD session for the transfer. */
esp_err_t shell_fs_copy_file(const char *source_path, const char *dest_path);

/**
 * Copy the R/H/S/A attributes from one file to another.
 *
 * Called after a successful transfer so a read-only or hidden source keeps
 * those attributes at the destination, matching DOS `copy` and `xcopy`.
 * Applied after the data is written because a read-only destination cannot
 * be opened for writing.
 *
 * @param source_path  Resolved source path.
 * @param dest_path    Resolved destination path.
 * @return ESP_OK on success, or the FATFS error translated to esp_err_t.
 *         A failure here is non-fatal: the file data is already copied.
 */
esp_err_t storage_copy_attributes(const char *source_path, const char *dest_path);

/** Print a bounded directory listing using the FATFS long-file-name API. */
esp_err_t shell_list_directory_path(const char *normalized_path);

/** Print the contents of a text file, sanitizing non-printable bytes. */
esp_err_t shell_print_file_text(const char *normalized_path);

/**
 * Write captured command output to an SD file for the `>` and `>>` operators.
 *
 * @param path         Redirection target as typed by the user.
 * @param text         Captured transcript delta (may be empty, never NULL).
 * @param append_mode  true for `>`, false for `>`.
 */
esp_err_t shell_write_redirect_output(const char *path, const char *text, bool append_mode);

/* ========================================================================
 * VOLUME CAPACITY AND GUARDRAILS
 * ========================================================================
 * Larger file operations ask the volume how much room is left before they
 * start, so a copy fails with a clear message instead of part-way through
 * with a truncated destination.
 */

/** Capacity snapshot for the mounted volume. */
typedef struct {
    uint64_t total_bytes;      /**< Total formatted capacity. */
    uint64_t free_bytes;       /**< Currently free space. */
    uint64_t used_bytes;       /**< total_bytes - free_bytes. */
    uint32_t cluster_bytes;    /**< Allocation unit size. */
    uint32_t total_clusters;   /**< Total cluster count. */
    uint32_t free_clusters;    /**< Free cluster count. */
} storage_space_info_t;

/**
 * Query free and total space on the mounted volume.
 *
 * Opens its own guarded SD session, so the caller does not need one.
 *
 * @param info_out  Receives the capacity snapshot.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a NULL argument, or the mount or
 *         FATFS error.
 */
esp_err_t storage_get_space_info(storage_space_info_t *info_out);

/**
 * Check that writing @p needed_bytes would leave a safe amount of headroom.
 *
 * Enforces P4_CONFIG_STORAGE_FREE_MARGIN_BYTES so the volume never fills to
 * the point where FAT metadata updates begin to fail.
 *
 * @param needed_bytes    Bytes the operation intends to write.
 * @param reclaim_bytes   Bytes the operation will free first, such as the
 *                        size of a destination file about to be overwritten.
 * @param operation       Command name used in the refusal message.
 * @return true when the write may proceed. On false a message has already
 *         been printed to the transcript.
 */
bool storage_check_free_space(uint64_t needed_bytes, uint64_t reclaim_bytes, const char *operation);

/**
 * Report whether two resolved paths refer to the same file.
 * Used to refuse a copy or move onto itself before it truncates the source.
 */
bool storage_paths_are_same(const char *left, const char *right);

/**
 * Get the size of a file, or 0 when it does not exist or is not a file.
 * Convenience wrapper used by the free-space prechecks.
 */
uint64_t storage_get_file_size(const char *resolved_path);

/* ========================================================================
 * INPUT REDIRECTION
 * ========================================================================
 * The `<` operator and the `|` pipe operator both need to hand a file to the
 * next command as its input. Rather than inventing a per-command argument,
 * the resolved path is parked here and the text-consuming commands (`find`,
 * `more`, `sort`, `fc`) pick it up when no filename argument was supplied.
 * This matches DOS, where `sort < file.txt` and `type f | sort` are the same
 * thing from `sort`'s point of view.
 */

/**
 * Set the pending input-redirection source.
 *
 * @param resolved_path  Absolute VFS path, or NULL to clear.
 */
void storage_set_input_redirect(const char *resolved_path);

/**
 * Get the pending input-redirection source.
 * @return Absolute VFS path, or NULL when no redirection is active.
 */
const char *storage_get_input_redirect(void);

/** Clear the pending input-redirection source. */
void storage_clear_input_redirect(void);

/**
 * Resolve the input file for a text-processing command.
 *
 * Prefers an explicit filename argument; falls back to the pending input
 * redirection when the argument is absent. The result is always an absolute
 * VFS path suitable for fopen().
 *
 * @param argument     Filename as typed by the user, or NULL when omitted.
 * @param output       Receives the resolved absolute path.
 * @param output_size  Size of @p output in bytes.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when neither source exists,
 *         or the resolution error.
 */
esp_err_t storage_resolve_input_source(const char *argument, char *output, size_t output_size);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/** Reset the storage module state (cwd, mount tracking). Call once at boot. */
void storage_init(void);

/** Check if the storage module is initialized. */
bool storage_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_STORAGE_H */

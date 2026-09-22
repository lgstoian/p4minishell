/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
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
 * SDMMC HOST PRE-INIT
 * ======================================================================== */

/**
 * Pre-initialize the shared SDMMC host driver once at boot so the SD card
 * mount (slot 0) and the ESP-Hosted C6 transport (slot 1) never call
 * sdmmc_host_init() concurrently (it is not thread-safe; a concurrent
 * double-call corrupts the shared host state and fails both slots, N1).
 * Call once from app_main before networking_init(). See storage.c.
 */
esp_err_t storage_sdmmc_host_preinit(void);

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
    uint32_t magic;     /**< set by shell_sd_begin; shell_sd_end ignores handles that never began. */
    unsigned int saved_priority; /**< calling task priority saved on boost, restored on end. */
    bool boosted;       /**< true when this session raised the task priority. */
    int64_t op_start_us; /**< esp_timer timestamp taken on begin (timing probe). */
} shell_sd_session_t;

/** Magic stamped into shell_sd_session_t by shell_sd_begin(). */
#define SHELL_SD_SESSION_MAGIC  0x53445353u

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
 * Cache the SDMMC DMA scratch buffer on the host after a mount.
 *
 * The IDF sdmmc driver allocates a temporary DMA-capable buffer per card
 * transaction unless the host carries a cached `dma_aligned_buffer`. On the
 * P4 the internal DMA-capable heap is shared with the WiFi/SDIO transport
 * and LVGL, so that per-op allocation can fail once the heap fragments,
 * taking every SD command down. Allocating the cache at mount time (when
 * internal RAM is abundant) makes all later transactions reuse it.
 *
 * Safe to call any number of times; the buffer is only allocated once and
 * the chunk size derived from P4_CONFIG_SD_DMA_BUFFER_BYTES is applied.
 */
void storage_sd_ensure_dma_buffer(void);

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
 * Register a callback invoked once per boot when the SD card mounts for the
 * first time (at startup or when a freshly-inserted card is mounted by the
 * first SD command). Used by main to generate default boot files and show a
 * first-run welcome. Pass NULL to clear.
 */
void storage_register_sd_first_mount_callback(void (*callback)(void));

/** Re-arm the one-shot first-mount callback so it fires on the next mount.
 * Used when the callback ran but could not complete its work (e.g. the VFS
 * was not readable yet); the next mount retries instead of silently
 * skipping the boot restore for the rest of the session. */
void storage_sd_first_mount_reset(void);

/**
 * Unmount the SD card for safe removal and report the result to the user.
 * Implements the `sd eject` / `sdeject` command.
 */
void shell_command_sd_eject(void);

/**
 * Mount (or re-mount) the SD card, clearing the eject latch so a re-inserted
 * card can be used without rebooting. Implements the `sd mount` command.
 */
void shell_command_sd_mount(void);

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

/**
 * Expand a DOS wildcard pattern into matching paths.
 *
 * Splits @p pattern into a directory part and a filename pattern, lists the
 * directory, and returns every entry whose name matches the pattern (via
 * shell_wildcard_match). The returned tokens are full paths (directory +
 * name) so they can be substituted straight into a `for` body. The array and
 * every token are heap-allocated; the caller frees them with
 * storage_free_wildcard_expansion(). Returns an empty result (not an error)
 * when nothing matches.
 *
 * @param pattern    Wildcard pattern as typed (e.g. `*.txt`, `sub\*.bat`).
 * @param tokens_out Receives a NULL-terminated heap array of heap strings.
 * @param count_out  Receives the number of tokens.
 * @return ESP_OK, or the resolution/read error.
 */
esp_err_t storage_expand_wildcard(const char *pattern, char ***tokens_out, int *count_out);

/**
 * Expand a DOS wildcard pattern into matching DIRECTORY paths (`for /D`).
 * Same shape as storage_expand_wildcard(), but keeps directory entries and
 * skips files. The caller frees the result with
 * storage_free_wildcard_expansion().
 */
esp_err_t storage_expand_dirs(const char *pattern, char ***tokens_out, int *count_out);

/**
 * Recursively expand a filename pattern under @p root into matching FILE
 * paths (`for /R`). Walks to `P4_CONFIG_DIR_RECURSE_DEPTH_MAX`, emits at
 * most `P4_CONFIG_SD_LIST_LIMIT` tokens (full VFS paths, so a `for` body can
 * use them directly), and skips unreadable subdirectories rather than
 * aborting the walk. An empty @p name_pattern matches everything. The caller
 * frees the result with storage_free_wildcard_expansion().
 */
esp_err_t storage_expand_recursive(const char *root, const char *name_pattern,
                                   char ***tokens_out, int *count_out);

/** Free a wildcard expansion returned by storage_expand_wildcard(). */
void storage_free_wildcard_expansion(char **tokens, int count);

/**
 * Stream one resolved file line by line (trailing CR/LF stripped), invoking
 * @p cb per line. This is the ONE line-reading loop shared by `findstr` and
 * `gfind`; nothing else reads a file line-by-line for searching.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND when the file cannot be opened, or
 *         ESP_ERR_INVALID_ARG. @p cb returning false stops early (still OK).
 */
typedef bool (*storage_line_cb_t)(const char *vfs_path, int line_no,
                                  const char *line, void *ctx);
esp_err_t storage_scan_file_lines(const char *resolved_path,
                                  storage_line_cb_t cb, void *ctx);

/**
 * Visit every file under @p vfs_root recursively (depth-bounded by
 * `P4_CONFIG_DIR_RECURSE_DEPTH_MAX`, one heap block per level, "."/".."
 * skipped). This is the ONE search tree walker shared by `findstr /S` and
 * `gfind /files`.
 *
 * @param vfs_root  Directory (or single file) to visit.
 * @param skip      Optional predicate; return true to skip an entry (the walk
 *                  does not descend a skipped directory). NULL visits all.
 * @param on_file   Called with each file's full VFS path.
 * @return ESP_OK, ESP_ERR_NOT_FOUND for a missing root, or ESP_ERR_INVALID_ARG.
 */
typedef bool (*storage_walk_skip_cb_t)(const char *dir_vfs, const char *entry_name,
                                       bool is_dir, void *ctx);
typedef void (*storage_file_cb_t)(const char *vfs_path, void *ctx);
esp_err_t storage_walk_files(const char *vfs_root, storage_walk_skip_cb_t skip,
                             storage_file_cb_t on_file, void *ctx);

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
 * DISK AND PARTITION SERVICES (diskpart / DOS FORMAT style)
 * ========================================================================
 * Physical-disk operations: geometry queries, MBR partition-table
 * inspection and editing, and the format engine. The command layer
 * (storage_commands.c) owns argument parsing and the destructive-confirmation
 * contract; the mechanics live here so the storage module stays the single
 * owner of mount state and low-level FATFS/sdmmc access.
 *
 * Volume targeting is parameterized so a second volume (USB OTG MSC at
 * /usb0) can be added later without changing the command surface.
 */

/** Physical storage volume that a disk/format operation targets. */
typedef enum {
    STORAGE_VOLUME_SD = 0,   /**< SD card on the BSP SDMMC host. */
    /* Future: STORAGE_VOLUME_USB for the USB OTG MSC device at /usb0. */
} storage_volume_t;

/** Physical disk geometry (from the SDMMC card descriptor). */
typedef struct {
    char card_name[32];      /**< Card name from the CID register. */
    uint32_t sector_size;    /**< Logical sector size in bytes. */
    uint32_t sector_count;   /**< Total sector count. */
    uint64_t capacity_bytes; /**< sector_size * sector_count. */
    uint32_t max_freq_khz;   /**< Negotiated bus clock. */
} storage_disk_info_t;

/** One MBR partition-table entry (16-byte PTE decoded). */
typedef struct {
    bool bootable;           /**< Boot-indicator flag (0x80). */
    uint8_t type;            /**< Partition type byte (0x0C FAT32 LBA, ...). */
    uint32_t start_lba;      /**< First sector of the partition. */
    uint32_t size_lba;       /**< Partition length in sectors. */
} storage_partition_t;

/** MBR partition table decoded from sector 0. */
typedef struct {
    bool valid;              /**< 0x55AA signature present. */
    storage_partition_t partitions[4];
} storage_mbr_t;

/** Options for storage_format_volume(). */
typedef struct {
    uint32_t alloc_unit_bytes;  /**< Cluster size, 0 = FATFS automatic. */
    char label[12];             /**< Volume label, empty for none. */
    bool quick;                 /**< Accepted for DOS /Q familiarity. */
} storage_format_opts_t;

/**
 * Get physical disk geometry for a volume.
 *
 * @param vol   Target volume (STORAGE_VOLUME_SD).
 * @param out   Receives the geometry.
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED for an unknown volume, or the mount
 *         error when the card cannot be brought up.
 */
esp_err_t storage_disk_get_info(storage_volume_t vol, storage_disk_info_t *out);

/**
 * Read and decode the MBR partition table of a volume.
 *
 * @param vol   Target volume.
 * @param out   Receives the decoded table.
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED, or the mount/read error.
 */
esp_err_t storage_disk_read_mbr(storage_volume_t vol, storage_mbr_t *out);

/**
 * Remove the partition table (diskpart `clean`).
 *
 * Unmounts the FATFS volume, zeroes the four MBR partition entries (keeping
 * the 0x55AA signature), and leaves the volume unmounted so the card state
 * matches reality. Run `format` afterwards to create a fresh filesystem.
 *
 * @param vol   Target volume.
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED, or the mount/write error.
 */
esp_err_t storage_disk_clean(storage_volume_t vol);

/**
 * Create a primary MBR partition (diskpart `create partition primary`).
 *
 * Writes a FAT32-LBA (0x0C) partition-table entry aligned to
 * P4_CONFIG_DISK_PARTITION_ALIGN_SECTORS. When @p size_bytes is 0 the
 * partition spans the remaining card capacity. The volume is unmounted first.
 *
 * @param vol         Target volume.
 * @param size_bytes  Desired partition size, or 0 for the rest of the card.
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_STATE when the
 *         requested slot is already occupied, or the mount/write error.
 */
esp_err_t storage_disk_create_primary_partition(storage_volume_t vol, uint64_t size_bytes);

/**
 * Delete an MBR partition (diskpart `delete partition`).
 *
 * Zeroes the partition-table entry at @p partition_index (0..3) and writes
 * the table back. The volume is unmounted first.
 *
 * @param vol              Target volume.
 * @param partition_index  0-based index into the MBR partition table.
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_ARG for an out-of-
 *         range index, or the mount/write error.
 */
esp_err_t storage_disk_delete_partition(storage_volume_t vol, unsigned partition_index);

/**
 * Format a volume (DOS FORMAT.COM / diskpart `format`).
 *
 * Uses esp_vfs_fat_sdcard_format_cfg(), which unmounts, formats, and remounts
 * the volume. FAT type is selected size-appropriately (FAT12/16 for small
 * volumes, FAT32 for large); exFAT is not available in this firmware build.
 * The requested label is applied after formatting.
 *
 * @param vol   Target volume.
 * @param opts  Format options (cluster size, label, quick).
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED, or the format error.
 */
esp_err_t storage_format_volume(storage_volume_t vol, const storage_format_opts_t *opts);

/**
 * Report the mounted volume's FAT type as a short string.
 *
 * @return "FAT12", "FAT16", "FAT32", or "unknown".
 */
const char *storage_get_fat_type(void);

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
 * RECYCLE BIN (TRASH)
 * ========================================================================
 * `del`/`erase` move matching files and `rd /s` moves whole directory trees
 * into a hidden `.trash` folder; `undelete` / `trash restore` bring them
 * back, and `trash`/`recycle` manage the bin. A side-car `.meta` file next to
 * each entry records its original path so a restore can reconstruct the
 * original location. Limits (size / age / count) are enforced on every trash
 * operation, purging the oldest entries first.
 *
 * Every function opens its own guarded SD session, so callers do not need
 * one. A failed move or restore always leaves the source intact.
 */

/** Report whether the recycle bin is enabled by configuration. */
bool storage_trash_enabled(void);

/** Resolve the trash folder path (absolute, under the SD mount point). */
const char *storage_trash_path(void);

/**
 * Delete a single regular file: move it into the trash, or unlink it when
 * @p permanent is true.
 * @param resolved_path  Resolved absolute path of the file (not a directory).
 * @param permanent      true for a true delete (bypasses the trash).
 * @return ESP_OK, or the mount / rename error.
 */
esp_err_t storage_trash_delete_file(const char *resolved_path, bool permanent);

/**
 * Delete every wildcard match in a directory: move each matching file into
 * the trash, or unlink it when @p permanent is true.
 * @param resolved_dir  Resolved absolute directory path.
 * @param pattern       DOS wildcard filename pattern (files only).
 * @param recursive     When true, walk the whole subtree collecting matches.
 * @param permanent     true for a true delete (bypasses the trash).
 * @param deleted_out   Receives the number of entries removed.
 */
esp_err_t storage_trash_delete_pattern(const char *resolved_dir, const char *pattern,
                                       bool recursive, bool permanent, int *deleted_out);

/**
 * Remove a whole directory tree: move it into the trash (one atomic rename),
 * or delete it permanently when @p permanent is true (bounded recursive
 * delete). Used by `rd /s`.
 * @param resolved_path  Resolved absolute directory path.
 * @param permanent      true for a true delete.
 */
esp_err_t storage_trash_remove_tree(const char *resolved_path, bool permanent);

/**
 * Restore a trash entry to its original location.
 * @param name_or_index  Entry name (as shown by `trash list`) or its 1-based
 *                       index in the listing order.
 * @return ESP_OK, ESP_ERR_NOT_FOUND for an unknown entry, or the move error.
 */
esp_err_t storage_trash_restore(const char *name_or_index);

/** Permanently purge a single trash entry (caller has already confirmed). */
esp_err_t storage_trash_purge(const char *name_or_index);

/** Permanently purge every trash entry (caller has already confirmed). */
esp_err_t storage_trash_empty(void);

/** Print the trash contents (name, original path, size, age) with 1-based
 *  indexes usable by restore/purge. */
esp_err_t storage_trash_list(void);

/** Print trash usage: entry count, total bytes, oldest entry age. */
esp_err_t storage_trash_info(void);

/** Enforce the size/age/count limits, purging the oldest entries first.
 *  Called automatically after every trash operation. */
void storage_trash_enforce_limits(void);

/* ========================================================================
 * INI-STYLE PERSISTENT STATE + TEMP FILES (storage_ini.c)
 * ========================================================================
 * Simple `KEY=VALUE` config files (DOS-style INI) and SD-backed temporary
 * files, shared by the batch `ini`/`temp` commands and the applib state
 * group. All file access goes through the guarded SD session and is written
 * atomically (temp file + rename).
 */

/** Read a `KEY=value` directive from a text buffer (comment/blank-aware).
 *  @return the value length (>= 0) when found, or -1 when the key is absent. */
int storage_ini_get_value(const char *text, const char *key,
                          char *out, size_t out_size);

/** Set a `KEY=value` directive in a text buffer (replaces existing, appends).
 *  @return true when the buffer now holds the updated text. */
bool storage_ini_upsert(char *text, size_t cap,
                        const char *key, const char *value);

/** Remove every `KEY=value` directive line from a text buffer.
 *  @return true when at least one matching line was removed. */
bool storage_ini_remove(char *text, size_t cap, const char *key);

/** Read the value of @p key from an INI file on the SD card. */
esp_err_t storage_ini_file_get(const char *path, const char *key,
                               char *value, size_t value_size);

/** Create or update @p key in an INI file on the SD card (atomic write). */
esp_err_t storage_ini_file_set(const char *path, const char *key, const char *value);

/** Remove @p key from an INI file on the SD card (atomic write). */
esp_err_t storage_ini_file_delete(const char *path, const char *key);

/** Iterate every `KEY=VALUE` line of an INI file; @p cb returns false to stop. */
esp_err_t storage_ini_file_foreach(const char *path,
                                   bool (*cb)(const char *key, const char *value, void *ctx),
                                   void *ctx);

/** Write a whole text file on the SD card atomically (free-space guardrail). */
/**
 * Ensure @p vfs_dir exists on the SD card, creating missing parents.
 *
 * Single implementation of the recursive `mkdir -p` used by the INI/temp core,
 * the record store, the archive, and the alarm store. Opens its own guarded SD
 * session (nested calls are safe), so callers do not manage a session for this.
 */
esp_err_t storage_mkdir_p(const char *vfs_dir);

esp_err_t storage_write_text_file(const char *path, const char *text);

/** Create a unique temporary file under the SD temp directory and return its
 *  resolved path in @p buf (the file is created so the path is reserved). */
esp_err_t storage_temp_path(char *buf, size_t size, const char *ext);

/** Delete every file under the SD temp directory. */
esp_err_t storage_temp_cleanup(void);

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

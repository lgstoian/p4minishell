/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file storage_path.h
 * @brief Hardware-free path resolution, the shell cwd and the USB slot map.
 *
 * Split out of storage.h so pure path users (the batch environment core, the
 * himem verb, the host unit-test lane) can resolve `sd:/`, `/sdcard/`,
 * absolute, relative and `usbN:` inputs without pulling in esp_vfs_fat.h,
 * FATFS or the SDMMC driver. storage.h includes this header, so existing
 * callers are unchanged.
 *
 * Nothing declared here performs a VFS or FATFS operation: the functions are
 * string surgery over the compile-time mount points plus the RAM-only cwd.
 * `shell_fs_resolve_path()` may call the registered USB mount hook when a
 * path names an attached-but-unmounted slot, exactly as it always has; the
 * hook is NULL until main.c registers it.
 *
 * @module storage
 * @layer storage
 * @domain fs
 * @provides cwd, path resolution, USB drive/slot mapping, USB ops registration,
 *           the guarded SD session handle (value type only)
 * @status stable
 * @test test/main/test_calc.c, test/main/test_batch_expr.c, test/main/test_svc.c
 * @hard-rule pure string/path helpers and value types only - no VFS, FATFS or
 *            SD access here
 */

#ifndef P4MINISHELL_STORAGE_PATH_H
#define P4MINISHELL_STORAGE_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * USB MSC OPERATIONS TABLE
 * ========================================================================
 * Owned here (not in storage.c) because path resolution maps `u:`/`usb:/`
 * inputs to a slot base and may ask the slot to automount. The table is a
 * plain pointer handoff; the driver lives in the usb component.
 */

/** USB MSC volume operations, owned by the usb component and registered by
 *  main.c at boot (the storage layer never includes usb headers, keeping the
 *  dependency direction one-way). Every hook is NULL-checked; without
 *  registration USB volumes report unavailable. */
typedef struct {
    /** True when a device is attached in the slot (0-based). */
    bool (*is_connected)(int slot);
    /** True when the slot is VFS-mounted. */
    bool (*is_mounted)(int slot);
    /** Mount the slot's device (no-op when already mounted/absent). */
    void (*mount)(int slot);
    /** Geometry for `disk list`; false when unknown. */
    bool (*get_geometry)(int slot, uint32_t *sector_size_out,
                         uint32_t *sector_count_out, char *label_out,
                         size_t label_size);
    /** VFS base path for the slot (e.g. "/usb0"); NULL when unmounted. */
    const char *(*base_path)(int slot);
} storage_usb_ops_t;

/**
 * Register the USB MSC operations table. Called once from app_main;
 * passing NULL clears it (USB volumes report unavailable).
 */
void storage_register_usb_ops(const storage_usb_ops_t *ops);

/** The registered table, or NULL when no USB backend is installed. */
const storage_usb_ops_t *storage_usb_ops_get(void);

/** Drive letter for a USB slot (`u:`/`v:`/`w:`/`x`), or '\0' when out of range. */
char storage_usb_drive_letter(int slot);

/** USB slot for a drive letter (`u`/`v`/`w:`/`x:`, case-insensitive), or -1. */
int storage_usb_slot_for_drive(char letter);

/**
 * VFS base path for a USB slot ("/usb0".."/usbN", derived from the slot-0
 * base by bumping a trailing digit so a retargeted base keeps working).
 */
void storage_usb_slot_base(int slot, char *out, size_t out_size);

/* ========================================================================
 * GUARDED SD SESSION HANDLE
 * ========================================================================
 * Value type only: the functions that fill and end the handle
 * (`shell_sd_begin()` / `shell_sd_end()`) stay in storage.c. It lives here so
 * callers that must not pull in esp_vfs_fat.h - the batch I/O seam, the host
 * unit-test lane - can still hold one. storage.h includes this header, so the
 * type keeps its historical spelling for every existing caller.
 */

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

/* ========================================================================
 * PATH RESOLUTION
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

/** true when the path contains a `/` or `\` directory separator. */
bool shell_path_has_directory_component(const char *path);

/** Case-insensitive check for a trailing file extension such as ".bat". */
bool shell_path_has_extension(const char *path, const char *extension);

/* ========================================================================
 * CURRENT WORKING DIRECTORY
 * ======================================================================== */

/**
 * Get the shell current working directory as an absolute VFS path.
 * @return Static string owned by this module (do not free).
 */
const char *shell_get_cwd(void);

/**
 * Convert an absolute VFS path back to the shell spelling: `/sdcard/APPS/X`
 * becomes `sd:/APPS/X`, `/usb0/X` becomes `usb:/X`. A path already in
 * `sd:/`/`usb:/` form, or one that is not on a mounted volume, is copied
 * verbatim. The one place that answers "which drive is this?" for batch apps.
 */
void storage_shell_path(const char *path, char *out, size_t out_size);

/**
 * Replace the shell current working directory.
 * @param absolute_path  Absolute VFS path under the SD mount point.
 */
void storage_set_cwd(const char *absolute_path);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_STORAGE_PATH_H */

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_io.h
 * @brief The batch verbs' I/O seam: one optional backend for file and console
 *        access.
 *
 * @module batch
 * @layer batch
 * @domain core
 * @provides registered I/O backend for the batch verbs (guarded SD session,
 *           input-source resolution, INI state files, console line read)
 * @status stable
 * @test test/main/test_batch_expr.c, test/main/test_svc.c
 * @hard-rule declarations only - no FATFS, VFS, display or board header here
 *
 * Two batch translation units own verbs that touch the outside world but are
 * otherwise pure: `batch_expr.c` (`set /p` reads one line from a file or the
 * keyboard) and `svc.c` (the service registry lives in `sd:/SERVICES.INI`).
 * Both run on the host unit-test lane, which has no SD card, no key source and
 * no FATFS. Rather than teach those files about FATFS, they reach the outside
 * world through this table.
 *
 * The firmware installs the table once from `batch_init()` with pointers to
 * the real implementations (`shell_sd_begin()`, `storage_resolve_input_source()`,
 * the `storage_ini_file_*` core, `shell_read_line()`); the host lane installs
 * nothing and every pointer stays NULL. A NULL pointer is a real, supported
 * configuration, not a stub: the callers already treat "no card" and "no key
 * source" as ordinary failures, report them, and carry on. Nothing here fakes
 * success, a file or an INI value.
 *
 * This mirrors the existing registration tables (`batch_command_ops_t`,
 * `batch_bg_ops_t`, `storage_usb_ops_t`): a plain pointer handoff, checked for
 * NULL at every call, so the dependency direction stays one-way
 * (`batch` -> `storage` -> `shell`).
 */

#ifndef P4MINISHELL_BATCH_IO_H
#define P4MINISHELL_BATCH_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "storage_path.h"   /* shell_sd_session_t (value type, no FATFS) */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * I/O operations the batch verbs need from the firmware.
 *
 * Every member is optional. A NULL member means the backend cannot provide it
 * on this build/target, and the caller must report the failure it already
 * reports when the underlying resource is missing (no SD card, no interactive
 * key source). No member ever invents a value.
 */
typedef struct {
    /**
     * Begin a guarded SD session (firmware: `shell_sd_begin()`).
     * @return ESP_OK when the card is mounted and usable, or the same errors
     *         `shell_sd_begin()` reports (ESP_ERR_INVALID_STATE after eject).
     */
    esp_err_t (*sd_begin)(shell_sd_session_t *session);

    /**
     * End a session opened by @ref sd_begin (firmware: `shell_sd_end()`).
     * @param operation  Command name used in diagnostics.
     */
    void (*sd_end)(shell_sd_session_t *session, const char *operation);

    /**
     * Resolve the active input source for a text-consuming command
     * (firmware: `storage_resolve_input_source()`).
     *
     * @return ESP_OK with an absolute VFS path in @p output, or
     *         ESP_ERR_NOT_FOUND when no `< file` / pipe input is pending.
     */
    esp_err_t (*resolve_input_source)(const char *argument, char *output,
                                      size_t output_size);

    /**
     * Read one line from the interactive key source (firmware:
     * `shell_read_line()`), echoing as it is typed.
     *
     * @return true when a line was completed with Enter, false on cancel,
     *         timeout, or when no interactive key source is attached.
     */
    bool (*read_line)(char *output, size_t output_size, uint32_t timeout_ms);

    /** Password-mode sibling of @ref read_line (`shell_read_line_hidden()`). */
    bool (*read_line_hidden)(char *output, size_t output_size,
                             uint32_t timeout_ms);

    /** Create or update one key in an INI file (firmware:
     *  `storage_ini_file_set()`, atomic write). */
    esp_err_t (*ini_file_set)(const char *path, const char *key,
                              const char *value);

    /** Remove one key from an INI file (firmware: `storage_ini_file_delete()`). */
    esp_err_t (*ini_file_delete)(const char *path, const char *key);

    /** Visit every `KEY=VALUE` line of an INI file (firmware:
     *  `storage_ini_file_foreach()`); @p cb returns false to stop. */
    esp_err_t (*ini_file_foreach)(const char *path,
                                  bool (*cb)(const char *key, const char *value,
                                             void *ctx),
                                  void *ctx);

    /**
     * Open a batch script or spool file. Firmware leaves this NULL and the
     * caller uses plain `fopen()`. A host test installs a backend that maps
     * the SD mount point onto a fixture directory; no firmware behaviour
     * changes. @p mode is the ordinary C `fopen` mode string.
     */
    FILE *(*file_open)(const char *path, const char *mode);

    /**
     * stat() wrapper for batch script discovery (firmware:
     * `shell_sd_stat_path()`). NULL means use the storage implementation.
     */
    esp_err_t (*stat_path)(const char *path, struct stat *st);
} batch_io_ops_t;

/**
 * Install (or clear, with NULL) the I/O backend.
 *
 * Called once by `batch_init()`. The table is boot-lived and process-global;
 * clearing it turns every operation above into "unavailable", which is the
 * host unit-test lane's configuration.
 */
void batch_register_io_ops(const batch_io_ops_t *ops);

/** The installed table, or NULL when no I/O backend is installed. */
const batch_io_ops_t *batch_io_ops_get(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_BATCH_IO_H */

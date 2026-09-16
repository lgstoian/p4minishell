/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file applib_state.h
 * @brief Persistent-state group of the applib (INI files + temp files).
 *
 * DOS-like apps kept state in environment variables, temporary files, and
 * simple `KEY=VALUE` INI files. This group gives native apps that surface:
 * `app_ini_*` reads and updates INI-style config files, and `app_temp_*`
 * creates and cleans SD-backed temporary files. All storage lives on the SD
 * card (paths resolve relative to the shell current directory; temp files go
 * under `sd:/tmp`). The mechanics are implemented once in
 * components/storage (storage_ini.c) and shared with the batch `ini` / `temp`
 * commands, so nothing is duplicated.
 */

#ifndef P4MINISHELL_APPLIB_STATE_H
#define P4MINISHELL_APPLIB_STATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read the value of @p key from an INI file on the SD card.
 *
 * @param path       File path (relative to the shell cwd or an `sd:` path).
 * @param key        Case-insensitive key name.
 * @param value      Receives the trimmed value (NUL-terminated).
 * @param value_size Capacity of @p value.
 * @return true when the key was found, false on a missing file / key or an
 *         I/O error.
 */
bool app_ini_get(const char *path, const char *key, char *value, size_t value_size);

/**
 * Create or update @p key in an INI file on the SD card (atomic write; a
 * missing file is created). Comments and unrelated lines are preserved.
 *
 * @return true on success, false on an I/O error.
 */
bool app_ini_set(const char *path, const char *key, const char *value);

/**
 * Remove @p key from an INI file on the SD card (atomic write).
 *
 * @return true when the key was removed, false when it was absent or an I/O
 *         error occurred.
 */
bool app_ini_delete(const char *path, const char *key);

/**
 * Create a unique temporary file under the SD temp directory and write its
 * resolved path into @p buf (the file is created so the path is reserved).
 *
 * @param ext  Optional extension (without the dot, e.g. "csv"); "tmp" by
 *             default.
 * @return true on success, false on an I/O error.
 */
bool app_temp_path(char *buf, size_t size, const char *ext);

/** Delete every file under the SD temp directory. @return true on success. */
bool app_temp_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_STATE_H */

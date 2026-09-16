/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file applib_mem.h
 * @brief Memory allocation policy + error reporting group of the applib.
 *
 * The shared allocation policy, exposed as a single set of helpers:
 *
 *   - Blocks of `P4_CONFIG_APPLIB_PSRAM_THRESHOLD_BYTES` or more are taken
 *     from PSRAM when available (heap_caps SPIRAM), falling back to the
 *     internal heap when PSRAM is absent or exhausted.
 *   - Smaller blocks use the internal heap.
 *   - Every `app_*` allocation returns NULL on failure — the caller MUST
 *     check it. `app_free` is the matching release for all of them.
 */

#ifndef P4MINISHELL_APPLIB_MEM_H
#define P4MINISHELL_APPLIB_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Allocate @p size bytes (policy above). Returns NULL on failure. */
void *app_alloc(size_t size);

/** Zeroed allocation of @p count * @p size bytes. Returns NULL on failure. */
void *app_calloc(size_t count, size_t size);

/** Resize an allocation from `app_*`. Returns NULL on failure (old pointer kept). */
void *app_realloc(void *ptr, size_t size);

/** Duplicate a string through the policy. Returns NULL on failure. */
char *app_strdup(const char *s);

/** Duplicate at most @p n bytes of a string. Returns NULL on failure. */
char *app_strndup(const char *s, size_t n);

/** Release any `app_*` allocation (works for internal and PSRAM blocks). */
void app_free(void *ptr);

/**
 * Report an error to the shell debug log and the transcript.
 *
 * @param tag    Short component tag (e.g. "myapp").
 * @param error  ESP-IDF error code (or 0 when not applicable).
 */
void app_report_error(const char *tag, int error, const char *format, ...);

/** Report a warning to the shell debug log. */
void app_report_warning(const char *tag, const char *format, ...);

/** Report an informational entry to the shell debug log. */
void app_report_info(const char *tag, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_MEM_H */

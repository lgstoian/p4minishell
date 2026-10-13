/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file json.h
 * @brief Shared JSON reader: validation, pretty-printing, and path walking.
 *
 * This is the ONE JSON parser in the firmware. It was moved out of
 * components/command (which used to own it privately inside
 * json_commands.c) so that command-layer consumers and library components
 * such as the RSS reader share a single implementation. The scanner —
 * strings, escapes, numbers, literals, arrays, objects — is unchanged; the
 * traversal is driven by the same recursive descent that backs
 * `json validate` and `json pretty`.
 *
 * The walk mode is additive: `json_walk()` reuses that traversal and reports
 * every scalar in document order with its dotted path. Array elements are
 * reported with their numeric index, so `items` in a JSON Feed document
 * yields `items.0.title`, `items.1.title`, ... Callers filter on the path;
 * no second scanner exists.
 */

#ifndef P4MINISHELL_JSON_H
#define P4MINISHELL_JSON_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Scalar visitor invoked once per scalar value in document order.
 *
 * @param path     Dotted path, e.g. "title", "items.3.url", "authors.0.name".
 *                 Object keys and array indices are '.'-separated. Valid only
 *                 for the duration of the call; copy it if you need it.
 * @param value    Raw value span as it appears in the source, WITHOUT
 *                 surrounding quotes for strings and WITHOUT escape
 *                 decoding. A string therefore still carries its \" \\ \n
 *                 escapes; use json_unescape_span() to decode them.
 * @param value_len Byte length of @p value.
 * @param ctx      Caller context passed to json_walk().
 * @return false to abort the walk (json_walk then returns false).
 */
typedef bool (*json_visit_fn)(const char *path, const char *value,
                             size_t value_len, void *ctx);

/**
 * Validate JSON text.
 *
 * @param text  Input text (need not be NUL-terminated).
 * @param len   Input length.
 * @param err   Optional buffer receiving "msg at line L col C" on failure.
 * @param err_size Size of @p err.
 * @return true when structurally valid.
 */
bool json_validate_text(const char *text, size_t len, char *err, size_t err_size);

/**
 * Pretty-print JSON text with 2-space indent.
 *
 * @return bytes written excluding the NUL, or 0 on failure (with @p err set).
 */
size_t json_pretty_text(const char *text, size_t len, char *out, size_t out_size,
                        char *err, size_t err_size);

/**
 * Walk every scalar value in @p text, invoking @p visit with dotted paths.
 *
 * Uses the same recursive descent as validation; there is no separate scanner.
 * Aborting from @p visit (returning false) stops the walk and makes
 * json_walk() return false.
 *
 * @return true when the document parsed AND the visitor never aborted.
 */
bool json_walk(const char *text, size_t len, json_visit_fn visit, void *ctx,
               char *err, size_t err_size);

/**
 * Decode JSON string escapes (\" \\ \/ \b \f \n \r \t \uXXXX, including
 * surrogate pairs) from a raw span into @p out.
 *
 * Pure and unit-tested. Invalid escapes are passed through literally rather
 * than rejected, so a malformed span still yields displayable text.
 *
 * @return bytes written excluding the NUL; 0 when @p out is too small.
 */
size_t json_unescape_span(const char *src, size_t len, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_JSON_H */
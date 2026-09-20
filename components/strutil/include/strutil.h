/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file strutil.h
 * @brief Shared string helpers (case-insensitive compare, argument split).
 *
 * Leaf component with no IDF dependencies so any module (including the USB
 * leaf) can share one implementation instead of carrying its own copy.
 */

#ifndef STRUTIL_H
#define STRUTIL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Case-insensitive equality; false when either pointer is NULL. */
bool strutil_text_equals_ignore_case(const char *left, const char *right);

/** Whitespace-split @p text in place into @p argv (up to @p max_args).
 *  Returns the token count. */
int strutil_split_args(char *text, char **argv, int max_args);

#ifdef __cplusplus
}
#endif

#endif /* STRUTIL_H */

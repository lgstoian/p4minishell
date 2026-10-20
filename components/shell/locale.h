/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * @file locale.h
 * @brief Lightweight internationalization: locale-aware date/time/number formatting.
 *
 * Inspired by MS-DOS country code tables. Instead of full translation tables
 * (which would bloat firmware), this module provides formatting rules: date
 * order, time separator, decimal/thousands separators, and 12/24-hour clock.
 *
 * A single byte (`s_locale_id`) selects the active locale. The `locale` command
 * sets it; the `date`/`time` virtual variables read it.
 *
 * Locale IDs match the MS-DOS country code scheme for familiarity:
 *   0 = US (default), 1 = UK, 2 = DE (German), 3 = FR (French)
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Locale identifiers (matches MS-DOS country codes). */
typedef enum {
    LOCALE_US = 0,   /**< MM-DD-YYYY, 12h, period decimal */
    LOCALE_UK = 1,   /**< DD-MM-YYYY, 24h, period decimal */
    LOCALE_DE = 2,   /**< DD.MM.YYYY, 24h, comma decimal */
    LOCALE_FR = 3,   /**< DD/MM/YYYY, 24h, comma decimal */
    LOCALE_COUNT     /**< Sentinel — number of supported locales */
} locale_id_t;

/** Get the current locale ID. */
locale_id_t locale_get(void);

/** Set the current locale by ID (ignored if @p id >= LOCALE_COUNT). */
void locale_set(locale_id_t id);

/** Get a human-readable name for a locale ("US", "UK", "DE", "FR"). */
const char *locale_name(locale_id_t id);

/**
 * Parse a locale selector: a name ("US", "UK", "DE", "FR", case-insensitive)
 * or a numeric MS-DOS country code ("0".."3"). Returns true and writes @p out
 * on success; false leaves @p out untouched.
 */
bool locale_parse(const char *selector, locale_id_t *out);

/** Format a date into @p buf according to the current locale.
 *  buf must hold at least 11 bytes (e.g. "12-31-2026\0"). */
void locale_format_date(int month, int day, int year, char *buf, size_t buf_size);

/** Format a time into @p buf according to the current locale.
 *  buf must hold at least 9 bytes (e.g. "23:59:59\0"). */
void locale_format_time(int hour, int min, int sec, char *buf, size_t buf_size);

/** Format a number with locale-appropriate thousands separator.
 *  Writes at most buf_size-1 chars + NUL. */
void locale_format_number(unsigned long value, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

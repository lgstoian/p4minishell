/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file locale.c
 * @brief Lightweight locale-aware formatting.
 *
 * Each locale defines date order, time separator, decimal separator, and
 * 12/24-hour preference. The active locale is one byte in RAM.
 */

#include "locale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include "esp_log.h"

/** Per-locale formatting rules. */
typedef struct {
    uint8_t date_order;      /**< 0=MDY (US), 1=DMY (UK/DE/FR). */
    char    date_sep;        /**< Date separator: '-', '.', '/' */
    char    time_sep;        /**< Time separator (always ':'). */
    char    dec_sep;         /**< Decimal separator: '.' or ',' */
    char    thousands_sep;   /**< Thousands separator: ',' or '.' */
    bool    use_24h;         /**< true = 24h, false = 12h AM/PM. */
    const char *name;        /**< Human-readable name. */
    const char *am_str;      /**< AM suffix (12h locales only). */
    const char *pm_str;      /**< PM suffix (12h locales only). */
} locale_rules_t;

static const locale_rules_t s_rules[] = {
    /* US: MM-DD-YYYY, 12h AM/PM, period decimal */
    [LOCALE_US] = {
        .date_order = 0, .date_sep = '-', .time_sep = ':',
        .dec_sep = '.', .thousands_sep = ',',
        .use_24h = false, .name = "US", .am_str = "AM", .pm_str = "PM",
    },
    /* UK: DD-MM-YYYY, 24h, period decimal */
    [LOCALE_UK] = {
        .date_order = 1, .date_sep = '-', .time_sep = ':',
        .dec_sep = '.', .thousands_sep = ',',
        .use_24h = true, .name = "UK", .am_str = "", .pm_str = "",
    },
    /* DE: DD.MM.YYYY, 24h, comma decimal */
    [LOCALE_DE] = {
        .date_order = 1, .date_sep = '.', .time_sep = ':',
        .dec_sep = ',', .thousands_sep = '.',
        .use_24h = true, .name = "DE", .am_str = "", .pm_str = "",
    },
    /* FR: DD/MM/YYYY, 24h, comma decimal */
    [LOCALE_FR] = {
        .date_order = 1, .date_sep = '/', .time_sep = ':',
        .dec_sep = ',', .thousands_sep = ' ',
        .use_24h = true, .name = "FR", .am_str = "", .pm_str = "",
    },
};

static locale_id_t s_locale = LOCALE_US;

locale_id_t locale_get(void)
{
    return s_locale;
}

void locale_set(locale_id_t id)
{
    if (id < LOCALE_COUNT) {
        s_locale = id;
    }
}

const char *locale_name(locale_id_t id)
{
    if (id < LOCALE_COUNT) {
        return s_rules[id].name;
    }
    return "??";
}

bool locale_parse(const char *selector, locale_id_t *out)
{
    size_t i;

    if (selector == NULL || out == NULL) {
        return false;
    }
    while (*selector == ' ' || *selector == '\t') {
        selector++;
    }
    /* Numeric MS-DOS country code. */
    if (selector[0] >= '0' && selector[0] <= '9') {
        char *end = NULL;
        long code = strtol(selector, &end, 10);
        while (end != NULL && (*end == ' ' || *end == '\t')) {
            end++;
        }
        if (end != NULL && *end == '\0' && code >= 0 && code < LOCALE_COUNT) {
            *out = (locale_id_t)code;
            return true;
        }
        return false;
    }
    for (i = 0; i < LOCALE_COUNT; i++) {
        if (strcasecmp(selector, s_rules[i].name) == 0) {
            *out = (locale_id_t)i;
            return true;
        }
    }
    return false;
}

void locale_format_date(int month, int day, int year, char *buf, size_t buf_size)
{
    const locale_rules_t *r = &s_rules[s_locale];
    if (buf == NULL || buf_size < 11) {
        return;
    }
    if (r->date_order == 0) {
        /* MDY */
        snprintf(buf, buf_size, "%02d%c%02d%c%04d",
                 month, r->date_sep, day, r->date_sep, year);
    } else {
        /* DMY */
        snprintf(buf, buf_size, "%02d%c%02d%c%04d",
                 day, r->date_sep, month, r->date_sep, year);
    }
}

void locale_format_time(int hour, int min, int sec, char *buf, size_t buf_size)
{
    const locale_rules_t *r = &s_rules[s_locale];
    if (buf == NULL || buf_size < 9) {
        return;
    }
    if (r->use_24h) {
        snprintf(buf, buf_size, "%02d%c%02d%c%02d",
                 hour, r->time_sep, min, r->time_sep, sec);
    } else {
        int h = hour % 12;
        if (h == 0) {
            h = 12;
        }
        const char *suffix = (hour < 12) ? r->am_str : r->pm_str;
        snprintf(buf, buf_size, "%2d%c%02d%c%02d%s",
                 h, r->time_sep, min, r->time_sep, sec, suffix);
    }
}

void locale_format_number(unsigned long value, char *buf, size_t buf_size)
{
    const locale_rules_t *r = &s_rules[s_locale];
    char raw[24];
    char *dst = buf;
    size_t len;
    size_t i;

    if (buf == NULL || buf_size < 2) {
        return;
    }

    snprintf(raw, sizeof(raw), "%lu", value);
    len = strlen(raw);

    /* Insert a thousands separator before every 3-digit group counted from the
     * RIGHT (so 1234567 -> 1,234,567). A group boundary is where the number of
     * remaining digits is a multiple of 3. */
    for (i = 0; i < len && (size_t)(dst - buf) < buf_size - 1; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            *dst++ = r->thousands_sep;
            if ((size_t)(dst - buf) >= buf_size - 1) {
                break;
            }
        }
        *dst++ = raw[i];
    }
    *dst = '\0';
}

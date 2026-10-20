/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_locale.c
 * @brief Locale formatting unit tests (date order, 12/24h, decimal/thousands).
 */

#include "unity.h"
#include "locale.h"

#include <string.h>

void test_locale_date_orders(void)
{
    char b[16];

    locale_set(LOCALE_US);
    locale_format_date(3, 5, 2026, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("03-05-2026", b);

    locale_set(LOCALE_UK);
    locale_format_date(3, 5, 2026, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("05-03-2026", b);

    locale_set(LOCALE_DE);
    locale_format_date(3, 5, 2026, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("05.03.2026", b);

    locale_set(LOCALE_FR);
    locale_format_date(3, 5, 2026, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("05/03/2026", b);

    locale_set(LOCALE_US);
}

void test_locale_time_24h_and_12h(void)
{
    char b[16];

    locale_set(LOCALE_UK);
    locale_format_time(13, 4, 5, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("13:04:05", b);

    locale_set(LOCALE_US);
    locale_format_time(13, 4, 5, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(" 1:04:05PM", b);

    locale_format_time(0, 0, 0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("12:00:00AM", b);

    locale_set(LOCALE_US);
}

void test_locale_number_thousands_separator(void)
{
    char b[24];

    locale_set(LOCALE_US);
    locale_format_number(1234567UL, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("1,234,567", b);
    locale_format_number(42UL, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("42", b);

    locale_set(LOCALE_DE);
    locale_format_number(1234567UL, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("1.234.567", b);

    locale_set(LOCALE_US);
}

void test_locale_parse_names_and_codes(void)
{
    locale_id_t id = LOCALE_US;

    TEST_ASSERT_TRUE(locale_parse("DE", &id));
    TEST_ASSERT_EQUAL_INT(LOCALE_DE, id);
    TEST_ASSERT_TRUE(locale_parse("fr", &id));
    TEST_ASSERT_EQUAL_INT(LOCALE_FR, id);
    TEST_ASSERT_TRUE(locale_parse("2", &id));
    TEST_ASSERT_EQUAL_INT(LOCALE_DE, id);
    TEST_ASSERT_TRUE(locale_parse(" 0 ", &id));
    TEST_ASSERT_EQUAL_INT(LOCALE_US, id);

    /* Invalid selectors leave @p id untouched and report false. */
    id = LOCALE_UK;
    TEST_ASSERT_FALSE(locale_parse("XX", &id));
    TEST_ASSERT_FALSE(locale_parse("9", &id));
    TEST_ASSERT_FALSE(locale_parse("", &id));
    TEST_ASSERT_FALSE(locale_parse(NULL, &id));
    TEST_ASSERT_EQUAL_INT(LOCALE_UK, id);
}

void test_locale_get_set_name_and_bounds(void)
{
    locale_set(LOCALE_FR);
    TEST_ASSERT_EQUAL_INT(LOCALE_FR, locale_get());
    TEST_ASSERT_EQUAL_STRING("FR", locale_name(LOCALE_FR));
    TEST_ASSERT_EQUAL_STRING("US", locale_name(LOCALE_US));

    /* An out-of-range id is ignored (no clobber). */
    locale_set((locale_id_t)99);
    TEST_ASSERT_EQUAL_INT(LOCALE_FR, locale_get());

    locale_set(LOCALE_US);
}

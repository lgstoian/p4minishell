/**
 * @file test_import.c
 * @brief Unit tests for the pure `import` interchange parsers.
 *
 * Covers import_vcf_prop_split(), import_json_unescape(), and
 * import_ics_datetime() from components/command/import_commands.c.
 * Store I/O (db_add/alarm_add round-trips) stays hardware-verified: it
 * needs the guarded SD sessions.
 *
 * Pure logic with no hardware dependency.
 */

#include "unity.h"
#include "command.h"
#include <string.h>
#include <time.h>

/* ========================================================================
 * vCard/iCalendar property-line splitter
 * ======================================================================== */

void test_import_vcf_prop_split_plain(void)
{
    char name[32];
    const char *value = NULL;

    TEST_ASSERT_TRUE(import_vcf_prop_split("FN:John Doe", name, sizeof(name), &value));
    TEST_ASSERT_EQUAL_STRING("FN", name);
    TEST_ASSERT_EQUAL_STRING("John Doe", value);
}

void test_import_vcf_prop_split_params(void)
{
    char name[32];
    const char *value = NULL;

    TEST_ASSERT_TRUE(import_vcf_prop_split("TEL;TYPE=CELL,VOICE:+1 555 1234",
                                           name, sizeof(name), &value));
    TEST_ASSERT_EQUAL_STRING("TEL", name);
    TEST_ASSERT_EQUAL_STRING("+1 555 1234", value);
}

void test_import_vcf_prop_split_group(void)
{
    char name[32];
    const char *value = NULL;

    TEST_ASSERT_TRUE(import_vcf_prop_split("item1.EMAIL;TYPE=HOME:a@b.c",
                                           name, sizeof(name), &value));
    TEST_ASSERT_EQUAL_STRING("EMAIL", name);
    TEST_ASSERT_EQUAL_STRING("a@b.c", value);
}

void test_import_vcf_prop_split_first_colon(void)
{
    char name[32];
    const char *value = NULL;

    /* The value may hold colons (URLs, times); only the first splits. */
    TEST_ASSERT_TRUE(import_vcf_prop_split("URL:https://x.test:8080/a",
                                           name, sizeof(name), &value));
    TEST_ASSERT_EQUAL_STRING("URL", name);
    TEST_ASSERT_EQUAL_STRING("https://x.test:8080/a", value);
}

void test_import_vcf_prop_split_rejects(void)
{
    char name[32];
    const char *value = NULL;

    TEST_ASSERT_FALSE(import_vcf_prop_split("NOVALUE", name, sizeof(name), &value));
    TEST_ASSERT_FALSE(import_vcf_prop_split(":x", name, sizeof(name), &value));
    TEST_ASSERT_FALSE(import_vcf_prop_split("", name, sizeof(name), &value));
    TEST_ASSERT_FALSE(import_vcf_prop_split(NULL, name, sizeof(name), &value));
    TEST_ASSERT_FALSE(import_vcf_prop_split("FN:x", NULL, sizeof(name), &value));
    TEST_ASSERT_FALSE(import_vcf_prop_split("FN:x", name, 0, &value));
    TEST_ASSERT_FALSE(import_vcf_prop_split("FN:x", name, sizeof(name), NULL));
    /* A 2-byte buffer cannot hold "FN" plus the terminator. */
    TEST_ASSERT_FALSE(import_vcf_prop_split("FN:x", name, 2, &value));
}

/* ========================================================================
 * JSON string unescaper
 * ======================================================================== */

void test_import_json_unescape_basic(void)
{
    char dst[64];
    const char *src = "a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t!";
    size_t n = import_json_unescape(src, strlen(src), dst, sizeof(dst));

    TEST_ASSERT_EQUAL_UINT(13, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("a\"b\\c/d\b\f\n\r\t!", dst);
}

void test_import_json_unescape_unicode(void)
{
    char dst[64];

    /* U+0041 -> 'A'. */
    TEST_ASSERT_EQUAL_UINT(1, import_json_unescape("\\u0041", 6, dst, sizeof(dst)));
    TEST_ASSERT_EQUAL_STRING("A", dst);
    /* U+00E9 -> 2-byte UTF-8 (0xC3 0xA9). */
    TEST_ASSERT_EQUAL_UINT(2, import_json_unescape("\\u00e9", 6, dst, sizeof(dst)));
    TEST_ASSERT_EQUAL_UINT8(0xC3, (uint8_t)dst[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA9, (uint8_t)dst[1]);
    TEST_ASSERT_EQUAL('\0', dst[2]);
    /* U+4E2D -> 3-byte UTF-8 (0xE4 0xB8 0xAD). */
    TEST_ASSERT_EQUAL_UINT(3, import_json_unescape("\\u4e2d", 6, dst, sizeof(dst)));
    TEST_ASSERT_EQUAL_UINT8(0xE4, (uint8_t)dst[0]);
    TEST_ASSERT_EQUAL_UINT8(0xB8, (uint8_t)dst[1]);
    TEST_ASSERT_EQUAL_UINT8(0xAD, (uint8_t)dst[2]);
    /* Lone surrogate degrades to '?'. */
    TEST_ASSERT_EQUAL_UINT(1, import_json_unescape("\\uD800", 6, dst, sizeof(dst)));
    TEST_ASSERT_EQUAL_STRING("?", dst);
}

void test_import_json_unescape_safety(void)
{
    char dst[4];

    /* Truncation stays NUL-terminated and bounded. */
    TEST_ASSERT_EQUAL_UINT(3, import_json_unescape("abcdef", 6, dst, sizeof(dst)));
    TEST_ASSERT_EQUAL_STRING("abc", dst);
    /* NULL-tolerant. */
    TEST_ASSERT_EQUAL_UINT(0, import_json_unescape(NULL, 0, dst, sizeof(dst)));
    TEST_ASSERT_EQUAL_STRING("", dst);
    TEST_ASSERT_EQUAL_UINT(0, import_json_unescape("ab", 2, NULL, 0));
    /* A trailing lone backslash is literal. */
    TEST_ASSERT_EQUAL_UINT(2, import_json_unescape("a\\", 2, dst, sizeof(dst)));
    TEST_ASSERT_EQUAL_STRING("a\\", dst);
}

/* ========================================================================
 * iCalendar DATE-TIME parser
 * ======================================================================== */

void test_import_ics_datetime_full(void)
{
    struct tm tmv;

    TEST_ASSERT_TRUE(import_ics_datetime("20260915T093000", &tmv));
    TEST_ASSERT_EQUAL_INT(126, tmv.tm_year);
    TEST_ASSERT_EQUAL_INT(8, tmv.tm_mon);
    TEST_ASSERT_EQUAL_INT(15, tmv.tm_mday);
    TEST_ASSERT_EQUAL_INT(9, tmv.tm_hour);
    TEST_ASSERT_EQUAL_INT(30, tmv.tm_min);
    TEST_ASSERT_EQUAL_INT(0, tmv.tm_sec);
}

void test_import_ics_datetime_forms(void)
{
    struct tm tmv;

    /* Date-only defaults to midnight. */
    TEST_ASSERT_TRUE(import_ics_datetime("20240229", &tmv));
    TEST_ASSERT_EQUAL_INT(124, tmv.tm_year);
    TEST_ASSERT_EQUAL_INT(1, tmv.tm_mon);
    TEST_ASSERT_EQUAL_INT(29, tmv.tm_mday);
    TEST_ASSERT_EQUAL_INT(0, tmv.tm_hour);
    /* A trailing Z (UTC) is accepted as device-local (documented). */
    TEST_ASSERT_TRUE(import_ics_datetime("20260915T093000Z", &tmv));
    TEST_ASSERT_EQUAL_INT(9, tmv.tm_hour);
}

void test_import_ics_datetime_rejects(void)
{
    struct tm tmv;

    TEST_ASSERT_FALSE(import_ics_datetime("20260229T000000", &tmv)); /* not a leap year */
    TEST_ASSERT_FALSE(import_ics_datetime("20261301", &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime("20260015", &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime("20260900", &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime("20260932", &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime("20260915T240000", &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime("20260915T093061", &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime("2026-09-15", &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime("garbage", &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime("", &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime(NULL, &tmv));
    TEST_ASSERT_FALSE(import_ics_datetime("20260915T093000", NULL));
}

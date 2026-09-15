/**
 * @file test_archive.c
 * @brief Unit tests for the pure USTAR core (components/archive).
 *
 * Covers the CRC-32, octal fields, header format/parse round-trips, the
 * name/prefix fit rule, and the extract path-safety gate. Store I/O
 * (create/extract/list/verify round-trips) stays hardware-verified: it
 * needs the guarded SD sessions.
 *
 * Pure logic with no hardware dependency.
 */

#include "unity.h"
#include "archive.h"
#include <string.h>
#include <stdint.h>

/* ========================================================================
 * CRC-32 (IEEE 802.3 reference vector)
 * ======================================================================== */

void test_archive_crc32_reference(void)
{
    uint32_t crc = archive_crc32_update(0xFFFFFFFFu,
                                        (const uint8_t *)"123456789", 9);
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, crc ^ 0xFFFFFFFFu);
}

void test_archive_crc32_incremental(void)
{
    uint32_t whole = archive_crc32_update(0xFFFFFFFFu,
                                          (const uint8_t *)"hello world", 11);
    uint32_t split = archive_crc32_update(0xFFFFFFFFu,
                                          (const uint8_t *)"hello ", 6);
    split = archive_crc32_update(split, (const uint8_t *)"world", 5);
    TEST_ASSERT_EQUAL_UINT32(whole, split);
    /* Empty input and NULL data leave the accumulator untouched. */
    TEST_ASSERT_EQUAL_UINT32(whole, archive_crc32_update(whole, NULL, 10));
    TEST_ASSERT_EQUAL_UINT32(whole, archive_crc32_update(whole,
                                                         (const uint8_t *)"", 0));
}

/* ========================================================================
 * Octal fields
 * ======================================================================== */

void test_archive_octal_roundtrip(void)
{
    char field[12];
    uint64_t value = 0;

    archive_octal(0, field, sizeof(field));
    TEST_ASSERT_TRUE(archive_unoctal(field, sizeof(field), &value));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)value);
    archive_octal(12345, field, sizeof(field));
    TEST_ASSERT_TRUE(archive_unoctal(field, sizeof(field), &value));
    TEST_ASSERT_EQUAL_UINT32(12345, (uint32_t)value);
    archive_octal(0777, field, sizeof(field));
    TEST_ASSERT_TRUE(archive_unoctal(field, sizeof(field), &value));
    TEST_ASSERT_EQUAL_UINT32(0777, (uint32_t)value);
    /* Tar-style space-padded checksum field parses too. */
    TEST_ASSERT_TRUE(archive_unoctal("012345 \0", 8, &value));
    TEST_ASSERT_EQUAL_UINT32(012345, (uint32_t)value);
}

void test_archive_unoctal_rejects(void)
{
    uint64_t value = 42;

    TEST_ASSERT_FALSE(archive_unoctal("", 0, &value));
    TEST_ASSERT_FALSE(archive_unoctal(NULL, 8, &value));
    TEST_ASSERT_FALSE(archive_unoctal("0123", 4, NULL));
    TEST_ASSERT_FALSE(archive_unoctal("   ", 3, &value));
    TEST_ASSERT_EQUAL_UINT32(42, (uint32_t)value); /* untouched on failure */
}

/* ========================================================================
 * Header format / parse
 * ======================================================================== */

void test_archive_header_roundtrip(void)
{
    archive_entry_t in;
    archive_entry_t out;
    uint8_t block[512];

    memset(&in, 0, sizeof(in));
    snprintf(in.path, sizeof(in.path), "%s", "DOCS/NOTES.TXT");
    in.size = 12345;
    in.mtime = 1700000000LL;
    in.is_dir = false;
    archive_format_header(&in, block);
    TEST_ASSERT_EQUAL_INT(1, archive_parse_header(block, &out));
    TEST_ASSERT_EQUAL_STRING("DOCS/NOTES.TXT", out.path);
    TEST_ASSERT_EQUAL_UINT32(12345, (uint32_t)out.size);
    TEST_ASSERT_EQUAL_INT32(1700000000L, (int32_t)out.mtime);
    TEST_ASSERT_FALSE(out.is_dir);
}

void test_archive_header_dir_and_split(void)
{
    archive_entry_t in;
    archive_entry_t out;
    uint8_t block[512];
    char prefix[156];
    char name[51];

    /* Directory entries keep the trailing slash and typeflag 5. */
    memset(&in, 0, sizeof(in));
    snprintf(in.path, sizeof(in.path), "%s", "DOCS/");
    in.is_dir = true;
    archive_format_header(&in, block);
    TEST_ASSERT_EQUAL_INT('5', block[156]);
    TEST_ASSERT_EQUAL_INT(1, archive_parse_header(block, &out));
    TEST_ASSERT_EQUAL_STRING("DOCS/", out.path);
    TEST_ASSERT_TRUE(out.is_dir);

    /* A 155-char prefix + 50-char name splits and reassembles. */
    memset(prefix, 'p', 155);
    prefix[155] = '\0';
    memset(name, 'n', 50);
    name[50] = '\0';
    memset(&in, 0, sizeof(in));
    snprintf(in.path, sizeof(in.path), "%s/%s", prefix, name);
    archive_format_header(&in, block);
    TEST_ASSERT_EQUAL_INT(1, archive_parse_header(block, &out));
    TEST_ASSERT_EQUAL_STRING(in.path, out.path);
}

void test_archive_header_rejects(void)
{
    archive_entry_t out;
    uint8_t block[512];
    uint8_t zero[512];

    memset(zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_INT(0, archive_parse_header(zero, &out));
    /* Corrupt checksum. */
    memset(&block, 'A', sizeof(block));
    TEST_ASSERT_EQUAL_INT(-1, archive_parse_header(block, &out));
    TEST_ASSERT_EQUAL_INT(-1, archive_parse_header(NULL, &out));
    TEST_ASSERT_EQUAL_INT(-1, archive_parse_header(zero, NULL));
}

/* ========================================================================
 * Fit rule + path safety
 * ======================================================================== */

void test_archive_entry_fits(void)
{
    char long_ok[210];
    char too_long[300];

    TEST_ASSERT_TRUE(archive_entry_fits("A.TXT"));
    TEST_ASSERT_TRUE(archive_entry_fits("DOCS/NOTES.TXT"));
    memset(long_ok, 'a', 150);
    long_ok[150] = '\0';
    strcat(long_ok, "/");
    memset(long_ok + 151, 'b', 50);
    long_ok[201] = '\0';
    TEST_ASSERT_TRUE(archive_entry_fits(long_ok)); /* 150 + 50 split */
    memset(too_long, 'x', 260);
    too_long[260] = '\0';
    TEST_ASSERT_FALSE(archive_entry_fits(too_long)); /* no slash at all */
    TEST_ASSERT_FALSE(archive_entry_fits(NULL));
}

void test_archive_path_safe(void)
{
    TEST_ASSERT_TRUE(archive_path_safe("A.TXT"));
    TEST_ASSERT_TRUE(archive_path_safe("DOCS/NOTES.TXT"));
    TEST_ASSERT_TRUE(archive_path_safe("DOCS/"));
    TEST_ASSERT_TRUE(archive_path_safe("a/./b"));
    TEST_ASSERT_TRUE(archive_path_safe("a/..hidden/b"));
    TEST_ASSERT_FALSE(archive_path_safe("/ABS.TXT"));
    TEST_ASSERT_FALSE(archive_path_safe(".."));
    TEST_ASSERT_FALSE(archive_path_safe("../ESCAPE.TXT"));
    TEST_ASSERT_FALSE(archive_path_safe("DOCS/../../ESCAPE.TXT"));
    TEST_ASSERT_FALSE(archive_path_safe(""));
    TEST_ASSERT_FALSE(archive_path_safe(NULL));
}

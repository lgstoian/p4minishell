/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_asset.c
 * @brief Unit tests for the asset core (asset_commands.c pure helpers).
 *
 * Covers the one-shot CRC-32 (zlib parity vectors) and the manifest line
 * parser (comments, whitespace, hex, escapes). Verb file I/O stays
 * hardware-verified (crc32/asset check against pushed files).
 */

#include "unity.h"
#include "command.h"
#include <stdint.h>
#include <string.h>

void test_asset_crc32_vectors(void)
{
    /* Standard check value: "123456789" -> 0xCBF43926. */
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u,
        asset_crc32_data((const uint8_t *)"123456789", 9));
    /* Empty input inverts the init accumulator to 0. */
    TEST_ASSERT_EQUAL_UINT32(0x00000000u, asset_crc32_data((const uint8_t *)"", 0));
    TEST_ASSERT_EQUAL_UINT32(0x00000000u, asset_crc32_data(NULL, 0));
    /* Single byte 'A' (0x41) -> 0xD3D99E8B. */
    TEST_ASSERT_EQUAL_UINT32(0xD3D99E8Bu, asset_crc32_data((const uint8_t *)"A", 1));
}

void test_asset_parse_ok(void)
{
    char path[64];
    uint32_t crc = 0;

    TEST_ASSERT_TRUE(asset_parse_line("BOUNCE.BAT=1A2B3C4D", path, sizeof(path), &crc));
    TEST_ASSERT_EQUAL_STRING("BOUNCE.BAT", path);
    TEST_ASSERT_EQUAL_UINT32(0x1A2B3C4Du, crc);

    /* Lowercase hex + surrounding whitespace + subdir. */
    TEST_ASSERT_TRUE(asset_parse_line("  APPS/BOUNCE.APPINFO = cbf43926  ", path, sizeof(path), &crc));
    TEST_ASSERT_EQUAL_STRING("APPS/BOUNCE.APPINFO", path);
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, crc);
}

void test_asset_parse_skip(void)
{
    char path[64];
    uint32_t crc = 0xDEADu;

    /* Comments and blanks are "not entries" (false, outputs cleared). */
    TEST_ASSERT_FALSE(asset_parse_line("", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("   ", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("# comment", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("; ver=3", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line(NULL, path, sizeof(path), &crc));
    TEST_ASSERT_EQUAL_STRING("", path);
}

void test_asset_parse_bad(void)
{
    char path[64];
    uint32_t crc = 0;

    /* No '=', empty sides, short/long CRC, non-hex, escapes. */
    TEST_ASSERT_FALSE(asset_parse_line("NOEQUALS", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("=12345678", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("A.BAT=", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("A.BAT=1234567", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("A.BAT=123456789", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("A.BAT=ZZZZZZZZ", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("A.BAT=12345678 extra!", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("../EVIL.BAT=12345678", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("/abs.BAT=12345678", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line("A BAT.X=12345678", path, sizeof(path), &crc));
    TEST_ASSERT_FALSE(asset_parse_line(NULL, NULL, 0, NULL));
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_serial.c
 * @brief Unit tests for the pure BMP header writer.
 *
 * Covers screenshot_write_bmp_headers() from
 * components/command/serial_commands.c (promoted to command.h in v0.35.5
 * for testability; previously static). The byte-streaming itself needs
 * USB-Serial-JTAG hardware and stays board-verified.
 */

#include "unity.h"
#include "command.h"
#include <stdint.h>
#include <string.h>

static uint32_t read_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void test_serial_bmp_headers(void)
{
    uint8_t buf[64];
    uint32_t img;

    memset(buf, 0xAA, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(54, screenshot_write_bmp_headers(buf, 1024, 600));

    /* Magic + file size + pixel-data offset. */
    TEST_ASSERT_EQUAL_UINT8('B', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('M', buf[1]);
    TEST_ASSERT_EQUAL_UINT32(54 + 1024u * 3u * 600u, read_le32(&buf[2]));
    TEST_ASSERT_EQUAL_UINT32(54, read_le32(&buf[10]));

    /* Info header: size, dimensions, planes, bpp. */
    TEST_ASSERT_EQUAL_UINT32(40, read_le32(&buf[14]));
    TEST_ASSERT_EQUAL_UINT32(1024, read_le32(&buf[18]));
    TEST_ASSERT_EQUAL_UINT32(600, read_le32(&buf[22]));
    TEST_ASSERT_EQUAL_UINT32(1, buf[26] | ((uint32_t)buf[27] << 8));
    TEST_ASSERT_EQUAL_UINT32(24, buf[28] | ((uint32_t)buf[29] << 8));

    /* Image size + 96 DPI resolution. */
    img = 1024u * 3u * 600u;
    TEST_ASSERT_EQUAL_UINT32(img, read_le32(&buf[34]));
    TEST_ASSERT_EQUAL_UINT32(3780, read_le32(&buf[38]));
    TEST_ASSERT_EQUAL_UINT32(3780, read_le32(&buf[42]));
}

void test_serial_bmp_headers_small(void)
{
    uint8_t buf[64];

    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(54, screenshot_write_bmp_headers(buf, 1, 1));
    TEST_ASSERT_EQUAL_UINT32(54 + 3u, read_le32(&buf[2]));
    TEST_ASSERT_EQUAL_UINT32(1, read_le32(&buf[18]));
    TEST_ASSERT_EQUAL_UINT32(1, read_le32(&buf[22]));
}

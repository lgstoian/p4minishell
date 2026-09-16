/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_userial.c
 * @brief Unit tests for the pure USB CDC-ACM serial parsers (usb/userial.c).
 *
 * Covers the `<vid:pid>` device-id parser and the line-coding validator.
 * The driver/session itself is hardware-verified when a CDC-ACM device is
 * attached (USB serial adapter / microcontroller).
 */

#include "unity.h"
#include "usb.h"

#include <string.h>

void test_userial_parse_id_ok(void)
{
    uint16_t vid = 0;
    uint16_t pid = 0;

    TEST_ASSERT_TRUE(userial_parse_id("303a:1001", &vid, &pid));
    TEST_ASSERT_EQUAL_UINT16(0x303A, vid);
    TEST_ASSERT_EQUAL_UINT16(0x1001, pid);
    TEST_ASSERT_TRUE(userial_parse_id("ABCD:ef01", &vid, &pid));
    TEST_ASSERT_EQUAL_UINT16(0xABCD, vid);
    TEST_ASSERT_EQUAL_UINT16(0xEF01, pid);
    TEST_ASSERT_TRUE(userial_parse_id("1:2", &vid, &pid));
    TEST_ASSERT_EQUAL_UINT16(0x0001, vid);
    TEST_ASSERT_EQUAL_UINT16(0x0002, pid);
    TEST_ASSERT_TRUE(userial_parse_id("FFFF:FFFF", &vid, &pid));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, vid);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, pid);
}

void test_userial_parse_id_rejects(void)
{
    uint16_t vid = 0;
    uint16_t pid = 0;

    TEST_ASSERT_FALSE(userial_parse_id(NULL, &vid, &pid));
    TEST_ASSERT_FALSE(userial_parse_id("", &vid, &pid));
    TEST_ASSERT_FALSE(userial_parse_id("303a", &vid, &pid));
    TEST_ASSERT_FALSE(userial_parse_id(":1001", &vid, &pid));
    TEST_ASSERT_FALSE(userial_parse_id("303a:", &vid, &pid));
    TEST_ASSERT_FALSE(userial_parse_id("303a:1001:5", &vid, &pid));
    TEST_ASSERT_FALSE(userial_parse_id("303g:1001", &vid, &pid));
    TEST_ASSERT_FALSE(userial_parse_id("303a:xyz", &vid, &pid));
    TEST_ASSERT_FALSE(userial_parse_id("10001:1", &vid, &pid));
    TEST_ASSERT_FALSE(userial_parse_id("303a:1001", NULL, &pid));
    TEST_ASSERT_FALSE(userial_parse_id("303a:1001", &vid, NULL));
}

void test_userial_parse_coding_defaults(void)
{
    userial_coding_t coding;

    TEST_ASSERT_TRUE(userial_parse_coding(NULL, NULL, NULL, NULL, &coding));
    TEST_ASSERT_EQUAL_UINT32(115200, coding.baud);
    TEST_ASSERT_EQUAL_UINT8(8, coding.data_bits);
    TEST_ASSERT_EQUAL_UINT8(0, coding.parity);
    TEST_ASSERT_EQUAL_UINT8(1, coding.stop_bits);
}

void test_userial_parse_coding_values(void)
{
    userial_coding_t coding;

    TEST_ASSERT_TRUE(userial_parse_coding("9600", "7", "E", "2", &coding));
    TEST_ASSERT_EQUAL_UINT32(9600, coding.baud);
    TEST_ASSERT_EQUAL_UINT8(7, coding.data_bits);
    TEST_ASSERT_EQUAL_UINT8(2, coding.parity);
    TEST_ASSERT_EQUAL_UINT8(2, coding.stop_bits);
    /* Case-insensitive parity letters. */
    TEST_ASSERT_TRUE(userial_parse_coding(NULL, NULL, "o", NULL, &coding));
    TEST_ASSERT_EQUAL_UINT8(1, coding.parity);
    TEST_ASSERT_TRUE(userial_parse_coding(NULL, NULL, "n", NULL, &coding));
    TEST_ASSERT_EQUAL_UINT8(0, coding.parity);
}

void test_userial_parse_coding_rejects(void)
{
    userial_coding_t coding;

    TEST_ASSERT_FALSE(userial_parse_coding(NULL, NULL, NULL, NULL, NULL));
    TEST_ASSERT_FALSE(userial_parse_coding("0", NULL, NULL, NULL, &coding));
    TEST_ASSERT_FALSE(userial_parse_coding("99999999", NULL, NULL, NULL, &coding));
    TEST_ASSERT_FALSE(userial_parse_coding("abc", NULL, NULL, NULL, &coding));
    TEST_ASSERT_FALSE(userial_parse_coding(NULL, "9", NULL, NULL, &coding));
    TEST_ASSERT_FALSE(userial_parse_coding(NULL, "4", NULL, NULL, &coding));
    TEST_ASSERT_FALSE(userial_parse_coding(NULL, NULL, "X", NULL, &coding));
    TEST_ASSERT_FALSE(userial_parse_coding(NULL, NULL, "NN", NULL, &coding));
    TEST_ASSERT_FALSE(userial_parse_coding(NULL, NULL, NULL, "3", &coding));
    TEST_ASSERT_FALSE(userial_parse_coding(NULL, NULL, NULL, "0", &coding));
}

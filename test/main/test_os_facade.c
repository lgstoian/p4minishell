/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_os_facade.c
 * @brief Pure parser/formatter tests for the `os` diagnostic facade.
 *
 * The live verb fills fields from owner accessors (task count, board caps,
 * devices, heap, ...); the pure parse/format core is separated so the machine
 * contract is testable without SD, LVGL, FreeRTOS or a connected device. The
 * same core is compiled by tools/host_os_gate.py with mock snapshots.
 */

#include "unity.h"
#include "command.h"

#include <stddef.h>
#include <string.h>

static char *g_argv[8];

void test_os_parse_area_and_bare(void)
{
    os_area_t area = OS_AREA_INVALID;
    os_mode_t mode = OS_MODE_INVALID;
    bool bare = false;

    g_argv[0] = "os";
    g_argv[1] = "mem";
    g_argv[2] = "status";
    g_argv[3] = "/b";
    TEST_ASSERT_EQUAL_INT(0, command_os_parse(4, g_argv, &area, &mode, &bare));
    TEST_ASSERT_EQUAL_INT(OS_AREA_MEM, (int)area);
    TEST_ASSERT_EQUAL_INT(OS_MODE_STATUS, (int)mode);
    TEST_ASSERT_TRUE(bare);
}

void test_os_parse_case_insensitive(void)
{
    os_area_t area = OS_AREA_INVALID;
    os_mode_t mode = OS_MODE_STATUS;
    bool bare = true;

    g_argv[0] = "os";
    g_argv[1] = "PM";
    g_argv[2] = "LIST";
    TEST_ASSERT_EQUAL_INT(0, command_os_parse(3, g_argv, &area, &mode, &bare));
    TEST_ASSERT_EQUAL_INT(OS_AREA_PM, (int)area);
    TEST_ASSERT_EQUAL_INT(OS_MODE_LIST, (int)mode);
    TEST_ASSERT_FALSE(bare);
}

void test_os_parse_ver_no_mode(void)
{
    os_area_t area = OS_AREA_INVALID;
    os_mode_t mode = OS_MODE_LIST;
    bool bare = false;

    g_argv[0] = "os";
    g_argv[1] = "ver";
    g_argv[2] = "/b";
    TEST_ASSERT_EQUAL_INT(0, command_os_parse(3, g_argv, &area, &mode, &bare));
    TEST_ASSERT_EQUAL_INT(OS_AREA_VER, (int)area);
    TEST_ASSERT_EQUAL_INT(OS_MODE_STATUS, (int)mode);
    TEST_ASSERT_TRUE(bare);
}

void test_os_parse_pwr_status(void)
{
    os_area_t area = OS_AREA_INVALID;
    os_mode_t mode = OS_MODE_INVALID;
    bool bare = false;

    g_argv[0] = "os";
    g_argv[1] = "pwr";
    g_argv[2] = "status";
    g_argv[3] = "/b";
    TEST_ASSERT_EQUAL_INT(0, command_os_parse(4, g_argv, &area, &mode, &bare));
    TEST_ASSERT_EQUAL_INT(OS_AREA_PWR, (int)area);
    TEST_ASSERT_EQUAL_INT(OS_MODE_STATUS, (int)mode);
    TEST_ASSERT_TRUE(bare);
    TEST_ASSERT_TRUE(command_os_mode_allows(OS_AREA_PWR, OS_MODE_STATUS));
}

void test_os_pwr_rejects_list_and_free(void)
{
    g_argv[0] = "os";
    g_argv[1] = "pwr";
    g_argv[2] = "list";
    TEST_ASSERT_FALSE(command_os_mode_allows(OS_AREA_PWR, OS_MODE_LIST));
    TEST_ASSERT_FALSE(command_os_mode_allows(OS_AREA_PWR, OS_MODE_FREE));
}

void test_os_parse_rejects_unknown_area(void)
{
    os_area_t area = OS_AREA_PM;
    os_mode_t mode = OS_MODE_STATUS;
    bool bare = false;

    g_argv[0] = "os";
    g_argv[1] = "bogus";
    TEST_ASSERT_EQUAL_INT(2, command_os_parse(2, g_argv, &area, &mode, &bare));
    TEST_ASSERT_EQUAL_INT(OS_AREA_INVALID, (int)area);
}

void test_os_parse_rejects_bad_mode(void)
{
    os_area_t area = OS_AREA_PM;
    os_mode_t mode = OS_MODE_STATUS;
    bool bare = false;

    g_argv[0] = "os";
    g_argv[1] = "net";
    g_argv[2] = "list";
    TEST_ASSERT_EQUAL_INT(2, command_os_parse(3, g_argv, &area, &mode, &bare));
}

void test_os_parse_rejects_duplicate_mode(void)
{
    os_area_t area = OS_AREA_PM;
    os_mode_t mode = OS_MODE_STATUS;
    bool bare = false;

    g_argv[0] = "os";
    g_argv[1] = "pm";
    g_argv[2] = "status";
    g_argv[3] = "list";
    TEST_ASSERT_EQUAL_INT(2, command_os_parse(4, g_argv, &area, &mode, &bare));
}

void test_os_parse_rejects_missing_argv(void)
{
    os_area_t area = OS_AREA_PM;
    os_mode_t mode = OS_MODE_STATUS;
    bool bare = false;

    TEST_ASSERT_EQUAL_INT(2, command_os_parse(1, g_argv, &area, &mode, &bare));
    g_argv[0] = "os";
    g_argv[1] = NULL;
    TEST_ASSERT_EQUAL_INT(2, command_os_parse(2, g_argv, &area, &mode, &bare));
    TEST_ASSERT_EQUAL_INT(2, command_os_parse(2, g_argv, NULL, &mode, &bare));
}

void test_os_mode_allows_matrix(void)
{
    TEST_ASSERT_TRUE(command_os_mode_allows(OS_AREA_FS, OS_MODE_FREE));
    TEST_ASSERT_FALSE(command_os_mode_allows(OS_AREA_FS, OS_MODE_LIST));
    TEST_ASSERT_TRUE(command_os_mode_allows(OS_AREA_PM, OS_MODE_LIST));
    TEST_ASSERT_TRUE(command_os_mode_allows(OS_AREA_DISK, OS_MODE_LIST));
    TEST_ASSERT_FALSE(command_os_mode_allows(OS_AREA_VER, OS_MODE_LIST));
    TEST_ASSERT_FALSE(command_os_mode_allows(OS_AREA_NET, OS_MODE_FREE));
    TEST_ASSERT_FALSE(command_os_mode_allows(OS_AREA_INVALID, OS_MODE_STATUS));
    TEST_ASSERT_FALSE(command_os_mode_allows(OS_AREA_PM, OS_MODE_INVALID));
}

void test_os_area_names(void)
{
    TEST_ASSERT_EQUAL_STRING("pm", command_os_area_name(OS_AREA_PM));
    TEST_ASSERT_EQUAL_STRING("drv", command_os_area_name(OS_AREA_DRV));
    TEST_ASSERT_EQUAL_STRING("io", command_os_area_name(OS_AREA_IO));
    TEST_ASSERT_EQUAL_STRING("fs", command_os_area_name(OS_AREA_FS));
    TEST_ASSERT_EQUAL_STRING("net", command_os_area_name(OS_AREA_NET));
    TEST_ASSERT_EQUAL_STRING("mem", command_os_area_name(OS_AREA_MEM));
    TEST_ASSERT_EQUAL_STRING("disk", command_os_area_name(OS_AREA_DISK));
    TEST_ASSERT_EQUAL_STRING("sec", command_os_area_name(OS_AREA_SEC));
    TEST_ASSERT_EQUAL_STRING("pwr", command_os_area_name(OS_AREA_PWR));
    TEST_ASSERT_EQUAL_STRING("ver", command_os_area_name(OS_AREA_VER));
    TEST_ASSERT_EQUAL_STRING("invalid", command_os_area_name(OS_AREA_INVALID));
}

void test_os_status_names(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", command_os_status_name(true));
    TEST_ASSERT_EQUAL_STRING("unavailable", command_os_status_name(false));
}

void test_os_format_field_basic(void)
{
    char out[64];

    TEST_ASSERT_EQUAL_INT(strlen("os.pm.tasks=7"),
                          (int)command_os_format_field("os.pm.tasks", "7", true,
                                                       out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("os.pm.tasks=7", out);
}

void test_os_format_field_unavailable(void)
{
    char out[64];

    (void)command_os_format_field("os.net.wifi_state", "stale", false,
                                  out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("os.net.wifi_state=unavailable", out);
}

void test_os_format_field_null_value(void)
{
    char out[64];

    (void)command_os_format_field("os.io.userial", NULL, true, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("os.io.userial=unavailable", out);
}

void test_os_format_field_sanitizes(void)
{
    char out[64];

    (void)command_os_format_field("os.disk.usb", "a|b\nc\rd", true,
                                  out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("os.disk.usb=a/b c d", out);
}

void test_os_format_field_truncation_is_explicit(void)
{
    char out[32];

    (void)command_os_format_field("os.ver.build", "a very long build string",
                                  true, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("os.ver.build=unavailable", out);
}

void test_os_format_field_bad_args(void)
{
    char out[16];

    TEST_ASSERT_EQUAL_UINT(0, (unsigned)command_os_format_field(NULL, "x", true,
                                                                out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)command_os_format_field("k", "x", true,
                                                                NULL, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)command_os_format_field("k", "x", true,
                                                                out, 0));
}

void test_os_format_status_ok(void)
{
    char out[64];

    (void)command_os_format_status(OS_AREA_NET, true, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("os.net.status=ok", out);
}

void test_os_format_status_unavailable(void)
{
    char out[64];

    (void)command_os_format_status(OS_AREA_DISK, false, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("os.disk.status=unavailable", out);
}

void test_os_format_status_invalid_area(void)
{
    char out[64];

    (void)command_os_format_status(OS_AREA_INVALID, true, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("os.invalid.status=ok", out);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)command_os_format_status(OS_AREA_PM,
                                                                true, NULL, 0));
}

/**
 * @file test_tcpterm.c
 * @brief Unit tests for the pure tcpterm helpers (networking/tcpterm.c).
 *
 * Covers target validation, backslash-escape expansion, and reply
 * sanitization. The socket session itself is hardware-verified
 * (tools/tcpterm_test.py against the local httpd loopback).
 */

#include "unity.h"
#include "networking.h"

#include <string.h>

void test_tcp_parse_target_ok(void)
{
    int port = 0;

    TEST_ASSERT_TRUE(networking_tcp_parse_target("example.com", "80", &port));
    TEST_ASSERT_EQUAL_INT(80, port);
    TEST_ASSERT_TRUE(networking_tcp_parse_target("127.0.0.1", "65535", &port));
    TEST_ASSERT_EQUAL_INT(65535, port);
    TEST_ASSERT_TRUE(networking_tcp_parse_target("host", "1", &port));
    TEST_ASSERT_EQUAL_INT(1, port);
}

void test_tcp_parse_target_rejects(void)
{
    int port = 0;

    TEST_ASSERT_FALSE(networking_tcp_parse_target("", "80", &port));
    TEST_ASSERT_FALSE(networking_tcp_parse_target(NULL, "80", &port));
    TEST_ASSERT_FALSE(networking_tcp_parse_target("host", "0", &port));
    TEST_ASSERT_FALSE(networking_tcp_parse_target("host", "65536", &port));
    TEST_ASSERT_FALSE(networking_tcp_parse_target("host", "http", &port));
    TEST_ASSERT_FALSE(networking_tcp_parse_target("host", "80x", &port));
    TEST_ASSERT_FALSE(networking_tcp_parse_target("host", "", &port));
    TEST_ASSERT_FALSE(networking_tcp_parse_target("host", "80", NULL));
}

void test_tcp_unescape(void)
{
    char out[64];

    /* Source literals carry 2x backslashes; the unescaped output has one. */
    TEST_ASSERT_EQUAL_UINT(5, networking_tcp_unescape("a\\rb\\nc", out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("a\rb\nc", out, 5);
    TEST_ASSERT_EQUAL_UINT(3, networking_tcp_unescape("a\\tb", out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("a\tb", out, 3);
    TEST_ASSERT_EQUAL_UINT(3, networking_tcp_unescape("a\\\\b", out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("a\\b", out, 3);
    /* Unknown escapes pass through literally; trailing backslash stays. */
    TEST_ASSERT_EQUAL_UINT(4, networking_tcp_unescape("a\\xb", out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("a\\xb", out, 4);
    TEST_ASSERT_EQUAL_UINT(1, networking_tcp_unescape("\\", out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("\\", out, 1);
    TEST_ASSERT_EQUAL_UINT(0, networking_tcp_unescape(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0, networking_tcp_unescape("abc", NULL, 0));
}

void test_tcp_sanitize(void)
{
    const uint8_t mixed[] = {'H', 'i', 0x00, 0x1B, '[', '3', '2', 'm', '\n', 0x07, '\t'};
    char out[64];

    TEST_ASSERT_EQUAL_UINT(sizeof(mixed), networking_tcp_sanitize(mixed, sizeof(mixed),
                                                                 out, sizeof(out)));
    /* NUL and BEL become dots; ESC survives for SGR colours. */
    TEST_ASSERT_EQUAL_MEMORY("Hi.\x1B[32m\n.\t", out, sizeof(mixed));
    TEST_ASSERT_EQUAL_UINT(0, networking_tcp_sanitize(NULL, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT(0, networking_tcp_sanitize(mixed, 1, NULL, 0));
}

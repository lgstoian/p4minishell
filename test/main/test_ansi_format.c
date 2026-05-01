/**
 * @file test_ansi_format.c
 * @brief Unit tests for the ANSI format string builder.
 *
 * Tests ansi_format(), ansi_vformat(), and ansi_strip_to_plain()
 * from components/ansi/ansi.c.
 */

#include "unity.h"
#include "ansi.h"
#include <string.h>

/* ========================================================================
 * ANSI FORMAT BASIC TESTS
 * ======================================================================== */

void test_ansi_format_basic(void)
{
    char buf[256];
    int len;

    /* Initialize ANSI module */
    ansi_init();

    /* Plain text without format specifiers */
    len = ansi_format(buf, sizeof(buf), "hello world");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_STRING("hello world", buf);

    /* Reset specifier */
    len = ansi_format(buf, sizeof(buf), "@R");
    TEST_ASSERT_TRUE(len > 0);
    /* Should contain ESC[0m */
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[0m") != NULL);

    /* Bold specifier */
    len = ansi_format(buf, sizeof(buf), "@Bbold@R");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[1m") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[0m") != NULL);

    /* Literal @@ */
    len = ansi_format(buf, sizeof(buf), "email@@example.com");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_STRING("email@example.com", buf);
}

/* ========================================================================
 * ANSI FORMAT COLOR TESTS
 * ======================================================================== */

void test_ansi_format_colors(void)
{
    char buf[256];
    int len;

    ansi_init();

    /* Green text */
    len = ansi_format(buf, sizeof(buf), "@ggreen@R");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[32m") != NULL);  /* ANSI_FG_GREEN = 32 */

    /* Red text */
    len = ansi_format(buf, sizeof(buf), "@rred@R");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[31m") != NULL);  /* ANSI_FG_RED = 31 */

    /* Bright green */
    len = ansi_format(buf, sizeof(buf), "@Gbright@R");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[92m") != NULL);  /* ANSI_FG_BRIGHT_GREEN = 92 */

    /* Cyan */
    len = ansi_format(buf, sizeof(buf), "@ccyan@R");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[36m") != NULL);  /* ANSI_FG_CYAN = 36 */

    /* Yellow */
    len = ansi_format(buf, sizeof(buf), "@yyellow@R");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[33m") != NULL);  /* ANSI_FG_YELLOW = 33 */

    /* Multiple colors */
    len = ansi_format(buf, sizeof(buf), "@GOK@R @rFAIL@R");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[92m") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "\x1B[31m") != NULL);
}

/* ========================================================================
 * ANSI STRIP TO PLAIN TESTS
 * ======================================================================== */

void test_ansi_strip_to_plain(void)
{
    char buf[256];
    int len;

    ansi_init();

    /* Plain text passes through */
    len = ansi_strip_to_plain(buf, sizeof(buf), "hello world");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_STRING("hello world", buf);

    /* ANSI codes stripped */
    len = ansi_strip_to_plain(buf, sizeof(buf), "\x1B[32mgreen\x1B[0m");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_STRING("green", buf);

    /* Multiple ANSI codes */
    len = ansi_strip_to_plain(buf, sizeof(buf), "\x1B[92mOK\x1B[0m \x1B[31mFAIL\x1B[0m");
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_STRING("OK FAIL", buf);

    /* No ANSI codes */
    TEST_ASSERT_FALSE(ansi_contains_escapes("plain text"));
    TEST_ASSERT_TRUE(ansi_contains_escapes("\x1B[32mcolored\x1B[0m"));

    /* NULL safety */
    len = ansi_strip_to_plain(buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL(0, len);
    TEST_ASSERT_FALSE(ansi_contains_escapes(NULL));
}

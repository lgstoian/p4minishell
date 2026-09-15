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

/* ========================================================================
 * PRINTF WIDTH AND PRECISION FLAGS
 * ========================================================================
 * ansi_format() re-implements printf specifier handling so it can interleave
 * colour codes. It must honour flags, width, precision, and length modifiers
 * exactly as printf does: the shell relies on them for column alignment in
 * chkdsk, dir, and the capacity reports.
 */

void test_ansi_format_width_flags(void)
{
    char buf[128];
    char plain[128];

    /* Right-aligned string width. */
    ansi_format(buf, sizeof(buf), "[%8s]", "ab");
    ansi_strip_to_plain(plain, sizeof(plain), buf);
    TEST_ASSERT_EQUAL_STRING("[      ab]", plain);

    /* Left-aligned string width. */
    ansi_format(buf, sizeof(buf), "[%-8s]", "ab");
    ansi_strip_to_plain(plain, sizeof(plain), buf);
    TEST_ASSERT_EQUAL_STRING("[ab      ]", plain);

    /* Integer width and zero padding. */
    ansi_format(buf, sizeof(buf), "[%5d]", 42);
    ansi_strip_to_plain(plain, sizeof(plain), buf);
    TEST_ASSERT_EQUAL_STRING("[   42]", plain);

    ansi_format(buf, sizeof(buf), "[%05d]", 42);
    ansi_strip_to_plain(plain, sizeof(plain), buf);
    TEST_ASSERT_EQUAL_STRING("[00042]", plain);

    /* Unsigned width, as the allocation-unit report uses. */
    ansi_format(buf, sizeof(buf), "[%6u]", 123u);
    ansi_strip_to_plain(plain, sizeof(plain), buf);
    TEST_ASSERT_EQUAL_STRING("[   123]", plain);

    /* Float precision. */
    ansi_format(buf, sizeof(buf), "[%.2f]", 1.5);
    ansi_strip_to_plain(plain, sizeof(plain), buf);
    TEST_ASSERT_EQUAL_STRING("[1.50]", plain);

    /* Long modifier. */
    ansi_format(buf, sizeof(buf), "[%ld]", 1234567L);
    ansi_strip_to_plain(plain, sizeof(plain), buf);
    TEST_ASSERT_EQUAL_STRING("[1234567]", plain);

    /* Width combined with a colour code: the padding must be unaffected by
     * the escape bytes, which is the property the aligned reports depend on. */
    ansi_format(buf, sizeof(buf), "@c%8s@R", "ab");
    ansi_strip_to_plain(plain, sizeof(plain), buf);
    TEST_ASSERT_EQUAL_STRING("      ab", plain);

    /* Hex with width. */
    ansi_format(buf, sizeof(buf), "[%04x]", 0xABu);
    ansi_strip_to_plain(plain, sizeof(plain), buf);
    TEST_ASSERT_EQUAL_STRING("[00ab]", plain);
}

/* ========================================================================
 * ANSI -> LVGL RECOLOR MARKUP TESTS
 * ======================================================================== */

void test_ansi_to_lvgl_recolor(void)
{
    char buf[512];
    char out[512];
    int len;

    ansi_init();

    /* Plain text with no escapes is still wrapped in the default-foreground
     * recolor markup: the transcript label has no explicit text colour, so
     * `#CCCCCC text #` is what keeps the plain (uncoloured) runs visible on
     * the dark background. P4_CONFIG_ANSI_DEFAULT_FG = 0xCCCCCC. */
    len = ansi_to_lvgl_recolor("hello world", out, sizeof(out));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_STRING("#CCCCCC hello world #", out);

    /* A green segment becomes #rrggbb text # markup. ANSI green is
     * P4_CONFIG_ANSI_GREEN = 0x13A10E. */
    ansi_format(buf, sizeof(buf), "@ggreen@R");
    len = ansi_to_lvgl_recolor(buf, out, sizeof(out));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(out, "#13A10E") != NULL);
    TEST_ASSERT_TRUE(strstr(out, "green") != NULL);
    /* The markup must not contain a raw ESC byte. */
    TEST_ASSERT_NULL(strchr(out, '\x1B'));

    /* Two colours on one line produce two spans, each closed with ' #'. */
    ansi_format(buf, sizeof(buf), "@rred@R @ccyan@R");
    len = ansi_to_lvgl_recolor(buf, out, sizeof(out));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(out, "#C50F1F") != NULL);   /* ANSI_RED */
    TEST_ASSERT_TRUE(strstr(out, "#3A96DD") != NULL);   /* ANSI_CYAN */
    TEST_ASSERT_TRUE(strstr(out, "red") != NULL);
    TEST_ASSERT_TRUE(strstr(out, "cyan") != NULL);

    /* NULL safety. */
    TEST_ASSERT_EQUAL(0, ansi_to_lvgl_recolor(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0, ansi_to_lvgl_recolor("x", NULL, sizeof(out)));
    TEST_ASSERT_EQUAL(0, ansi_to_lvgl_recolor("x", out, 0));

    /* A too-small output buffer must not leave a dangling '#' (which would
     * make LVGL render colour codes literally). The result, if any, must be a
     * closed span. */
    ansi_format(buf, sizeof(buf), "@ggreen@R text");
    ansi_to_lvgl_recolor(buf, out, 8);
    TEST_ASSERT_TRUE(strlen(out) < 8);
    TEST_ASSERT_TRUE(strchr(out, '#') == NULL || strrchr(out, '#') != strchr(out, '#'));
}

/* ========================================================================
 * CSI SPLIT-SEQUENCE TESTS (ansi_csi_trailing, VT100 stream pump)
 * ======================================================================== */

void test_ansi_csi_trailing_complete(void)
{
    /* Plain text and terminated sequences hold nothing back. */
    TEST_ASSERT_EQUAL_UINT(0, ansi_csi_trailing((const uint8_t *)"hello", 5));
    TEST_ASSERT_EQUAL_UINT(0, ansi_csi_trailing((const uint8_t *)"\x1B[31mhi", 7));
    TEST_ASSERT_EQUAL_UINT(0, ansi_csi_trailing((const uint8_t *)"\x1B[2J", 4));
    TEST_ASSERT_EQUAL_UINT(0, ansi_csi_trailing((const uint8_t *)"\x1B[?25l", 6));
    TEST_ASSERT_EQUAL_UINT(0, ansi_csi_trailing(NULL, 0));
    TEST_ASSERT_EQUAL_UINT(0, ansi_csi_trailing((const uint8_t *)"", 0));
    TEST_ASSERT_EQUAL_UINT(0, ansi_csi_trailing(NULL, 5));
}

void test_ansi_csi_trailing_split(void)
{
    /* A lone trailing ESC is always incomplete. */
    TEST_ASSERT_EQUAL_UINT(1, ansi_csi_trailing((const uint8_t *)"hi\x1B", 3));
    /* Dangling CSI openers and parameter runs are held back whole. */
    TEST_ASSERT_EQUAL_UINT(2, ansi_csi_trailing((const uint8_t *)"hi\x1B[", 4));
    TEST_ASSERT_EQUAL_UINT(3, ansi_csi_trailing((const uint8_t *)"hi\x1B[3", 5));
    TEST_ASSERT_EQUAL_UINT(4, ansi_csi_trailing((const uint8_t *)"hi\x1B[31", 6));
    TEST_ASSERT_EQUAL_UINT(3, ansi_csi_trailing((const uint8_t *)"\x1B[?", 3));
    /* A completed sequence earlier in the buffer does not confuse it. */
    TEST_ASSERT_EQUAL_UINT(3, ansi_csi_trailing((const uint8_t *)"\x1B[0m\x1B[3", 7));
    /* Bare ESC + non-'[' is complete (single-char escape or plain text). */
    TEST_ASSERT_EQUAL_UINT(0, ansi_csi_trailing((const uint8_t *)"hi\x1BM", 4));
}
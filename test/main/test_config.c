/**
 * @file test_config.c
 * @brief Unit tests for the pure CONFIG.SYS directive line-editing helpers.
 *
 * Covers config_directive_get/upsert/remove from components/command/config_cmd.c.
 * These are pure text functions with no hardware or filesystem dependency.
 */

#include "unity.h"
#include "config_cmd.h"
#include <string.h>

/* ========================================================================
 * GET
 * ======================================================================== */

void test_config_get_simple(void)
{
    const char *text = "; header\nBRIGHTNESS=40\nVOLUME=70\n";
    char value[64] = {0};

    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "BRIGHTNESS", value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("40", value);
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "VOLUME", value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("70", value);
}

void test_config_get_case_and_spacing(void)
{
    const char *text = "  brightness = 40  \nPROMPT=PS $p$g\n";
    char value[128] = {0};

    /* Key is case-insensitive; spaces around '=' are trimmed. */
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "BRIGHTNESS", value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("40", value);
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "brightness", value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("40", value);
    TEST_ASSERT_EQUAL_INT(7, config_directive_get(text, "prompt", value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("PS $p$g", value);
}

void test_config_get_comments_and_absent(void)
{
    const char *text = "; BRIGHTNESS=99\nREM ROTATE=90\nBRIGHTNESS=40\n";
    char value[64] = {0};

    /* Commented/REM lines are not directives. */
    TEST_ASSERT_EQUAL_INT(-1, config_directive_get(text, "ROTATE", value, sizeof(value)));
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "BRIGHTNESS", value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("40", value);

    TEST_ASSERT_EQUAL_INT(-1, config_directive_get(text, "MISSING", value, sizeof(value)));
    TEST_ASSERT_EQUAL_INT(-1, config_directive_get("", "BRIGHTNESS", value, sizeof(value)));
}

void test_config_get_prefix_does_not_match(void)
{
    const char *text = "BRIGHTNESS=40\n";
    char value[64] = {0};

    /* "BRIGHTNESSX" must not match "BRIGHTNESS". */
    TEST_ASSERT_EQUAL_INT(-1, config_directive_get(text, "BRIGHTNESSX", value, sizeof(value)));
}

/* ========================================================================
 * UPSERT
 * ======================================================================== */

void test_config_upsert_replaces_existing(void)
{
    char text[256] = "; header\nBRIGHTNESS=99\nVOLUME=70\n";

    TEST_ASSERT_TRUE(config_directive_upsert(text, sizeof(text), "BRIGHTNESS", "40"));
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "BRIGHTNESS", NULL, 0));
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "VOLUME", NULL, 0));
    /* The header comment survives. */
    TEST_ASSERT_NOT_NULL(strstr(text, "; header"));
}

void test_config_upsert_appends_when_absent(void)
{
    char text[256] = "; header\n";

    TEST_ASSERT_TRUE(config_directive_upsert(text, sizeof(text), "OSK", "OFF"));
    TEST_ASSERT_EQUAL_INT(3, config_directive_get(text, "OSK", NULL, 0));
    TEST_ASSERT_NOT_NULL(strstr(text, "OSK=OFF"));
}

void test_config_upsert_dedupes_multiple_lines(void)
{
    char text[256] = "BRIGHTNESS=10\nBRIGHTNESS=20\nVOLUME=70\n";

    TEST_ASSERT_TRUE(config_directive_upsert(text, sizeof(text), "BRIGHTNESS", "30"));
    /* Only one BRIGHTNESS line remains, holding the new value. */
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "BRIGHTNESS", NULL, 0));
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "VOLUME", NULL, 0));
}

void test_config_upsert_into_empty(void)
{
    char text[8] = "";

    TEST_ASSERT_TRUE(config_directive_upsert(text, sizeof(text), "OSK", "ON"));
    TEST_ASSERT_EQUAL_STRING("OSK=ON\n", text);
}

void test_config_upsert_overflow_refused(void)
{
    char text[32] = "BRIGHTNESS=99\n";

    TEST_ASSERT_FALSE(config_directive_upsert(text, sizeof(text), "PROMPT",
                                              "this value is far too long to ever fit"));
}

/* ========================================================================
 * REMOVE
 * ======================================================================== */

void test_config_remove_existing(void)
{
    char text[256] = "; header\nBRIGHTNESS=40\nVOLUME=70\n";

    TEST_ASSERT_TRUE(config_directive_remove(text, sizeof(text), "BRIGHTNESS"));
    TEST_ASSERT_EQUAL_INT(-1, config_directive_get(text, "BRIGHTNESS", NULL, 0));
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "VOLUME", NULL, 0));
    TEST_ASSERT_NOT_NULL(strstr(text, "; header"));
}

void test_config_remove_absent(void)
{
    char text[256] = "VOLUME=70\n";

    TEST_ASSERT_FALSE(config_directive_remove(text, sizeof(text), "BRIGHTNESS"));
    TEST_ASSERT_EQUAL_STRING("VOLUME=70\n", text);
}

void test_config_remove_multiple(void)
{
    char text[256] = "BRIGHTNESS=10\nVOLUME=70\nBRIGHTNESS=20\n";

    TEST_ASSERT_TRUE(config_directive_remove(text, sizeof(text), "BRIGHTNESS"));
    TEST_ASSERT_EQUAL_INT(-1, config_directive_get(text, "BRIGHTNESS", NULL, 0));
    TEST_ASSERT_EQUAL_INT(2, config_directive_get(text, "VOLUME", NULL, 0));
}

void test_config_remove_last_line_no_newline(void)
{
    char text[256] = "VOLUME=70\nBRIGHTNESS=40";

    TEST_ASSERT_TRUE(config_directive_remove(text, sizeof(text), "BRIGHTNESS"));
    TEST_ASSERT_EQUAL_STRING("VOLUME=70\n", text);
}

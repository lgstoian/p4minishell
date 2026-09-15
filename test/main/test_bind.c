/**
 * @file test_bind.c
 * @brief Unit tests for the key bind table (components/batch/batch.c).
 *
 * Covers shell_bind_set/lookup_fkey/lookup_chord/count/get_by_index:
 * F-key and Ctrl+letter chord validation, set/overwrite/clear, lookup
 * hits and misses, and index enumeration. The table is RAM-only and the
 * runner calls batch_init() before any suite; each test clears the slots
 * it uses.
 */

#include "unity.h"
#include "batch.h"

#include <string.h>

static void bind_clear_all(void)
{
    uint8_t code;
    for (code = 0x3A; code <= 0x45; code++) {
        (void)shell_bind_set(code, "");
    }
    for (code = 0x84; code <= 0x9D; code++) {
        (void)shell_bind_set(code, "");
    }
}

void test_bind_set_and_lookup(void)
{
    char out[64];

    bind_clear_all();
    TEST_ASSERT_EQUAL_INT(0, shell_bind_count());
    TEST_ASSERT_EQUAL_INT(0, shell_bind_set(0x3E, "launch tcmd"));
    TEST_ASSERT_EQUAL_INT(1, shell_bind_count());
    TEST_ASSERT_TRUE(shell_bind_lookup_fkey(0x3E, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("launch tcmd", out);
    /* Misses: unbound key, non-F-key code, null/short buffers. */
    TEST_ASSERT_FALSE(shell_bind_lookup_fkey(0x3F, out, sizeof(out)));
    TEST_ASSERT_FALSE(shell_bind_lookup_fkey(0x04, out, sizeof(out)));
    TEST_ASSERT_FALSE(shell_bind_lookup_fkey(0x3E, NULL, 0));
    /* Overwrite + clear. */
    TEST_ASSERT_EQUAL_INT(0, shell_bind_set(0x3E, "sysinfo"));
    TEST_ASSERT_TRUE(shell_bind_lookup_fkey(0x3E, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("sysinfo", out);
    TEST_ASSERT_EQUAL_INT(0, shell_bind_set(0x3E, ""));
    TEST_ASSERT_EQUAL_INT(0, shell_bind_count());
    TEST_ASSERT_FALSE(shell_bind_lookup_fkey(0x3E, out, sizeof(out)));
    bind_clear_all();
}

void test_bind_rejects_non_fkey(void)
{
    TEST_ASSERT_TRUE(shell_bind_set(0x04, "x") != 0);
    TEST_ASSERT_TRUE(shell_bind_set(0x39, "x") != 0);
    TEST_ASSERT_TRUE(shell_bind_set(0x46, "x") != 0);
    TEST_ASSERT_EQUAL_INT(0, shell_bind_count());
    bind_clear_all();
}

void test_bind_get_by_index(void)
{
    uint8_t code = 0;
    char value[64];

    bind_clear_all();
    TEST_ASSERT_FALSE(shell_bind_get_by_index(0, &code, value, sizeof(value)));
    TEST_ASSERT_EQUAL_INT(0, shell_bind_set(0x3A, "first"));
    TEST_ASSERT_EQUAL_INT(0, shell_bind_set(0x45, "last"));
    TEST_ASSERT_TRUE(shell_bind_get_by_index(0, &code, value, sizeof(value)));
    TEST_ASSERT_EQUAL_UINT8(0x3A, code);
    TEST_ASSERT_EQUAL_STRING("first", value);
    TEST_ASSERT_TRUE(shell_bind_get_by_index(1, &code, value, sizeof(value)));
    TEST_ASSERT_EQUAL_UINT8(0x45, code);
    TEST_ASSERT_EQUAL_STRING("last", value);
    TEST_ASSERT_FALSE(shell_bind_get_by_index(2, &code, value, sizeof(value)));
    TEST_ASSERT_FALSE(shell_bind_get_by_index(0, NULL, value, sizeof(value)));
    bind_clear_all();
}

void test_bind_chord_set_and_lookup(void)
{
    char out[64];
    uint8_t code = 0;

    bind_clear_all();
    /* ^G is HID 0x0A with the chord flag. */
    TEST_ASSERT_EQUAL_INT(0, shell_bind_set(0x8A, "macro play g.bat"));
    TEST_ASSERT_EQUAL_INT(1, shell_bind_count());
    /* Ctrl+G fires; missing Ctrl, wrong letter, and ^C never do. */
    TEST_ASSERT_TRUE(shell_bind_lookup_chord(0x0A, 0x01, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("macro play g.bat", out);
    TEST_ASSERT_TRUE(shell_bind_lookup_chord(0x0A, 0x10, out, sizeof(out)));
    TEST_ASSERT_FALSE(shell_bind_lookup_chord(0x0A, 0x00, out, sizeof(out)));
    TEST_ASSERT_FALSE(shell_bind_lookup_chord(0x0B, 0x01, out, sizeof(out)));
    TEST_ASSERT_FALSE(shell_bind_lookup_chord(0x06, 0x01, out, sizeof(out)));
    TEST_ASSERT_FALSE(shell_bind_lookup_chord(0x0A, 0x01, NULL, 0));
    /* Index enumeration reports the chord code back. */
    TEST_ASSERT_TRUE(shell_bind_get_by_index(0, &code, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8(0x8A, code);
    /* Clear via the chord code. */
    TEST_ASSERT_EQUAL_INT(0, shell_bind_set(0x8A, ""));
    TEST_ASSERT_EQUAL_INT(0, shell_bind_count());
    TEST_ASSERT_FALSE(shell_bind_lookup_chord(0x0A, 0x01, out, sizeof(out)));
    bind_clear_all();
}

void test_bind_chord_rejects(void)
{
    /* ^C is reserved for break; bare letters and out-of-range chords fail. */
    TEST_ASSERT_TRUE(shell_bind_set(0x86, "x") != 0);
    TEST_ASSERT_TRUE(shell_bind_set(0x04, "x") != 0);
    TEST_ASSERT_TRUE(shell_bind_set(0x80, "x") != 0);
    TEST_ASSERT_TRUE(shell_bind_set(0x9E, "x") != 0);
    TEST_ASSERT_EQUAL_INT(0, shell_bind_count());
    bind_clear_all();
}

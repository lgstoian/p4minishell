/**
 * @file test_comp.c
 * @brief Unit tests for the pure `comp` compare helper.
 *
 * Covers shell_comp_first_diff() from components/storage/storage_commands.c,
 * the byte-comparison core behind the classic DOS `comp` command added in
 * v0.24.27. It is pure (no I/O) so it is tested directly.
 */

#include "unity.h"
#include "storage_commands.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

void test_comp_identical(void)
{
    const uint8_t a[] = "hello";
    const uint8_t b[] = "hello";
    size_t pos = 0;
    uint8_t va = 0;
    uint8_t vb = 0;

    TEST_ASSERT_FALSE(shell_comp_first_diff(a, 5, b, 5, false, &pos, &va, &vb));

    /* Empty buffers compare equal. */
    TEST_ASSERT_FALSE(shell_comp_first_diff(NULL, 0, NULL, 0, false, &pos, &va, &vb));
}

void test_comp_byte_difference(void)
{
    const uint8_t a[] = "hello";
    const uint8_t b[] = "hallo";
    size_t pos = 0;
    uint8_t va = 0;
    uint8_t vb = 0;

    TEST_ASSERT_TRUE(shell_comp_first_diff(a, 5, b, 5, false, &pos, &va, &vb));
    TEST_ASSERT_EQUAL_UINT32(1, pos);
    TEST_ASSERT_EQUAL_UINT8('e', va);
    TEST_ASSERT_EQUAL_UINT8('a', vb);
}

void test_comp_length_difference(void)
{
    const uint8_t a[] = "hello";
    const uint8_t b[] = "hello world";
    size_t pos = 0;
    uint8_t va = 0;
    uint8_t vb = 0;

    /* The shorter side reads as 0x00 for the missing byte. */
    TEST_ASSERT_TRUE(shell_comp_first_diff(a, 5, b, 11, false, &pos, &va, &vb));
    TEST_ASSERT_EQUAL_UINT32(5, pos);
    TEST_ASSERT_EQUAL_UINT8(0, va);
    TEST_ASSERT_EQUAL_UINT8(' ', vb);
}

void test_comp_case(void)
{
    const uint8_t a[] = "Hello";
    const uint8_t b[] = "hello";
    size_t pos = 0;
    uint8_t va = 0;
    uint8_t vb = 0;

    /* Case-sensitive: the first byte differs. */
    TEST_ASSERT_TRUE(shell_comp_first_diff(a, 5, b, 5, false, &pos, &va, &vb));
    TEST_ASSERT_EQUAL_UINT32(0, pos);

    /* /C ignores case. */
    TEST_ASSERT_FALSE(shell_comp_first_diff(a, 5, b, 5, true, &pos, &va, &vb));
}

void test_comp_mid_buffer_difference(void)
{
    const uint8_t a[] = { 1, 2, 3, 4, 5 };
    const uint8_t b[] = { 1, 2, 9, 4, 5 };
    size_t pos = 0;
    uint8_t va = 0;
    uint8_t vb = 0;

    TEST_ASSERT_TRUE(shell_comp_first_diff(a, 5, b, 5, false, &pos, &va, &vb));
    TEST_ASSERT_EQUAL_UINT32(2, pos);
    TEST_ASSERT_EQUAL_UINT8(3, va);
    TEST_ASSERT_EQUAL_UINT8(9, vb);
}

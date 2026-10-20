/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_error_strings.c
 * @brief Centralized error-code -> message table unit tests.
 */

#include "unity.h"
#include "error_strings.h"

#include <string.h>

void test_error_code_string_known(void)
{
    TEST_ASSERT_EQUAL_STRING("Success", error_code_string(ERR_BASE_OK));
    TEST_ASSERT_NOT_NULL(strstr(error_code_string(ERR_BASE_USAGE), "usage"));
    TEST_ASSERT_NOT_NULL(strstr(error_code_string(ERR_BASE_NOENT), "not found"));
    TEST_ASSERT_NOT_NULL(strstr(error_code_string(ERR_STORAGE_NO_SD), "SD"));
}

void test_error_code_string_unknown(void)
{
    TEST_ASSERT_EQUAL_STRING("Unknown error", error_code_string(99999));
    TEST_ASSERT_EQUAL_STRING("Unknown error", error_code_string(-1));
}

void test_error_code_string_never_null(void)
{
    TEST_ASSERT_NOT_NULL(error_code_string(0));
    TEST_ASSERT_NOT_NULL(error_code_string(ERR_NET_TLS));
    TEST_ASSERT_NOT_NULL(error_code_string(ERR_APP_SIGN));
    TEST_ASSERT_NOT_NULL(error_code_string(ERR_HW_AUDIO));
    TEST_ASSERT_GREATER_THAN(20, (int)error_code_count());
}

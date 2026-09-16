/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_shell_variables.c
 * @brief Unit tests for shell variable expansion (shell_expand_variables).
 *
 * Tests the batch module's variable expansion engine: %VAR% environment
 * variables, %0/%1..%9/%* batch arguments, %% escaping, single-quote
 * protection, and caret-escape passthrough.
 */

#include "unity.h"
#include "batch.h"
#include <string.h>

/* ========================================================================
 * BASIC VARIABLE EXPANSION
 * ======================================================================== */

void test_variable_expansion_env_var(void)
{
    char out[256];

    /* Set a test variable, expand it, verify. */
    shell_env_set("MYVAR", "hello");
    shell_expand_variables("echo %MYVAR%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo hello", out);

    /* Undefined variable expands to the empty string (cmd.exe parity), so the
     * DOS `if "%var%"==""` idiom works for unset variables. */
    shell_expand_variables("echo %UNDEFINED%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo ", out);
}

void test_variable_expansion_empty_name(void)
{
    char out[256];

    /* %% → literal % */
    shell_expand_variables("echo %%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo %", out);

    /* % with no closing % → literal % */
    shell_expand_variables("echo %abc", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo %abc", out);
}

void test_variable_expansion_single_quotes(void)
{
    char out[256];

    shell_env_set("MYVAR", "replaced");
    /* Inside single quotes, %VAR% is literal. */
    shell_expand_variables("echo '%MYVAR%'", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo '%MYVAR%'", out);

    /* Inside double quotes, %VAR% IS expanded. */
    shell_expand_variables("echo \"%MYVAR%\"", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo \"replaced\"", out);
}

void test_variable_expansion_caret_escape(void)
{
    char out[256];

    /* ^% passes both characters through (caret preserved in output,
     * consumed by unescape later); the following %VAR% still expands.
     * Since v0.33.0 an undefined variable expands to empty (cmd.exe). */
    shell_env_set("VAR", "vval");
    shell_expand_variables("echo ^%%VAR%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo ^%vval", out);
    shell_expand_variables("echo ^%%UNSET_XYZ%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo ^%", out);
}

void test_variable_expansion_multiple(void)
{
    char out[256];

    shell_env_set("A", "foo");
    shell_env_set("B", "bar");
    shell_expand_variables("%A% %B%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("foo bar", out);
}

/* ========================================================================
 * %ERRORLEVEL% (batch process exit code)
 * ======================================================================== */

void test_variable_expansion_errorlevel(void)
{
    char out[256];
    int saved = batch_get_errorlevel();

    batch_set_errorlevel(7);
    shell_expand_variables("echo %ERRORLEVEL%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo 7", out);

    /* Case-insensitive, and usable mid-line like any %VAR%. */
    batch_set_errorlevel(42);
    shell_expand_variables("code=%errorlevel% done", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("code=42 done", out);

    /* %% still yields a literal %. */
    batch_set_errorlevel(3);
    shell_expand_variables("echo %% and %ERRORLEVEL%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo % and 3", out);

    /* Restore so other suites are unaffected. */
    batch_set_errorlevel(saved);
}

/* ========================================================================
 * BUFFER BOUNDS
 * ======================================================================== */

void test_variable_expansion_output_truncated(void)
{
    char out[8];
    shell_env_set("LONG", "abcdefghijklmnopqrstuvwxyz");
    shell_expand_variables("%LONG%", out, sizeof(out));
    /* Should be truncated to fit the buffer. */
    TEST_ASSERT_LESS_THAN(8, strlen(out));
}

/* ========================================================================
 * DYNAMIC PSEUDO-VARIABLES (%DATE% %TIME% %RANDOM% %CD%)
 * ======================================================================== */

void test_variable_expansion_pseudo_vars(void)
{
    char out[256];
    size_t i;

    /* %RANDOM% expands to a 0..32767 decimal integer, never the literal. */
    shell_expand_variables("x%RANDOM%y", out, sizeof(out));
    TEST_ASSERT_TRUE(strstr(out, "%RANDOM%") == NULL);
    TEST_ASSERT_TRUE(strlen(out) >= 3);   /* "x" + at least one digit + "y" */
    for (i = 1; i + 1 < strlen(out); i++) {
        TEST_ASSERT_TRUE(out[i] >= '0' && out[i] <= '9');
    }

    /* %CD% expands to the working directory (possibly empty), never literal. */
    shell_expand_variables("%CD%", out, sizeof(out));
    TEST_ASSERT_TRUE(strstr(out, "%CD%") == NULL);

    /* %DATE% -> MM-DD-YYYY, %TIME% -> HH:MM:SS, never literal. */
    shell_expand_variables("%DATE%", out, sizeof(out));
    TEST_ASSERT_TRUE(strstr(out, "%DATE%") == NULL);
    TEST_ASSERT_TRUE(strlen(out) == 10);
    TEST_ASSERT_TRUE(out[2] == '-' && out[5] == '-');

    shell_expand_variables("%TIME%", out, sizeof(out));
    TEST_ASSERT_TRUE(strstr(out, "%TIME%") == NULL);
    TEST_ASSERT_TRUE(strlen(out) == 8);
    TEST_ASSERT_TRUE(out[2] == ':' && out[5] == ':');
}

/* ========================================================================
 * NULL SAFETY
 * ======================================================================== */

void test_variable_expansion_null_input(void)
{
    char out[32];
    shell_expand_variables(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_variable_expansion_null_output(void)
{
    /* Should not crash. */
    shell_expand_variables("echo %VAR%", NULL, 0);
}

/* ========================================================================
 * BATCH ARGUMENTS (requires active batch frame)
 * ======================================================================== */

void test_variable_expansion_no_batch_frame(void)
{
    char out[256];

    /* Without an active batch frame, %0, %1, %* expand to empty. */
    shell_expand_variables("echo %1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo ", out);

    shell_expand_variables("echo %0", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo ", out);

    shell_expand_variables("echo %*", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo ", out);
}

void test_variable_expansion_tilde_modifiers(void)
{
    char out[256];

    /* Without an active batch frame, %~N reads the same empty slot as %N. */
    shell_expand_variables("echo %~1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo ", out);
    shell_expand_variables("echo %~nx1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo ", out);

    /* Unknown modifiers and a missing digit stay literal (never inject). */
    shell_expand_variables("echo %~z1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo %~z1", out);
    shell_expand_variables("echo %~", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo %~", out);
    shell_expand_variables("echo %~f", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo %~f", out);
}

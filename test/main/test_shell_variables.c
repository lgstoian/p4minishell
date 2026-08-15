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

    /* Undefined variable is left untouched (not expanded to empty) — the
     * literal %UNDEFINED% passes through so the user can see it was not
     * substituted. This matches the documented "unknown names left untouched"
     * rule in batch.h. */
    shell_expand_variables("echo %UNDEFINED%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo %UNDEFINED%", out);
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

    /* ^% → literal % (caret preserved in output, consumed by unescape later). */
    shell_expand_variables("echo ^%%VAR%", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo ^%%VAR%", out);
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

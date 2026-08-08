/**
 * @file test_shell_prompt.c
 * @brief Unit tests for the runtime DOS prompt template engine.
 *
 * Tests shell_prompt_set_template(), shell_prompt_get_template(),
 * shell_prompt_render_plain(), and shell_prompt_reset() from
 * components/shell/shell.c.
 *
 * These run without hardware: the renderer reads the current working
 * directory through the command operations table, which is unset in the test
 * binary, so the shell core falls back to the SD mount point.
 */

#include "unity.h"
#include "shell.h"
#include "p4minishell_config.h"
#include <string.h>

/* ========================================================================
 * TEMPLATE STORAGE
 * ======================================================================== */

void test_shell_prompt_template_roundtrip(void)
{
    shell_prompt_reset();
    TEST_ASSERT_EQUAL_STRING(P4_CONFIG_PROMPT_DEFAULT_TEMPLATE, shell_prompt_get_template());

    shell_prompt_set_template("$p$g ");
    TEST_ASSERT_EQUAL_STRING("$p$g ", shell_prompt_get_template());

    /* An empty or NULL template restores the configured default. */
    shell_prompt_set_template("");
    TEST_ASSERT_EQUAL_STRING(P4_CONFIG_PROMPT_DEFAULT_TEMPLATE, shell_prompt_get_template());

    shell_prompt_set_template("$p$g ");
    shell_prompt_set_template(NULL);
    TEST_ASSERT_EQUAL_STRING(P4_CONFIG_PROMPT_DEFAULT_TEMPLATE, shell_prompt_get_template());

    shell_prompt_reset();
}

/* ========================================================================
 * METACHARACTER EXPANSION
 * ======================================================================== */

void test_shell_prompt_metacharacters(void)
{
    const char *rendered;

    /* Literal text passes through untouched. */
    shell_prompt_set_template("ready");
    TEST_ASSERT_EQUAL_STRING("ready", shell_prompt_render_plain());

    /* Single-character substitutions. */
    shell_prompt_set_template("$g");
    TEST_ASSERT_EQUAL_STRING(">", shell_prompt_render_plain());

    shell_prompt_set_template("$l");
    TEST_ASSERT_EQUAL_STRING("<", shell_prompt_render_plain());

    shell_prompt_set_template("$b");
    TEST_ASSERT_EQUAL_STRING("|", shell_prompt_render_plain());

    shell_prompt_set_template("$q");
    TEST_ASSERT_EQUAL_STRING("=", shell_prompt_render_plain());

    shell_prompt_set_template("$a");
    TEST_ASSERT_EQUAL_STRING("&", shell_prompt_render_plain());

    shell_prompt_set_template("$c$f");
    TEST_ASSERT_EQUAL_STRING("()", shell_prompt_render_plain());

    /* $$ yields a single literal dollar sign. */
    shell_prompt_set_template("$$");
    TEST_ASSERT_EQUAL_STRING("$", shell_prompt_render_plain());

    /* $s is a space. */
    shell_prompt_set_template("a$sb");
    TEST_ASSERT_EQUAL_STRING("a b", shell_prompt_render_plain());

    /* Metacharacters are case-insensitive, matching COMMAND.COM. */
    shell_prompt_set_template("$G");
    TEST_ASSERT_EQUAL_STRING(">", shell_prompt_render_plain());

    /* $n reports the DOS drive letter. */
    shell_prompt_set_template("$n");
    TEST_ASSERT_EQUAL_STRING(P4_CONFIG_SD_DRIVE_LETTER, shell_prompt_render_plain());

    /* $v reports the firmware version. */
    shell_prompt_set_template("$v");
    TEST_ASSERT_EQUAL_STRING(P4_CONFIG_VERSION_STRING, shell_prompt_render_plain());

    /* $h is a destructive backspace over the previous character. */
    shell_prompt_set_template("ab$h");
    TEST_ASSERT_EQUAL_STRING("a", shell_prompt_render_plain());

    /* A trailing '$' with nothing after it is literal. */
    shell_prompt_set_template("x$");
    TEST_ASSERT_EQUAL_STRING("x$", shell_prompt_render_plain());

    /* An unknown metacharacter renders literally, including the '$'. */
    shell_prompt_set_template("$z");
    rendered = shell_prompt_render_plain();
    TEST_ASSERT_EQUAL_STRING("$z", rendered);

    shell_prompt_reset();
}

/* ========================================================================
 * PATH EXPANSION
 * ======================================================================== */

void test_shell_prompt_path_expansion(void)
{
    char cwd_buf[P4_CONFIG_PS_PATH_MAX_DISPLAY + 8];
    const char *rendered;

    shell_get_cwd_for_prompt(cwd_buf, sizeof(cwd_buf));
    TEST_ASSERT_NOT_EMPTY(cwd_buf);

    /* $p expands to exactly what the prompt path helper reports. */
    shell_prompt_set_template("$p");
    rendered = shell_prompt_render_plain();
    TEST_ASSERT_EQUAL_STRING(cwd_buf, rendered);

    /* The composed default style keeps the path in the middle. */
    shell_prompt_set_template("PS $p$g ");
    rendered = shell_prompt_render_plain();
    TEST_ASSERT_EQUAL(0, strncmp(rendered, "PS ", 3));
    TEST_ASSERT_NOT_NULL(strstr(rendered, cwd_buf));
    TEST_ASSERT_NOT_NULL(strstr(rendered, "> "));

    shell_prompt_reset();
}

/* ========================================================================
 * KEY WAIT STATE MACHINE
 * ======================================================================== */

void test_shell_key_wait_state(void)
{
    char key = '\0';

    /* No wait is active at rest, and submissions are ignored. */
    TEST_ASSERT_FALSE(shell_key_wait_is_active());
    TEST_ASSERT_FALSE(shell_key_wait_submit('x'));

    /* Requesting a key outside a wait fails immediately rather than
     * blocking, so a caller that forgets to begin cannot hang. */
    TEST_ASSERT_FALSE(shell_wait_for_key(1, &key));

    shell_key_wait_begin();
    TEST_ASSERT_TRUE(shell_key_wait_is_active());

    /* A submitted key is delivered to the waiter. */
    TEST_ASSERT_TRUE(shell_key_wait_submit('Y'));
    TEST_ASSERT_TRUE(shell_wait_for_key(1000, &key));
    TEST_ASSERT_EQUAL('Y', key);

    /* With nothing queued the wait times out instead of blocking forever. */
    TEST_ASSERT_FALSE(shell_wait_for_key(10, &key));

    shell_key_wait_end();
    TEST_ASSERT_FALSE(shell_key_wait_is_active());

    /* Ending the wait discards anything still queued. */
    TEST_ASSERT_FALSE(shell_key_wait_submit('Z'));
}

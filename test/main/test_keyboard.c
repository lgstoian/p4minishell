/**
 * @file test_keyboard.c
 * @brief Unit tests for the on-screen keyboard input deduplication logic.
 *
 * Covers the pure, injectable-clock variant keyboard_osk_accept_at(): the
 * guard that drops a re-fire of the same button id within the debounce window
 * (the same LVGL event reaching a second, stray handler) so double OSK input
 * is impossible. The LVGL widget surface itself is hardware/display bound and
 * is exercised on the board.
 */

#include "unity.h"
#include "keyboard.h"
#include "p4minishell_config.h"

void test_keyboard_mode_names(void)
{
    TEST_ASSERT_EQUAL_STRING("text_lower", keyboard_mode_name(KEYBOARD_MODE_TEXT_LOWER));
    TEST_ASSERT_EQUAL_STRING("text_upper", keyboard_mode_name(KEYBOARD_MODE_TEXT_UPPER));
    TEST_ASSERT_EQUAL_STRING("number", keyboard_mode_name(KEYBOARD_MODE_NUMBER));
    TEST_ASSERT_EQUAL_STRING("symbols", keyboard_mode_name(KEYBOARD_MODE_SYMBOLS));
    TEST_ASSERT_EQUAL_STRING("nav", keyboard_mode_name(KEYBOARD_MODE_NAV));
    TEST_ASSERT_EQUAL_STRING("nav2", keyboard_mode_name(KEYBOARD_MODE_NAV2));
    TEST_ASSERT_EQUAL_STRING("text_lower", keyboard_mode_name(KEYBOARD_MODE_COUNT));
}

void test_keyboard_mode_parse(void)
{
    keyboard_mode_t mode = KEYBOARD_MODE_COUNT;

    TEST_ASSERT_TRUE(keyboard_mode_parse("nav", &mode));
    TEST_ASSERT_EQUAL_INT(KEYBOARD_MODE_NAV, mode);
    TEST_ASSERT_TRUE(keyboard_mode_parse("NAV2", &mode));
    TEST_ASSERT_EQUAL_INT(KEYBOARD_MODE_NAV2, mode);
    TEST_ASSERT_TRUE(keyboard_mode_parse("symbols", &mode));
    TEST_ASSERT_EQUAL_INT(KEYBOARD_MODE_SYMBOLS, mode);
    TEST_ASSERT_FALSE(keyboard_mode_parse("bogus", &mode));
    TEST_ASSERT_FALSE(keyboard_mode_parse(NULL, &mode));
    TEST_ASSERT_FALSE(keyboard_mode_parse("nav", NULL));
}

void test_keyboard_osk_dedup(void)
{
    /* A large, distinct base id so this suite is independent of the shared
     * static state left by any other test. */
    const uint32_t id = 0xAAAA0001u;
    const int64_t t0 = 1000000; /* ms */

    /* A fresh button id is always accepted. */
    TEST_ASSERT_TRUE(keyboard_osk_accept_at(id, t0));

    /* The same id at the same instant is the same event twice -> dropped. */
    TEST_ASSERT_FALSE(keyboard_osk_accept_at(id, t0));

    /* The same id just inside the window is still a duplicate -> dropped. */
    TEST_ASSERT_FALSE(keyboard_osk_accept_at(id, t0 + P4_CONFIG_OSK_DEBOUNCE_MS - 1));

    /* At/after the window boundary it is a new press -> accepted. */
    TEST_ASSERT_TRUE(keyboard_osk_accept_at(id, t0 + P4_CONFIG_OSK_DEBOUNCE_MS));

    /* A different id is always a new press, even within the window. */
    TEST_ASSERT_TRUE(keyboard_osk_accept_at(id + 1, t0 + P4_CONFIG_OSK_DEBOUNCE_MS + 1));

    /* The same id again, still inside its own window -> dropped. */
    TEST_ASSERT_FALSE(keyboard_osk_accept_at(id + 1, t0 + P4_CONFIG_OSK_DEBOUNCE_MS + 2));

    /* Auto-repeat style: a different id spaced well beyond the window. */
    TEST_ASSERT_TRUE(keyboard_osk_accept_at(id + 2, t0 + P4_CONFIG_OSK_DEBOUNCE_MS + 100));

    /* A repeat of that id after the window elapses -> accepted (not merged),
     * so genuine fast typing / auto-repeat is never lost. */
    TEST_ASSERT_TRUE(keyboard_osk_accept_at(id + 2, t0 + P4_CONFIG_OSK_DEBOUNCE_MS + 140));
}

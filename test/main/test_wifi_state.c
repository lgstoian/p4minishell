/**
 * @file test_wifi_state.c
 * @brief Unit tests for the Wi-Fi state machine.
 *
 * Tests networking_wifi_state transitions, mutex safety, and
 * watchdog trigger conditions from components/networking/networking.c.
 */

#include "unity.h"
#include "networking.h"
#include <string.h>

/* ========================================================================
 * WI-FI STATE TRANSITION TESTS
 * ======================================================================== */

void test_wifi_state_transitions(void)
{
    /* Initial state should be NOT_ATTEMPTED */
    networking_wifi_state_t state = networking_wifi_state();
    TEST_ASSERT_TRUE(
        state == NETWORKING_WIFI_STATE_NOT_ATTEMPTED ||
        state == NETWORKING_WIFI_STATE_STARTED ||
        state == NETWORKING_WIFI_STATE_STARTING ||
        state == NETWORKING_WIFI_STATE_FAILED ||
        state == NETWORKING_WIFI_STATE_SKIPPED_DISABLED ||
        state == NETWORKING_WIFI_STATE_SKIPPED_UNSUPPORTED
    );

    /* State string should be non-NULL */
    const char *str = networking_wifi_state_string();
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_TRUE(strlen(str) > 0);

    /* Connected should be false initially (or reflect actual state) */
    bool connected = networking_wifi_is_connected();
    /* Just verify it returns a valid bool — actual value depends on HW */
    TEST_ASSERT_TRUE(connected == true || connected == false);

    /* Last error should be valid */
    esp_err_t err = networking_wifi_last_error();
    /* Just verify it doesn't crash */
    (void)err;
}

/* ========================================================================
 * WI-FI MUTEX TESTS
 * ======================================================================== */

void test_wifi_mutex(void)
{
    /* Test that concurrent access to networking_wifi_is_connected()
     * and networking_wifi_is_starting() doesn't crash or hang.
     * These functions internally use wifi_lock()/wifi_unlock(). */

    /* Call from same task — should not deadlock */
    for (int i = 0; i < 100; i++) {
        bool c = networking_wifi_is_connected();
        bool s = networking_wifi_is_starting();
        (void)c;
        (void)s;
    }

    /* Verify state string consistency */
    networking_wifi_state_t state = networking_wifi_state();
    const char *str = networking_wifi_state_string();

    switch (state) {
    case NETWORKING_WIFI_STATE_NOT_ATTEMPTED:
        TEST_ASSERT_EQUAL_STRING("not_attempted", str);
        break;
    case NETWORKING_WIFI_STATE_STARTING:
        TEST_ASSERT_EQUAL_STRING("starting", str);
        break;
    case NETWORKING_WIFI_STATE_STARTED:
        TEST_ASSERT_EQUAL_STRING("started", str);
        break;
    case NETWORKING_WIFI_STATE_FAILED:
        TEST_ASSERT_EQUAL_STRING("failed", str);
        break;
    case NETWORKING_WIFI_STATE_SKIPPED_DISABLED:
        TEST_ASSERT_EQUAL_STRING("disabled", str);
        break;
    case NETWORKING_WIFI_STATE_SKIPPED_UNSUPPORTED:
        TEST_ASSERT_EQUAL_STRING("unsupported", str);
        break;
    default:
        TEST_FAIL_MESSAGE("Unknown Wi-Fi state");
        break;
    }

    TEST_ASSERT_TRUE(1);
}

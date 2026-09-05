/**
 * @file test_applib.c
 * @brief Unit tests for the native-app runtime library (components/applib).
 *
 * Covers the memory-allocation policy (alloc/calloc/realloc/strdup/strndup/
 * free, zero-size and NULL handling), the time/sysinfo helpers, the console
 * output entry points (they must not crash and must return a byte count), and
 * the Wi-Fi state accessors through a fake registered ops table.
 *
 * Pure logic with no hardware dependency; the shell transcript append helpers
 * run synchronously in the test environment (windows_get_transcript() is NULL).
 */

#include "unity.h"
#include "applib.h"
#include "shell.h"
#include <string.h>

/* ========================================================================
 * MEMORY ALLOCATION POLICY
 * ======================================================================== */

void test_applib_alloc_free(void)
{
    void *block = app_alloc(64);

    TEST_ASSERT_NOT_NULL(block);
    memset(block, 0xAB, 64);
    app_free(block);

    /* A large block still allocates (PSRAM fallback exists on every board). */
    block = app_alloc(4096);
    TEST_ASSERT_NOT_NULL(block);
    app_free(block);

    TEST_ASSERT_NULL(app_alloc(0));
}

void test_applib_calloc_zeroes(void)
{
    unsigned char *block = app_calloc(16, 4);

    TEST_ASSERT_NOT_NULL(block);
    for (int i = 0; i < 16 * 4; i++) {
        TEST_ASSERT_EQUAL(0, block[i]);
    }
    app_free(block);
}

void test_applib_realloc(void)
{
    char *block = app_alloc(4);

    TEST_ASSERT_NOT_NULL(block);
    memcpy(block, "abcd", 4);

    /* Grow: the first four bytes must survive. */
    block = app_realloc(block, 64);
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_EQUAL_STRING_LEN("abcd", block, 4);

    /* Shrink: contents preserved up to the new size. */
    block = app_realloc(block, 2);
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_EQUAL_STRING_LEN("ab", block, 2);
    app_free(block);

    /* NULL pointer behaves like alloc; size 0 frees and returns NULL. */
    TEST_ASSERT_NOT_NULL(app_realloc(NULL, 16));
    TEST_ASSERT_NULL(app_realloc(app_alloc(8), 0));
}

void test_applib_strdup(void)
{
    char *copy = app_strdup("hello applib");

    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_STRING("hello applib", copy);
    app_free(copy);

    TEST_ASSERT_NULL(app_strdup(NULL));
}

void test_applib_strndup(void)
{
    char *copy = app_strndup("hello applib", 5);

    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_STRING("hello", copy);
    app_free(copy);

    /* n larger than the string is clamped. */
    copy = app_strndup("abc", 32);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_STRING("abc", copy);
    app_free(copy);

    TEST_ASSERT_NULL(app_strndup(NULL, 4));
}

void test_applib_free_null(void)
{
    app_free(NULL);   /* must be a no-op */
}

/* ========================================================================
 * TIME / SYSTEM INFORMATION HELPERS
 * ======================================================================== */

void test_applib_time_helpers(void)
{
    uint32_t before = app_uptime_sec();
    int64_t ms_before = app_now_ms();

    app_delay_ms(2);   /* must return promptly, no crash */

    /* Uptime / monotonic ms advance (or at least do not go backwards). */
    TEST_ASSERT_TRUE(app_uptime_sec() >= before);
    TEST_ASSERT_TRUE(app_now_ms() >= ms_before);
}

void test_applib_uptime_formatted(void)
{
    char buf[32];

    app_uptime_formatted(buf, sizeof(buf));
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_applib_sysinfo(void)
{
    char buf[256];

    app_sysinfo(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "heap_free="));
    TEST_ASSERT_NOT_NULL(strstr(buf, "uptime="));
}

/* ========================================================================
 * CONSOLE OUTPUT ENTRY POINTS
 * ======================================================================== */

void test_applib_printf(void)
{
    int n = app_printf("applib hello %d", 42);

    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(app_printf("%s", "applib ANSI") > 0);
    TEST_ASSERT_TRUE(app_printf(NULL) == 0);

    /* Semantic helpers must not crash and must accept @-data safely. */
    app_print_heading("%s", "heading @ literal");
    app_print_field("label", "%s", "value @ literal");
    app_print_ok("ok");
    app_print_error("err");
    app_print_warning("warn");
    app_print_muted("muted");
    app_print_usage("usage");
}

/* ========================================================================
 * WI-FI STATE ACCESSORS (fake ops table)
 * ======================================================================== */

static bool s_fake_connected;
static int s_fake_rssi;
static const char *s_fake_state;
static int s_rssi_calls;

static bool fake_wifi_is_connected(void)
{
    return s_fake_connected;
}

static bool fake_wifi_get_rssi(int *rssi_out)
{
    s_rssi_calls++;
    *rssi_out = s_fake_rssi;
    return true;
}

static const char *fake_wifi_state_string(void)
{
    return s_fake_state;
}

void test_applib_wifi_ops(void)
{
    applib_net_ops_t ops = {
        .wifi_is_connected = fake_wifi_is_connected,
        .wifi_get_rssi = fake_wifi_get_rssi,
        .wifi_state_string = fake_wifi_state_string,
    };

    /* Unregistered: every helper degrades to a safe default. */
    applib_register_net_ops(NULL);
    TEST_ASSERT_FALSE(app_wifi_is_connected());
    TEST_ASSERT_EQUAL(0, app_wifi_get_rssi());
    TEST_ASSERT_EQUAL_STRING("n/a", app_wifi_state_string());

    /* Registered: helpers route through the table. */
    applib_register_net_ops(&ops);
    s_fake_connected = true;
    s_fake_rssi = -55;
    s_fake_state = "connected";
    TEST_ASSERT_TRUE(app_wifi_is_connected());
    TEST_ASSERT_EQUAL(-55, app_wifi_get_rssi());
    TEST_ASSERT_EQUAL(1, s_rssi_calls);
    TEST_ASSERT_EQUAL_STRING("connected", app_wifi_state_string());

    /* Clearing the table restores the safe defaults. */
    applib_register_net_ops(NULL);
    TEST_ASSERT_FALSE(app_wifi_is_connected());
    TEST_ASSERT_EQUAL_STRING("n/a", app_wifi_state_string());
}

void test_applib_input_timeout(void)
{
    char buf[16];

    /* Both helpers are bounded: with no interactive key source (the test
     * runner) they return false without stalling, and they never hang. */
    TEST_ASSERT_FALSE(app_wait_key(10, NULL));
    TEST_ASSERT_FALSE(app_read_line(buf, sizeof(buf), 10));

    /* NULL safety. */
    TEST_ASSERT_FALSE(app_read_line(NULL, 0, 10));
    TEST_ASSERT_FALSE(app_read_line(NULL, 16, 10));
}

void test_applib_state(void)
{
    char value[64];

    /* NULL / malformed arguments are rejected before any SD access (the SD
     * card is unavailable in the test runner; the real helpers are verified
     * on board). */
    TEST_ASSERT_FALSE(app_ini_get(NULL, "K", value, sizeof(value)));
    TEST_ASSERT_FALSE(app_ini_get("f.ini", NULL, value, sizeof(value)));
    TEST_ASSERT_FALSE(app_ini_get("f.ini", "K", NULL, 0));
    TEST_ASSERT_FALSE(app_ini_set(NULL, "K", "v"));
    TEST_ASSERT_FALSE(app_ini_set("f.ini", NULL, "v"));
    TEST_ASSERT_FALSE(app_ini_set("f.ini", "K", NULL));
    TEST_ASSERT_FALSE(app_ini_delete(NULL, "K"));
    TEST_ASSERT_FALSE(app_ini_delete("f.ini", NULL));
    TEST_ASSERT_FALSE(app_temp_path(NULL, 0, NULL));
    TEST_ASSERT_FALSE(app_temp_path(value, 0, NULL));
}

void test_applib_menu_primitives(void)
{
    const char *items[] = { "one", "two", "three" };

    /* app_print_styled wraps text in SGR codes and renders it (returns the
     * formatted length); it never crashes in the headless runner. */
    TEST_ASSERT_TRUE(app_print_styled("1;7", "highlight %s", "me") > 0);
    TEST_ASSERT_TRUE(app_print_styled("7", "plain reverse") > 0);

    /* app_menu with no items / NULL returns 0 without reading input. */
    TEST_ASSERT_EQUAL(0, app_menu(NULL, NULL, 0, 10));
    TEST_ASSERT_EQUAL(0, app_menu("t", NULL, 3, 10));
    TEST_ASSERT_EQUAL(0, app_menu(NULL, items, 0, 10));
    TEST_ASSERT_EQUAL(0, app_menu(NULL, items, -1, 10));
}

void test_applib_read_password(void)
{
    char buf[16];

    /* No interactive key source in the runner: hidden input degrades to
     * false without hanging, and NULL arguments are rejected. */
    TEST_ASSERT_FALSE(app_read_password(buf, sizeof(buf), 10));
    TEST_ASSERT_FALSE(app_read_password(NULL, 0, 10));
    TEST_ASSERT_FALSE(app_read_password(NULL, 16, 10));
}

void test_applib_app_mode(void)
{
    /* Headless runner (no LVGL screen): enter/exit are safe; only the
     * non-LVGL path is exercised. */
    TEST_ASSERT_TRUE(app_mode_enter(false));
    TEST_ASSERT_TRUE(app_mode_exit());
    TEST_ASSERT_FALSE(shell_app_mode_active());

    /* Screen primitives are NULL-safe. */
    shell_screen_restore(NULL);
    shell_screen_discard(NULL);
}

/* ========================================================================
 * PROCESS ENVIRONMENT (applib_env.h, fake ops table)
 * ======================================================================== */

static const char *s_fake_env_name;
static const char *s_fake_env_value;
static const char *s_fake_cwd;

static const char *fake_get_env(const char *name)
{
    s_fake_env_name = name;
    return s_fake_env_value;
}

static esp_err_t fake_set_env(const char *name, const char *value)
{
    s_fake_env_name = name;
    s_fake_env_value = value;
    return ESP_OK;
}

static const char *fake_get_cwd(void)
{
    return s_fake_cwd;
}

void test_applib_env_ops(void)
{
    applib_env_ops_t ops = {
        .get_env = fake_get_env,
        .set_env = fake_set_env,
        .get_cwd = fake_get_cwd,
    };

    /* Unregistered / NULL args: every helper degrades to a safe default. */
    applib_register_env_ops(NULL);
    TEST_ASSERT_NULL(app_env_get("X"));
    TEST_ASSERT_NULL(app_env_get(NULL));
    TEST_ASSERT_FALSE(app_env_set("X", "v"));
    TEST_ASSERT_FALSE(app_env_set(NULL, "v"));
    TEST_ASSERT_NULL(app_get_cwd());

    /* Registered: helpers route through the table. */
    applib_register_env_ops(&ops);
    s_fake_env_value = "hello";
    s_fake_cwd = "/sdcard";
    TEST_ASSERT_EQUAL_STRING("hello", app_env_get("X"));
    TEST_ASSERT_TRUE(app_env_set("Y", "z"));
    TEST_ASSERT_EQUAL_STRING("Y", s_fake_env_name);
    TEST_ASSERT_EQUAL_STRING("/sdcard", app_get_cwd());

    /* Clearing the table restores the safe defaults. */
    applib_register_env_ops(NULL);
    TEST_ASSERT_NULL(app_env_get("X"));
    TEST_ASSERT_FALSE(app_env_set("X", "v"));
    TEST_ASSERT_NULL(app_get_cwd());
}

/* ========================================================================
 * NATIVE-APP ABI (applib_app.h: registry + dispatch)
 * ======================================================================== */

static int s_app_dispatch_count;

static int fake_app_main(int argc, char **argv)
{
    s_app_dispatch_count++;
    if (argc >= 2 && argv[1] != NULL && strcmp(argv[1], "fail") == 0) {
        return 1;
    }
    return 0;
}

void test_applib_app_registry(void)
{
    char name[P4_CONFIG_APP_NAME_BYTES];
    char desc[P4_CONFIG_APP_DESC_BYTES];
    int errorlevel = 99;

    /* Invalid registrations are rejected. */
    TEST_ASSERT_FALSE(app_register(NULL, "d", fake_app_main));
    TEST_ASSERT_FALSE(app_register("x", "d", NULL));
    TEST_ASSERT_FALSE(app_register("x", "d", NULL));

    /* Register and find (case-insensitive). */
    TEST_ASSERT_TRUE(app_register("testapp", "a test app", fake_app_main));
    TEST_ASSERT_TRUE(app_find("testapp"));
    TEST_ASSERT_TRUE(app_find("TESTAPP"));
    TEST_ASSERT_FALSE(app_find("missing"));

    /* Dispatch argv[0]="testapp": entry runs, return value becomes errorlevel. */
    {
        char *argv[] = { "testapp", "ok", NULL };
        s_app_dispatch_count = 0;
        TEST_ASSERT_TRUE(app_dispatch(2, argv, &errorlevel));
        TEST_ASSERT_EQUAL(1, s_app_dispatch_count);
        TEST_ASSERT_EQUAL(0, errorlevel);
    }
    /* "fail" -> entry returns 1. */
    {
        char *argv[] = { "testapp", "fail", NULL };
        TEST_ASSERT_TRUE(app_dispatch(2, argv, &errorlevel));
        TEST_ASSERT_EQUAL(1, errorlevel);
    }
    /* Unknown name -> not dispatched. */
    {
        char *argv[] = { "nope", NULL };
        errorlevel = 99;
        TEST_ASSERT_FALSE(app_dispatch(1, argv, &errorlevel));
        TEST_ASSERT_EQUAL(99, errorlevel);
    }
    /* NULL errorlevel_out still runs the app. */
    {
        char *argv[] = { "testapp", "ok", NULL };
        TEST_ASSERT_TRUE(app_dispatch(2, argv, NULL));
    }

    /* Listing: slot 0 is the registered app; out-of-range is skipped. */
    TEST_ASSERT_TRUE(app_get(0, name, sizeof(name), desc, sizeof(desc)));
    TEST_ASSERT_EQUAL_STRING("testapp", name);
    TEST_ASSERT_EQUAL_STRING("a test app", desc);
    TEST_ASSERT_FALSE(app_get(P4_CONFIG_APP_MAX, name, sizeof(name), desc, sizeof(desc)));
}

/**
 * @file test_header.c
 * @brief Unit tests for the pure header layout policy
 * (components/header/header_layout.c).
 *
 * Widgets, measurement, and rendering are hardware-verified with
 * tools/header_test.py; this covers the width/priority decisions.
 */

#include "unity.h"
#include "header_layout.h"
#include "header_status.h"
#include "header_notify_queue.h"
#include "header_refresh.h"
#include "p4minishell_config.h"

#include <string.h>

/* Convenience: all levels equal except the ones under test. */
static void fill(int out[HEADER_LEVEL_COUNT], int full, int shrt, int mn)
{
    out[HEADER_LEVEL_FULL] = full;
    out[HEADER_LEVEL_SHORT] = shrt;
    out[HEADER_LEVEL_MIN] = mn;
}

void test_header_layout_all_full(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 260, 180, 110);
    header_layout_compute(&l, 1024, 4, status, sys, 100);

    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_FULL, l.status_level);
    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_FULL, l.sys_level);
    TEST_ASSERT_TRUE(l.show_status);
    TEST_ASSERT_TRUE(l.show_sys);
    TEST_ASSERT_TRUE(l.show_center);
    TEST_ASSERT_TRUE(l.show_sep);
    TEST_ASSERT_TRUE(l.show_cpu_graph);
    TEST_ASSERT_TRUE(l.show_battery_bar);
    TEST_ASSERT_FALSE(l.smaller_font);
    TEST_ASSERT_EQUAL_INT(300, l.left_w);
    TEST_ASSERT_EQUAL_INT(260, l.right_w);
    TEST_ASSERT_EQUAL_INT(1024 - 300 - 260 - 8, l.center_w);
}

void test_header_layout_compacts_sides(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 300, 180, 110);
    /* 600 avail: full/full needs 600+8+100 > 600; full/short needs
     * 300+180+8+100 = 588 -> fits. */
    header_layout_compute(&l, 600, 4, status, sys, 100);

    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_FULL, l.status_level);
    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_SHORT, l.sys_level);
    TEST_ASSERT_TRUE(l.show_center);
    TEST_ASSERT_FALSE(l.show_sep);
    TEST_ASSERT_FALSE(l.show_cpu_graph);
    TEST_ASSERT_TRUE(l.show_battery_bar);
    TEST_ASSERT_EQUAL_INT(600 - 300 - 180 - 8, l.center_w);
}

void test_header_layout_center_yields_first(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 300, 180, 110);
    /* 340 avail, center_min 150: no combo leaves room for the center, but
     * short/min sides (200+110+8 = 318) fit -> center hidden, sides kept,
     * no smaller font needed. Notifications yield first. */
    header_layout_compute(&l, 340, 4, status, sys, 150);

    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_SHORT, l.status_level);
    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_MIN, l.sys_level);
    TEST_ASSERT_FALSE(l.show_center);
    TEST_ASSERT_EQUAL_INT(0, l.center_w);
    TEST_ASSERT_FALSE(l.show_sep);
    TEST_ASSERT_FALSE(l.show_cpu_graph);
    TEST_ASSERT_FALSE(l.show_battery_bar);
    TEST_ASSERT_FALSE(l.smaller_font);
}

void test_header_layout_requests_smaller_font(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 300, 180, 110);
    /* 200 avail: even min/min (238) doesn't fit -> smaller font requested;
     * with sides 120+110+4 = 234 > 200, the sys panel is dropped. */
    header_layout_compute(&l, 200, 4, status, sys, 100);

    TEST_ASSERT_TRUE(l.smaller_font);
    TEST_ASSERT_FALSE(l.show_center);
    TEST_ASSERT_FALSE(l.show_sys);
    TEST_ASSERT_TRUE(l.show_status);
    TEST_ASSERT_EQUAL_INT(120, l.left_w);
    TEST_ASSERT_EQUAL_INT(0, l.right_w);
}

void test_header_layout_degenerate(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 300, 180, 110);
    /* Zero width: everything hides, no overlap, no crash. */
    header_layout_compute(&l, 0, 4, status, sys, 100);
    TEST_ASSERT_FALSE(l.show_status);
    TEST_ASSERT_FALSE(l.show_sys);
    TEST_ASSERT_FALSE(l.show_center);
    TEST_ASSERT_EQUAL_INT(0, l.left_w);
    TEST_ASSERT_EQUAL_INT(0, l.right_w);

    /* NULL-safe. */
    header_layout_compute(NULL, 100, 4, status, sys, 10);
}

/* ---- Pure status mapping (header_status.c) ---- */

void test_header_status_glyphs(void)
{
    TEST_ASSERT_EQUAL_STRING("W", header_status_glyph(HEADER_STATUS_WIFI));
    TEST_ASSERT_EQUAL_STRING("BT", header_status_glyph(HEADER_STATUS_BLUETOOTH));
    TEST_ASSERT_EQUAL_STRING("U", header_status_glyph(HEADER_STATUS_USB));
    TEST_ASSERT_EQUAL_STRING("S", header_status_glyph(HEADER_STATUS_SD));
    TEST_ASSERT_EQUAL_STRING("A", header_status_glyph(HEADER_STATUS_ACTIVITY));
    TEST_ASSERT_EQUAL_STRING("M", header_status_glyph(HEADER_STATUS_MEM));
    TEST_ASSERT_EQUAL_STRING("C", header_status_glyph(HEADER_STATUS_CPU));
    TEST_ASSERT_EQUAL_STRING("B", header_status_glyph(HEADER_STATUS_BATTERY));
}

void test_header_status_wifi(void)
{
    TEST_ASSERT_EQUAL_STRING("HI", header_status_wifi_label(-40));
    TEST_ASSERT_EQUAL_STRING("MID", header_status_wifi_label(-60));
    TEST_ASSERT_EQUAL_STRING("LOW", header_status_wifi_label(-75));
    TEST_ASSERT_EQUAL_STRING("WEAK", header_status_wifi_label(-90));

    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR, header_status_wifi_tone(false, -40));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK, header_status_wifi_tone(true, -40));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK, header_status_wifi_tone(true, -60));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_WARN, header_status_wifi_tone(true, -75));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR, header_status_wifi_tone(true, -90));
}

void test_header_status_bt_usb(void)
{
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR, header_status_bluetooth_tone(false, false));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_WARN, header_status_bluetooth_tone(true, false));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK, header_status_bluetooth_tone(true, true));

    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR, header_status_usb_tone(false));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK, header_status_usb_tone(true));
}

void test_header_status_sd(void)
{
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_MUTED, header_status_sd_tone(HEADER_SD_NONE));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_WARN, header_status_sd_tone(HEADER_SD_INSERTED));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK, header_status_sd_tone(HEADER_SD_MOUNTED));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR, header_status_sd_tone(HEADER_SD_ERROR));
}

void test_header_status_system_tone(void)
{
    /* Memory: 50% healthy, 20% amber (<= 30), 10% red (<= 15), 0 total muted. */
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK, header_status_mem_tone(50, 100));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_WARN, header_status_mem_tone(20, 100));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR, header_status_mem_tone(10, 100));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_MUTED, header_status_mem_tone(0, 0));

    /* CPU: 10% green, 85% amber, 96% red. */
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK, header_status_cpu_tone(10));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_WARN, header_status_cpu_tone(85));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR, header_status_cpu_tone(96));

    /* Battery: 80% green, 10% amber, 3% red, no ADC muted. */
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK, header_status_battery_tone(80, true));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_WARN, header_status_battery_tone(10, true));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR, header_status_battery_tone(3, true));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_MUTED, header_status_battery_tone(80, false));
}

/* ---- Notification queue (header_notify_queue.c) ---- */

void test_header_notify_queue_fifo(void)
{
    header_notify_queue_t q;
    header_notify_item_t it;

    header_notify_queue_init(&q);
    TEST_ASSERT_TRUE(header_notify_queue_empty(&q));
    TEST_ASSERT_FALSE(header_notify_queue_pop(&q, &it));

    TEST_ASSERT_TRUE(header_notify_queue_push(&q, "one", HEADER_NOTIFY_INFO, 1000));
    TEST_ASSERT_TRUE(header_notify_queue_push(&q, "two", HEADER_NOTIFY_WARN, 2000));
    TEST_ASSERT_EQUAL_INT(2, header_notify_queue_count(&q));

    TEST_ASSERT_TRUE(header_notify_queue_pop(&q, &it));
    TEST_ASSERT_EQUAL_STRING("one", it.text);
    TEST_ASSERT_EQUAL_INT(HEADER_NOTIFY_INFO, it.level);
    TEST_ASSERT_EQUAL_INT(1000, it.timeout_ms);

    TEST_ASSERT_TRUE(header_notify_queue_pop(&q, &it));
    TEST_ASSERT_EQUAL_STRING("two", it.text);
    TEST_ASSERT_EQUAL_INT(HEADER_NOTIFY_WARN, it.level);
    TEST_ASSERT_FALSE(header_notify_queue_pop(&q, &it));
}

void test_header_notify_queue_overflow_drops_oldest(void)
{
    header_notify_queue_t q;
    header_notify_item_t it;
    int i;

    header_notify_queue_init(&q);
    for (i = 0; i < P4_CONFIG_HEADER_NOTIFY_QUEUE; i++) {
        TEST_ASSERT_TRUE(header_notify_queue_push(&q, "x", HEADER_NOTIFY_INFO, 1));
    }
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_NOTIFY_QUEUE, header_notify_queue_count(&q));

    /* One more overflows: the oldest queued entry is dropped, newest kept. */
    TEST_ASSERT_TRUE(header_notify_queue_push(&q, "newest", HEADER_NOTIFY_ERR, 5000));
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_NOTIFY_QUEUE, header_notify_queue_count(&q));
    while (header_notify_queue_pop(&q, &it)) {
        if (strcmp(it.text, "newest") == 0) {
            TEST_ASSERT_EQUAL_INT(HEADER_NOTIFY_ERR, it.level);
            TEST_ASSERT_EQUAL_INT(5000, it.timeout_ms);
        }
    }
    TEST_ASSERT_TRUE(header_notify_queue_empty(&q));
}

void test_header_notify_queue_clear_and_blank(void)
{
    header_notify_queue_t q;

    header_notify_queue_init(&q);
    /* A blank/empty text never enters the queue. */
    TEST_ASSERT_FALSE(header_notify_queue_push(&q, "", HEADER_NOTIFY_INFO, 0));
    TEST_ASSERT_FALSE(header_notify_queue_push(&q, NULL, HEADER_NOTIFY_INFO, 0));
    TEST_ASSERT_TRUE(header_notify_queue_empty(&q));

    TEST_ASSERT_TRUE(header_notify_queue_push(&q, "x", HEADER_NOTIFY_INFO, 0));
    header_notify_queue_clear(&q);
    TEST_ASSERT_TRUE(header_notify_queue_empty(&q));
    TEST_ASSERT_EQUAL_INT(0, header_notify_queue_count(&q));
}

/* ---- Adaptive refresh policy (header_refresh.c) ---- */

void test_header_refresh_priority(void)
{
    header_refresh_state_t s = { 0 };

    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_REFRESH_IDLE_MS,
                          header_refresh_interval_ms(&s));
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_REFRESH_IDLE_MS,
                          header_refresh_interval_ms(NULL));

    s.startup = true;
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_REFRESH_STARTUP_MS,
                          header_refresh_interval_ms(&s));

    s.wifi_connecting = true;
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_REFRESH_CONNECTING_MS,
                          header_refresh_interval_ms(&s));

    s.busy = true;
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_REFRESH_BUSY_MS,
                          header_refresh_interval_ms(&s));

    s.display_off = true;
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_REFRESH_WAKE_MS,
                          header_refresh_interval_ms(&s));
}

void test_header_refresh_clock_and_idle_off(void)
{
    header_refresh_state_t s = { 0 };

    /* Clock enabled, 10 s to the next minute: idle cap wins. */
    s.clock_enabled = true;
    s.seconds_to_next_minute = 10;
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_REFRESH_IDLE_MS,
                          header_refresh_interval_ms(&s));

    /* Clock enabled, 1 s to the next minute: wake just after the boundary. */
    s.seconds_to_next_minute = 1;
    TEST_ASSERT_EQUAL_INT(1050, (int)header_refresh_interval_ms(&s));

    /* Pending idle-off deadline is sooner than everything else. */
    s.ms_to_idle_off = 500;
    TEST_ASSERT_EQUAL_INT(500, (int)header_refresh_interval_ms(&s));

    /* A tiny deadline clamps to the configured minimum. */
    s.ms_to_idle_off = 5;
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HEADER_REFRESH_MIN_MS,
                          (int)header_refresh_interval_ms(&s));
}

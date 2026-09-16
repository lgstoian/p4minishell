/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_clock.c
 * @brief Unit tests for the pure header-clock formatter (components/clock).
 */

#include "unity.h"
#include "clock.h"

#include <string.h>
#include <time.h>

void test_clock_format_hm_snapshot(void)
{
    struct tm tm;
    char buf[16];

    memset(&tm, 0, sizeof(tm));
    tm.tm_hour = 9;
    tm.tm_min = 5;
    clock_format_hm_snapshot(&tm, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("09:05", buf);

    tm.tm_hour = 23;
    tm.tm_min = 59;
    clock_format_hm_snapshot(&tm, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("23:59", buf);

    /* NULL broken-down time renders the unsynchronized placeholder. */
    clock_format_hm_snapshot(NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("--:--", buf);

    /* Zero-size buffer must not write. */
    clock_format_hm_snapshot(&tm, NULL, 0);
}

void test_clock_timer_start_stop(void)
{
    int64_t elapsed_ms = -1;

    TEST_ASSERT_EQUAL_INT(0, clock_timer_start_at("unittest", 1000000));
    /* Stopping an unknown run fails without touching the output. */
    TEST_ASSERT_EQUAL_INT(1, clock_timer_stop_at("nope", 2000000, &elapsed_ms));
    TEST_ASSERT_EQUAL_INT(-1, (int)elapsed_ms);
    /* 1.5 s later the frozen total reads back. */
    TEST_ASSERT_EQUAL_INT(0, clock_timer_stop_at("unittest", 2500000, &elapsed_ms));
    TEST_ASSERT_EQUAL_INT(1500, (int)elapsed_ms);
    /* A stopped run cannot stop twice. */
    TEST_ASSERT_EQUAL_INT(1, clock_timer_stop_at("unittest", 3000000, NULL));
}

void test_clock_timer_lap_and_status(void)
{
    bool running = false;
    uint32_t laps = 0;
    int64_t elapsed_ms = -1;

    TEST_ASSERT_EQUAL_INT(0, clock_timer_start_at("lapper", 0));
    TEST_ASSERT_EQUAL_INT(0, clock_timer_lap_at("lapper", 500000, &elapsed_ms));
    TEST_ASSERT_EQUAL_INT(500, (int)elapsed_ms);
    TEST_ASSERT_EQUAL_INT(0, clock_timer_status_at("lapper", 750000, &running, &elapsed_ms, &laps));
    TEST_ASSERT_TRUE(running);
    TEST_ASSERT_EQUAL_INT(750, (int)elapsed_ms);
    TEST_ASSERT_EQUAL_UINT32(1, laps);
    /* Lap on an idle run fails; status of an unknown run fails. */
    TEST_ASSERT_EQUAL_INT(0, clock_timer_stop_at("lapper", 1000000, NULL));
    TEST_ASSERT_EQUAL_INT(1, clock_timer_lap_at("lapper", 1100000, NULL));
    TEST_ASSERT_EQUAL_INT(1, clock_timer_status_at("ghost", 1100000, NULL, NULL, NULL));
}

void test_clock_timer_usage_and_restart(void)
{
    /* Empty names fall back to the default run; over-long names are usage. */
    TEST_ASSERT_EQUAL_INT(0, clock_timer_start_at(NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, clock_timer_stop_at("", 100000, NULL));
    TEST_ASSERT_EQUAL_INT(2, clock_timer_start_at("this-name-is-far-too-long", 0));
    TEST_ASSERT_EQUAL_INT(2, clock_timer_status_at("this-name-is-far-too-long", 0, NULL, NULL, NULL));
    /* Starting an existing run restarts it from zero. */
    TEST_ASSERT_EQUAL_INT(0, clock_timer_start_at("rerun", 0));
    TEST_ASSERT_EQUAL_INT(0, clock_timer_start_at("rerun", 900000));
    {
        int64_t elapsed_ms = -1;
        TEST_ASSERT_EQUAL_INT(0, clock_timer_stop_at("rerun", 1000000, &elapsed_ms));
        TEST_ASSERT_EQUAL_INT(100, (int)elapsed_ms);
    }
}

/* ========================================================================
 * RTC ANCHOR REPLAY (clock_rtc_restore_math) + BCD HELPERS
 * ======================================================================== */

void test_clock_rtc_restore_math(void)
{
    int64_t unix = -1;
    const int64_t epoch = 1577836800LL; /* P4_CONFIG_CLOCK_VALID_EPOCH */

    /* Counter kept running: 10 s of RTC elapse replay as 10 s of wall time. */
    TEST_ASSERT_EQUAL_INT(CLOCK_RTC_RESTORED,
        clock_rtc_restore_math(1700000000LL, 5000000u, 15000000u, epoch, &unix));
    TEST_ASSERT_EQUAL_INT32(1700000010L, (int32_t)unix);
    /* Zero elapse replays the anchor exactly. */
    TEST_ASSERT_EQUAL_INT(CLOCK_RTC_RESTORED,
        clock_rtc_restore_math(1700000000LL, 5000000u, 5000000u, epoch, &unix));
    TEST_ASSERT_EQUAL_INT32(1700000000L, (int32_t)unix);
    /* Counter reset below the anchor: stale last-known time. */
    TEST_ASSERT_EQUAL_INT(CLOCK_RTC_STALE,
        clock_rtc_restore_math(1700000000LL, 9000000u, 1000u, epoch, &unix));
    TEST_ASSERT_EQUAL_INT32(1700000000L, (int32_t)unix);
    /* Predated anchor: invalid, output cleared. */
    TEST_ASSERT_EQUAL_INT(CLOCK_RTC_INVALID,
        clock_rtc_restore_math(1000LL, 5000000u, 15000000u, epoch, &unix));
    TEST_ASSERT_EQUAL_INT32(0, (int32_t)unix);
    /* Implausible delta (> 10 years): invalid. */
    TEST_ASSERT_EQUAL_INT(CLOCK_RTC_INVALID,
        clock_rtc_restore_math(1700000000LL, 0u, (uint64_t)11LL * 365 * 24 * 3600 * 1000000u,
                               epoch, &unix));
    /* NULL output: invalid. */
    TEST_ASSERT_EQUAL_INT(CLOCK_RTC_INVALID,
        clock_rtc_restore_math(1700000000LL, 0u, 0u, epoch, NULL));
}

void test_clock_rtc_bcd(void)
{
    TEST_ASSERT_EQUAL_UINT8(0x00, clock_rtc_bin_to_bcd(0));
    TEST_ASSERT_EQUAL_UINT8(0x09, clock_rtc_bin_to_bcd(9));
    TEST_ASSERT_EQUAL_UINT8(0x10, clock_rtc_bin_to_bcd(10));
    TEST_ASSERT_EQUAL_UINT8(0x59, clock_rtc_bin_to_bcd(59));
    TEST_ASSERT_EQUAL_UINT8(0x99, clock_rtc_bin_to_bcd(99));
    TEST_ASSERT_EQUAL_UINT8(0, clock_rtc_bcd_to_bin(0x00));
    TEST_ASSERT_EQUAL_UINT8(9, clock_rtc_bcd_to_bin(0x09));
    TEST_ASSERT_EQUAL_UINT8(10, clock_rtc_bcd_to_bin(0x10));
    TEST_ASSERT_EQUAL_UINT8(59, clock_rtc_bcd_to_bin(0x59));
    TEST_ASSERT_EQUAL_UINT8(99, clock_rtc_bcd_to_bin(0x99));
    /* Time-register masks used by the DS3231 decode path. */
    TEST_ASSERT_EQUAL_UINT8(45, clock_rtc_bcd_to_bin(0x45));
    TEST_ASSERT_EQUAL_UINT8(23, clock_rtc_bcd_to_bin(0x23 & 0x3F));
}

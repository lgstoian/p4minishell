/**
 * @file test_alarm.c
 * @brief Unit tests for the alarm module's pure time / recurrence helpers.
 *
 * The SD-backed store, checker task, and fire actions are exercised on
 * hardware (see apps/companion/alarm_test.py and command.md). These tests
 * cover the pure, hardware-free logic in components/alarm:
 *   - weekday-mask matching for weekly recurrences
 *   - recurrence advance (none / daily / weekly / monthly / yearly)
 *   - local "YYYY-MM-DD HH:MM" parsing into a Unix timestamp
 *
 * Date assertions are TZ-relative (weekday, month, day, time-of-day via
 * localtime_r) so they hold in any timezone, DST quirks aside.
 */

#include "unity.h"
#include "alarm.h"
#include <string.h>
#include <time.h>

/* ========================================================================
 * WEEKDAY MASK
 * ======================================================================== */

void test_alarm_recur_weekday_matches(void)
{
    /* Weekly mask bit 0 = Sunday .. bit 6 = Saturday. */
    uint8_t mon_wed_fri = (uint8_t)(ALARM_RECUR_WEEKLY | 0x2A);   /* Mon|Wed|Fri */

    TEST_ASSERT_TRUE(alarm_recur_weekday_matches(mon_wed_fri, 1)); /* Mon */
    TEST_ASSERT_TRUE(alarm_recur_weekday_matches(mon_wed_fri, 3)); /* Wed */
    TEST_ASSERT_TRUE(alarm_recur_weekday_matches(mon_wed_fri, 5)); /* Fri */
    TEST_ASSERT_FALSE(alarm_recur_weekday_matches(mon_wed_fri, 0));/* Sun */
    TEST_ASSERT_FALSE(alarm_recur_weekday_matches(mon_wed_fri, 2));/* Tue */
    TEST_ASSERT_FALSE(alarm_recur_weekday_matches(mon_wed_fri, 6));/* Sat */

    /* Non-weekly recurrences never match a weekday mask. */
    TEST_ASSERT_FALSE(alarm_recur_weekday_matches(ALARM_RECUR_DAILY, 1));
    TEST_ASSERT_FALSE(alarm_recur_weekday_matches(ALARM_RECUR_NONE, 1));

    /* Out-of-range weekday. */
    TEST_ASSERT_FALSE(alarm_recur_weekday_matches(mon_wed_fri, 7));
    TEST_ASSERT_FALSE(alarm_recur_weekday_matches(mon_wed_fri, -1));
}

/* ========================================================================
 * RECURRENCE ADVANCE
 * ======================================================================== */

void test_alarm_advance_daily(void)
{
    time_t t;
    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-08-18", "09:00", &t));
    /* Daily advances exactly one day (same wall-clock time, modulo DST). */
    TEST_ASSERT_TRUE(alarm_advance_recur(t, ALARM_RECUR_DAILY) > t);
    TEST_ASSERT_TRUE(alarm_advance_recur(t, ALARM_RECUR_DAILY) - t >= 86400 - 3600);
}

void test_alarm_advance_none(void)
{
    time_t t;
    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-08-18", "09:00", &t));
    /* INT32: Unity here builds without 64-bit support, and time_t is
     * 32-bit long on this target (2026 epoch fits). */
    TEST_ASSERT_EQUAL_INT32((int32_t)t, (int32_t)alarm_advance_recur(t, ALARM_RECUR_NONE));
}

void test_alarm_advance_weekly_all_days(void)
{
    time_t t;
    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-08-18", "09:00", &t));
    /* A mask covering every weekday advances one day. */
    TEST_ASSERT_TRUE(alarm_advance_recur(t, (uint8_t)(ALARM_RECUR_WEEKLY | 0x7F)) > t);
    TEST_ASSERT_TRUE(alarm_advance_recur(t, (uint8_t)(ALARM_RECUR_WEEKLY | 0x7F)) - t >= 86400 - 3600);
}

void test_alarm_advance_weekly_single_day(void)
{
    time_t t;
    struct tm tmv;

    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-08-18", "09:00", &t));
    /* Force a fixed weekday by choosing a mask that matches only the weekday
     * of the day AFTER t, then verify the advance lands on that weekday. */
    localtime_r(&t, &tmv);
    {
        int target_wday = (tmv.tm_wday + 1) % 7;
        uint8_t mask = (uint8_t)(ALARM_RECUR_WEEKLY | (1u << target_wday));
        time_t next = alarm_advance_recur(t, mask);
        struct tm ntm;

        TEST_ASSERT_TRUE(next > t);
        localtime_r(&next, &ntm);
        TEST_ASSERT_EQUAL_INT(target_wday, ntm.tm_wday);
        /* The time-of-day is preserved. */
        TEST_ASSERT_EQUAL_INT(tmv.tm_hour, ntm.tm_hour);
        TEST_ASSERT_EQUAL_INT(tmv.tm_min, ntm.tm_min);
    }
}

/* ========================================================================
 * DATE / TIME PARSING
 * ======================================================================== */

void test_alarm_parse_valid(void)
{
    time_t t;
    struct tm tmv;

    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-08-18", "09:30", &t));
    localtime_r(&t, &tmv);
    TEST_ASSERT_EQUAL_INT(2026, tmv.tm_year + 1900);
    TEST_ASSERT_EQUAL_INT(8, tmv.tm_mon + 1);
    TEST_ASSERT_EQUAL_INT(18, tmv.tm_mday);
    TEST_ASSERT_EQUAL_INT(9, tmv.tm_hour);
    TEST_ASSERT_EQUAL_INT(30, tmv.tm_min);
    TEST_ASSERT_EQUAL_INT(0, tmv.tm_sec);

    /* Seconds are optional. */
    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-01-01", "00:00", &t));
    /* Leading zeros are fine. */
    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-01-01", "07:05:09", &t));
}

void test_alarm_parse_invalid(void)
{
    time_t t = 12345;

    /* Wrong date order (the shell's date command is MM-DD-YYYY; alarm is ISO). */
    TEST_ASSERT_FALSE(alarm_parse_datetime("18-08-2026", "09:00", &t));
    /* Out-of-range fields. */
    TEST_ASSERT_FALSE(alarm_parse_datetime("2026-13-01", "09:00", &t));
    TEST_ASSERT_FALSE(alarm_parse_datetime("2026-08-32", "09:00", &t));
    TEST_ASSERT_FALSE(alarm_parse_datetime("2026-08-18", "25:00", &t));
    TEST_ASSERT_FALSE(alarm_parse_datetime("2026-08-18", "09:60", &t));
    /* Garbage. */
    TEST_ASSERT_FALSE(alarm_parse_datetime("abc", "09:00", &t));
    TEST_ASSERT_FALSE(alarm_parse_datetime("2026-08-18", "zzz", &t));
    /* NULL args. */
    TEST_ASSERT_FALSE(alarm_parse_datetime(NULL, "09:00", &t));
    TEST_ASSERT_FALSE(alarm_parse_datetime("2026-08-18", NULL, &t));
    /* Output untouched on failure (INT32: no 64-bit Unity support here). */
    TEST_ASSERT_EQUAL_INT32(12345, (int32_t)t);
}

/* ========================================================================
 * MONTHLY / YEARLY ADVANCE
 * ======================================================================== */

void test_alarm_advance_monthly_by_day(void)
{
    time_t t;
    time_t next;
    struct tm ntm;

    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-01-15", "09:00", &t));
    next = alarm_advance_monthly(t, 15, 0, 0);
    TEST_ASSERT_TRUE(next > t);
    localtime_r(&next, &ntm);
    TEST_ASSERT_EQUAL_INT(2, ntm.tm_mon + 1);
    TEST_ASSERT_EQUAL_INT(15, ntm.tm_mday);
    TEST_ASSERT_EQUAL_INT(9, ntm.tm_hour);

    /* Day 31 skips short months: Jan 31 -> Mar 31. */
    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-01-31", "09:00", &t));
    next = alarm_advance_monthly(t, 31, 0, 0);
    TEST_ASSERT_TRUE(next > t);
    localtime_r(&next, &ntm);
    TEST_ASSERT_EQUAL_INT(3, ntm.tm_mon + 1);
    TEST_ASSERT_EQUAL_INT(31, ntm.tm_mday);

    /* Zero day derives from `when`. */
    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-01-20", "09:00", &t));
    next = alarm_advance_monthly(t, 0, 0, 0);
    localtime_r(&next, &ntm);
    TEST_ASSERT_EQUAL_INT(2, ntm.tm_mon + 1);
    TEST_ASSERT_EQUAL_INT(20, ntm.tm_mday);
}

void test_alarm_advance_monthly_nth(void)
{
    time_t t;
    time_t next;
    struct tm ntm;

    /* 2026-09-15 is a Tuesday: 2nd Tuesday lands in October. */
    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-09-15", "09:00", &t));
    next = alarm_advance_monthly(t, 0, 2, 2);
    TEST_ASSERT_TRUE(next > t);
    localtime_r(&next, &ntm);
    TEST_ASSERT_EQUAL_INT(10, ntm.tm_mon + 1);
    TEST_ASSERT_EQUAL_INT(2, ntm.tm_wday);
    TEST_ASSERT_EQUAL_INT(2, (ntm.tm_mday - 1) / 7 + 1);
    TEST_ASSERT_EQUAL_INT(9, ntm.tm_hour);

    /* Last Tuesday of October 2026 is the 27th. */
    next = alarm_advance_monthly(t, 0, -1, 2);
    TEST_ASSERT_TRUE(next > t);
    localtime_r(&next, &ntm);
    TEST_ASSERT_EQUAL_INT(10, ntm.tm_mon + 1);
    TEST_ASSERT_EQUAL_INT(27, ntm.tm_mday);
}

void test_alarm_advance_yearly(void)
{
    time_t t;
    time_t next;
    struct tm ntm;

    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-09-15", "09:00", &t));
    next = alarm_advance_yearly(t, 0, 0);
    TEST_ASSERT_TRUE(next > t);
    localtime_r(&next, &ntm);
    TEST_ASSERT_EQUAL_INT(2027, ntm.tm_year + 1900);
    TEST_ASSERT_EQUAL_INT(9, ntm.tm_mon + 1);
    TEST_ASSERT_EQUAL_INT(15, ntm.tm_mday);

    /* Feb 29 skips common years: 2024 -> 2028. */
    TEST_ASSERT_TRUE(alarm_parse_datetime("2024-02-29", "09:00", &t));
    next = alarm_advance_yearly(t, 2, 29);
    TEST_ASSERT_TRUE(next > t);
    localtime_r(&next, &ntm);
    TEST_ASSERT_EQUAL_INT(2028, ntm.tm_year + 1900);
    TEST_ASSERT_EQUAL_INT(2, ntm.tm_mon + 1);
    TEST_ASSERT_EQUAL_INT(29, ntm.tm_mday);
}

void test_alarm_advance_event_dispatch(void)
{
    time_t t;
    alarm_event_t e;

    TEST_ASSERT_TRUE(alarm_parse_datetime("2026-01-15", "09:00", &t));
    memset(&e, 0, sizeof(e));
    /* Monthly with zero params derives the day from `when`. */
    e.recur = ALARM_RECUR_MONTHLY;
    {
        time_t next = alarm_advance_event(t, &e);
        struct tm ntm;
        TEST_ASSERT_TRUE(next > t);
        localtime_r(&next, &ntm);
        TEST_ASSERT_EQUAL_INT(2, ntm.tm_mon + 1);
        TEST_ASSERT_EQUAL_INT(15, ntm.tm_mday);
    }
    /* Yearly derives month and day. */
    e.recur = ALARM_RECUR_YEARLY;
    {
        time_t next = alarm_advance_event(t, &e);
        struct tm ntm;
        TEST_ASSERT_TRUE(next > t);
        localtime_r(&next, &ntm);
        TEST_ASSERT_EQUAL_INT(2027, ntm.tm_year + 1900);
        TEST_ASSERT_EQUAL_INT(1, ntm.tm_mon + 1);
    }
    /* None and NULL pass through. */
    e.recur = ALARM_RECUR_NONE;
    TEST_ASSERT_TRUE(alarm_advance_event(t, &e) == t);
    TEST_ASSERT_TRUE(alarm_advance_event(t, NULL) == t);
}

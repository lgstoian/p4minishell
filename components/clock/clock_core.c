/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file clock_core.c
 * @brief Hardware-free clock core: display formatting and the RTC restore math.
 *
 * The clock component mixes three things: a wall clock with SNTP/tz handling,
 * an NVS-backed RTC anchor with an optional external I2C chip, and the pure
 * arithmetic and formatting around them. Only the last belongs here, so the
 * host unit-test lane (test/host) can run it without NVS, I2C or a board.
 *
 * @module components/clock
 * @layer leaf
 * @domain time
 * @provides clock_format_hm_snapshot, clock_rtc_restore_math, BCD conversion
 * @status active
 * @test test/main/test_clock.c
 * @hard-rule pure C only - no NVS, I2C, SNTP, board BSP or LVGL in this file
 */

#include "clock.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define RTC_MAX_DELTA_US (10LL * 365 * 24 * 3600 * 1000000LL)

void clock_format_hm_snapshot(const struct tm *tm, char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) {
        return;
    }
    if (tm == NULL) {
        snprintf(buf, buflen, "--:--");
        return;
    }
    snprintf(buf, buflen, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

/* ========================================================================
 * Pure core (unit-tested)
 * ======================================================================== */

clock_rtc_restore_t clock_rtc_restore_math(int64_t anchor_unix, uint64_t anchor_rtc,
                                           uint64_t rtc_now, int64_t valid_epoch,
                                           int64_t *unix_out)
{
    if (unix_out == NULL) {
        return CLOCK_RTC_INVALID;
    }
    *unix_out = 0;
    if (anchor_unix < valid_epoch) {
        return CLOCK_RTC_INVALID;
    }
    if (rtc_now < anchor_rtc) {
        /* The counter reset: power loss without VBAT backup. */
        *unix_out = anchor_unix;
        return CLOCK_RTC_STALE;
    }
    {
        uint64_t delta_us = rtc_now - anchor_rtc;
        if (delta_us > (uint64_t)RTC_MAX_DELTA_US) {
            return CLOCK_RTC_INVALID;
        }
        *unix_out = anchor_unix + (int64_t)(delta_us / 1000000u);
    }
    if (*unix_out < valid_epoch) {
        return CLOCK_RTC_INVALID;
    }
    return CLOCK_RTC_RESTORED;
}

uint8_t clock_rtc_bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10) + (bcd & 0x0F));
}

uint8_t clock_rtc_bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10) << 4) | (bin % 10));
}

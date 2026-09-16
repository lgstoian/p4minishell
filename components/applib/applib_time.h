/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file applib_time.h
 * @brief Time, timer, sleep, and system-information group of the applib.
 */

#ifndef P4MINISHELL_APPLIB_TIME_H
#define P4MINISHELL_APPLIB_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Current Unix time (seconds since epoch; 0 before the clock is set). */
time_t app_time(void);

/** Current local time in the configured timezone. */
struct tm app_time_local(void);

/** Current UTC time. */
struct tm app_time_utc(void);

/** Seconds since boot. */
uint32_t app_uptime_sec(void);

/** Monotonic milliseconds since boot (esp_timer). */
int64_t app_now_ms(void);

/** Block the calling task for @p ms milliseconds (FreeRTOS tick delay). */
void app_delay_ms(uint32_t ms);

/** True when the SNTP clock has synchronized at least once. */
bool app_time_synced(void);

/** Format the uptime as "Xd Xh Xm Xs" (days included when non-zero). */
void app_uptime_formatted(char *buf, size_t buflen);

/**
 * Compact system summary into @p buf: free/total heap, internal free, PSRAM
 * free, uptime, clock-sync state, and Wi-Fi state. Sized for a single line.
 */
void app_sysinfo(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_TIME_H */

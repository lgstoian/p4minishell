/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file header_refresh.h
 * @brief Pure adaptive-cadence policy for the header status poll.
 *
 * The header is polled by one LVGL timer. A fixed 5 s period is both too slow
 * to reflect hot-plug/connect changes and slow to wake the display from
 * idle-off, so the next interval is chosen from the current situation. This
 * module is pure (no LVGL, no I/O) and unit-tested; the shell assembles the
 * inputs and main reschedules the timer with the result.
 *
 * Priority (first match wins):
 *   1. display off by idle -> WAKE      (prompt touch wake)
 *   2. OTA/bg job active   -> BUSY      (activity feedback)
 *   3. Wi-Fi starting      -> CONNECTING
 *   4. early boot          -> STARTUP
 *   5. otherwise           -> IDLE, clamped down to the next minute boundary
 *                             (exact HH:MM clock) and to the pending
 *                             idle-display-off deadline.
 */

#ifndef P4MINISHELL_HEADER_REFRESH_H
#define P4MINISHELL_HEADER_REFRESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Situation inputs for one scheduling decision. */
typedef struct {
    bool display_off;             /**< Backlight off by idle. */
    bool busy;                    /**< C6 OTA or a background job is active. */
    bool wifi_connecting;         /**< Wi-Fi stack starting, not yet associated. */
    bool startup;                 /**< Within the early-boot grace window. */
    bool clock_enabled;           /**< Clock shown AND synchronized. */
    int seconds_to_next_minute;   /**< 1..60 when clock-aligned, else 0. */
    int ms_to_idle_off;           /**< >0 when an idle-off deadline is pending. */
} header_refresh_state_t;

/** Resolve the next poll interval in milliseconds (always > 0). */
uint32_t header_refresh_interval_ms(const header_refresh_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_HEADER_REFRESH_H */

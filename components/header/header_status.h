/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file header_status.h
 * @brief Pure status-indicator mapping for the top status bar.
 *
 * Turns raw peripheral/system state into a compact ASCII glyph and a semantic
 * color tone. It contains NO LVGL and NO I/O, so the classification thresholds
 * live in exactly one place and are unit-tested (test/main/test_header.c).
 * header.c owns the widgets and maps the tone onto a theme color.
 *
 * This module is the SINGLE source of truth for:
 *   - the Wi-Fi signal label (HI/MID/LOW/WEAK) and its color tone
 *   - the on/off/error classification for Bluetooth, USB, and SD
 *   - the healthy/warn/critical thresholds for memory, CPU, and battery
 *   - the one-letter glyph each indicator shows in the compact style
 */

#ifndef P4MINISHELL_HEADER_STATUS_H
#define P4MINISHELL_HEADER_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "header.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Semantic color tone for one indicator (header.c maps this to a theme color). */
typedef enum {
    HEADER_TONE_MUTED = 0, /**< dim / inactive but not a fault (e.g. no SD card). */
    HEADER_TONE_OK,        /**< healthy (green). */
    HEADER_TONE_WARN,      /**< degraded (amber). */
    HEADER_TONE_ERR,       /**< off or failed (red). */
} header_tone_t;

/** Fixed compact glyph for an indicator (always ASCII). */
const char *header_status_glyph(header_status_kind_t kind);

/** Verbose Wi-Fi quality label for the words style (HI/MID/LOW/WEAK). */
const char *header_status_wifi_label(int rssi);

/** Wi-Fi tone: off = red, weak = amber, usable = green. */
header_tone_t header_status_wifi_tone(bool connected, int rssi);

/** Bluetooth tone: connected = green, enabled/idle = amber, off = red. */
header_tone_t header_status_bluetooth_tone(bool enabled, bool connected);

/** USB tone: connected = green, off = red. */
header_tone_t header_status_usb_tone(bool connected);

/** SD tone: mounted = green, inserted = amber, error = red, none = muted. */
header_tone_t header_status_sd_tone(header_sd_state_t state);

/** Memory tone from the free percentage: healthy = green, low = amber, crit = red. */
header_tone_t header_status_mem_tone(uint32_t free_bytes, uint32_t total_bytes);

/** CPU tone: below warn = green, warn = amber, crit = red. */
header_tone_t header_status_cpu_tone(int cpu_percent);

/** Battery tone: > low = green, low = amber, crit = red, no ADC = muted. */
header_tone_t header_status_battery_tone(int percent, bool adc_ready);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_HEADER_STATUS_H */

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file header_status.c
 * @brief Pure status-indicator mapping for the top status bar.
 *
 * Thresholds are read from p4minishell_config.h so the pure policy and the
 * widgets can never disagree. No LVGL, no allocation, no I/O.
 */

#include "header_status.h"
#include "p4minishell_config.h"

const char *header_status_glyph(header_status_kind_t kind)
{
    switch (kind) {
    case HEADER_STATUS_WIFI:      return "W";
    case HEADER_STATUS_BLUETOOTH: return "BT";
    case HEADER_STATUS_USB:       return "U";
    case HEADER_STATUS_SD:        return "S";
    case HEADER_STATUS_ACTIVITY:  return "A";
    case HEADER_STATUS_MEM:       return "M";
    case HEADER_STATUS_CPU:       return "C";
    case HEADER_STATUS_BATTERY:   return "B";
    default:                      return "?";
    }
}

const char *header_status_wifi_label(int rssi)
{
    if (rssi >= P4_CONFIG_HEADER_RSSI_STRONG) return "HI";
    if (rssi >= P4_CONFIG_HEADER_RSSI_GOOD)   return "MID";
    if (rssi >= P4_CONFIG_HEADER_RSSI_WEAK)   return "LOW";
    return "WEAK";
}

header_tone_t header_status_wifi_tone(bool connected, int rssi)
{
    if (!connected) {
        return HEADER_TONE_ERR;
    }
    /* Associated but with no usable RSSI (the hosted get_ap_info RPC reports 0,
     * and the cached scan record may be missing): show the connected colour
     * instead of a false error. */
    if (rssi == P4_CONFIG_HEADER_RSSI_UNKNOWN) {
        return HEADER_TONE_OK;
    }
    if (rssi >= P4_CONFIG_HEADER_RSSI_GOOD) {
        return HEADER_TONE_OK;
    }
    if (rssi >= P4_CONFIG_HEADER_RSSI_WEAK) {
        return HEADER_TONE_WARN;
    }
    return HEADER_TONE_ERR;
}

header_tone_t header_status_bluetooth_tone(bool enabled, bool connected)
{
    if (!enabled) {
        return HEADER_TONE_ERR;
    }
    return connected ? HEADER_TONE_OK : HEADER_TONE_WARN;
}

header_tone_t header_status_usb_tone(bool connected)
{
    return connected ? HEADER_TONE_OK : HEADER_TONE_ERR;
}

header_tone_t header_status_sd_tone(header_sd_state_t state)
{
    switch (state) {
    case HEADER_SD_MOUNTED:  return HEADER_TONE_OK;
    case HEADER_SD_INSERTED: return HEADER_TONE_WARN;
    case HEADER_SD_ERROR:    return HEADER_TONE_ERR;
    case HEADER_SD_NONE:
    default:                 return HEADER_TONE_MUTED;
    }
}

header_tone_t header_status_mem_tone(uint32_t free_bytes, uint32_t total_bytes)
{
    int percent;

    if (total_bytes == 0) {
        return HEADER_TONE_MUTED;
    }
    percent = (int)((free_bytes * 100u) / total_bytes);
    if (percent <= P4_CONFIG_HEADER_MEM_CRIT_PCT) {
        return HEADER_TONE_ERR;
    }
    if (percent <= P4_CONFIG_HEADER_MEM_LOW_PCT) {
        return HEADER_TONE_WARN;
    }
    return HEADER_TONE_OK;
}

header_tone_t header_status_cpu_tone(int cpu_percent)
{
    if (cpu_percent >= P4_CONFIG_HEADER_CPU_CRIT_PCT) {
        return HEADER_TONE_ERR;
    }
    if (cpu_percent >= P4_CONFIG_HEADER_CPU_WARN_PCT) {
        return HEADER_TONE_WARN;
    }
    return HEADER_TONE_OK;
}

header_tone_t header_status_battery_tone(int percent, bool adc_ready)
{
    if (!adc_ready) {
        return HEADER_TONE_MUTED;
    }
    if (percent <= P4_CONFIG_HEADER_BAT_CRIT_PCT) {
        return HEADER_TONE_ERR;
    }
    if (percent <= P4_CONFIG_HEADER_BAT_LOW_PCT) {
        return HEADER_TONE_WARN;
    }
    return HEADER_TONE_OK;
}

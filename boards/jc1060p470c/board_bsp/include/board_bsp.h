/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file board_bsp.h
 * @brief Board BSP compatibility shim - ESP32-P4 Function EV Board.
 *
 * The firmware links against the generic `board_bsp` component, whose active
 * directory is selected by -DP4_BOARD=. This header exposes the handful of
 * macros that let board-agnostic code (display/storage/clock) reach the
 * underlying BSP without knowing which one is in the build.
 *
 * For the EV board the managed BSP exports a global `bsp_sdcard` pointer and a
 * `bsp_display_cfg_t` that carries a full `hw_cfg` block.
 */

#pragma once

#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "esp_sleep.h"

/** SD card handle accessor (the EV BSP exposes the global directly). */
#define P4_BSP_SDCARD bsp_sdcard

/** Whether bsp_display_cfg_t carries the `hw_cfg` (DSI/HDMI) sub-struct. */
#define P4_BSP_DISPLAY_CFG_HAS_HW_CFG 1

/** Human-readable panel / touch driver names for diagnostics. */
#define P4_BSP_PANEL_DRIVER "JD9165"
#define P4_BSP_TOUCH_DRIVER "GT911"

/**
 * @brief Board power/enable rails that must be asserted before the C6 hosted
 *        transport and USB host come up.
 *
 * The reference board's co-processor and USB rails are not expander-gated, so
 * this is a no-op. Kept for API symmetry with gated boards (see the Tab5
 * profile).
 */
static inline esp_err_t board_bsp_early_init(void)
{
    return ESP_OK;
}

/**
 * @brief Cut the board's power.
 *
 * The reference board has no software power latch, so this enters deep sleep
 * (the lowest power state it can reach). Never returns.
 */
static inline esp_err_t board_bsp_poweroff(void)
{
    esp_deep_sleep_start();
    return ESP_OK; /* unreachable */
}

/**
 * @brief Enable/disable battery charging.
 *
 * The reference board has no software charge gate (and no pack gauge), so this
 * is a no-op.
 */
static inline esp_err_t board_bsp_charge_enable(bool enable)
{
    (void)enable;
    return ESP_OK;
}

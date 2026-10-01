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

#include <stdbool.h>
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

/**
 * @brief Read the 3.5 mm headphone-detect line.
 *
 * The reference board has no headphone jack; @p inserted_out is set to false
 * and ESP_ERR_NOT_SUPPORTED is returned (see the Tab5 profile for the real one).
 * The audio layer treats this as "route always speaker".
 */
static inline esp_err_t bsp_audio_headphone_detected(bool *inserted_out)
{
    if (inserted_out != NULL) {
        *inserted_out = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Enable/disable the speaker power amplifier.
 *
 * The reference board's amp is owned by the ES8311 codec driver (GPIO20 PA
 * pin), not the shell; ESP_ERR_NOT_SUPPORTED is returned and the audio layer
 * leaves amp control to the driver.
 */
static inline esp_err_t bsp_audio_speaker_enable(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Post-open fixup for the speaker codec.
 *
 * Tab5-only (separate ADC/DAC frame clocks, F28). No-op here.
 */
static inline esp_err_t bsp_audio_codec_speaker_post_open(esp_codec_dev_handle_t dev)
{
    (void)dev;
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Dump speaker codec registers for diagnostics.
 *
 * Tab5-only (F28). No-op here.
 */
static inline esp_err_t bsp_audio_codec_speaker_dump_regs(uint8_t out[50])
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Read the board's charge-status line (corroborates pack presence).
 *
 * The reference board has no charge-status line; @p level_out is set to -1 and
 * ESP_ERR_NOT_SUPPORTED is returned (see the Tab5 profile for the real one).
 */
static inline esp_err_t board_bsp_charge_status_level(int *level_out)
{
    if (level_out != NULL) {
        *level_out = -1;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

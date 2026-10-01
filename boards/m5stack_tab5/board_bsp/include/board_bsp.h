/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file board_bsp.h
 * @brief Board BSP compatibility shim - M5Stack Tab5.
 *
 * The firmware links against the generic `board_bsp` component, whose active
 * directory is selected by -DP4_BOARD=. This header exposes the handful of
 * macros that let board-agnostic code (display/storage/clock) reach the
 * underlying BSP without knowing which one is in the build.
 *
 * The Tab5 BSP keeps its SD card handle private (use bsp_sdcard_get_handle())
 * and its `bsp_display_cfg_t` has no `hw_cfg` member; the DSI bus is configured
 * internally by the BSP.
 */

#pragma once

#include <stdbool.h>
#include "board_config.h"
#include "bsp/esp-bsp.h"

/** SD card handle accessor (the Tab5 BSP keeps the pointer private). */
#define P4_BSP_SDCARD bsp_sdcard_get_handle()

/** Whether bsp_display_cfg_t carries the `hw_cfg` (DSI/HDMI) sub-struct. */
#define P4_BSP_DISPLAY_CFG_HAS_HW_CFG 0

/** Human-readable panel / touch driver names for diagnostics. */
#define P4_BSP_PANEL_DRIVER "ST7123"
#define P4_BSP_TOUCH_DRIVER "ST7123"

/**
 * @brief Board power/enable rails that must be asserted before the C6 hosted
 *        transport and USB host come up.
 *
 * On the Tab5 the co-processor and USB power are gated behind the PI4IOE5V6408
 * IO expander, so both features must be enabled before esp_hosted / USB host
 * init; otherwise SDIO card init fails. On boards without gated rails this is a
 * no-op. Call once from app_main before networking_init()/usb_init().
 */
static inline esp_err_t board_bsp_early_init(void)
{
    esp_err_t wifi = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    esp_err_t usb = bsp_feature_enable(BSP_FEATURE_USB, true);
    return (wifi != ESP_OK) ? wifi : usb;
}

/**
 * @brief Cut the board's main power.
 *
 * Pules the PMIC power-off latch on the Tab5. On success the board powers off
 * within ~0.5 s and this call never returns; it returns ESP_OK only if the latch
 * did not take effect (so the caller can fall back).
 */
static inline esp_err_t board_bsp_poweroff(void)
{
    bsp_generate_poweroff_signal();
    return ESP_OK;
}

/**
 * @brief Enable/disable charging the battery pack.
 *
 * Charging is gated off after reset on the Tab5, so the firmware enables it at
 * boot. This only gates the charge path; charge voltage/current stay under the
 * PMIC's own control.
 */
static inline esp_err_t board_bsp_charge_enable(bool enable)
{
    bsp_set_charge_qc_en(enable);
    bsp_set_charge_en(enable);
    return ESP_OK;
}

/**
 * @brief Read the 3.5 mm headphone-detect line.
 *
 * On the Tab5 this is HP_DET on the first IO expander (0x43 P7), active-high
 * when a plug is inserted. @p inserted_out receives true/false. Boards without
 * a jack return ESP_ERR_NOT_SUPPORTED and leave @p inserted_out false.
 */
esp_err_t bsp_audio_headphone_detected(bool *inserted_out);

/**
 * @brief Enable/disable the speaker power amplifier.
 *
 * On the Tab5 this drives NS4150B SPK_EN (0x43 P1). Boards whose amp is owned
 * by the codec driver return ESP_ERR_NOT_SUPPORTED; the audio layer tolerates
 * that and leaves amp control to the driver.
 */
esp_err_t bsp_audio_speaker_enable(bool enable);

/**
 * @brief Post-open fixup for the speaker codec (F28).
 *
 * Re-selects separate ADC/DAC frame clocks (ES8388 DACCONTROL21 = 0x08,
 * matching M5Unified's proven Tab5 sequence) after every
 * `esp_codec_dev_open()`: the vendored driver leaves the DAC waiting on a
 * shared ADC LRCK while the ADC block stays powered down, so the DAC never
 * converts. Best-effort; the audio layer ignores failures. Call after a
 * successful open, before enabling the amp.
 */
esp_err_t bsp_audio_codec_speaker_post_open(esp_codec_dev_handle_t dev);

/**
 * @brief Dump ES8388 registers 0x00..0x31 for diagnostics (F28).
 *
 * Reads the live DAC power/mute/mixer/clock state through the fixup control
 * handle; used by `audio diag` to prove the chip configuration on hardware.
 */
esp_err_t bsp_audio_codec_speaker_dump_regs(uint8_t out[50]);

/**
 * @brief Read the board's charge-status line (corroborates pack presence).
 *
 * On the Tab5 this is the IP2326 CHG_STAT_LED on the second IO expander
 * (0x44 P6). @p level_out receives the raw pin level (0/1). Boards without a
 * status line return ESP_ERR_NOT_SUPPORTED and leave @p level_out at -1.
 */
static inline esp_err_t board_bsp_charge_status_level(int *level_out)
{
    int level;

    if (level_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    level = bsp_get_charge_status_level();
    *level_out = level;
    return (level < 0) ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

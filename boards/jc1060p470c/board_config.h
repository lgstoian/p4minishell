/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file board_config.h
 * @brief Hardware configuration constants for the P4MiniShell board baseline.
 *
 * Single source of truth for all hardware pin assignments, display timing,
 * touch configuration, battery ADC parameters, and storage mount points.
 * Generated from board_config.yaml - do not edit manually without updating
 * the YAML source as well.
 *
 * Board baseline: ESP32-P4-Function-EV-Board (requested: JC1060P470C)
 * Target: esp32p4
 * Display: JD9165 1024x600 MIPI-DSI
 * Touch: GT911 via I2C
 */

#pragma once

#include "driver/gpio.h"

// ---- Board identity ----
// BOARD_CFG_ID is the machine-readable profile slug used by the host tools to
// map a COM port to a board (see tools/board_ports.py + PORTING.md). It must
// match the boards/<name>/ directory name and the -DP4_BOARD= value.
#define BOARD_CFG_ID "jc1060p470c"
#define BOARD_CFG_NAME "JC1060P470C"
#define BOARD_CFG_DETECTED_NAME "ESP32-P4-Function-EV-Board"

// ---- BSP backend selector ----
// 0 = ESP32-P4 Function EV Board managed BSP (espressif__esp32_p4_function_ev_board).
// Set to 1 on the M5Stack Tab5 profile. Consumed by board_bsp.h and the
// board-conditional adapters in display/storage/clock.
#define BOARD_CFG_BSP_M5STACK_TAB5 0

// ---- I2C bus (shared by GT911 touch and onboard peripherals) ----
#define BOARD_CFG_I2C_PORT 1
#define BOARD_CFG_I2C_SDA_GPIO GPIO_NUM_7
#define BOARD_CFG_I2C_SCL_GPIO GPIO_NUM_8
#define BOARD_CFG_I2C_CLK_SPEED_HZ 400000
#define BOARD_CFG_I2C_ENABLE_INTERNAL_PULLUP 1

#define BOARD_CFG_I2S_PORT 1
#define BOARD_CFG_I2S_SCLK_GPIO GPIO_NUM_12
#define BOARD_CFG_I2S_MCLK_GPIO GPIO_NUM_13
#define BOARD_CFG_I2S_LCLK_GPIO GPIO_NUM_10
#define BOARD_CFG_I2S_DOUT_GPIO GPIO_NUM_9
#define BOARD_CFG_I2S_DSIN_GPIO GPIO_NUM_11
#define BOARD_CFG_POWER_AMP_GPIO GPIO_NUM_20

#define BOARD_CFG_LCD_BACKLIGHT_GPIO GPIO_NUM_23
#define BOARD_CFG_LCD_RST_GPIO GPIO_NUM_27
#define BOARD_CFG_LCD_TOUCH_RST_GPIO GPIO_NUM_NC
#define BOARD_CFG_LCD_TOUCH_INT_GPIO GPIO_NUM_NC

#define BOARD_CFG_LCD_WIDTH 1024
#define BOARD_CFG_LCD_HEIGHT 600
#define BOARD_CFG_DISPLAY_DEFAULT_ROTATION 0
#define BOARD_CFG_LCD_PIXEL_CLOCK_MHZ 80
#define BOARD_CFG_LCD_HSYNC 1344
#define BOARD_CFG_LCD_HBP 160
#define BOARD_CFG_LCD_HFP 160
#define BOARD_CFG_LCD_VSYNC 635
#define BOARD_CFG_LCD_VBP 23
#define BOARD_CFG_LCD_VFP 12
#define BOARD_CFG_LCD_MIPI_DSI_LANE_NUM 2
#define BOARD_CFG_LCD_MIPI_DSI_LANE_BITRATE_MBPS_MACRO 1000
#define BOARD_CFG_LCD_DSI_BUS_LANE_BITRATE_MBPS_RUNTIME 550
#define BOARD_CFG_LCD_TYPE_1024_600 1
#define BOARD_CFG_LCD_COLOR_FORMAT_RGB888 0
#define BOARD_CFG_LCD_PANEL_SUPPORTS_HW_SWAP_XY 0

#define BOARD_CFG_LCD_DRAW_BUFFER_SIZE (BOARD_CFG_LCD_WIDTH * 50)
#define BOARD_CFG_LCD_DRAW_BUFFER_DOUBLE 0
#define BOARD_CFG_LCD_DPI_BUFFER_NUMS 1

#define BOARD_CFG_DISPLAY_BRIGHTNESS_LEDC_CH 1
#define BOARD_CFG_LCD_BACKLIGHT_USE_LEDC_PWM 1
#define BOARD_CFG_LCD_BACKLIGHT_PWM_TIMER 1
#define BOARD_CFG_LCD_BACKLIGHT_PWM_FREQ_HZ 5000
#define BOARD_CFG_LCD_BACKLIGHT_PWM_RESOLUTION_BITS 10

#define BOARD_CFG_TOUCH_SWAP_XY 0
#define BOARD_CFG_TOUCH_MIRROR_X 0
#define BOARD_CFG_TOUCH_MIRROR_Y 0

#define BOARD_CFG_BATTERY_ADC_GPIO GPIO_NUM_53
/* Preprocessor-friendly battery-sense presence flag (GPIO_NUM_NC is an enum,
 * so it cannot be compared in an #if). 1 = an ADC divider is wired. */
#define BOARD_CFG_BATTERY_ADC_PRESENT 1
#define BOARD_CFG_BATTERY_DIVIDER_NUMERATOR 2
#define BOARD_CFG_BATTERY_DIVIDER_DENOMINATOR 1
#define BOARD_CFG_BATTERY_EMPTY_MV 3300
#define BOARD_CFG_BATTERY_FULL_MV 4200
/* Below this scaled pack voltage the sense input is treated as unconnected
 * (the divider floats well under any real pack), so telemetry reports
 * "BAT N/C" instead of a bogus 0%. */
#define BOARD_CFG_BATTERY_PRESENT_MV 2500

#define BOARD_CFG_RGB_LED_GPIO GPIO_NUM_26
#define BOARD_CFG_RGB_LED_IS_WS2812 1

#define BOARD_CFG_CAMERA_SUPPORTED 0
#define BOARD_CFG_IMU_SUPPORTED 0

#define BOARD_CFG_APP_BUFFER_DMA 1
#define BOARD_CFG_APP_BUFFER_SPIRAM 1
#define BOARD_CFG_APP_SW_ROTATE 1
#define BOARD_CFG_BSP_DEFAULT_SW_ROTATE 1
#define BOARD_CFG_LVGL_AVOID_TEAR 0
#define BOARD_CFG_LVGL_FULL_REFRESH 0
#define BOARD_CFG_LVGL_DIRECT_MODE 0

#define BOARD_CFG_SD_MOUNT_POINT "/sdcard"
#define BOARD_CFG_SD_FORMAT_ON_MOUNT_FAIL 0
#define BOARD_CFG_SPIFFS_MOUNT_POINT "/spiffs"
#define BOARD_CFG_SPIFFS_PARTITION_LABEL "storage"
#define BOARD_CFG_SPIFFS_MAX_FILES 5
#define BOARD_CFG_SPIFFS_FORMAT_ON_MOUNT_FAIL 0

// ---- External RTC ----
// No external RTC is fitted on the reference board; the clock falls back to the
// ESP32-P4 internal RTC + NVS anchor. See P4_CONFIG_RTC_EXT_* in
// p4minishell_config.h for the DS3231-class hook.
#define BOARD_CFG_RTC_EXT_ENABLE 0
#define BOARD_CFG_RTC_USE_BSP_I2C 0
#define BOARD_CFG_RTC_EXT_ADDR 0x68
#define BOARD_CFG_RTC_EXT_SDA (-1)
#define BOARD_CFG_RTC_EXT_SCL (-1)
#define BOARD_CFG_RTC_EXT_PORT 0

// ---- Tab5Keyboard module (not present on this board) ----
#define BOARD_CFG_TAB5KBD_PRESENT 0
#define BOARD_CFG_TAB5KBD_I2C_ADDR 0x6D
#define BOARD_CFG_TAB5KBD_INT_GPIO GPIO_NUM_NC

// ---- MicroSD SDMMC pins (slot 0, 4-bit) ----
// Consumed by the BSP uSD block (BSP_SD_*); the mount point above stays the
// only thing most firmware code touches.
#define BOARD_CFG_SD_D0_GPIO GPIO_NUM_39
#define BOARD_CFG_SD_D1_GPIO GPIO_NUM_40
#define BOARD_CFG_SD_D2_GPIO GPIO_NUM_41
#define BOARD_CFG_SD_D3_GPIO GPIO_NUM_42
#define BOARD_CFG_SD_CMD_GPIO GPIO_NUM_44
#define BOARD_CFG_SD_CLK_GPIO GPIO_NUM_43

// ---- ESP-Hosted SDIO link (ESP32-C6 co-processor, slot 1) ----
// The SDIO bus pins themselves live in sdkconfig (CONFIG_ESP_HOSTED_HOST_SDIO_*
// Kconfig); these macros mirror them for the GPIO table, `gpio`/`i2c` guards,
// and status output so a board port keeps one authoritative pin list.
#define BOARD_CFG_HOSTED_SDIO_D0_GPIO GPIO_NUM_14
#define BOARD_CFG_HOSTED_SDIO_D1_GPIO GPIO_NUM_15
#define BOARD_CFG_HOSTED_SDIO_D2_GPIO GPIO_NUM_16
#define BOARD_CFG_HOSTED_SDIO_D3_GPIO GPIO_NUM_17
#define BOARD_CFG_HOSTED_SDIO_CLK_GPIO GPIO_NUM_18
#define BOARD_CFG_HOSTED_SDIO_CMD_GPIO GPIO_NUM_19
#define BOARD_CFG_HOSTED_SDIO_SLOT 1
// C6 reset line. Must match CONFIG_ESP_HOSTED_HOST_RESET_GPIO in sdkconfig.
#define BOARD_CFG_C6_HOST_RESET_GPIO GPIO_NUM_54
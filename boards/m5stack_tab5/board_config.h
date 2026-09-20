/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file board_config.h
 * @brief Hardware configuration constants for the M5Stack Tab5 board profile.
 *
 * Single source of truth for all hardware pin assignments, display timing,
 * touch configuration, battery ADC parameters, and storage mount points.
 * Generated from board_config.yaml - do not edit manually without updating
 * the YAML source as well.
 *
 * Board baseline: M5Stack Tab5 (ESP32-P4 + ESP32-C6-MINI-1U)
 * Target: esp32p4
 * Display: MIPI-DSI 1280x720, auto-detected ILI9881C / ST7123 / ST7121 TDDI
 * Touch: GT911 (ILI9881C units) or integrated Sitronix TDDI (ST7123/ST7121)
 *
 * This profile is backed by the official espressif/m5stack_tab5 BSP (vendored
 * in-tree under board_bsp). The Tab5 display/touch pins are owned by the BSP
 * and its IO expander, so the BOARD_CFG_LCD and BOARD_CFG_TOUCH values here
 * are informational (used by diagnostics and the reserved-pin table) rather
 * than consumed by the BSP; the BSP probes the panel revision itself.
 */

#pragma once

#include "driver/gpio.h"

// ---- Board identity ----
// BOARD_CFG_ID is the machine-readable profile slug used by the host tools to
// map a COM port to a board (see tools/board_ports.py + PORTING.md). It must
// match the boards/<name>/ directory name and the -DP4_BOARD= value.
#define BOARD_CFG_ID "m5stack_tab5"
#define BOARD_CFG_NAME "M5Stack Tab5"
#define BOARD_CFG_DETECTED_NAME "M5Stack Tab5"

// ---- BSP backend selector ----
// 0 = ESP32-P4 Function EV Board managed BSP (espressif__esp32_p4_function_ev_board).
// Set to 1 on the M5Stack Tab5 profile. Consumed by board_bsp.h and the
// board-conditional adapters in display/storage/clock.
#define BOARD_CFG_BSP_M5STACK_TAB5 1

// ---- I2C bus (shared by touch, codec, IO expander, RTC) ----
#define BOARD_CFG_I2C_PORT 1
#define BOARD_CFG_I2C_SDA_GPIO GPIO_NUM_31
#define BOARD_CFG_I2C_SCL_GPIO GPIO_NUM_32
#define BOARD_CFG_I2C_CLK_SPEED_HZ 400000
#define BOARD_CFG_I2C_ENABLE_INTERNAL_PULLUP 1

// ---- I2S audio (ES8388 codec + ES7210 AEC front end) ----
#define BOARD_CFG_I2S_PORT 1
#define BOARD_CFG_I2S_MCLK_GPIO GPIO_NUM_30
#define BOARD_CFG_I2S_SCLK_GPIO GPIO_NUM_27
#define BOARD_CFG_I2S_LCLK_GPIO GPIO_NUM_29
#define BOARD_CFG_I2S_DOUT_GPIO GPIO_NUM_26
#define BOARD_CFG_I2S_DSIN_GPIO GPIO_NUM_28
// The speaker amplifier enable is on the IO expander, not a GPIO; the BSP owns it.
#define BOARD_CFG_POWER_AMP_GPIO GPIO_NUM_NC

// ---- Display / touch (owned by the BSP + IO expander on this board) ----
#define BOARD_CFG_LCD_BACKLIGHT_GPIO GPIO_NUM_22
#define BOARD_CFG_LCD_RST_GPIO GPIO_NUM_NC
#define BOARD_CFG_LCD_TOUCH_RST_GPIO GPIO_NUM_NC
#define BOARD_CFG_LCD_TOUCH_INT_GPIO GPIO_NUM_23

// Panel revision override. 0 = auto-detect (probe the TDDI/GT911), 1 = force
// ILI9881C + GT911, 2 = force ST7123 TDDI, 3 = force ST7121 TDDI. The Tab5
// shipped three revisions and the integrated TDDI only answers on I2C after its
// power rails settle, so a field unit whose auto-detect fails can be pinned
// here from `display info` / the boot log rather than patching code.
#define BOARD_CFG_LCD_FORCE_VERSION 0

/* The MIPI-DSI panel is natively 720x1280 (portrait); the Tab5 is used in
 * landscape, so the display manager rotates it 90 degrees at boot. */
#define BOARD_CFG_LCD_WIDTH 720
#define BOARD_CFG_LCD_HEIGHT 1280
#define BOARD_CFG_DISPLAY_DEFAULT_ROTATION 90
#define BOARD_CFG_LCD_PIXEL_CLOCK_MHZ 70
#define BOARD_CFG_LCD_HSYNC 1360
#define BOARD_CFG_LCD_HBP 40
#define BOARD_CFG_LCD_HFP 40
#define BOARD_CFG_LCD_VSYNC 730
#define BOARD_CFG_LCD_VBP 8
#define BOARD_CFG_LCD_VFP 220
#define BOARD_CFG_LCD_MIPI_DSI_LANE_NUM 2
#define BOARD_CFG_LCD_MIPI_DSI_LANE_BITRATE_MBPS_MACRO 1000
#define BOARD_CFG_LCD_DSI_BUS_LANE_BITRATE_MBPS_RUNTIME 1000
#define BOARD_CFG_LCD_TYPE_1024_600 0
#define BOARD_CFG_LCD_COLOR_FORMAT_RGB888 0
#define BOARD_CFG_LCD_PANEL_SUPPORTS_HW_SWAP_XY 0

#define BOARD_CFG_LCD_DRAW_BUFFER_SIZE (BOARD_CFG_LCD_WIDTH * 50)
#define BOARD_CFG_LCD_DRAW_BUFFER_DOUBLE 0
#define BOARD_CFG_LCD_DPI_BUFFER_NUMS 1

#define BOARD_CFG_DISPLAY_BRIGHTNESS_LEDC_CH 1
#define BOARD_CFG_LCD_BACKLIGHT_USE_LEDC_PWM 1
#define BOARD_CFG_LCD_BACKLIGHT_PWM_TIMER 0
#define BOARD_CFG_LCD_BACKLIGHT_PWM_FREQ_HZ 5000
#define BOARD_CFG_LCD_BACKLIGHT_PWM_RESOLUTION_BITS 10

#define BOARD_CFG_TOUCH_SWAP_XY 0
#define BOARD_CFG_TOUCH_MIRROR_X 0
#define BOARD_CFG_TOUCH_MIRROR_Y 0

// ---- Battery: Tab5 has an INA226 fuel gauge on the SYS I2C bus ----
// The ADC sense pin is not wired; the pack voltage/current come from the
// INA226 (addr 0x41, 5 mOhm shunt) on the shared SYS I2C bus (GPIO31/32).
// Telemetry is READ-ONLY: the firmware never toggles the charge rails, so it
// cannot damage the pack.
#define BOARD_CFG_BATTERY_ADC_GPIO GPIO_NUM_NC
/* Preprocessor-friendly battery-sense presence flag (GPIO_NUM_NC is an enum,
 * so it cannot be compared in an #if). 0 = no ADC divider wired. */
#define BOARD_CFG_BATTERY_ADC_PRESENT 0
#define BOARD_CFG_BATTERY_INA226_PRESENT 1
#define BOARD_CFG_BATTERY_INA226_ADDR 0x41
#define BOARD_CFG_BATTERY_INA226_SHUNT_MILLIOHM 5
#define BOARD_CFG_BATTERY_INA226_MAX_CURRENT_MA 8192
#define BOARD_CFG_BATTERY_DIVIDER_NUMERATOR 1
#define BOARD_CFG_BATTERY_DIVIDER_DENOMINATOR 1
/* NP-F550 class pack: 2S Li-ion (7.4 V nominal, 8.4 V full, ~6.0 V empty). */
#define BOARD_CFG_BATTERY_EMPTY_MV 6000
#define BOARD_CFG_BATTERY_FULL_MV 8400
#define BOARD_CFG_BATTERY_PRESENT_MV 5000

// ---- RGB: the Tab5 status LEDs live on the Tab5Keyboard module ----
// There is no WS2812 on the main board; the two keyboard RGB LEDs are driven
// over I2C through components/tab5kbd. The `rgb` command and the auto status
// colour route there when the module is attached.
#define BOARD_CFG_RGB_LED_GPIO GPIO_NUM_NC
#define BOARD_CFG_RGB_LED_IS_WS2812 0
#define BOARD_CFG_RGB_VIA_TAB5KBD 1

#define BOARD_CFG_CAMERA_SUPPORTED 1
#define BOARD_CFG_IMU_SUPPORTED 1

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

// ---- MicroSD SDMMC pins (slot 0, 4-bit) ----
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
#define BOARD_CFG_HOSTED_SDIO_D0_GPIO GPIO_NUM_11
#define BOARD_CFG_HOSTED_SDIO_D1_GPIO GPIO_NUM_10
#define BOARD_CFG_HOSTED_SDIO_D2_GPIO GPIO_NUM_9
#define BOARD_CFG_HOSTED_SDIO_D3_GPIO GPIO_NUM_8
#define BOARD_CFG_HOSTED_SDIO_CLK_GPIO GPIO_NUM_12
#define BOARD_CFG_HOSTED_SDIO_CMD_GPIO GPIO_NUM_13
#define BOARD_CFG_HOSTED_SDIO_SLOT 1
// C6 reset line. Must match CONFIG_ESP_HOSTED_HOST_RESET_GPIO in sdkconfig.
#define BOARD_CFG_C6_HOST_RESET_GPIO GPIO_NUM_15

// ---- External RTC (RX8130CE on the shared BSP I2C bus) ----
// The RX8130CE is wired on the shared BSP I2C bus (0x32). Its time registers
// live at 0x10..0x16 (vs the DS3231's 0x00..0x06) and it has a STOP bit in
// control 0x1E, so components/clock needs the RX8130 flavour of the external
// hook (selected by these macros).
#define BOARD_CFG_RTC_USE_BSP_I2C 1
#define BOARD_CFG_RTC_EXT_ENABLE 1
#define BOARD_CFG_RTC_EXT_ADDR 0x32
#define BOARD_CFG_RTC_EXT_TIME_REG 0x10
#define BOARD_CFG_RTC_EXT_KIND_RX8130 1
#define BOARD_CFG_RTC_EXT_SDA 31
#define BOARD_CFG_RTC_EXT_SCL 32
#define BOARD_CFG_RTC_EXT_PORT 1

// ---- Tab5Keyboard module (STM32F030, external I2C 0x6D + INT) ----
// The module plugs into the Tab5 expansion port: EXT I2C on GPIO0 (SDA) /
// GPIO1 (SCL) at 400 kHz, interrupt on GPIO50. Driven on I2C controller 0 so it
// never shares the SYS I2C (controller 1, GPIO31/32) used by touch/codec/RTC/
// IO-expander/INA226. See PORTING.md / the M5Stack Keyboard-UserDemo.
#define BOARD_CFG_TAB5KBD_PRESENT 1
#define BOARD_CFG_TAB5KBD_I2C_ADDR 0x6D
#define BOARD_CFG_TAB5KBD_I2C_PORT 0
#define BOARD_CFG_TAB5KBD_SDA_GPIO GPIO_NUM_0
#define BOARD_CFG_TAB5KBD_SCL_GPIO GPIO_NUM_1
#define BOARD_CFG_TAB5KBD_INT_GPIO GPIO_NUM_50

// ---- BMI270 IMU (main board, SYS I2C addr 0x68) ----
// Six-axis accelerometer/gyroscope on the shared SYS I2C bus. Used by the `imu`
// command for diagnostics and for optional tilt-based screen rotation.
#define BOARD_CFG_IMU_PRESENT 1
#define BOARD_CFG_IMU_I2C_ADDR 0x68

// ---- MIPI-CSI camera (SC202CS / SC2356, esp_video) ----
// The sensor is powered by the first PI4IOE expander (BSP_FEATURE_CAMERA, P6)
// and streams RGB565 through esp_video. Only BMP stills are captured today.
#define BOARD_CFG_CAMERA_PRESENT 1

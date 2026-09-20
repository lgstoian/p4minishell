# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# board_bsp - M5Stack Tab5 profile.
#
# Vendored copy of the official espressif/m5stack_tab5 BSP (v1.3.1),
# trimmed to the display/touch/audio/storage/IO-expander/USB subsystems this
# firmware uses. Camera (esp_video) and IMU (bmi270/sensor_hub) sources are
# intentionally omitted: the camera stack is a documented hardware gap and the
# IMU has no consumer, and dropping them keeps the managed-dependency tree
# light. The panel is auto-detected at runtime (ILI9881C / ST7123 / ST7121).
#
# Only the board_bsp directory matching the active -DP4_BOARD= is added to
# EXTRA_COMPONENT_DIRS, so exactly one BSP is in the build.
file(GLOB_RECURSE BOARD_BSP_SRC src/*.c)

idf_component_register(
    SRCS ${BOARD_BSP_SRC}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_sdmmc
        esp_driver_sdspi
        esp_hw_support
        sdmmc
        spiffs
        fatfs
        vfs
        espressif__esp_lcd_ili9881c
        espressif__esp_lcd_st7123
        espressif__esp_lcd_touch
        espressif__esp_lcd_touch_gt911
        espressif__esp_lcd_touch_st7123
        espressif__esp_io_expander
        espressif__esp_io_expander_pi4ioe5v6408
        espressif__esp_codec_dev
        espressif__usb
        espressif__esp_lvgl_port
        lvgl__lvgl
    PRIV_REQUIRES
        esp_lcd
        esp_driver_ledc
        esp_psram
)

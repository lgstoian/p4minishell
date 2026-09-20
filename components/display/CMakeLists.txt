# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        board_bsp
        espressif__esp_lcd_touch
        espressif__esp_lvgl_port
        lvgl__lvgl
        hal
        soc
)

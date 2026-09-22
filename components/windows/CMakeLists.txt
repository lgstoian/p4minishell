# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "windows.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        font
        display
        header
        keyboard
        ansi
        p4heap
        board_bsp
        espressif__esp_lvgl_port
        lvgl__lvgl
)

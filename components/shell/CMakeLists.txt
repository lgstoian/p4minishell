# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "shell.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        display
        header
        keyboard
        clock
        windows
        p4heap
        board_bsp
        espressif__esp_lvgl_port
        lvgl__lvgl
        driver
        vfs
        esp_app_format
)

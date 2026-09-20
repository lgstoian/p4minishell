# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "font.c" "font_fs.c" "theme.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        lvgl__lvgl
        espressif__esp_lvgl_port
        board_bsp
        ansi
)

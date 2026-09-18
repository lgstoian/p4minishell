# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "tui.c" "tui_fonts.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        windows
        ansi
        display
        gfx
        espressif__esp_lvgl_port
        lvgl__lvgl
)

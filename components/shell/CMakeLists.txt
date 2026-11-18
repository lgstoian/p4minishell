# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "shell.c" "shell_core.c" "shell_print.c" "shell_log.c" "shell_transcript.c" "locale.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        display
        header
        selection
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
        strutil
)

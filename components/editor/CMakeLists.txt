# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "editor.c" "editor_view.c" "editor_spell.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        shell
        storage
        windows
        keyboard
        header
        display
        font
        filetype
        markdown
        modal
        p4heap
        espressif__esp_lvgl_port
        lvgl__lvgl
)

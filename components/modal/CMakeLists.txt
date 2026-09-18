# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "modal.c" "modal_surf.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        freertos
        esp_common
        log
        shell
        storage
        windows
        keyboard
        espressif__esp_lvgl_port
        lvgl__lvgl
        markdown
        filetype
        font
        gfx
)

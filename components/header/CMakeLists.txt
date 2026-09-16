# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "header.c" "header_layout.c" "header_status.c" "header_notify_queue.c" "header_refresh.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        lvgl__lvgl
        espressif__esp_lvgl_port
        esp_app_format
        font
)
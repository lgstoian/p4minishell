# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "c6ota.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        board_bsp
        bootloader_support
        esp_app_format
        esp_http_client
        esp_hosted
        esp-tls
        fatfs
        networking
        strutil
)
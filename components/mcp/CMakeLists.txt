# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "mcp.c"
    INCLUDE_DIRS "include"
    REQUIRES
        networking
        json
        p4heap
        esp_http_client
        esp_timer
        freertos
)

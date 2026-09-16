# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS
        "networking.c"
        "bluetooth.c"
        "wifi_known.c"
        "http_server.c"
        "netdiag.c"
        "netbench.c"
        "tcpterm.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        bt
        esp_event
        esp_hosted
        esp_http_client
        esp_http_server
        esp_netif
        esp-tls
        esp_wifi
        led
        lwip
        nvs_flash
        storage
)
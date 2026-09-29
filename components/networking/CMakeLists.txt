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
        "mqtt_codec.c"
        "netsvc.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        bt
        certs
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
        p4heap
        pim
        portal
        storage
        strutil
)
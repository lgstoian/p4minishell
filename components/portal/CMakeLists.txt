# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "portal.c" "dns_server.c" "portal_http.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_netif esp_http_server networking storage strutil
    PRIV_REQUIRES esp_wifi
)

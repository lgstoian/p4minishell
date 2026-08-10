idf_component_register(
    SRCS
        "networking.c"
        "bluetooth.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        bt
        esp_event
        esp_hosted
        esp_http_client
        esp_netif
        esp-tls
        esp_wifi
        lwip
        nvs_flash
)
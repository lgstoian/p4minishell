idf_component_register(
    SRCS
        "networking.c"
        "bluetooth.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        bt
        esp_event
        esp_hosted
        esp_netif
        esp_wifi
        nvs_flash
)
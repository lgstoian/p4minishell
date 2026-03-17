idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__esp32_p4_function_ev_board
        espressif__esp_lvgl_port
        bootloader_support
        esp_app_format
        esp_event
        esp_http_client
        esp_hosted
        esp_netif
        fatfs
        lvgl__lvgl
        mbedtls
        nvs_flash
        esp-tls
        esp_wifi
        esp_psram
)

idf_component_register(
    SRCS "c6ota.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        espressif__esp32_p4_function_ev_board
        bootloader_support
        esp_app_format
        esp_http_client
        esp_hosted
        esp-tls
        fatfs
        networking
        storage
)
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__esp32_p4_function_ev_board
        espressif__esp_lcd_touch
        espressif__esp_lvgl_port
        bootloader_support
        c6ota
        networking
        esp_adc
        esp_event
        esp_hosted
        esp_netif
        fatfs
        lvgl__lvgl
        nvs_flash
        esp_wifi
        esp_psram
)

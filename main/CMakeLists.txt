idf_component_register(
    SRCS "main.c" "p4minishell.c"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__esp32_p4_function_ev_board
        espressif__esp_lvgl_port
        esp_event
        esp_hosted
        esp_netif
        lvgl__lvgl
        nvs_flash
        esp_wifi
        esp_psram
)

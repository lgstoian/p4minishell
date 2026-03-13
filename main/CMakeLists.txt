idf_component_register(
    SRCS "main.c" "p4minishell.c"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__esp32_p4_function_ev_board
        espressif__esp_lvgl_port
        lvgl__lvgl
        esp_psram
)

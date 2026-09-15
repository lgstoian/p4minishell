idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        espressif__esp32_p4_function_ev_board
        espressif__esp_lcd_touch
        espressif__esp_lvgl_port
        lvgl__lvgl
        hal
        soc
)

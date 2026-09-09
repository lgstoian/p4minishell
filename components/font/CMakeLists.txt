idf_component_register(
    SRCS "font.c" "font_fs.c" "theme.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        lvgl__lvgl
        espressif__esp_lvgl_port
        espressif__esp32_p4_function_ev_board
)

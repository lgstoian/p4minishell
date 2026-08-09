idf_component_register(
    SRCS "windows.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        display
        header
        keyboard
        ansi
        espressif__esp32_p4_function_ev_board
        espressif__esp_lvgl_port
        lvgl__lvgl
)

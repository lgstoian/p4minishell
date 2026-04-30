idf_component_register(
    SRCS "command.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        display
        keyboard
        windows
        shell
        networking
        c6ota
        p4_usb
        main
        espressif__esp32_p4_function_ev_board
        espressif__esp_lvgl_port
        lvgl__lvgl
)

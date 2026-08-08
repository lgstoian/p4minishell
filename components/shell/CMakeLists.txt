idf_component_register(
    SRCS "shell.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        display
        header
        keyboard
        clock
        windows
        networking
        c6ota
        p4_usb
        espressif__esp32_p4_function_ev_board
        espressif__esp_lvgl_port
        lvgl__lvgl
        driver
        vfs
)

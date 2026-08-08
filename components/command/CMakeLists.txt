idf_component_register(
    SRCS "command.c" "command_ui.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        batch
        display
        header
        keyboard
        storage
        windows
        shell
        clock
        networking
        c6ota
        p4_usb
        espressif__esp32_p4_function_ev_board
        espressif__esp_lvgl_port
        lvgl__lvgl
        esp_adc
        fatfs
        esp_pm
)

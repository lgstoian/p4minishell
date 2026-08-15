idf_component_register(
    SRCS "command.c" "command_ui.c" "config_cmd.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        audio
        batch
        display
        editor
        header
        keyboard
        storage
        windows
        shell
        clock
        networking
        c6ota
        p4_usb
        led
        espressif__esp32_p4_function_ev_board
        espressif__esp_lvgl_port
        lvgl__lvgl
        esp_adc
        fatfs
        esp_pm
)

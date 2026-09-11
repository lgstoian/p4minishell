idf_component_register(
    SRCS "command.c" "command_ui.c" "config_cmd.c" "db_commands.c"
"alarm_commands.c" "gfind_commands.c" "audio_commands.c" "tui_commands.c" "periph_commands.c" "power_commands.c"
"serial_commands.c" "font_commands.c" "md_commands.c" "json_commands.c" "gfx_commands.c" "asset_commands.c" "pkg_commands.c" "plot_commands.c" "header_commands.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        applib
        audio
        alarm
        batch
        db
        display
        editor
        font
        header
        keyboard
        markdown
        filetype
        storage
        windows
        shell
        clock
        networking
        c6ota
        usb
        led
        modal
        tui
        gfx
        espressif__esp32_p4_function_ev_board
        espressif__esp_lvgl_port
        lvgl__lvgl
        esp_adc
        fatfs
        esp_pm
)

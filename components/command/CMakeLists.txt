# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "command.c" "command_ui.c" "config_cmd.c" "db_commands.c" "security_commands.c"
"alarm_commands.c" "gfind_commands.c" "audio_commands.c" "tui_commands.c" "periph_commands.c" "power_commands.c"
"serial_commands.c" "font_commands.c" "md_commands.c" "json_commands.c" "gfx_commands.c" "image_commands.c" "asset_commands.c" "pkg_commands.c" "plot_commands.c" "header_commands.c" "ui_commands.c" "csv_commands.c" "export_commands.c" "import_commands.c" "archive_commands.c" "crypt_commands.c" "userial_commands.c" "imu_commands.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        applib
        archive
        audio
        alarm
        batch
        camera
        db
        display
        editor
        font
        header
        imagefmt
        keyboard
        markdown
        filetype
        storage
        windows
        shell
        clock
        networking
        power_monitor
        c6ota
        usb
        led
        tab5kbd
        imu
        modal
        tui
        gfx
        uitest
        mbedtls
        board_bsp
        espressif__esp_lvgl_port
        lvgl__lvgl
        esp_adc
        fatfs
        esp_pm
)

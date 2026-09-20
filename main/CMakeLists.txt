# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "main.c" "p4minishell.c" "native_apps.c" "assets/icon_splash.c"
    INCLUDE_DIRS "." "assets"
    REQUIRES
        board_bsp
        espressif__esp_lcd_touch
        espressif__esp_lvgl_port
        applib
        bootloader_support
        c6ota
        command
        display
        editor
        header
        keyboard
        led
        networking
        usb
        shell
        boot
        clock
        windows
        tab5kbd
        imu
        whoami
        esp_adc
        esp_event
        esp_hosted
        esp_netif
        fatfs
        lvgl__lvgl
        nvs_flash
        esp_wifi
        esp_psram
)

# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "alarm.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        clock
        storage
        led
        audio
        espressif__esp32_p4_function_ev_board
        freertos
        esp_timer
)

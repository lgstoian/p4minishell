# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "power_monitor.c"
    INCLUDE_DIRS "include" "${CMAKE_SOURCE_DIR}"
    REQUIRES
        board_bsp
        esp_driver_i2c
        espressif__esp_io_expander
    PRIV_REQUIRES
        freertos
        log
)

# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# camera - M5Stack Tab5 MIPI-CSI still capture via the managed esp_video stack.
# On boards without a camera the source compiles to a no-op.

idf_component_register(
    SRCS "src/camera.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board_bsp
        storage
        imagefmt
        p4heap
        espressif__esp_video
    PRIV_REQUIRES
        esp_driver_i2c
        esp_driver_gpio
)

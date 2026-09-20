# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# imu - BMI270 accelerometer/gyroscope leaf (M5Stack Tab5).
#
# The Bosch bmi2/bmi270 driver and the M5Stack `accel_gyro_bmi270` wrapper are
# vendored under src/ + include/ (BSD-3-Clause / MIT; see licence.md).

idf_component_register(
    SRCS
        "src/imu.c"
        "src/accel_gyro_bmi270.c"
        "src/bmi2.c"
        "src/bmi270.c"
        "src/bmi270_context.c"
        "src/bmi270_legacy.c"
        "src/bmi270_maximum_fifo.c"
        "src/bmi2_ois.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board_bsp
        esp_driver_i2c
        esp_driver_gpio
        esp_timer
)

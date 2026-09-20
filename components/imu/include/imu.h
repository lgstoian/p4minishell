/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file imu.h
 * @brief BMI270 accelerometer/gyroscope leaf (M5Stack Tab5).
 *
 * Wraps the vendored Bosch bmi2/bmi270 driver behind a small, board-agnostic
 * surface: read an acceleration/gyro sample, classify the physical orientation
 * from gravity, and (optionally) drive display rotation as the board is turned.
 *
 * On boards without an IMU (`BOARD_CFG_IMU_PRESENT == 0`) every entry point is a
 * cheap no-op and `imu_available()` stays false, so the `imu` command reports the
 * gap instead of crashing.
 *
 * The module is a leaf: it reads the board SYS I2C bus and never calls into the
 * display/shell modules. The auto-rotate feature is delivered through a callback
 * the owner installs, so `imu` has no dependency on `display`.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Physical orientation derived from the gravity vector. */
typedef enum {
    IMU_ORIENT_UNKNOWN = -1,        /*!< Not enough gravity to decide */
    IMU_ORIENT_PORTRAIT = 0,        /*!< Upright portrait */
    IMU_ORIENT_LANDSCAPE = 1,       /*!< Rotated to landscape (default held pose) */
    IMU_ORIENT_PORTRAIT_INV = 2,    /*!< Inverted portrait */
    IMU_ORIENT_LANDSCAPE_INV = 3,   /*!< Inverted landscape */
} imu_orientation_t;

/** One IMU sample (display-frame axes: x right, y up, z out of the screen). */
typedef struct {
    float accel_mps2[3];    /*!< Acceleration in m/s^2 (x, y, z) */
    float accel_mg[3];      /*!< Acceleration in milli-g (x, y, z) */
    float gyro_dps[3];      /*!< Angular rate in deg/s (x, y, z) */
    float pitch_deg;        /*!< Pitch from gravity (-90..90) */
    float roll_deg;         /*!< Roll from gravity (-180..180) */
    imu_orientation_t orientation; /*!< Classified held orientation */
} imu_sample_t;

/**
 * @brief Probe and configure the IMU. Idempotent.
 *
 * @return ESP_OK when the IMU answered and the sensors are enabled,
 *         ESP_ERR_NOT_SUPPORTED when the board has no IMU, ESP_ERR_NOT_FOUND
 *         when the board has one but it did not answer, or another error.
 */
esp_err_t imu_init(void);

/** @brief Release the IMU (stop the auto-rotate task and the I2C device). */
void imu_deinit(void);

/** @brief True once imu_init() found and configured the IMU. */
bool imu_available(void);

/**
 * @brief Read one sample and classify the orientation.
 * @return ESP_OK, ESP_ERR_INVALID_STATE when no IMU, ESP_ERR_INVALID_ARG on NULL.
 */
esp_err_t imu_read(imu_sample_t *out);

/** Classify a sample's gravity vector into an orientation (pure, unit-tested). */
imu_orientation_t imu_orientation_from_sample(const imu_sample_t *sample);

/** Display rotation (0/90/180/270) for an orientation. */
int imu_orientation_rotation_deg(imu_orientation_t orientation);

/** Stable name ("portrait", "landscape", "portrait-inv", "landscape-inv"). */
const char *imu_orientation_name(imu_orientation_t orientation);

/** Rotation sink installed by the owner (typically display_set_rotation). */
typedef void (*imu_rotation_cb_t)(int rotation_deg, void *user);

/** Install the rotation callback used while auto-rotate is enabled. */
void imu_set_rotation_callback(imu_rotation_cb_t callback, void *user);

/**
 * @brief Enable/disable automatic display rotation from the held orientation.
 *
 * Off by default. When enabled, a background sampler applies the rotation for
 * the current orientation (and again on each change, with hysteresis) through
 * the installed callback. Enabling also applies the current orientation once.
 */
esp_err_t imu_set_auto_rotate(bool enable);

/** True while auto-rotate is enabled. */
bool imu_auto_rotate_enabled(void);

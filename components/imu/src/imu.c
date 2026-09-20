/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file imu.c
 * @brief BMI270 accelerometer/gyroscope leaf (M5Stack Tab5).
 *
 * Reads the SYS I2C bus through the board BSP and exposes samples plus a
 * gravity-based orientation. Auto-rotate runs a small low-priority sampler; the
 * actual display rotation is applied via a callback, so this module has no
 * dependency on the display component.
 */

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu.h"
#include "accel_gyro_bmi270.h"
#include "board_bsp.h"
#include "board_config.h"

/* Boards whose profile omits the IMU macros are treated as having none. */
#ifndef BOARD_CFG_IMU_PRESENT
#define BOARD_CFG_IMU_PRESENT 0
#endif

#define IMU_TAG "imu"

/* Configured ranges (see accel_gyro_bmi270_enable_sensor): accel +-4 g,
 * gyro +-1000 dps, both 16-bit. */
#define IMU_ACCEL_LSB_PER_G 8192.0f
#define IMU_GYRO_LSB_PER_DPS 32.768f
#define IMU_GRAVITY_MPS2 9.80665f

/* Auto-rotate sampling. */
#define IMU_ROTATE_POLL_MS 100
#define IMU_ROTATE_STABLE_SAMPLES 3
#define IMU_ROTATE_TASK_STACK 3072

static bool s_available;
static bool s_auto_rotate;
static TaskHandle_t s_rotate_task;
static imu_rotation_cb_t s_rotation_cb;
static void *s_rotation_user;

#if BOARD_CFG_IMU_PRESENT
/* The BMI270 is mounted rotated on the Tab5; map its axes into the display
 * frame (matching the M5Stack reference). */
static void imu_map_axes(const struct bmi2_sens_data *raw, float accel_mps2[3],
                         float accel_mg[3], float gyro_dps[3])
{
    float ax = (float)raw->acc.y;
    float ay = (float)(-raw->acc.x);
    float az = (float)(-raw->acc.z);
    float gx = (float)raw->gyr.y;
    float gy = (float)raw->gyr.x;
    float gz = (float)(-raw->gyr.z);

    accel_mps2[0] = (ax / IMU_ACCEL_LSB_PER_G) * IMU_GRAVITY_MPS2;
    accel_mps2[1] = (ay / IMU_ACCEL_LSB_PER_G) * IMU_GRAVITY_MPS2;
    accel_mps2[2] = (az / IMU_ACCEL_LSB_PER_G) * IMU_GRAVITY_MPS2;

    accel_mg[0] = (ax / IMU_ACCEL_LSB_PER_G) * 1000.0f;
    accel_mg[1] = (ay / IMU_ACCEL_LSB_PER_G) * 1000.0f;
    accel_mg[2] = (az / IMU_ACCEL_LSB_PER_G) * 1000.0f;

    gyro_dps[0] = gx / IMU_GYRO_LSB_PER_DPS;
    gyro_dps[1] = gy / IMU_GYRO_LSB_PER_DPS;
    gyro_dps[2] = gz / IMU_GYRO_LSB_PER_DPS;
}
#endif /* BOARD_CFG_IMU_PRESENT */

imu_orientation_t imu_orientation_from_sample(const imu_sample_t *sample)
{
    float ax;
    float ay;
    const float threshold = IMU_GRAVITY_MPS2 * 0.4f; /* ~0.4 g */

    if (sample == NULL) {
        return IMU_ORIENT_UNKNOWN;
    }
    ax = sample->accel_mps2[0];
    ay = sample->accel_mps2[1];

    /* The in-plane gravity component points toward the floor; the dominant axis
     * and its sign pick the held orientation. */
    if (fabsf(ax) >= fabsf(ay)) {
        if (ax >= threshold) {
            return IMU_ORIENT_LANDSCAPE;
        }
        if (ax <= -threshold) {
            return IMU_ORIENT_LANDSCAPE_INV;
        }
    } else {
        if (ay >= threshold) {
            return IMU_ORIENT_PORTRAIT;
        }
        if (ay <= -threshold) {
            return IMU_ORIENT_PORTRAIT_INV;
        }
    }
    return IMU_ORIENT_UNKNOWN;
}

int imu_orientation_rotation_deg(imu_orientation_t orientation)
{
    switch (orientation) {
    case IMU_ORIENT_PORTRAIT:      return 0;
    case IMU_ORIENT_LANDSCAPE:     return 90;
    case IMU_ORIENT_PORTRAIT_INV:  return 180;
    case IMU_ORIENT_LANDSCAPE_INV: return 270;
    default:                       return -1;
    }
}

const char *imu_orientation_name(imu_orientation_t orientation)
{
    switch (orientation) {
    case IMU_ORIENT_PORTRAIT:      return "portrait";
    case IMU_ORIENT_LANDSCAPE:     return "landscape";
    case IMU_ORIENT_PORTRAIT_INV:  return "portrait-inv";
    case IMU_ORIENT_LANDSCAPE_INV: return "landscape-inv";
    default:                       return "unknown";
    }
}

void imu_set_rotation_callback(imu_rotation_cb_t callback, void *user)
{
    s_rotation_cb = callback;
    s_rotation_user = user;
}

#if BOARD_CFG_IMU_PRESENT
static void imu_rotate_task(void *arg)
{
    imu_orientation_t candidate = IMU_ORIENT_UNKNOWN;
    imu_orientation_t applied = IMU_ORIENT_UNKNOWN;
    int stable = 0;

    (void)arg;

    for (;;) {
        imu_sample_t sample;

        if (!s_auto_rotate) {
            vTaskDelay(pdMS_TO_TICKS(IMU_ROTATE_POLL_MS));
            continue;
        }
        if (imu_read(&sample) == ESP_OK) {
            imu_orientation_t o = sample.orientation;

            if (o != candidate) {
                candidate = o;
                stable = 1;
            } else if (stable < IMU_ROTATE_STABLE_SAMPLES) {
                stable++;
            }
            if (o != IMU_ORIENT_UNKNOWN && stable >= IMU_ROTATE_STABLE_SAMPLES && o != applied) {
                int rotation = imu_orientation_rotation_deg(o);

                if (rotation >= 0 && s_rotation_cb != NULL) {
                    s_rotation_cb(rotation, s_rotation_user);
                    applied = o;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_ROTATE_POLL_MS));
    }
}
#endif /* BOARD_CFG_IMU_PRESENT */

esp_err_t imu_init(void)
{
#if !BOARD_CFG_IMU_PRESENT
    return ESP_ERR_NOT_SUPPORTED;
#else
    i2c_master_bus_handle_t bus;
    esp_err_t error;

    if (s_available) {
        return ESP_OK;
    }
    bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        (void)bsp_i2c_init();
        bus = bsp_i2c_get_handle();
    }
    if (bus == NULL) {
        return ESP_FAIL;
    }
    error = accel_gyro_bmi270_init(bus);
    if (error != ESP_OK) {
        ESP_LOGW(IMU_TAG, "BMI270 not responding on the SYS I2C bus");
        return ESP_ERR_NOT_FOUND;
    }
    accel_gyro_bmi270_enable_sensor();
    s_available = true;

    if (s_rotate_task == NULL) {
        if (xTaskCreate(imu_rotate_task, "imu_rotate", IMU_ROTATE_TASK_STACK, NULL,
                        tskIDLE_PRIORITY + 1, &s_rotate_task) != pdPASS) {
            s_rotate_task = NULL;
            ESP_LOGW(IMU_TAG, "auto-rotate task not started");
        }
    }
    ESP_LOGI(IMU_TAG, "BMI270 ready");
    return ESP_OK;
#endif
}

void imu_deinit(void)
{
    if (s_rotate_task != NULL) {
        vTaskDelete(s_rotate_task);
        s_rotate_task = NULL;
    }
    s_auto_rotate = false;
    s_available = false;
}

bool imu_available(void)
{
    return s_available;
}

esp_err_t imu_read(imu_sample_t *out)
{
#if !BOARD_CFG_IMU_PRESENT
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
#else
    struct bmi2_sens_data raw;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&raw, 0, sizeof(raw));
    accel_gyro_bmi270_get_data(&raw);

    memset(out, 0, sizeof(*out));
    imu_map_axes(&raw, out->accel_mps2, out->accel_mg, out->gyro_dps);

    out->pitch_deg = atan2f(-out->accel_mps2[0],
                            sqrtf(out->accel_mps2[1] * out->accel_mps2[1] +
                                  out->accel_mps2[2] * out->accel_mps2[2])) *
                     180.0f / (float)M_PI;
    out->roll_deg = atan2f(out->accel_mps2[1], out->accel_mps2[2]) * 180.0f / (float)M_PI;
    out->orientation = imu_orientation_from_sample(out);
    return ESP_OK;
#endif
}

esp_err_t imu_set_auto_rotate(bool enable)
{
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    s_auto_rotate = enable;
    if (enable) {
        /* Apply the current held orientation immediately. */
        imu_sample_t sample = { 0 };

        if (imu_read(&sample) == ESP_OK) {
            int rotation = imu_orientation_rotation_deg(sample.orientation);

            if (rotation >= 0 && s_rotation_cb != NULL) {
                s_rotation_cb(rotation, s_rotation_user);
            }
        }
    }
    return ESP_OK;
}

bool imu_auto_rotate_enabled(void)
{
    return s_auto_rotate;
}

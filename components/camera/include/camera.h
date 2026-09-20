/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file camera.h
 * @brief M5Stack Tab5 MIPI-CSI camera (SC202CS/SC2356) via esp_video.
 *
 * Wraps the Espressif `esp_video` V4L2 stack behind a small surface: probe/init
 * the sensor and capture a single still to a 24-bit BMP on the SD card. The
 * capture path is split into reusable open/queue/dequeue primitives so a live
 * preview can be layered on later without reworking this module.
 *
 * On boards without a camera (`BOARD_CFG_CAMERA_PRESENT == 0`) every entry point
 * is a no-op and `camera_available()` stays false; the `camera` command reports
 * the gap honestly.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Power the sensor and initialise esp_video. Idempotent.
 *
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED when the board has no camera, or an
 *         esp_video/sensor error.
 */
esp_err_t camera_init(void);

/** @brief Release the video device (stop streaming, close the sensor). */
void camera_deinit(void);

/** @brief True once camera_init() configured the sensor. */
bool camera_available(void);

/**
 * @brief Capture one still and write it as a 24-bit BMP.
 *
 * @param path        VFS destination path (SD card).
 * @param width_out   Receives the captured width (may be NULL).
 * @param height_out  Receives the captured height (may be NULL).
 * @return ESP_OK, or an error from the camera/SD path.
 */
esp_err_t camera_capture_bmp(const char *path, int *width_out, int *height_out);

/**
 * @brief Groundwork for live preview: fill @p out with the most recent RGB565
 *        frame dimensions and a pointer to the mapped frame buffer.
 *
 * A future preview consumer calls this from the capture loop; today it is used
 * by the still path internally. The pointer is owned by the camera and is only
 * valid until the next capture.
 */
typedef struct {
    const uint16_t *pixels; /*!< RGB565 frame (may be NULL) */
    int width;
    int height;
    int stride;             /*!< Pixels per row */
} camera_frame_t;

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file camera.c
 * @brief M5Stack Tab5 MIPI-CSI camera still capture via esp_video.
 *
 * The V4L2 plumbing (open / set RGB565 / queue MMAP buffers / stream) mirrors
 * the M5Stack Tab5 reference. Capture is kept as open-stream + per-frame
 * dequeue so a live preview can consume the same buffers later.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera.h"
#include "board_bsp.h"
#include "board_config.h"
#include "p4minishell_config.h"

#define CAM_TAG "camera"

#ifndef BOARD_CFG_CAMERA_PRESENT
#define BOARD_CFG_CAMERA_PRESENT 0
#endif

#if BOARD_CFG_CAMERA_PRESENT

#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"

#include "storage.h"
#include "imagefmt.h"

#define CAM_BUFFER_COUNT 2

static int s_fd = -1;
static void *s_buffers[CAM_BUFFER_COUNT];
static size_t s_buffer_len[CAM_BUFFER_COUNT];
static int s_width;
static int s_height;
static bool s_streaming;

static int camera_open_device(void)
{
    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = NULL,
            .freq = 400000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    /* Only the CSI device is configured; designated init zeroes the rest. */
    esp_video_init_config_t config = {
        .csi = &csi_config,
    };
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int i;

    csi_config.sccb_config.i2c_handle = bsp_i2c_get_handle();
    if (csi_config.sccb_config.i2c_handle == NULL) {
        ESP_LOGE(CAM_TAG, "SYS I2C bus unavailable for the camera SCCB");
        return -1;
    }

    esp_err_t error = esp_video_init(&config);
    if (error != ESP_OK) {
        ESP_LOGE(CAM_TAG, "esp_video_init failed: %s", esp_err_to_name(error));
        return -1;
    }

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(CAM_TAG, "open %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return -1;
    }

    memset(&format, 0, sizeof(format));
    format.type = type;
    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(CAM_TAG, "VIDIOC_G_FMT failed");
        close(fd);
        return -1;
    }
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format set = {
            .type = type,
            .fmt = { .pix = {
                .width = format.fmt.pix.width,
                .height = format.fmt.pix.height,
                .pixelformat = V4L2_PIX_FMT_RGB565,
            } },
        };
        if (ioctl(fd, VIDIOC_S_FMT, &set) != 0) {
            ESP_LOGE(CAM_TAG, "VIDIOC_S_FMT(RGB565) failed");
            close(fd);
            return -1;
        }
        format = set;
    }
    s_width = (int)format.fmt.pix.width;
    s_height = (int)format.fmt.pix.height;

    memset(&req, 0, sizeof(req));
    req.count = CAM_BUFFER_COUNT;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(CAM_TAG, "VIDIOC_REQBUFS failed");
        close(fd);
        return -1;
    }

    for (i = 0; i < CAM_BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(CAM_TAG, "VIDIOC_QUERYBUF failed");
            goto fail;
        }
        s_buffer_len[i] = buf.length;
        s_buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (s_buffers[i] == MAP_FAILED) {
            s_buffers[i] = NULL;
            ESP_LOGE(CAM_TAG, "mmap failed");
            goto fail;
        }
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(CAM_TAG, "VIDIOC_QBUF failed");
            goto fail;
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(CAM_TAG, "VIDIOC_STREAMON failed");
        goto fail;
    }
    s_streaming = true;
    s_fd = fd;
    ESP_LOGI(CAM_TAG, "sensor ready at %dx%d", s_width, s_height);
    return fd;

fail:
    for (i = 0; i < CAM_BUFFER_COUNT; i++) {
        if (s_buffers[i] != NULL) {
            munmap(s_buffers[i], s_buffer_len[i]);
            s_buffers[i] = NULL;
        }
    }
    close(fd);
    return -1;
}

esp_err_t camera_init(void)
{
    if (camera_available()) {
        return ESP_OK;
    }
    /* Power the sensor rail (PI4IOE P6 on the first expander). */
    esp_err_t error = bsp_feature_enable(BSP_FEATURE_CAMERA, true);
    if (error != ESP_OK) {
        ESP_LOGW(CAM_TAG, "camera power enable failed: %s", esp_err_to_name(error));
    }

    /* The sensor needs a few tens of ms after the rail comes up before it
     * answers SCCB; the first esp_video_init() otherwise loses the race and
     * reports "Get sensor ID failed". Retry once after a settle delay. */
    vTaskDelay(pdMS_TO_TICKS(120));
    if (camera_open_device() < 0) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (camera_open_device() < 0) {
            ESP_LOGE(CAM_TAG, "sensor not detected after power-up");
            return ESP_ERR_NOT_FOUND;
        }
    }
    return ESP_OK;
}

void camera_deinit(void)
{
    int i;

    if (s_fd >= 0) {
        if (s_streaming) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void)ioctl(s_fd, VIDIOC_STREAMOFF, &type);
            s_streaming = false;
        }
        for (i = 0; i < CAM_BUFFER_COUNT; i++) {
            if (s_buffers[i] != NULL) {
                munmap(s_buffers[i], s_buffer_len[i]);
                s_buffers[i] = NULL;
            }
        }
        close(s_fd);
        s_fd = -1;
    }
}

bool camera_available(void)
{
    return s_fd >= 0 && s_width > 0 && s_height > 0;
}

esp_err_t camera_capture_bmp(const char *path, int *width_out, int *height_out)
{
    struct v4l2_buffer buf;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    shell_sd_session_t session;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    uint8_t headers[IMAGEFMT_BMP_HEADER_SIZE];
    uint8_t *row_buf;
    uint32_t row_bytes;
    FILE *file;
    uint64_t needed;
    int32_t y;
    bool write_ok = true;
    esp_err_t result = ESP_OK;

    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!camera_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
        ESP_LOGE(CAM_TAG, "VIDIOC_DQBUF failed");
        return ESP_FAIL;
    }

    row_bytes = (uint32_t)s_width * 3u;
    row_buf = p4heap_alloc_psram(row_bytes);
    if (row_buf == NULL) {
        (void)ioctl(s_fd, VIDIOC_QBUF, &buf);
        return ESP_ERR_NO_MEM;
    }

    if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        result = ESP_ERR_INVALID_ARG;
        goto out_free;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        result = ESP_ERR_INVALID_STATE;
        goto out_free;
    }

    needed = IMAGEFMT_BMP_HEADER_SIZE + (uint64_t)row_bytes * (uint64_t)s_height;
    if (!storage_check_free_space(needed, storage_get_file_size(resolved), "camera")) {
        result = ESP_ERR_NO_MEM;
        goto out_session;
    }
    file = fopen(resolved, "wb");
    if (file == NULL) {
        result = ESP_FAIL;
        goto out_session;
    }

    imagefmt_write_bmp_header(headers, (uint32_t)s_width, (uint32_t)s_height);
    if (fwrite(headers, 1, sizeof(headers), file) != sizeof(headers)) {
        write_ok = false;
    }
    /* BMP stores rows bottom-up. */
    for (y = s_height - 1; write_ok && y >= 0; y--) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)s_buffers[buf.index] +
                                                 (uint32_t)y * (uint32_t)s_width * 2u);
        imagefmt_rgb565_to_bgr24(row, row_buf, (size_t)s_width);
        if (fwrite(row_buf, 1, row_bytes, file) != row_bytes) {
            write_ok = false;
        }
    }
    fclose(file);
    shell_sd_end(&session, "camera");
    (void)ioctl(s_fd, VIDIOC_QBUF, &buf);

    free(row_buf);
    if (!write_ok) {
        remove(resolved);
        return ESP_FAIL;
    }
    if (width_out != NULL) {
        *width_out = s_width;
    }
    if (height_out != NULL) {
        *height_out = s_height;
    }
    return ESP_OK;

out_session:
    shell_sd_end(&session, "camera");
out_free:
    (void)ioctl(s_fd, VIDIOC_QBUF, &buf);
    free(row_buf);
    return result;
}

#else /* !BOARD_CFG_CAMERA_PRESENT */

esp_err_t camera_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void camera_deinit(void)
{
}

bool camera_available(void)
{
    return false;
}

esp_err_t camera_capture_bmp(const char *path, int *width_out, int *height_out)
{
    (void)path;
    (void)width_out;
    (void)height_out;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* BOARD_CFG_CAMERA_PRESENT */

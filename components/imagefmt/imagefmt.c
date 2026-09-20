/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "imagefmt.h"

int imagefmt_write_bmp_header(uint8_t *buf, uint32_t width, uint32_t height)
{
    uint32_t row_bytes = width * 3; /* 24-bit BGR */
    uint32_t img_size = row_bytes * height;
    uint32_t file_size = IMAGEFMT_BMP_HEADER_SIZE + img_size;
    int32_t ppm = 3780; /* 96 DPI */

    memset(buf, 0, IMAGEFMT_BMP_HEADER_SIZE);

    /* File header. */
    buf[0] = 'B';
    buf[1] = 'M';
    buf[2] = (uint8_t)file_size;
    buf[3] = (uint8_t)(file_size >> 8);
    buf[4] = (uint8_t)(file_size >> 16);
    buf[5] = (uint8_t)(file_size >> 24);
    buf[10] = IMAGEFMT_BMP_HEADER_SIZE;

    /* Info header. */
    buf[14] = 40;
    buf[18] = (uint8_t)width;
    buf[19] = (uint8_t)(width >> 8);
    buf[20] = (uint8_t)(width >> 16);
    buf[21] = (uint8_t)(width >> 24);
    buf[22] = (uint8_t)height;
    buf[23] = (uint8_t)(height >> 8);
    buf[24] = (uint8_t)(height >> 16);
    buf[25] = (uint8_t)(height >> 24);
    buf[26] = 1;  /* planes */
    buf[28] = 24; /* bits per pixel */
    buf[30] = 0;  /* BI_RGB */
    buf[34] = (uint8_t)img_size;
    buf[35] = (uint8_t)(img_size >> 8);
    buf[36] = (uint8_t)(img_size >> 16);
    buf[37] = (uint8_t)(img_size >> 24);
    buf[38] = (uint8_t)ppm;
    buf[39] = (uint8_t)(ppm >> 8);
    buf[40] = (uint8_t)(ppm >> 16);
    buf[41] = (uint8_t)(ppm >> 24);
    buf[42] = (uint8_t)ppm;
    buf[43] = (uint8_t)(ppm >> 8);
    buf[44] = (uint8_t)(ppm >> 16);
    buf[45] = (uint8_t)(ppm >> 24);

    return IMAGEFMT_BMP_HEADER_SIZE;
}

void imagefmt_rgb565_to_bgr24(const uint16_t *src, uint8_t *dst, size_t pixels)
{
    size_t i;

    for (i = 0; i < pixels; i++) {
        uint16_t v = src[i];
        uint8_t r5 = (uint8_t)((v >> 11) & 0x1F);
        uint8_t g6 = (uint8_t)((v >> 5) & 0x3F);
        uint8_t b5 = (uint8_t)(v & 0x1F);
        /* Expand to 8 bits with bit replication for full-scale white. */
        uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
        uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
        uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));

        dst[i * 3 + 0] = b8;
        dst[i * 3 + 1] = g8;
        dst[i * 3 + 2] = r8;
    }
}

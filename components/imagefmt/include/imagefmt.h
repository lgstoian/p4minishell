/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file imagefmt.h
 * @brief Tiny image-format helpers shared by the screenshot and camera paths.
 *
 * The only format this firmware emits today is a bottom-up 24-bit BI_RGB BMP,
 * so the header writer and the RGB565->BGR24 row conversion live here once and
 * are reused by `screenshot`, `gfx save`, and `camera snap`.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/** Size in bytes of the BMP file (14) + info (40) header. */
#define IMAGEFMT_BMP_HEADER_SIZE 54

/**
 * Fill @p buf (at least IMAGEFMT_BMP_HEADER_SIZE bytes) with a BMP file header
 * and BITMAPINFOHEADER for a bottom-up 24-bit BI_RGB image.
 *
 * @return The header size (IMAGEFMT_BMP_HEADER_SIZE).
 */
int imagefmt_write_bmp_header(uint8_t *buf, uint32_t width, uint32_t height);

/**
 * Convert @p pixels RGB565 values to tightly packed 24-bit BGR (the byte order
 * BMP stores), writing 3 bytes per pixel to @p dst.
 */
void imagefmt_rgb565_to_bgr24(const uint16_t *src, uint8_t *dst, size_t pixels);

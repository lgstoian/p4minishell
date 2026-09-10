/**
 * @file gfx.c
 * @brief RGB565 raster ops. No LVGL, no display: pure buffer math.
 */

#include "gfx.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

uint16_t gfx_rgb_to_565(uint32_t rgb)
{
    uint16_t r = (uint16_t)((rgb >> 16) & 0xFFu);
    uint16_t g = (uint16_t)((rgb >> 8) & 0xFFu);
    uint16_t b = (uint16_t)(rgb & 0xFFu);

    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

bool gfx_surface_alloc(gfx_surface_t *s, int w, int h)
{
    size_t n;

    if (s == NULL) return false;
    if (w < 1 || w > GFX_MAX_W || h < 1 || h > GFX_MAX_H) return false;
    n = (size_t)w * (size_t)h;
    s->px = heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s->px == NULL) {
        s->px = malloc(n * sizeof(uint16_t));
    }
    if (s->px == NULL) {
        s->w = 0;
        s->h = 0;
        return false;
    }
    s->w = w;
    s->h = h;
    return true;
}

void gfx_surface_free(gfx_surface_t *s)
{
    if (s == NULL) return;
    heap_caps_free(s->px);
    s->px = NULL;
    s->w = 0;
    s->h = 0;
}

void gfx_surface_clear(gfx_surface_t *s, uint16_t color)
{
    size_t n;
    size_t i;

    if (s == NULL || s->px == NULL) return;
    n = (size_t)s->w * (size_t)s->h;
    for (i = 0; i < n; i++) {
        s->px[i] = color;
    }
}

void gfx_surface_pixel(gfx_surface_t *s, int x, int y, uint16_t color)
{
    if (s == NULL || s->px == NULL) return;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->px[(size_t)y * (size_t)s->w + (size_t)x] = color;
}

uint16_t gfx_surface_get(const gfx_surface_t *s, int x, int y)
{
    if (s == NULL || s->px == NULL) return 0;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    return s->px[(size_t)y * (size_t)s->w + (size_t)x];
}

void gfx_surface_blit(gfx_surface_t *dst, const gfx_surface_t *src, int x,
                      int y, bool use_transparent, uint16_t transparent)
{
    int sy;
    int sx;

    if (dst == NULL || dst->px == NULL) return;
    if (src == NULL || src->px == NULL) return;
    for (sy = 0; sy < src->h; sy++) {
        for (sx = 0; sx < src->w; sx++) {
            uint16_t px = src->px[(size_t)sy * (size_t)src->w + (size_t)sx];

            if (use_transparent && px == transparent) {
                continue;
            }
            gfx_surface_pixel(dst, x + sx, y + sy, px);
        }
    }
}

void gfx_565_to_888_row(uint8_t *dst, const uint16_t *src, int w)
{
    int i;

    if (dst == NULL || src == NULL) return;
    for (i = 0; i < w; i++) {
        uint16_t px = src[i];

        *dst++ = (uint8_t)((px & 0x1F) << 3);          /* B */
        *dst++ = (uint8_t)(((px >> 5) & 0x3F) << 2);   /* G */
        *dst++ = (uint8_t)(((px >> 11) & 0x1F) << 3);  /* R */
    }
}

/** Read a little-endian 16-bit value. */
static uint16_t gfx_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/** Read a little-endian 32-bit value. */
static uint32_t gfx_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool gfx_bmp_parse_header(const uint8_t *buf, size_t len,
                          gfx_bmp_info_t *out)
{
    uint32_t data_offset;
    uint32_t info_size;
    int32_t w;
    int32_t h;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint64_t row_stride;
    uint64_t need;

    if (out != NULL) {
        out->w = 0;
        out->h = 0;
        out->data_offset = 0;
        out->row_stride = 0;
    }
    if (buf == NULL || out == NULL) return false;
    if (len < 54) return false;
    if (buf[0] != 'B' || buf[1] != 'M') return false;

    data_offset = gfx_le32(buf + 10);
    info_size = gfx_le32(buf + 14);
    w = (int32_t)gfx_le32(buf + 18);
    h = (int32_t)gfx_le32(buf + 22);
    planes = gfx_le16(buf + 26);
    bpp = gfx_le16(buf + 28);
    compression = gfx_le32(buf + 30);

    if (info_size != 40) return false;
    if (planes != 1) return false;
    if (bpp != 24) return false;
    if (compression != 0) return false;
    if (w < 1 || w > GFX_SPR_MAX) return false;
    if (h < 1 || h > GFX_SPR_MAX) return false; /* negative = top-down */
    if (data_offset < 54 || data_offset >= len) return false;

    row_stride = ((uint64_t)(uint32_t)w * 3u + 3u) & ~3u;
    need = (uint64_t)data_offset + row_stride * (uint64_t)(uint32_t)h;
    if (need > (uint64_t)len) return false;

    out->w = w;
    out->h = h;
    out->data_offset = data_offset;
    out->row_stride = (uint32_t)row_stride;
    return true;
}

bool gfx_bmp_decode_565(const uint8_t *buf, size_t len,
                        const gfx_bmp_info_t *info, gfx_surface_t *out)
{
    gfx_surface_t tmp = {NULL, 0, 0};
    int y;
    int x;

    if (out != NULL) {
        out->px = NULL;
        out->w = 0;
        out->h = 0;
    }
    if (buf == NULL || info == NULL || out == NULL) return false;
    if (info->w < 1 || info->h < 1) return false;
    if (!gfx_surface_alloc(&tmp, info->w, info->h)) {
        return false;
    }
    /* BMP rows are bottom-up: file row 0 is the image bottom. Bounds were
     * validated by gfx_bmp_parse_header, re-checked here so decode stays
     * safe on its own. */
    for (y = 0; y < info->h; y++) {
        const uint8_t *row = buf + info->data_offset +
                             (uint32_t)(info->h - 1 - y) * info->row_stride;

        if ((size_t)(row - buf) + (size_t)info->w * 3u > len) {
            gfx_surface_free(&tmp);
            return false;
        }
        for (x = 0; x < info->w; x++) {
            uint8_t b = row[x * 3];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];

            tmp.px[(size_t)y * (size_t)tmp.w + (size_t)x] =
                gfx_rgb_to_565(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        }
    }
    *out = tmp;
    return true;
}

void gfx_surface_line(gfx_surface_t *s, int x1, int y1, int x2, int y2,
                      uint16_t color)
{
    int dx;
    int dy;
    int sx;
    int sy;
    int err;
    int x = x1;
    int y = y1;

    if (s == NULL || s->px == NULL) return;
    dx = abs(x2 - x1);
    dy = -abs(y2 - y1);
    sx = (x1 < x2) ? 1 : -1;
    sy = (y1 < y2) ? 1 : -1;
    err = dx + dy;
    for (;;) {
        int e2;
        gfx_surface_pixel(s, x, y, color);
        if (x == x2 && y == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
}

void gfx_surface_rect(gfx_surface_t *s, int x, int y, int w, int h,
                      uint16_t color, bool fill)
{
    int i;
    int j;

    if (s == NULL || s->px == NULL) return;
    if (w < 1 || h < 1) return;
    if (fill) {
        for (j = y; j < y + h; j++) {
            for (i = x; i < x + w; i++) {
                gfx_surface_pixel(s, i, j, color);
            }
        }
        return;
    }
    for (i = x; i < x + w; i++) {
        gfx_surface_pixel(s, i, y, color);
        gfx_surface_pixel(s, i, y + h - 1, color);
    }
    for (j = y; j < y + h; j++) {
        gfx_surface_pixel(s, x, j, color);
        gfx_surface_pixel(s, x + w - 1, j, color);
    }
}

void gfx_surface_circle(gfx_surface_t *s, int cx, int cy, int r,
                        uint16_t color, bool fill)
{
    int x;
    int y;
    int err;

    if (s == NULL || s->px == NULL) return;
    if (r < 0) return;
    if (r == 0) {
        gfx_surface_pixel(s, cx, cy, color);
        return;
    }
    /* Midpoint circle; filled discs paint horizontal spans per step. */
    x = r;
    y = 0;
    err = 1 - r;
    while (x >= y) {
        if (fill) {
            gfx_surface_line(s, cx - x, cy + y, cx + x, cy + y, color);
            gfx_surface_line(s, cx - x, cy - y, cx + x, cy - y, color);
            gfx_surface_line(s, cx - y, cy + x, cx + y, cy + x, color);
            gfx_surface_line(s, cx - y, cy - x, cx + y, cy - x, color);
        } else {
            gfx_surface_pixel(s, cx + x, cy + y, color);
            gfx_surface_pixel(s, cx - x, cy + y, color);
            gfx_surface_pixel(s, cx + x, cy - y, color);
            gfx_surface_pixel(s, cx - x, cy - y, color);
            gfx_surface_pixel(s, cx + y, cy + x, color);
            gfx_surface_pixel(s, cx - y, cy + x, color);
            gfx_surface_pixel(s, cx + y, cy - x, color);
            gfx_surface_pixel(s, cx - y, cy - x, color);
        }
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

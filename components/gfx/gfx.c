/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file gfx.c
 * @brief RGB565 raster ops. No LVGL, no display: pure buffer math.
 */

#include "gfx.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

uint16_t gfx_rgb_to_565(uint32_t rgb)
{
    uint16_t r = (uint16_t)((rgb >> 16) & 0xFFu);
    uint16_t g = (uint16_t)((rgb >> 8) & 0xFFu);
    uint16_t b = (uint16_t)(rgb & 0xFFu);

    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/** Shared bounded allocator: one implementation behind every surface size. */
static bool gfx_surface_alloc_bounded(gfx_surface_t *s, int w, int h,
                                      int max_w, int max_h)
{
    size_t n;

    if (s == NULL) return false;
    if (w < 1 || w > max_w || h < 1 || h > max_h) return false;
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

bool gfx_surface_alloc(gfx_surface_t *s, int w, int h)
{
    return gfx_surface_alloc_bounded(s, w, h, GFX_MAX_W, GFX_MAX_H);
}

bool gfx_image_surface_alloc(gfx_surface_t *s, int w, int h)
{
    return gfx_surface_alloc_bounded(s, w, h, GFX_IMAGE_MAX_W, GFX_IMAGE_MAX_H);
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

void gfx_surface_hline(gfx_surface_t *s, int x, int y, int w, uint16_t color)
{
    int x0;
    int x1;
    uint16_t *row;

    if (s == NULL || s->px == NULL || w <= 0) return;
    if (y < 0 || y >= s->h) return;
    x0 = (x < 0) ? 0 : x;
    x1 = x + w;               /* exclusive */
    if (x1 > s->w) x1 = s->w;
    if (x0 >= x1) return;
    row = &s->px[(size_t)y * (size_t)s->w];
    for (; x0 < x1; x0++) {
        row[x0] = color;
    }
}

void gfx_surface_vline(gfx_surface_t *s, int x, int y, int h, uint16_t color)
{
    int y0;
    int y1;

    if (s == NULL || s->px == NULL || h <= 0) return;
    if (x < 0 || x >= s->w) return;
    y0 = (y < 0) ? 0 : y;
    y1 = y + h;
    if (y1 > s->h) y1 = s->h;
    if (y0 >= y1) return;
    for (; y0 < y1; y0++) {
        s->px[(size_t)y0 * (size_t)s->w + (size_t)x] = color;
    }
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

bool gfx_bmp_parse_header_ex(const uint8_t *buf, size_t len,
                             gfx_bmp_info_t *out, int max_w, int max_h)
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
    bool top_down;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (buf == NULL || out == NULL) return false;
    if (max_w < 1 || max_h < 1) return false;
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
    if (bpp != 24 && bpp != 32) return false;
    if (compression != 0) return false;

    top_down = (h < 0);
    if (h < 0) h = -h; /* negative height = top-down */
    if (w < 1 || w > max_w) return false;
    if (h < 1 || h > max_h) return false;
    if (data_offset < 54 || data_offset >= len) return false;

    row_stride = ((uint64_t)(uint32_t)w * (uint64_t)(bpp / 8u) + 3u) & ~3u;
    need = (uint64_t)data_offset + row_stride * (uint64_t)(uint32_t)h;
    if (need > (uint64_t)len) return false;

    out->w = w;
    out->h = h;
    out->data_offset = data_offset;
    out->row_stride = (uint32_t)row_stride;
    out->bpp = bpp;
    out->top_down = top_down;
    return true;
}

bool gfx_bmp_parse_header(const uint8_t *buf, size_t len,
                          gfx_bmp_info_t *out)
{
    if (!gfx_bmp_parse_header_ex(buf, len, out, GFX_SPR_MAX, GFX_SPR_MAX)) {
        return false;
    }
    /* Strict sprite format: 24-bit bottom-up only (the `screenshot` layout). */
    if (out->bpp != 24 || out->top_down) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

/** Read one stored BMP pixel (BGR[A]) as RGB565. */
static uint16_t gfx_bmp_pixel(const uint8_t *row, int x, uint16_t bpp)
{
    const uint8_t *p = row + (size_t)x * (size_t)(bpp / 8u);

    return gfx_rgb_to_565(((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]);
}

bool gfx_bmp_decode_scaled_565(const uint8_t *buf, size_t len,
                               const gfx_bmp_info_t *info,
                               int dst_w, int dst_h, gfx_surface_t *out)
{
    gfx_surface_t tmp = {NULL, 0, 0};
    size_t src_row_bytes;

    if (out != NULL) {
        out->px = NULL;
        out->w = 0;
        out->h = 0;
    }
    if (buf == NULL || info == NULL || out == NULL) return false;
    if (info->w < 1 || info->h < 1) return false;
    if (info->bpp != 24 && info->bpp != 32) return false;
    if (dst_w < 1 || dst_h < 1) return false;
    if (!gfx_image_surface_alloc(&tmp, dst_w, dst_h)) {
        return false;
    }
    src_row_bytes = (size_t)info->w * (size_t)(info->bpp / 8u);

    for (int y = 0; y < dst_h; y++) {
        int sy = (int)((int64_t)y * info->h / dst_h);
        int file_row;
        const uint8_t *row;

        if (sy >= info->h) sy = info->h - 1;
        /* Stored rows are bottom-up unless the header height was negative. */
        file_row = info->top_down ? sy : (info->h - 1 - sy);
        row = buf + info->data_offset + (uint64_t)(uint32_t)file_row * info->row_stride;
        if ((size_t)(row - buf) + src_row_bytes > len) {
            gfx_surface_free(&tmp);
            return false;
        }
        for (int x = 0; x < dst_w; x++) {
            int sx = (int)((int64_t)x * info->w / dst_w);

            if (sx >= info->w) sx = info->w - 1;
            tmp.px[(size_t)y * (size_t)tmp.w + (size_t)x] =
                gfx_bmp_pixel(row, sx, info->bpp);
        }
    }
    *out = tmp;
    return true;
}

bool gfx_bmp_decode_565(const uint8_t *buf, size_t len,
                        const gfx_bmp_info_t *info, gfx_surface_t *out)
{
    if (info == NULL) {
        if (out != NULL) { out->px = NULL; out->w = 0; out->h = 0; }
        return false;
    }
    return gfx_bmp_decode_scaled_565(buf, len, info, info->w, info->h, out);
}

void gfx_surface_blit_scaled(gfx_surface_t *dst, const gfx_surface_t *src,
                             int x, int y, int dw, int dh,
                             bool use_transparent, uint16_t transparent)
{
    if (dst == NULL || dst->px == NULL) return;
    if (src == NULL || src->px == NULL) return;
    if (dw < 1 || dh < 1) return;
    for (int dy = 0; dy < dh; dy++) {
        int sy = (int)((int64_t)dy * src->h / dh);

        if (sy >= src->h) sy = src->h - 1;
        for (int dx = 0; dx < dw; dx++) {
            int sx = (int)((int64_t)dx * src->w / dw);
            uint16_t px;

            if (sx >= src->w) sx = src->w - 1;
            px = src->px[(size_t)sy * (size_t)src->w + (size_t)sx];
            if (use_transparent && px == transparent) continue;
            gfx_surface_pixel(dst, x + dx, y + dy, px);
        }
    }
}

void gfx_bmp_fit(int src_w, int src_h, int max_w, int max_h,
                 int *out_w, int *out_h)
{
    int w;
    int h;

    if (out_w != NULL) *out_w = 1;
    if (out_h != NULL) *out_h = 1;
    if (src_w < 1 || src_h < 1 || max_w < 1 || max_h < 1) return;

    if (src_w <= max_w && src_h <= max_h) {
        /* Already fits: show native (never upscale). */
        w = src_w;
        h = src_h;
    } else if ((int64_t)src_w * max_h >= (int64_t)max_w * src_h) {
        w = max_w;
        h = (int)((int64_t)src_h * max_w / src_w);
    } else {
        h = max_h;
        w = (int)((int64_t)src_w * max_h / src_h);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (out_w != NULL) *out_w = w;
    if (out_h != NULL) *out_h = h;
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
            gfx_surface_hline(s, x, j, w, color);
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
            gfx_surface_hline(s, cx - x, cy + y, 2 * x + 1, color);
            gfx_surface_hline(s, cx - x, cy - y, 2 * x + 1, color);
            gfx_surface_hline(s, cx - y, cy + x, 2 * y + 1, color);
            gfx_surface_hline(s, cx - y, cy - x, 2 * y + 1, color);
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

/** 2D cross product (P-A)x(B-A); sign gives the side of line A->B. */
static int64_t gfx_edge(int ax, int ay, int bx, int by, int px, int py)
{
    return (int64_t)(bx - ax) * (int64_t)(py - ay) -
           (int64_t)(by - ay) * (int64_t)(px - ax);
}

void gfx_surface_triangle(gfx_surface_t *s, int x1, int y1, int x2, int y2,
                          int x3, int y3, uint16_t color, bool fill)
{
    int minx;
    int maxx;
    int miny;
    int maxy;
    int x;
    int y;
    int64_t area;

    if (s == NULL || s->px == NULL) return;
    if (!fill) {
        gfx_surface_line(s, x1, y1, x2, y2, color);
        gfx_surface_line(s, x2, y2, x3, y3, color);
        gfx_surface_line(s, x3, y3, x1, y1, color);
        return;
    }
    area = gfx_edge(x1, y1, x2, y2, x3, y3);
    if (area == 0) { /* degenerate: draw the outline */
        gfx_surface_line(s, x1, y1, x2, y2, color);
        gfx_surface_line(s, x2, y2, x3, y3, color);
        gfx_surface_line(s, x3, y3, x1, y1, color);
        return;
    }
    minx = x1 < x2 ? x1 : x2;
    if (x3 < minx) minx = x3;
    maxx = x1 > x2 ? x1 : x2;
    if (x3 > maxx) maxx = x3;
    miny = y1 < y2 ? y1 : y2;
    if (y3 < miny) miny = y3;
    maxy = y1 > y2 ? y1 : y2;
    if (y3 > maxy) maxy = y3;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= s->w) maxx = s->w - 1;
    if (maxy >= s->h) maxy = s->h - 1;
    for (y = miny; y <= maxy; y++) {
        for (x = minx; x <= maxx; x++) {
            int64_t w0 = gfx_edge(x1, y1, x2, y2, x, y);
            int64_t w1 = gfx_edge(x2, y2, x3, y3, x, y);
            int64_t w2 = gfx_edge(x3, y3, x1, y1, x, y);
            bool inside;

            if (area > 0) {
                inside = (w0 >= 0 && w1 >= 0 && w2 >= 0);
            } else {
                inside = (w0 <= 0 && w1 <= 0 && w2 <= 0);
            }
            if (inside) {
                s->px[(size_t)y * (size_t)s->w + (size_t)x] = color;
            }
        }
    }
}

void gfx_surface_polygon(gfx_surface_t *s, const int *xy, int n,
                         uint16_t color, bool fill)
{
    int i;
    int j;
    int miny;
    int maxy;
    int y;

    if (s == NULL || s->px == NULL || xy == NULL) return;
    if (n > GFX_POLY_MAX_PTS) n = GFX_POLY_MAX_PTS;
    if (!fill) {
        for (i = 0; i < n; i++) {
            j = (i + 1) % n;
            gfx_surface_line(s, xy[i * 2], xy[i * 2 + 1],
                             xy[j * 2], xy[j * 2 + 1], color);
        }
        return;
    }
    if (n < 3) return;
    miny = xy[1];
    maxy = xy[1];
    for (i = 1; i < n; i++) {
        if (xy[i * 2 + 1] < miny) miny = xy[i * 2 + 1];
        if (xy[i * 2 + 1] > maxy) maxy = xy[i * 2 + 1];
    }
    if (miny < 0) miny = 0;
    if (maxy >= s->h) maxy = s->h - 1;
    for (y = miny; y <= maxy; y++) {
        int xs[GFX_POLY_MAX_PTS];
        int m = 0;

        for (i = 0; i < n; i++) {
            int ax;
            int ay;
            int bx;
            int by;

            j = (i + 1) % n;
            ax = xy[i * 2];
            ay = xy[i * 2 + 1];
            bx = xy[j * 2];
            by = xy[j * 2 + 1];
            /* Half-open edge rule: include the lower y, exclude the upper. */
            if ((ay <= y && by > y) || (by <= y && ay > y)) {
                double t = (double)(y - ay) / (double)(by - ay);
                if (m < GFX_POLY_MAX_PTS) {
                    xs[m++] = (int)(ax + t * (double)(bx - ax));
                }
            }
        }
        /* Insertion sort; m is small (<= vertex count). */
        for (i = 1; i < m; i++) {
            int key = xs[i];
            int k = i - 1;

            while (k >= 0 && xs[k] > key) {
                xs[k + 1] = xs[k];
                k--;
            }
            xs[k + 1] = key;
        }
        for (i = 0; i + 1 < m; i += 2) {
            gfx_surface_hline(s, xs[i], y, xs[i + 1] - xs[i] + 1, color);
        }
    }
}

void gfx_surface_ellipse(gfx_surface_t *s, int cx, int cy, int rx, int ry,
                         uint16_t color, bool fill)
{
    int dy;

    if (s == NULL || s->px == NULL) return;
    if (rx < 0 || ry < 0) return;
    if (rx == 0 && ry == 0) {
        gfx_surface_pixel(s, cx, cy, color);
        return;
    }
    if (rx == 0) {
        gfx_surface_vline(s, cx, cy - ry, 2 * ry + 1, color);
        return;
    }
    if (ry == 0) {
        gfx_surface_hline(s, cx - rx, cy, 2 * rx + 1, color);
        return;
    }
    /* Per-scanline half-width from the ellipse equation: exact enough for a
     * raster canvas and free of integer-overflow edge cases. */
    for (dy = -ry; dy <= ry; dy++) {
        double t = 1.0 - ((double)dy * (double)dy) / ((double)ry * (double)ry);
        int dx;

        if (t < 0.0) t = 0.0;
        dx = (int)((double)rx * sqrt(t) + 0.5);
        if (fill) {
            gfx_surface_hline(s, cx - dx, cy + dy, 2 * dx + 1, color);
        } else {
            gfx_surface_pixel(s, cx - dx, cy + dy, color);
            gfx_surface_pixel(s, cx + dx, cy + dy, color);
        }
    }
}

/** Flood-fill seed point (16-bit is ample for GFX_MAX_W/H). */
typedef struct {
    int16_t x;
    int16_t y;
} gfx_ff_pt_t;

/** Push a seed, growing the PSRAM-backed stack. @return false on OOM. */
static bool gfx_ff_push(gfx_ff_pt_t **st, size_t *n, size_t *cap, int x, int y)
{
    if (*n == *cap) {
        size_t ncap = *cap * 2;
        gfx_ff_pt_t *ns = heap_caps_realloc(*st, ncap * sizeof(**st),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (ns == NULL) ns = realloc(*st, ncap * sizeof(**st));
        if (ns == NULL) return false;
        *st = ns;
        *cap = ncap;
    }
    (*st)[*n].x = (int16_t)x;
    (*st)[*n].y = (int16_t)y;
    (*n)++;
    return true;
}

int gfx_surface_flood_fill(gfx_surface_t *s, int x, int y, uint16_t color)
{
    uint16_t target;
    gfx_ff_pt_t *st;
    size_t cap = 512;
    size_t n = 0;
    int filled = 0;

    if (s == NULL || s->px == NULL) return 0;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    target = gfx_surface_get(s, x, y);
    if (target == color) return 0;

    st = heap_caps_malloc(cap * sizeof(*st), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (st == NULL) st = malloc(cap * sizeof(*st));
    if (st == NULL) return 0;
    st[n].x = (int16_t)x;
    st[n].y = (int16_t)y;
    n++;

    while (n > 0) {
        int px;
        int py;
        int lx;
        int rx;
        int ny;

        n--;
        px = st[n].x;
        py = st[n].y;
        if (gfx_surface_get(s, px, py) != target) continue;
        lx = px;
        while (lx - 1 >= 0 && gfx_surface_get(s, lx - 1, py) == target) lx--;
        rx = px;
        while (rx + 1 < s->w && gfx_surface_get(s, rx + 1, py) == target) rx++;
        gfx_surface_hline(s, lx, py, rx - lx + 1, color);
        filled += rx - lx + 1;
        for (ny = py - 1; ny <= py + 1; ny += 2) {
            int ix;
            bool in_run = false;

            if (ny < 0 || ny >= s->h) continue;
            for (ix = lx; ix <= rx; ix++) {
                if (gfx_surface_get(s, ix, ny) == target) {
                    if (!in_run) {
                        if (!gfx_ff_push(&st, &n, &cap, ix, ny)) {
                            /* OOM: stop the fill early (the canvas is never
                             * corrupted); `filled` reports what was done. */
                            heap_caps_free(st);
                            return filled;
                        }
                        in_run = true;
                    }
                } else {
                    in_run = false;
                }
            }
        }
    }
    heap_caps_free(st);
    return filled;
}

int gfx_surface_text(gfx_surface_t *s, int x, int y, const char *text,
                     uint16_t color, uint16_t bg, bool use_bg, int scale)
{
    int startx;
    const unsigned char *p;

    if (s == NULL || s->px == NULL || text == NULL) return 0;
    if (scale < 1) scale = 1;
    startx = x;
    for (p = (const unsigned char *)text; *p != '\0'; p++) {
        unsigned char ch = *p;
        const uint8_t *glyph;
        int gy;

        if (ch == '\n') {
            x = startx;
            y += GFX_FONT_H * scale;
            continue;
        }
        if (ch < GFX_FONT_FIRST || ch > GFX_FONT_LAST) ch = '?';
        glyph = gfx_font8x8[ch - GFX_FONT_FIRST];
        for (gy = 0; gy < GFX_FONT_H; gy++) {
            uint8_t bits = glyph[gy];
            int gx;

            for (gx = 0; gx < GFX_FONT_W; gx++) {
                bool on = (bits & (uint8_t)(1u << (GFX_FONT_W - 1 - gx))) != 0;
                int sy;

                if (!on && !use_bg) continue;
                for (sy = 0; sy < scale; sy++) {
                    gfx_surface_hline(s, x + gx * scale,
                                      y + gy * scale + sy, scale,
                                      on ? color : bg);
                }
            }
        }
        x += GFX_FONT_W * scale;
    }
    return x - startx;
}

int gfx_text_width(const char *text, int scale)
{
    if (text == NULL) return 0;
    if (scale < 1) scale = 1;
    return (int)strlen(text) * GFX_FONT_W * scale;
}

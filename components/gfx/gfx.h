/**
 * @file gfx.h
 * @brief Tiny RGB565 raster surface for P4MiniShell `gfx` batch verbs.
 *
 * Pure pixel math, no LVGL dependency: the buffer is PSRAM-backed and the
 * command layer displays it through an lv_canvas. Every op clips to the
 * surface (out-of-bounds pixels are ignored, never an error), so computed
 * game coordinates are safe. All functions are headless-safe and unit
 * tested (test_gfx.c); LVGL display glue lives in
 * components/command/gfx_commands.c.
 */

#ifndef P4MINISHELL_GFX_H
#define P4MINISHELL_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time canvas budget (320x240 RGB565 = 150 KB PSRAM). */
#define GFX_MAX_W 320
#define GFX_MAX_H 240

/** Sprite bank budget (SNES-class 16-bit assets for `gfx load/blit`):
 * 8 slots x at most 64x64 RGB565 (8 KB each, 64 KB worst case). */
#define GFX_SPR_SLOTS 8
#define GFX_SPR_MAX 64

/** Largest BMP file `gfx load` will ingest (256 KB: a 64x64 24-bit BMP is
 * ~12 KB; the budget covers headers + padding with headroom while keeping
 * the PSRAM staging buffer bounded). */
#define GFX_BMP_MAX_BYTES 262144

/** Raster surface: row-major RGB565 pixels. */
typedef struct {
    uint16_t *px; /**< PSRAM pixel buffer (NULL when unallocated). */
    int w;        /**< Width in pixels. */
    int h;        /**< Height in pixels. */
} gfx_surface_t;

/** Convert 24-bit RGB to RGB565. */
uint16_t gfx_rgb_to_565(uint32_t rgb);

/** Allocate a w*h RGB565 PSRAM surface (1 <= w <= GFX_MAX_W,
 * 1 <= h <= GFX_MAX_H). @return true on success. */
bool gfx_surface_alloc(gfx_surface_t *s, int w, int h);

/** Free a surface (NULL-safe, clears dims). */
void gfx_surface_free(gfx_surface_t *s);

/** Fill the whole surface with one RGB565 color. */
void gfx_surface_clear(gfx_surface_t *s, uint16_t color);

/** Plot one pixel (clipped, ignored out of bounds). */
void gfx_surface_pixel(gfx_surface_t *s, int x, int y, uint16_t color);

/** Bresenham line (clipped per-pixel). */
void gfx_surface_line(gfx_surface_t *s, int x1, int y1, int x2, int y2,
                      uint16_t color);

/** Rectangle at x,y,w,h. fill=true paints the interior, else 1px border. */
void gfx_surface_rect(gfx_surface_t *s, int x, int y, int w, int h,
                      uint16_t color, bool fill);

/** Circle at cx,cy radius r. fill=true paints the disc, else 1px outline. */
void gfx_surface_circle(gfx_surface_t *s, int cx, int cy, int r,
                        uint16_t color, bool fill);

/** Read one pixel (0 outside the surface). Headless test helper. */
uint16_t gfx_surface_get(const gfx_surface_t *s, int x, int y);

/** Copy src onto dst at x,y (clipped; out-of-bounds ignored, never an
 * error). When use_transparent is true, pixels equal to transparent are
 * skipped (sprite transparency). */
void gfx_surface_blit(gfx_surface_t *dst, const gfx_surface_t *src, int x,
                      int y, bool use_transparent, uint16_t transparent);

/** Convert one RGB565 row to RGB888 BGR triples (BMP file order, one byte
 * per channel, no row padding). Pure row primitive shared by `gfx save`
 * (the screenshot path keeps its own local copy; this one is unit-tested
 * here in the raster core). */
void gfx_565_to_888_row(uint8_t *dst, const uint16_t *src, int w);

/** Parsed 24-bit BMP geometry (the only ingest format: 24-bit BI_RGB
 * bottom-up, exactly what `screenshot <file>` writes). */
typedef struct {
    int w;               /**< Width in pixels (1..GFX_SPR_MAX). */
    int h;               /**< Height in pixels (1..GFX_SPR_MAX). */
    uint32_t data_offset; /**< Byte offset of the first (bottom) row. */
    uint32_t row_stride;  /**< Padded bytes per row (4-byte aligned). */
} gfx_bmp_info_t;

/** Validate a BMP header (needs only the first 54 bytes; len is the total
 * buffer size). Accepts signature 'BM', 40-byte info, planes 1, 24 bpp,
 * BI_RGB, positive height (bottom-up), dims within GFX_SPR_MAX, and pixel
 * data fully inside the buffer. @return true with geometry in @p out. */
bool gfx_bmp_parse_header(const uint8_t *buf, size_t len,
                          gfx_bmp_info_t *out);

/** Decode validated BMP pixels to a freshly allocated RGB565 surface
 * (PSRAM, same allocator as gfx_surface_alloc). @return true on success
 * (caller frees with gfx_surface_free), false leaving @p out cleared. */
bool gfx_bmp_decode_565(const uint8_t *buf, size_t len,
                        const gfx_bmp_info_t *info, gfx_surface_t *out);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_GFX_H */

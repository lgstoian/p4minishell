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

/** Built-in 8x8 ASCII font (`gfx_surface_text`): glyph index = ch-0x20,
 * one byte per row, MSB = leftmost pixel. Data lives in gfx_font.c. */
#define GFX_FONT_W 8
#define GFX_FONT_H 8
#define GFX_FONT_FIRST 0x20
#define GFX_FONT_LAST 0x7E
#define GFX_FONT_GLYPHS (GFX_FONT_LAST - GFX_FONT_FIRST + 1)

/** Polygon vertex cap for `gfx_surface_polygon` (scanline intersection
 * scratch). */
#define GFX_POLY_MAX_PTS 64

extern const uint8_t gfx_font8x8[GFX_FONT_GLYPHS][GFX_FONT_H];

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

/** Horizontal span (x..x+w-1) at row y, clipped. Faster than per-pixel. */
void gfx_surface_hline(gfx_surface_t *s, int x, int y, int w, uint16_t color);

/** Vertical span (y..y+h-1) at column x, clipped. */
void gfx_surface_vline(gfx_surface_t *s, int x, int y, int h, uint16_t color);

/** Triangle through three vertices. fill=true paints the interior
 * (edge-function test over the bounding box), else the 3 edges. */
void gfx_surface_triangle(gfx_surface_t *s, int x1, int y1, int x2, int y2,
                          int x3, int y3, uint16_t color, bool fill);

/** Polygon through @p n vertices; @p xy holds 2n ints (x0,y0,x1,y1,...).
 * fill=true uses the even-odd scanline rule (convex or concave), else draws
 * the closed outline. At most GFX_POLY_MAX_PTS vertices are used. */
void gfx_surface_polygon(gfx_surface_t *s, const int *xy, int n,
                         uint16_t color, bool fill);

/** Axis-aligned ellipse at cx,cy radii rx,ry. fill=true paints the disc,
 * else a 1px outline. rx==0 or ry==0 degenerates to a line. */
void gfx_surface_ellipse(gfx_surface_t *s, int cx, int cy, int rx, int ry,
                         uint16_t color, bool fill);

/** 4-way flood fill starting at x,y. Fills the connected region whose color
 * equals the seed pixel (no-op when the seed already holds @p color or is
 * out of bounds). @return the number of pixels filled. */
int gfx_surface_flood_fill(gfx_surface_t *s, int x, int y, uint16_t color);

/** Draw 8x8 ASCII @p text at x,y. When @p use_bg is true, glyph cell
 * backgrounds are filled with @p bg (else transparent). @p scale is an
 * integer pixel multiplier (<=0 treated as 1). @return the advance width. */
int gfx_surface_text(gfx_surface_t *s, int x, int y, const char *text,
                     uint16_t color, uint16_t bg, bool use_bg, int scale);

/** Advance width for @p text at @p scale (GFX_FONT_W per char). */
int gfx_text_width(const char *text, int scale);

/** Read one pixel (0 outside the surface). Headless test helper. */
uint16_t gfx_surface_get(const gfx_surface_t *s, int x, int y);

/** World-coordinate viewport (gfx_view.c): maps a math window
 * (xmin..xmax, ymin..ymax, y up) onto an integer raster rect
 * (px, py, pw, ph). The rect is plain integer coordinates, so the same code
 * drives the 0-based `gfx` pixel canvas and the 1-based TUI cell grid —
 * the caller picks the origin convention. Pure, headless-safe. */
typedef struct {
    double xmin; /**< World left. */
    double xmax; /**< World right (must exceed xmin). */
    double ymin; /**< World bottom. */
    double ymax; /**< World top (must exceed ymin). */
    int px;      /**< Raster rect origin x. */
    int py;      /**< Raster rect origin y. */
    int pw;      /**< Raster rect width (> 0). */
    int ph;      /**< Raster rect height (> 0). */
} gfx_view_t;

/** Set a viewport (no validation beyond storing; map/clip reject
 * degenerate windows). */
void gfx_view_set(gfx_view_t *v, double xmin, double xmax, double ymin,
                  double ymax, int px, int py, int pw, int ph);

/** Map a world point to raster ints. @return true with @p sx/@p sy set when
 * the rounded point lands strictly inside the rect (non-finite inputs
 * and degenerate windows return false). */
bool gfx_view_map(const gfx_view_t *v, double x, double y, int *sx, int *sy);

/** Plot one world point (draws only when it lands inside the rect). */
void gfx_view_point(const gfx_view_t *v, gfx_surface_t *s, double x, double y,
                    uint16_t color);

/** Clip a world segment to the view rect (Cohen-Sutherland on the integer
 * rect, so no giant coordinates ever reach the rasterizer). @return true
 * with clipped integer endpoints when any part is visible. */
bool gfx_view_clip_line(const gfx_view_t *v, double x1, double y1, double x2,
                        double y2, int *ax, int *ay, int *bx, int *by);

/** Clip + draw a world segment on a surface. @return true when drawn. */
bool gfx_view_line(const gfx_view_t *v, gfx_surface_t *s, double x1, double y1,
                   double x2, double y2, uint16_t color);

/** "Nice" axis tick spacing for @p range aiming at @p ticks (1/2/5x10^n).
 * Returns 1.0 on a non-positive range or tick count. */
double gfx_view_nice_step(double range, int ticks);/** Copy src onto dst at x,y (clipped; out-of-bounds ignored, never an
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

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file gfx.h
 * @module gfx
 * @layer leaf
 * @domain ui
 * @provides RGB565 raster, BMP parser/decoder, scaling, 8x8 font
 * @status stable
 * @test test/main/test_gfx.c
 * @hard-rule ai-context.md#image-support-rules
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

/** General image budget for `view` / `draw image` / `gfx image`: a
 * fit-to-screen decode target stays within this (the native image is never
 * materialized). 1280x1280 RGB565 = 3.2 MB PSRAM worst case — fits the
 * Tab5's own screenshots in either rotation (1280x720 landscape,
 * 720x1280 portrait); the reference board's 1024x600 shots fit with room. */
#define GFX_IMAGE_MAX_W 1280
#define GFX_IMAGE_MAX_H 1280

/** Largest BMP file `gfx load` will ingest (256 KB: a 64x64 24-bit BMP is
 * ~12 KB; the budget covers headers + padding with headroom while keeping
 * the PSRAM staging buffer bounded). */
#define GFX_BMP_MAX_BYTES 262144

/** Built-in 8x8 CP437 font (`gfx_surface_text`): glyph index = ch,
 * one byte per row, MSB = leftmost pixel. Data lives in gfx_font.c.
 * Covers full CP437 range 0x00-0xFF (256 glyphs). */
#define GFX_FONT_W 8
#define GFX_FONT_H 8
#define GFX_FONT_FIRST 0x00
#define GFX_FONT_LAST 0xFF
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

/** Parsed BMP geometry for the supported ingest formats: 24-bit or 32-bit
 * BI_RGB, bottom-up (positive height) or top-down (negative height). */
typedef struct {
    int w;               /**< Width in pixels. */
    int h;               /**< Height in pixels (always positive here). */
    uint32_t data_offset; /**< Byte offset of the first stored row. */
    uint32_t row_stride;  /**< Padded bytes per row (4-byte aligned). */
    uint16_t bpp;         /**< Bits per pixel: 24 or 32. */
    bool top_down;        /**< true when the stored height was negative. */
} gfx_bmp_info_t;

/**
 * General BMP header validation (needs only the first 54 bytes; @p len is the
 * total buffer size). Accepts signature 'BM', 40-byte info header, planes 1,
 * 24- or 32-bit BI_RGB, positive (bottom-up) or negative (top-down) height,
 * dimensions within @p max_w x @p max_h, and pixel data fully inside the
 * buffer. @return true with geometry in @p out (zeroed on failure).
 */
bool gfx_bmp_parse_header_ex(const uint8_t *buf, size_t len,
                             gfx_bmp_info_t *out, int max_w, int max_h);

/**
 * Strict sprite-format wrapper over gfx_bmp_parse_header_ex(): accepts only
 * 24-bit BI_RGB bottom-up art within GFX_SPR_MAX x GFX_SPR_MAX — exactly the
 * format `screenshot <file>` writes. Used by `gfx load`.
 */
bool gfx_bmp_parse_header(const uint8_t *buf, size_t len,
                          gfx_bmp_info_t *out);

/** Allocate an image-sized RGB565 PSRAM surface (1..GFX_IMAGE_MAX_W x
 * 1..GFX_IMAGE_MAX_H). Same allocator/free as gfx_surface_alloc; used by the
 * fit-to-screen image path. */
bool gfx_image_surface_alloc(gfx_surface_t *s, int w, int h);

/** Decode + nearest-sample a validated BMP straight to a dst_w x dst_h RGB565
 * surface (never materializes the native image, so large art stays bounded).
 * Handles 24/32-bit and both orientations. @return true on success (caller
 * frees with gfx_surface_free), false leaving @p out cleared. */
bool gfx_bmp_decode_scaled_565(const uint8_t *buf, size_t len,
                               const gfx_bmp_info_t *info,
                               int dst_w, int dst_h, gfx_surface_t *out);

/** Decode validated BMP pixels at native size to a freshly allocated RGB565
 * surface (same allocator as gfx_surface_alloc). @return true on success
 * (caller frees with gfx_surface_free), false leaving @p out cleared. */
bool gfx_bmp_decode_565(const uint8_t *buf, size_t len,
                        const gfx_bmp_info_t *info, gfx_surface_t *out);

/** Blit @p src scaled (nearest-neighbour) into @p dst at x,y with target size
 * dw x dh (clipped; optional transparency skips matching source pixels). */
void gfx_surface_blit_scaled(gfx_surface_t *dst, const gfx_surface_t *src,
                             int x, int y, int dw, int dh,
                             bool use_transparent, uint16_t transparent);

/** Copy @p src rotated clockwise by @p turns quarter-turns (0..3, normalized)
 * into a freshly allocated surface (caller frees with gfx_surface_free).
 * @return true on success (caller owns @p out), false leaving @p out cleared. */
bool gfx_surface_rotate_cw(const gfx_surface_t *src, int turns, gfx_surface_t *out);

/** Largest w,h that preserves @p src_w x @p src_h aspect within
 * @p max_w x @p max_h. Never upscales (a source that already fits is
 * returned unchanged); always yields >= 1. */
void gfx_bmp_fit(int src_w, int src_h, int max_w, int max_h,
                 int *out_w, int *out_h);

/*
 * Additive toolkit extensions. Every function below is a NEW entry point: the
 * pixel-exact primitives above are untouched, so existing screenshots and
 * Unity cases keep their exact bytes. All of these stay pure buffer math with
 * no LVGL or display dependency.
 */

/** Blend @p src over @p dst with an integer coverage 0..256 (256 = opaque,
 * 0 = leave @p dst untouched). Rounded per 5/6/5 channel. Also the shared
 * blend primitive behind the anti-aliased and compositing helpers. */
uint16_t gfx_blend_565(uint16_t dst, uint16_t src, unsigned coverage);

/** Anti-aliased line from (x1,y1) to (x2,y2). Samples at pixel centers and
 * blends two rows per column by coverage; endpoints are fully opaque.
 * Clipped per pixel, NULL-safe. Unlike gfx_surface_line this writes blended
 * colours, so it is a separate verb (the original stays pixel-exact). */
void gfx_surface_line_aa(gfx_surface_t *s, int x1, int y1, int x2, int y2,
                         uint16_t color);

/** Anti-aliased 1px circle outline at cx,cy radius r, blended by the distance
 * from the ideal radius. r<0 is ignored; clipped and NULL-safe. */
void gfx_surface_circle_aa(gfx_surface_t *s, int cx, int cy, int r,
                           uint16_t color);

/** Anti-aliased 1px rectangle outline at x,y,w,h, blended by the distance to
 * the ideal border. Degenerate sizes are ignored; clipped and NULL-safe. */
void gfx_surface_rect_aa(gfx_surface_t *s, int x, int y, int w, int h,
                         uint16_t color);

/** Linear gradient over the rect x,y,w,h from @p c0 to @p c1. @p vertical
 * true interpolates down the rows, false across the columns. Endpoint cells
 * hold the exact endpoint colours; one-cell axes use @p c0. Clipped and
 * NULL-safe. */
void gfx_surface_gradient(gfx_surface_t *s, int x, int y, int w, int h,
                          uint16_t c0, uint16_t c1, bool vertical);

/** Blit one cell of a sprite sheet (atlas) onto @p dst at x,y. @p cols is the
 * cell grid width, @p cell_w/@p cell_h the cell size, @p index the
 * row-major cell number. Invalid geometry or an out-of-range index is a
 * no-op; transparency works exactly as in gfx_surface_blit. Reuses the one
 * blit path - there is no second sprite loader or surface type. */
void gfx_surface_blit_atlas(gfx_surface_t *dst, const gfx_surface_t *sheet,
                            int cell_w, int cell_h, int cols, int index,
                            int x, int y, bool use_transparent,
                            uint16_t transparent);

/** Composite a whole offscreen layer (any gfx_surface_t, including a sprite
 * slot) over @p dst at x,y with integer coverage @p coverage (0..256).
 * Pixels outside the target are clipped; caller keeps ownership of @p src. */
void gfx_surface_composite(gfx_surface_t *dst, const gfx_surface_t *src,
                           int x, int y, unsigned coverage);

/** As gfx_surface_composite but skips source pixels equal to @p transparent
 * before blending (sprite-style keyed layer). The key is never blended. */
void gfx_surface_composite_keyed(gfx_surface_t *dst, const gfx_surface_t *src,
                                 int x, int y, unsigned coverage,
                                 uint16_t transparent);

/*
 * Bounded game helpers. Tilemaps stamp cells through the ONE atlas blit core,
 * particles write through the ONE pixel/blend pair, and layers composite
 * through the ONE composite primitive. No second surface type or blit path.
 * All of it is pure buffer math, headless-safe and unit tested (test_gfx.c).
 */

/** Named tilemaps: 4 maps x at most 64x64 cells = 16 KB of cell indices each. */
#define GFX_TILEMAP_SLOTS 4
#define GFX_TILEMAP_MAX_COLS 64
#define GFX_TILEMAP_MAX_ROWS 64
#define GFX_TILEMAP_MAX_CELL 64
#define GFX_TILEMAP_NAME_MAX 16

/** Per-tile flags, stored in the upper byte of a packed cell. */
#define GFX_TILE_HIDDEN  0x01u /**< Skip this cell when drawing. */
#define GFX_TILE_FLIP_X  0x02u /**< Mirror the cell horizontally. */
#define GFX_TILE_FLIP_Y  0x04u /**< Mirror the cell vertically. */
#define GFX_TILE_ROT90   0x08u /**< Rotate the cell 90 degrees clockwise. */

/** One named map over a borrowed sprite sheet. The cell array is bounded
 *  PSRAM; the sheet stays owned by the caller (a `gfx load` sprite slot). */
typedef struct {
    char name[GFX_TILEMAP_NAME_MAX];
    const gfx_surface_t *sheet;
    uint32_t *cells;   /**< index in bits 0..15, flags in bits 16..23. */
    int cols;
    int rows;
    int cell_w;
    int cell_h;
    int z;             /**< Draw order: lower z is painted first. */
    bool use_transparent;
    uint16_t transparent;
} gfx_tilemap_t;

/** Bind a sheet to a map and allocate its bounded cell grid (zeroed). */
bool gfx_tilemap_create(gfx_tilemap_t *map, const char *name,
                        const gfx_surface_t *sheet, int cols, int rows,
                        int cell_w, int cell_h, int z,
                        bool use_transparent, uint16_t transparent);

/** Release the cell grid; the borrowed sheet is never freed. NULL-safe. */
void gfx_tilemap_free(gfx_tilemap_t *map);

/** Set one cell's tile index (0..65535) and flag mask. @return false OOB. */
bool gfx_tilemap_set(gfx_tilemap_t *map, int col, int row, int index,
                     unsigned flags);

/** Read a packed cell (index | flags<<16), or 0 for an unbound/out-of-range map. */
uint32_t gfx_tilemap_get(const gfx_tilemap_t *map, int col, int row);

/** Draw one map at x,y. Every stamp goes through gfx_surface_blit_atlas();
 *  flip/rotate use scratch gfx_surface_t cells, never a second blit routine. */
void gfx_tilemap_draw(const gfx_tilemap_t *map, gfx_surface_t *dst, int x, int y);

/** Draw @p count maps in ascending z order (ties keep caller order). */
void gfx_tilemap_draw_all(gfx_tilemap_t **maps, int count,
                          gfx_surface_t *dst, int x, int y);

/** Deterministic LCG (one RNG for emitter tests and games). */
uint32_t gfx_rand_next(uint32_t *state);

/** Bounded particle pool: 8.8 fixed-point motion, deterministic seed. */
#define GFX_PARTICLE_MAX 128

typedef struct {
    int16_t x;        /**< 8.8 fixed point. */
    int16_t y;        /**< 8.8 fixed point. */
    int16_t vx;       /**< 8.8 fixed point per tick. */
    int16_t vy;       /**< 8.8 fixed point per tick. */
    uint16_t life;    /**< Remaining ticks. */
    uint16_t max_life;
    uint16_t color;
    uint8_t size;
    uint8_t alive;
} gfx_particle_t;

typedef struct {
    uint32_t rng;     /**< Seed state; identical seed => identical particle stream. */
    gfx_particle_t p[GFX_PARTICLE_MAX];
} gfx_emitter_t;

/** Clear the pool and seed the generator (seed 0 is a valid state). */
void gfx_emitter_reset(gfx_emitter_t *e, uint32_t seed);

/** Spawn up to @p n particles around x,y. Offsets/velocities/lifetimes all
 *  come from the seeded LCG, so a host test can replay them exactly.
 *  @return the number actually spawned (bounded by free slots). */
int gfx_emitter_emit(gfx_emitter_t *e, int n, int x, int y, int spread,
                     uint16_t color, uint16_t life);

/** Advance @p ticks steps with @p gravity (8.8 per tick); retire at life 0.
 *  @return the number still alive. */
int gfx_emitter_step(gfx_emitter_t *e, int ticks, int gravity);

/** Draw live particles with life-proportional coverage via gfx_blend_565()
 *  and the existing pixel/rect primitives. */
void gfx_emitter_draw(const gfx_emitter_t *e, gfx_surface_t *dst,
                      int ox, int oy);

/** Number of live particles. */
int gfx_emitter_live(const gfx_emitter_t *e);

/** Layers: metadata over a borrowed gfx_surface_t, never a second surface. */
#define GFX_LAYER_SLOTS 8
#define GFX_LAYER_NAME_MAX 16

typedef struct {
    const gfx_surface_t *surface; /**< Borrowed pixel buffer. */
    char name[GFX_LAYER_NAME_MAX];
    int x;
    int y;
    int z;            /**< Present order: lower z first. */
    unsigned alpha;   /**< 0..256 integer coverage. */
    bool visible;
    bool dirty;
    int dx;           /**< Layer-local dirty rectangle. */
    int dy;
    int dw;
    int dh;
} gfx_layer_t;

/** Bind a borrowed surface to a layer slot (alpha clamped to 0..256). */
void gfx_layer_bind(gfx_layer_t *layer, const char *name,
                    const gfx_surface_t *surface, int x, int y,
                    unsigned alpha, int z);

/** Union a layer-local rect into the dirty rect (no rect = whole surface). */
bool gfx_layer_mark_dirty(gfx_layer_t *layer, int x, int y, int w, int h);

/** Clear the dirty flag after a successful present. */
void gfx_layer_clear_dirty(gfx_layer_t *layer);

/** Dirty rectangle in destination coordinates. @return false when clean. */
bool gfx_layer_dirty_rect(const gfx_layer_t *layer,
                          int *x, int *y, int *w, int *h);

/** Present one dirty layer through gfx_surface_composite() (no second path). */
void gfx_layer_present(gfx_surface_t *dst, gfx_layer_t *layer);

/** Present all dirty layers in ascending z order. */
void gfx_layer_present_all(gfx_surface_t *dst, gfx_layer_t **layers, int count);

/**
 * @brief Frame-present statistics for a graphical app loop.
 *
 * Pure and headless-safe so the policy is unit-tested (test_gfx.c). The
 * caller feeds present timestamps (from esp_timer_get_time()); the struct
 * derives the frame count, min/average/max interval, interval jitter
 * (standard deviation) and the count of frames that overran 150% of the
 * configured target (dropped frames).
 */
typedef struct {
    uint64_t started_us;  /**< First present timestamp of the run (0 = none). */
    uint64_t last_us;     /**< Previous present timestamp (0 = none yet). */
    uint64_t sum_us;      /**< Sum of measured intervals. */
    uint64_t sum_sq_us;   /**< Sum of squared intervals (for jitter). */
    uint32_t frames;      /**< Number of measured intervals. */
    uint32_t min_us;      /**< Shortest interval. */
    uint32_t max_us;      /**< Longest interval. */
    uint32_t dropped;     /**< Intervals over 150% of the target. */
    uint32_t target_us;   /**< Target interval in us (0 = auto/none). */
} gfx_frame_stats_t;

/** Clear all frame statistics. */
void gfx_frame_stats_reset(gfx_frame_stats_t *s);

/** Record one present at @p now_us (the first call only sets the baseline). */
void gfx_frame_stats_sample(gfx_frame_stats_t *s, uint64_t now_us);

/** Set the target frame rate used to count dropped frames (0 disables). */
void gfx_frame_stats_set_target_fps(gfx_frame_stats_t *s, uint32_t fps);

/** Average interval in microseconds (0 until two presents are sampled). */
uint32_t gfx_frame_stats_avg_us(const gfx_frame_stats_t *s);

/** Interval standard deviation ("jitter") in microseconds. */
uint32_t gfx_frame_stats_jitter_us(const gfx_frame_stats_t *s);

/** Frames per second derived from the average interval (0.0 if unknown). */
double gfx_frame_stats_fps(const gfx_frame_stats_t *s);

/**
 * @brief Format frame stats as a machine-readable, integer-only line body.
 *
 * Shared by `gfx stats` and `tui stats` (and parsed by tools/p4test/perf.py):
 * `frames=N min_us=N avg_us=N max_us=N jitter_us=N dropped=N fps10=N
 * target_fps=N`. Returns the number of characters written (excluding NUL).
 */
int gfx_frame_stats_format(const gfx_frame_stats_t *s, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_GFX_H */

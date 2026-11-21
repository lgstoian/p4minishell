/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file gfx_commands.c
 * @brief `gfx` pixel-canvas verbs for P4MiniShell batch games.
 *
 * Owns one RGB565 canvas (see components/gfx): `gfx init` allocates the
 * PSRAM buffer and shows an lv_canvas in the transcript region,
 * raster verbs mutate the buffer, `gfx show` pushes it to the display,
 * `gfx close` tears down. Ops never auto-show, so animations flicker-free
 * compose several ops per `gfx show`. Sprite slots (`gfx load/blit`,
 * 8 x 64x64 RGB565) hold BMP-ingested art for stamping. `gfx save`
 * writes the canvas as a 24-bit BMP. The canvas is exclusive with TUI
 * mode (`draw close` first) and refused in background jobs (shared
 * display surface, same rule as modal/key-wait/appmode verbs).
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "shell.h"
#include "batch.h"
#include "command.h"
#include "gfx.h"
#include "imgdec.h"
#include "tui.h"
#include "storage.h"
#include "windows.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_cache.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

static gfx_surface_t s_gfx = {NULL, 0, 0};
static lv_obj_t *s_gfx_canvas = NULL;
/* Last presented canvas pixels. `gfx show` is the present point: an app that
 * clears and redraws the whole canvas every frame still only changes the
 * moving sprite's pixels, so diffing against this shadow turns the present
 * from a full scaled-canvas repaint (the 15 fps, tearing BOUNCE) into a small
 * rectangle invalidation. PSRAM, one sample size, freed on teardown. */
static uint16_t *s_gfx_shadow = NULL;
static size_t s_gfx_shadow_bytes = 0;

/* ------------------------------------------------------------------------
 * Tear-free partial presents (v1.4.4 blocker).
 *
 * The DSI panel scans a single framebuffer while LVGL's flush copies the
 * dirty rectangle into it. A fast direction change (the ball hitting a wall)
 * can be caught mid-scan and show a one-frame tear. Instead of writing during
 * the scan, stage the changed canvas rows (already scaled to screen pixels by
 * the integer canvas scale) in a small internal-RAM back buffer and copy them
 * into the framebuffer from the DPI bridge's on_vsync hook, i.e. during
 * blanking. The hook runs from the bridge ISR, so it is IRAM_ATTR and every
 * object it reads/writes is forced into internal DRAM. The render cost is
 * still proportional to the sprite: width * changed_rows * 2 bytes, capped.
 * ------------------------------------------------------------------------ */
/** Prefix of lvgl_port_display_ctx_t, mirrored the way serial_commands.c
 *  reaches the panel handle without depending on the port's private header. */
typedef struct {
    int disp_type;
    void *io_handle;
    esp_lcd_panel_handle_t panel_handle;
} gfx_port_disp_prefix_t;

/** Upper bound on the internal-RAM staging buffer. A full-canvas change (the
 *  first present, or a `gfx image`) falls back to LVGL's normal invalidation. */
#define GFX_PRESENT_STAGE_MAX_BYTES (96u * 1024u)

static portMUX_TYPE s_gfx_present_mux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR static uint8_t *s_present_fb;          /**< scanned DSI framebuffer */
DRAM_ATTR static uint32_t s_present_fb_stride;   /**< bytes per screen row */
DRAM_ATTR static int s_present_fb_w;
DRAM_ATTR static int s_present_fb_h;
DRAM_ATTR static uint8_t *s_present_stage;       /**< internal-RAM back buffer */
DRAM_ATTR static size_t s_present_stage_cap;
DRAM_ATTR static volatile bool s_present_pending;
DRAM_ATTR static volatile bool s_present_in_isr;
DRAM_ATTR static uint32_t s_present_sx;
DRAM_ATTR static uint32_t s_present_sy;
DRAM_ATTR static uint32_t s_present_cols;
DRAM_ATTR static uint32_t s_present_rows;
DRAM_ATTR static uint32_t s_present_row_bytes;

/** Fullscreen state saved on `gfx init`, restored on `gfx close` (F25-C). */
static bool s_gfx_fullscreen_saved;

/* Frame pacing for the active canvas app: `gfx show` is the app's present
 * point, so the interval between consecutive shows is the app-driven frame
 * time. Exposed through `gfx stats` and parsed by tools/p4test/perf.py. */
static gfx_frame_stats_t s_gfx_stats;
#define GFX_DEFAULT_TARGET_FPS 30u

#define GFX_STATS_FIELDS 160

/* Sprite bank: PSRAM RGB565 slots for `gfx load/blit` (SNES-class 16-bit
 * assets, at most GFX_SPR_SLOTS x GFX_SPR_MAX x GFX_SPR_MAX). Zero-init:
 * a slot is live exactly when px != NULL. */
static gfx_surface_t s_gfx_spr[GFX_SPR_SLOTS];

/** Map a color arg to RGB565: 0..15 = CGA table, 16 = black (canvas has no
 * "default"), larger = 24-bit RGB hex. */
static uint16_t gfx_color_arg(const char *s, uint16_t fallback)
{
    unsigned long v;
    char *end = NULL;
    uint32_t rgb;

    if (s == NULL || *s == '\0') return fallback;
    v = strtoul(s, &end, 0);
    if (end == s) return fallback;
    if (v <= 16) {
        rgb = (v == 16) ? 0x000000 : tui_dos_color_rgb((uint8_t)v);
    } else {
        rgb = (v > 0xFFFFFF) ? 0xFFFFFF : (uint32_t)v;
    }
    return gfx_rgb_to_565(rgb);
}

/** Free every sprite slot (called by `gfx close`; sessions never leak). */
static void gfx_sprites_free_all(void)
{
    int i;

    for (i = 0; i < GFX_SPR_SLOTS; i++) {
        gfx_surface_free(&s_gfx_spr[i]);
    }
}

static bool gfx_require_open(void)
{
    if (s_gfx.px == NULL || s_gfx_canvas == NULL) {
        shell_transcript_appendf_ansi(SH_ERR "gfx: no canvas (gfx init <w> <h> first)\n" SH_RST);
        batch_set_errorlevel(1);
        return false;
    }
    return true;
}

static bool gfx_require_foreground(void)
{
    if (batch_bg_is_background()) {
        shell_transcript_appendf_ansi(SH_ERR "gfx: not available in background jobs (shared display)\n" SH_RST);
        batch_set_errorlevel(1);
        return false;
    }
    return true;
}

/** Shared canvas accessors for the `plot` coordinate-layer verbs
 * (plot_commands.c). Thin wrappers over this file's static canvas state;
 * the `gfx` verbs keep owning allocation, display glue, and sprites. */
uint16_t gfx_canvas_parse_color(const char *s, uint16_t fallback)
{
    return gfx_color_arg(s, fallback);
}

bool gfx_canvas_is_open(void)
{
    return s_gfx.px != NULL && s_gfx_canvas != NULL;
}

gfx_surface_t *gfx_canvas_surface(void)
{
    return gfx_canvas_is_open() ? &s_gfx : NULL;
}

/** Tear down the canvas and sprites and unhide the transcript. Idempotent. */
/** Drop the presented-frame shadow (canvas closed / reinitialised). */
static void gfx_shadow_free(void)
{
    if (s_gfx_shadow != NULL) {
        p4heap_free(s_gfx_shadow);
        s_gfx_shadow = NULL;
    }
    s_gfx_shadow_bytes = 0;
}

/** Wait for an in-flight ISR blit before touching the staged buffer pointer
 *  from task context (alloc/free happens only here, never from the hook). */
static void gfx_present_wait_idle(void)
{
    while (s_present_in_isr) {
        /* A staged dirty rectangle is tiny; the ISR copy finishes promptly. */
    }
}

/** Drop all present state. Called on `gfx close` / reinit. */
static void gfx_present_detach(void)
{
    portENTER_CRITICAL(&s_gfx_present_mux);
    s_present_pending = false;
    portEXIT_CRITICAL(&s_gfx_present_mux);
    gfx_present_wait_idle();
    s_present_fb = NULL;
    s_present_fb_stride = 0;
    s_present_fb_w = 0;
    s_present_fb_h = 0;
    if (s_present_stage != NULL) {
        heap_caps_free(s_present_stage);
        s_present_stage = NULL;
    }
    s_present_stage_cap = 0;
}

/** Locate the scanned framebuffer. Called under the LVGL lock on `gfx init`;
 *  a failure simply leaves partial presents on LVGL's normal flush path. */
static bool gfx_present_attach(void)
{
    lv_display_t *disp = lv_display_get_default();
    gfx_port_disp_prefix_t *ctx;
    void *fb = NULL;
    uint32_t w;
    uint32_t h;

    if (disp == NULL) {
        return false;
    }
    ctx = (gfx_port_disp_prefix_t *)lv_display_get_driver_data(disp);
    if (ctx == NULL || ctx->panel_handle == NULL) {
        return false;
    }
    if (esp_lcd_dpi_panel_get_frame_buffer(ctx->panel_handle, 1, &fb) != ESP_OK ||
        fb == NULL) {
        return false;
    }
    w = lv_display_get_horizontal_resolution(disp);
    h = lv_display_get_vertical_resolution(disp);
    if (w == 0 || h == 0) {
        return false;
    }
    s_present_fb = (uint8_t *)fb;
    s_present_fb_stride = w * 2u;  /* RGB565 on both reference boards */
    s_present_fb_w = (int)w;
    s_present_fb_h = (int)h;
    return true;
}

/** Stage one dirty canvas rectangle, expanded to screen pixels by the integer
 *  scale. @return false when staging is not possible (then the caller uses the
 *  existing LVGL invalidation path). Task context only. */
static bool gfx_present_stage(int32_t scale, const lv_area_t *obj,
                              int x0, int y0, int x1, int y1)
{
    int n = (int)(scale / 256u);
    int ux0 = obj->x1 + x0 * n;   /* unclipped scaled rect on screen */
    int uy0 = obj->y1 + y0 * n;
    int ux1 = obj->x1 + (x1 + 1) * n;
    int uy1 = obj->y1 + (y1 + 1) * n;
    int sx = ux0;
    int sy = uy0;
    int ex = ux1;
    int ey = uy1;
    uint32_t cols;
    uint32_t rows;
    uint32_t row_bytes;
    uint32_t need;
    uint8_t *stage;

    if (n < 1 || s_present_fb == NULL) {
        return false;
    }
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (ex > s_present_fb_w) ex = s_present_fb_w;
    if (ey > s_present_fb_h) ey = s_present_fb_h;
    if (sx >= ex || sy >= ey) {
        return false;
    }
    cols = (uint32_t)(ex - sx);
    rows = (uint32_t)(ey - sy);
    row_bytes = cols * 2u;
    need = row_bytes * rows;
    if (need == 0 || need > GFX_PRESENT_STAGE_MAX_BYTES) {
        return false;
    }

    gfx_present_wait_idle();
    if (s_present_stage_cap < need) {
        stage = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (stage == NULL) {
            return false;
        }
        if (s_present_stage != NULL) {
            heap_caps_free(s_present_stage);
        }
        s_present_stage = stage;
        s_present_stage_cap = need;
    }

    for (uint32_t r = 0; r < rows; r++) {
        int screen_y = sy + (int)r;
        int canvas_y = y0 + (screen_y - uy0) / n;
        const uint16_t *src = s_gfx.px + (size_t)canvas_y * (size_t)s_gfx.w;
        uint8_t *dst = s_present_stage + (size_t)r * row_bytes;

        if (canvas_y < 0 || canvas_y >= s_gfx.h) {
            return false;
        }
        for (uint32_t c = 0; c < cols; c++) {
            int screen_x = sx + (int)c;
            int canvas_x = x0 + (screen_x - ux0) / n;
            uint16_t px;

            if (canvas_x < 0 || canvas_x >= s_gfx.w) {
                return false;
            }
            px = src[canvas_x];
            dst[c * 2u] = (uint8_t)(px & 0xFFu);
            dst[c * 2u + 1u] = (uint8_t)(px >> 8);
        }
    }

    portENTER_CRITICAL(&s_gfx_present_mux);
    s_present_sx = (uint32_t)sx;
    s_present_sy = (uint32_t)sy;
    s_present_cols = cols;
    s_present_rows = rows;
    s_present_row_bytes = row_bytes;
    s_present_pending = true;
    portEXIT_CRITICAL(&s_gfx_present_mux);
    return true;
}

/** Strong override of the weak hook declared by the vendored LVGL port.
 *  Runs from the MIPI-DSI bridge ISR at VSYNC (vertical blanking). */
IRAM_ATTR void p4_gfx_panel_vsync_hook(void)
{
    uint8_t *fb;
    uint8_t *stage;
    uint32_t sx;
    uint32_t sy;
    uint32_t rows;
    uint32_t row_bytes;
    uint32_t stride;

    portENTER_CRITICAL_ISR(&s_gfx_present_mux);
    if (!s_present_pending) {
        portEXIT_CRITICAL_ISR(&s_gfx_present_mux);
        return;
    }
    s_present_pending = false;
    s_present_in_isr = true;
    fb = s_present_fb;
    stage = s_present_stage;
    sx = s_present_sx;
    sy = s_present_sy;
    rows = s_present_rows;
    row_bytes = s_present_row_bytes;
    stride = s_present_fb_stride;
    portEXIT_CRITICAL_ISR(&s_gfx_present_mux);

    if (fb != NULL && stage != NULL) {
        uint8_t *dst = fb + (size_t)sy * stride + (size_t)sx * 2u;

        for (uint32_t r = 0; r < rows; r++) {
            memcpy(dst, stage + (size_t)r * row_bytes, row_bytes);
            dst += stride;
        }
        /* The framebuffer lives in PSRAM: write back the dirty rows so the
         * DPI DMA fetches the pixels we just staged. esp_cache_msync is ISR
         * aware; CONFIG_LCD_DSI_ISR_CACHE_SAFE is off on both boards. */
        esp_cache_msync(fb + (size_t)sy * stride, (size_t)rows * stride,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    portENTER_CRITICAL_ISR(&s_gfx_present_mux);
    s_present_in_isr = false;
    portEXIT_CRITICAL_ISR(&s_gfx_present_mux);
}

static void gfx_teardown(void)
{
    /* Stop the ISR hook from touching canvas/staging memory before teardown. */
    gfx_present_detach();
    if (lvgl_port_lock(0)) {
        windows_set_surface_layout_cb(NULL);
        if (s_gfx_canvas != NULL) {
            lv_obj_del(s_gfx_canvas);
            s_gfx_canvas = NULL;
        }
        /* Restore the fullscreen state the app inherited, so a global
         * `draw fullscreen on` survives a gfx session (F25-C). */
        windows_set_fullscreen(s_gfx_fullscreen_saved);
        windows_set_transcript_apply_defer(false);
        windows_exit_app_surface();
        lvgl_port_unlock();
    } else if (s_gfx_canvas != NULL) {
        /* Lock failed: drop the buffer now; the canvas object leaks one
         * LVGL node rather than risking a use-after-free. Loud, rare. */
        s_gfx_canvas = NULL;
        shell_transcript_appendf_ansi(SH_WARN "gfx: LVGL lock failed, display object leaked\n" SH_RST);
    }
    gfx_surface_free(&s_gfx);
    gfx_shadow_free();
    gfx_sprites_free_all();
}

/**
 * Fit the canvas to the app viewport. Uniform "contain" scale (aspect
 * preserved, antialias off for crisp pixels), centered in the transcript
 * region. Registered as the window manager's app-surface layout callback so it
 * re-runs on keyboard/fullscreen changes. Must run under the LVGL port lock.
 */
static void gfx_relayout(void)
{
    window_rect_t vp;
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t scale;

    if (s_gfx_canvas == NULL || s_gfx.w <= 0 || s_gfx.h <= 0) {
        return;
    }
    vp = windows_get_app_viewport();
    if (vp.width <= 0 || vp.height <= 0) {
        return;
    }
    /* Scale is in 1/256 units; >256 upscales. Snap an upscale to a whole
     * multiple and anchor the pivot at the top-left, so one canvas pixel is
     * exactly `n` screen pixels at a known offset: the present's dirty box
     * then maps with integer maths (no transform-space guessing, no
     * fractional resampling). A downscale (canvas larger than the viewport)
     * keeps the fitted scale and the present falls back to a full
     * invalidation. */
    scale_x = (uint32_t)((int64_t)vp.width * 256 / s_gfx.w);
    scale_y = (uint32_t)((int64_t)vp.height * 256 / s_gfx.h);
    scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale >= 256) {
        scale = (scale / 256) * 256;
    } else if (scale < 1) {
        scale = 1;
    }
    lv_image_set_pivot(s_gfx_canvas, 0, 0);
    lv_image_set_antialias(s_gfx_canvas, false);
    lv_image_set_scale(s_gfx_canvas, scale);
    /* Centre the SCALED box in the viewport (integer pixels). */
    lv_obj_set_pos(s_gfx_canvas,
                   (vp.width - (lv_coord_t)((int64_t)s_gfx.w * scale / 256)) / 2,
                   (vp.height - (lv_coord_t)((int64_t)s_gfx.h * scale / 256)) / 2);
    lv_obj_invalidate(s_gfx_canvas);
}

void gfx_force_close(void)
{
    if (s_gfx.px == NULL && s_gfx_canvas == NULL) {
        return;
    }
    gfx_teardown();
}

bool shell_command_gfx(int argc, char **argv)
{
    if (argc < 2) {
        shell_transcript_appendf_ansi(SH_ERR "gfx: missing subcommand\n" SH_RST);
            shell_transcript_appendf_ansi("Usage: gfx init|close|status|stats|clear|pixel|line|rect|circle|hline|vline|triangle|ellipse|polygon|fill|text|show|image|load|blit|blitmany|free|slots|save <args>\n");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "init")) {
        int w;
        int h;
        lv_obj_t *parent;

        if (!gfx_require_foreground()) return false;
        if (s_gfx.px != NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx: canvas already open (gfx close first)\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        if (tui_is_active()) {
            shell_transcript_appendf_ansi(SH_ERR "gfx: TUI mode is active (draw close first)\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        if (argc != 4) {
            shell_transcript_appendf_ansi(SH_ERR "gfx init: usage: gfx init <w 1..320> <h 1..240>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        w = atoi(argv[2]);
        h = atoi(argv[3]);
        if (!gfx_surface_alloc(&s_gfx, w, h)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx init: bad size or out of memory (max %dx%d)\n" SH_RST,
                                          GFX_MAX_W, GFX_MAX_H);
            batch_set_errorlevel(1);
            return false;
        }
        gfx_surface_clear(&s_gfx, 0x0000);
        gfx_frame_stats_reset(&s_gfx_stats);
        gfx_frame_stats_set_target_fps(&s_gfx_stats, GFX_DEFAULT_TARGET_FPS);
        if (!lvgl_port_lock(0)) {
            gfx_surface_free(&s_gfx);
            shell_transcript_appendf_ansi(SH_ERR "gfx init: could not acquire LVGL lock\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        parent = windows_enter_app_surface();
        if (parent == NULL) {
            lvgl_port_unlock();
            gfx_surface_free(&s_gfx);
            shell_transcript_appendf_ansi(SH_ERR "gfx init: no transcript surface\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        gfx_shadow_free();
        s_gfx_shadow_bytes = (size_t)s_gfx.w * (size_t)s_gfx.h * sizeof(uint16_t);
        s_gfx_shadow = p4heap_alloc_psram(s_gfx_shadow_bytes);
        /* Best-effort: find the scanned DSI framebuffer for tear-free presents.
         * A non-DSI display (or a driver without the getter) keeps the old path. */
        gfx_present_detach();
        gfx_present_attach();
        s_gfx_canvas = lv_canvas_create(parent);
        lv_canvas_set_buffer(s_gfx_canvas, s_gfx.px, s_gfx.w, s_gfx.h,
                             LV_COLOR_FORMAT_RGB565);
        /* Give the canvas the whole panel: a gfx app is a full-screen surface,
         * so hide the header/input row for the session and restore the
         * inherited fullscreen state on close (F25-C). */
        s_gfx_fullscreen_saved = windows_is_fullscreen();
        windows_set_fullscreen(true);
        /* Fit the canvas to the live viewport; the callback re-runs on
         * keyboard/fullscreen changes. */
        windows_set_surface_layout_cb(gfx_relayout);
        gfx_relayout();
        /* A canvas animation may echo per frame; its transcript is hidden, so
         * defer the rebuild until the canvas closes (BOUNCE dropped from
         * 33 ms to 416 ms per frame without this). */
        windows_set_transcript_apply_defer(true);
        lvgl_port_unlock();
        shell_transcript_appendf("gfx: canvas %dx%d (%u KB PSRAM)\n", w, h,
                                 (unsigned)(((uint32_t)w * (uint32_t)h * 2u) / 1024u));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "close")) {
        if (s_gfx.px == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx: no canvas open\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        gfx_teardown();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "status")) {
        if (s_gfx.px == NULL) {
            shell_transcript_append_text("gfx: no canvas open\n");
        } else {
            int live = 0;

            for (int i = 0; i < GFX_SPR_SLOTS; i++) {
                if (s_gfx_spr[i].px != NULL) live++;
            }
            shell_transcript_appendf("gfx: canvas %dx%d (%u KB PSRAM), %d/%d sprite slots live\n",
                                     s_gfx.w, s_gfx.h,
                                     (unsigned)(((uint32_t)s_gfx.w * (uint32_t)s_gfx.h * 2u) / 1024u),
                                     live, GFX_SPR_SLOTS);
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "show")) {
        if (!gfx_require_open()) return false;
        if (!lvgl_port_lock(0)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx show: could not acquire LVGL lock\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        /* Present only what changed since the last show: the ball's old and
         * new pixels, not the whole scaled canvas. A full-canvas invalidation
         * here forced LVGL to re-render the ~690k-pixel scaled image every
         * frame (BOUNCE measured 64 ms/frame, 15 fps, and the long flush
         * tore). The diff is content-based, so a `clear` + redraw does not
         * dirty anything that did not actually change. */
        bool staged = false;

        if (s_gfx_shadow != NULL && s_gfx_shadow_bytes ==
                (size_t)s_gfx.w * (size_t)s_gfx.h * sizeof(uint16_t)) {
            int x0 = s_gfx.w;
            int y0 = s_gfx.h;
            int x1 = -1;
            int y1 = -1;

            for (int y = 0; y < s_gfx.h; y++) {
                const uint16_t *row = s_gfx.px + (size_t)y * (size_t)s_gfx.w;
                const uint16_t *old = s_gfx_shadow + (size_t)y * (size_t)s_gfx.w;
                int first = -1;
                int last = -1;

                for (int x = 0; x < s_gfx.w; x++) {
                    if (row[x] != old[x]) {
                        if (first < 0) {
                            first = x;
                        }
                        last = x;
                    }
                }
                if (first >= 0) {
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                    if (first < x0) x0 = first;
                    if (last > x1) x1 = last;
                }
            }
            memcpy(s_gfx_shadow, s_gfx.px, s_gfx_shadow_bytes);

            if (x1 >= x0 && y1 >= y0) {
                lv_area_t obj;
                int32_t sc = (int32_t)lv_image_get_scale(s_gfx_canvas);

                lv_obj_get_coords(s_gfx_canvas, &obj);
                /* Whole-multiple upscale: canvas (x,y) is exactly
                 * obj + n*(x,y). Anything else (a downscale, an orphaned
                 * object) takes the always-correct full invalidation. */
                if (sc >= 256 && (sc % 256) == 0) {
                    /* Tear-free path: stage the changed rows and let the DPI
                     * vsync hook copy them into the scanned framebuffer during
                     * blanking. No LVGL invalidation/flush means no mid-scan
                     * write. A too-large dirty rect falls back below. */
                    staged = gfx_present_stage(sc, &obj, x0, y0, x1, y1);
                }
                if (!staged) {
                    if (sc >= 256 && (sc % 256) == 0) {
                        int32_t n = sc / 256;
                        lv_area_t inv;

                        inv.x1 = obj.x1 + (lv_coord_t)(x0 * n) - 1;
                        inv.y1 = obj.y1 + (lv_coord_t)(y0 * n) - 1;
                        inv.x2 = obj.x1 + (lv_coord_t)((x1 + 1) * n) + 1;
                        inv.y2 = obj.y1 + (lv_coord_t)((y1 + 1) * n) + 1;
                        lv_obj_invalidate_area(s_gfx_canvas, &inv);
                    } else {
                        lv_obj_invalidate(s_gfx_canvas);
                    }
                }
            }
        } else {
            lv_obj_invalidate(s_gfx_canvas);
        }
        /* Wake the LVGL port task so the frame presents now. An invalidation
         * alone does not wake it: the port sleeps up to task_max_sleep_ms
         * (500 ms) between timers, so a 20 fps animation loop would only be
         * redrawn every few frames and the motion looks jerky. A staged
         * present needs no LVGL redraw - the vsync hook owns this frame. */
        if (!staged) {
            lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
        }
        lvgl_port_unlock();
        /* Present point for frame pacing (`gfx stats`). */
        gfx_frame_stats_sample(&s_gfx_stats, (uint64_t)esp_timer_get_time());
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "stats")) {
        /* gfx stats [reset|target <fps|off>] - frame pacing for the loop. */
        if (argc >= 3 && shell_text_equals_ignore_case(argv[2], "reset")) {
            gfx_frame_stats_reset(&s_gfx_stats);
            gfx_frame_stats_set_target_fps(&s_gfx_stats, GFX_DEFAULT_TARGET_FPS);
            shell_print_ok("gfx stats: reset");
            batch_set_errorlevel(0);
            return true;
        }
        if (argc >= 4 && shell_text_equals_ignore_case(argv[2], "target")) {
            uint32_t fps = (shell_text_equals_ignore_case(argv[3], "off") ||
                            shell_text_equals_ignore_case(argv[3], "0"))
                           ? 0u : (uint32_t)atoi(argv[3]);
            gfx_frame_stats_set_target_fps(&s_gfx_stats, fps);
            shell_transcript_appendf("gfx stats: target %u fps\n", (unsigned)fps);
            batch_set_errorlevel(0);
            return true;
        }
        {
            char fields[GFX_STATS_FIELDS];
            gfx_frame_stats_format(&s_gfx_stats, fields, sizeof(fields));
            shell_transcript_appendf("gfx stats: %s\n", fields);
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (!gfx_require_foreground()) return false;
    if (!gfx_require_open()) return false;

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        /* gfx clear [color] */
        uint16_t color = (argc > 2) ? gfx_color_arg(argv[2], 0x0000) : 0x0000;

        gfx_surface_clear(&s_gfx, color);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "pixel")) {
        /* gfx pixel <x> <y> <color> */
        if (argc != 5) {
            shell_transcript_appendf_ansi(SH_ERR "gfx pixel: usage: gfx pixel <x> <y> <color>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx_surface_pixel(&s_gfx, atoi(argv[2]), atoi(argv[3]),
                          gfx_color_arg(argv[4], 0xFFFF));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "line")) {
        /* gfx line <x1> <y1> <x2> <y2> <color> */
        if (argc != 7) {
            shell_transcript_appendf_ansi(SH_ERR "gfx line: usage: gfx line <x1> <y1> <x2> <y2> <color>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx_surface_line(&s_gfx, atoi(argv[2]), atoi(argv[3]),
                         atoi(argv[4]), atoi(argv[5]),
                         gfx_color_arg(argv[6], 0xFFFF));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "rect")) {
        /* gfx rect <x> <y> <w> <h> <color> [fill] */
        bool fill;

        if (argc != 7 && argc != 8) {
            shell_transcript_appendf_ansi(SH_ERR "gfx rect: usage: gfx rect <x> <y> <w> <h> <color> [fill]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        fill = (argc > 7) && shell_text_equals_ignore_case(argv[7], "fill");
        gfx_surface_rect(&s_gfx, atoi(argv[2]), atoi(argv[3]),
                         atoi(argv[4]), atoi(argv[5]),
                         gfx_color_arg(argv[6], 0xFFFF), fill);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "circle")) {
        /* gfx circle <x> <y> <r> <color> [fill] */
        bool fill;

        if (argc != 6 && argc != 7) {
            shell_transcript_appendf_ansi(SH_ERR "gfx circle: usage: gfx circle <x> <y> <r> <color> [fill]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        fill = (argc > 6) && shell_text_equals_ignore_case(argv[6], "fill");
        gfx_surface_circle(&s_gfx, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                           gfx_color_arg(argv[5], 0xFFFF), fill);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "hline")) {
        /* gfx hline <x> <y> <w> <color> */
        if (argc != 6) {
            shell_transcript_appendf_ansi(SH_ERR "gfx hline: usage: gfx hline <x> <y> <w> <color>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx_surface_hline(&s_gfx, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                          gfx_color_arg(argv[5], 0xFFFF));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "vline")) {
        /* gfx vline <x> <y> <h> <color> */
        if (argc != 6) {
            shell_transcript_appendf_ansi(SH_ERR "gfx vline: usage: gfx vline <x> <y> <h> <color>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx_surface_vline(&s_gfx, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                          gfx_color_arg(argv[5], 0xFFFF));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "triangle")) {
        /* gfx triangle <x1> <y1> <x2> <y2> <x3> <y3> <color> [fill] */
        bool fill;

        if (argc != 9 && argc != 10) {
            shell_transcript_appendf_ansi(SH_ERR "gfx triangle: usage: gfx triangle <x1> <y1> <x2> <y2> <x3> <y3> <color> [fill]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        fill = (argc > 9) && shell_text_equals_ignore_case(argv[9], "fill");
        gfx_surface_triangle(&s_gfx, atoi(argv[2]), atoi(argv[3]),
                             atoi(argv[4]), atoi(argv[5]),
                             atoi(argv[6]), atoi(argv[7]),
                             gfx_color_arg(argv[8], 0xFFFF), fill);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "ellipse")) {
        /* gfx ellipse <cx> <cy> <rx> <ry> <color> [fill] */
        bool fill;

        if (argc != 7 && argc != 8) {
            shell_transcript_appendf_ansi(SH_ERR "gfx ellipse: usage: gfx ellipse <cx> <cy> <rx> <ry> <color> [fill]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        fill = (argc > 7) && shell_text_equals_ignore_case(argv[7], "fill");
        gfx_surface_ellipse(&s_gfx, atoi(argv[2]), atoi(argv[3]),
                            atoi(argv[4]), atoi(argv[5]),
                            gfx_color_arg(argv[6], 0xFFFF), fill);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "polygon")) {
        /* gfx polygon <color> <fill|line> <x1> <y1> <x2> <y2> [<x3> <y3> ...] */
        int pts[GFX_POLY_MAX_PTS * 2];
        int npts;
        int k;
        bool fill;

        if (argc < 10 || ((argc - 4) % 2) != 0) {
            shell_transcript_appendf_ansi(SH_ERR "gfx polygon: usage: gfx polygon <color> <fill|line> <x1> <y1> <x2> <y2> <x3> <y3> [more x y ...]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        npts = (argc - 4) / 2;
        if (npts > GFX_POLY_MAX_PTS) {
            shell_transcript_appendf_ansi(SH_ERR "gfx polygon: too many vertices (%d, max %d)\n" SH_RST,
                                          npts, GFX_POLY_MAX_PTS);
            batch_set_errorlevel(2);
            return false;
        }
        for (k = 0; k < npts; k++) {
            pts[k * 2] = atoi(argv[4 + k * 2]);
            pts[k * 2 + 1] = atoi(argv[5 + k * 2]);
        }
        fill = shell_text_equals_ignore_case(argv[3], "fill");
        gfx_surface_polygon(&s_gfx, pts, npts, gfx_color_arg(argv[2], 0xFFFF), fill);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "fill")) {
        /* gfx fill <x> <y> <color>: 4-way flood fill of the seed's color. */
        int changed;

        if (argc != 5) {
            shell_transcript_appendf_ansi(SH_ERR "gfx fill: usage: gfx fill <x> <y> <color>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        changed = gfx_surface_flood_fill(&s_gfx, atoi(argv[2]), atoi(argv[3]),
                                         gfx_color_arg(argv[4], 0xFFFF));
        shell_transcript_appendf("gfx: filled %d pixel(s)\n", changed);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "text")) {
        /* gfx text [/bg:<color>] [/scale:<n>] <x> <y> <color> <text...> */
        int i = 2;
        int scale = 1;
        bool use_bg = false;
        uint16_t bg = 0;
        char joined[256];
        size_t used = 0;

        while (i < argc && argv[i][0] == '/') {
            if (strncasecmp(argv[i], "/scale:", 7) == 0) {
                scale = atoi(argv[i] + 7);
                if (scale < 1) scale = 1;
                if (scale > 16) scale = 16;
            } else if (strncasecmp(argv[i], "/bg:", 4) == 0) {
                bg = gfx_color_arg(argv[i] + 4, 0x0000);
                use_bg = true;
            } else {
                shell_transcript_appendf_ansi(SH_ERR "gfx text: unknown option %s\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return false;
            }
            i++;
        }
        if (argc - i < 4) {
            shell_transcript_appendf_ansi(SH_ERR "gfx text: usage: gfx text [/bg:<color>] [/scale:<n>] <x> <y> <color> <text...>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        joined[0] = '\0';
        {
            int t;

            for (t = i + 3; t < argc; t++) {
                size_t len = strlen(argv[t]);

                if (used != 0 && used < sizeof(joined) - 1) {
                    joined[used++] = ' ';
                }
                if (used + len >= sizeof(joined)) {
                    len = sizeof(joined) - 1 - used;
                }
                memcpy(joined + used, argv[t], len);
                used += len;
                joined[used] = '\0';
            }
        }
        gfx_surface_text(&s_gfx, atoi(argv[i]), atoi(argv[i + 1]),
                         joined, gfx_color_arg(argv[i + 2], 0xFFFF),
                         bg, use_bg, scale);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "load")) {
        /* gfx load <slot 0..N> <path>: ingest a 24-bit BI_RGB BMP (the exact
         * format `screenshot <file>` writes) into a sprite slot. File is
         * staged through one bounded PSRAM buffer (GFX_BMP_MAX_BYTES); the
         * pure header parser + decoder in components/gfx keep this verb a
         * thin file shell around unit-tested raster code. */
        uint8_t *buf = NULL;
        size_t got = 0;
        gfx_bmp_info_t info;
        gfx_surface_t decoded = {NULL, 0, 0};
        int slot;
        char *end = NULL;
        int rc;

        if (!gfx_require_foreground()) return false;
        if (argc != 4) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: usage: gfx load <slot 0..%d> <path>\n" SH_RST,
                                          GFX_SPR_SLOTS - 1);
            batch_set_errorlevel(2);
            return false;
        }
        slot = (int)strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || slot < 0 || slot >= GFX_SPR_SLOTS) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: bad slot '%s' (0..%d)\n" SH_RST,
                                          argv[2], GFX_SPR_SLOTS - 1);
            batch_set_errorlevel(2);
            return false;
        }
        rc = command_load_file_psram(argv[3], "gfx load", GFX_BMP_MAX_BYTES, &buf, &got);
        if (rc != 0) {
            batch_set_errorlevel(rc);
            return false;
        }
        if (!gfx_bmp_parse_header_ex(buf, got, &info, GFX_SPR_MAX, GFX_SPR_MAX)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: not a 24/32-bit BI_RGB BMP <= %dx%d (%s)\n" SH_RST,
                                          GFX_SPR_MAX, GFX_SPR_MAX, argv[3]);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        if (!gfx_bmp_decode_565(buf, got, &info, &decoded)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: decode failed (%s)\n" SH_RST, argv[3]);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        heap_caps_free(buf);
        gfx_surface_free(&s_gfx_spr[slot]);
        s_gfx_spr[slot] = decoded;
        shell_transcript_appendf("gfx: slot %d loaded %dx%d (%u KB PSRAM)\n", slot,
                                 info.w, info.h,
                                 (unsigned)(((uint32_t)info.w * (uint32_t)info.h * 2u) / 1024u));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "blit")) {
        /* gfx blit <slot> <x> <y> [transparent]: stamp a sprite onto the
         * canvas (clipped; needs `gfx show` like every other raster op). */
        int slot;
        char *end = NULL;
        bool use_t = false;
        uint16_t tcolor = 0x0000;

        if (!gfx_require_foreground()) return false;
        if (!gfx_require_open()) return false;
        if (argc != 5 && argc != 6) {
            shell_transcript_appendf_ansi(SH_ERR "gfx blit: usage: gfx blit <slot 0..%d> <x> <y> [transparent]\n" SH_RST,
                                          GFX_SPR_SLOTS - 1);
            batch_set_errorlevel(2);
            return false;
        }
        slot = (int)strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || slot < 0 || slot >= GFX_SPR_SLOTS) {
            shell_transcript_appendf_ansi(SH_ERR "gfx blit: bad slot '%s' (0..%d)\n" SH_RST,
                                          argv[2], GFX_SPR_SLOTS - 1);
            batch_set_errorlevel(2);
            return false;
        }
        if (s_gfx_spr[slot].px == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx blit: slot %d empty (gfx load first)\n" SH_RST, slot);
            batch_set_errorlevel(1);
            return false;
        }
        if (argc > 5) {
            use_t = true;
            tcolor = gfx_color_arg(argv[5], 0x0000);
        }
        gfx_surface_blit(&s_gfx, &s_gfx_spr[slot],
                         atoi(argv[3]), atoi(argv[4]), use_t, tcolor);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "blitmany")) {
        /* gfx blitmany <slot> [/s:N] [/r:deg] <x1> <y1> [<x2> <y2> ...]
         * [transparent]: stamp one sprite at many positions in a single
         * command, so a batch game draws a whole formation without one
         * transcript line per stamp. A trailing odd token is the transparent
         * color (same as `blit`). `/s:1..4` upscales every stamp
         * (nearest-neighbour); `/r:0|90|180|270` rotates clockwise first. */
        int slot;
        char *end = NULL;
        bool use_t = false;
        uint16_t tcolor = 0x0000;
        const char *color_arg = NULL;
        int scale = 1;
        int turns = 0;
        int coords = 0;
        int pairs = 0;
        int index;
        gfx_surface_t rotated = {NULL, 0, 0};
        const gfx_surface_t *spr = NULL;

        if (!gfx_require_foreground()) return false;
        if (!gfx_require_open()) return false;
        if (argc < 5) {
            shell_transcript_appendf_ansi(SH_ERR "gfx blitmany: usage: gfx blitmany <slot 0..%d> [/s:1..4] [/r:0|90|180|270] <x1> <y1> [<x2> <y2> ...] [transparent]\n" SH_RST,
                                          GFX_SPR_SLOTS - 1);
            batch_set_errorlevel(2);
            return false;
        }
        slot = (int)strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || slot < 0 || slot >= GFX_SPR_SLOTS) {
            shell_transcript_appendf_ansi(SH_ERR "gfx blitmany: bad slot '%s' (0..%d)\n" SH_RST,
                                          argv[2], GFX_SPR_SLOTS - 1);
            batch_set_errorlevel(2);
            return false;
        }
        if (s_gfx_spr[slot].px == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx blitmany: slot %d empty (gfx load first)\n" SH_RST, slot);
            batch_set_errorlevel(1);
            return false;
        }
        /* Two passes over argv[3..] keep stack use flat: first validate the
         * flags and count the coordinates, then stamp. */
        for (index = 3; index < argc; index++) {
            if (strncasecmp(argv[index], "/s:", 3) == 0) {
                scale = atoi(argv[index] + 3);
                if (scale < 1 || scale > 4) {
                    shell_transcript_appendf_ansi(SH_ERR "gfx blitmany: bad scale '%s' (1..4)\n" SH_RST,
                                                  argv[index]);
                    batch_set_errorlevel(2);
                    return false;
                }
            } else if (strncasecmp(argv[index], "/r:", 3) == 0) {
                int deg = atoi(argv[index] + 3);
                if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
                    shell_transcript_appendf_ansi(SH_ERR "gfx blitmany: bad rotation '%s' (0|90|180|270)\n" SH_RST,
                                                  argv[index]);
                    batch_set_errorlevel(2);
                    return false;
                }
                turns = deg / 90;
            } else {
                coords++;
            }
        }
        if (coords > P4_CONFIG_GFX_BLITMANY_MAX * 2 + 1) {
            shell_transcript_appendf_ansi(SH_ERR "gfx blitmany: needs 1..%d X Y pairs\n" SH_RST,
                                          P4_CONFIG_GFX_BLITMANY_MAX);
            batch_set_errorlevel(2);
            return false;
        }
        /* A trailing odd token is the transparent color (same as `blit`);
         * flags were consumed above, so the last non-flag token dangles. */
        if ((coords % 2) != 0) {
            for (index = argc - 1; index >= 3; index--) {
                if (strncasecmp(argv[index], "/s:", 3) != 0 &&
                    strncasecmp(argv[index], "/r:", 3) != 0) {
                    color_arg = argv[index];
                    break;
                }
            }
            use_t = true;
            tcolor = gfx_color_arg(color_arg != NULL ? color_arg : "", 0x0000);
            coords--;
        }
        pairs = coords / 2;
        if (pairs < 1 || pairs > P4_CONFIG_GFX_BLITMANY_MAX) {
            shell_transcript_appendf_ansi(SH_ERR "gfx blitmany: needs 1..%d X Y pairs\n" SH_RST,
                                          P4_CONFIG_GFX_BLITMANY_MAX);
            batch_set_errorlevel(2);
            return false;
        }
        spr = &s_gfx_spr[slot];
        if (turns != 0) {
            /* One rotated copy serves every stamp; freed on every path below. */
            if (!gfx_surface_rotate_cw(spr, turns, &rotated)) {
                shell_transcript_append_text("gfx blitmany: out of memory\n");
                batch_set_errorlevel(1);
                return false;
            }
            spr = &rotated;
        }
        if (spr->w * scale > GFX_MAX_W || spr->h * scale > GFX_MAX_H) {
            shell_transcript_appendf_ansi(SH_ERR "gfx blitmany: scaled %dx%d exceeds the %dx%d canvas\n" SH_RST,
                                          spr->w * scale, spr->h * scale,
                                          GFX_MAX_W, GFX_MAX_H);
            gfx_surface_free(&rotated);
            batch_set_errorlevel(2);
            return false;
        }
        /* Second pass over a heap index (this sits on the recursive batch
         * path, so no line-sized stack array): the pair order is the argv
         * order with flags and the color removed. */
        {
            int *pair_idx = malloc(sizeof(int) * (size_t)coords);
            int held = 0;

            if (pair_idx == NULL) {
                shell_transcript_append_text("gfx blitmany: out of memory\n");
                gfx_surface_free(&rotated);
                batch_set_errorlevel(1);
                return false;
            }
            for (index = 3; index < argc; index++) {
                if (strncasecmp(argv[index], "/s:", 3) == 0 ||
                    strncasecmp(argv[index], "/r:", 3) == 0 ||
                    (color_arg != NULL && argv[index] == color_arg)) {
                    continue;
                }
                pair_idx[held++] = index;
            }
            for (index = 0; index + 1 < held; index += 2) {
                int x = atoi(argv[pair_idx[index]]);
                int y = atoi(argv[pair_idx[index + 1]]);

                if (scale == 1) {
                    gfx_surface_blit(&s_gfx, spr, x, y, use_t, tcolor);
                } else {
                    gfx_surface_blit_scaled(&s_gfx, spr, x, y,
                                            spr->w * scale, spr->h * scale,
                                            use_t, tcolor);
                }
            }
            free(pair_idx);
        }
        gfx_surface_free(&rotated);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "image")) {
        /* gfx image <path> [x y [w h]]: decode a BMP/PNG/JPEG straight to the
         * target rect and blit it onto the canvas. Defaults to the native size
         * aspect-fit to the canvas at 0,0. Reuses the single image path
         * (gfx BMP / imgdec PNG+JPEG). */
        uint8_t *buf = NULL;
        size_t got = 0;
        gfx_bmp_info_t info;
        gfx_surface_t img = {NULL, 0, 0};
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int rc;
        int native_w = 0;
        int native_h = 0;

        if (argc != 3 && argc != 5 && argc != 7) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: usage: gfx image <path> [x y [w h]]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (argc >= 5) {
            x = atoi(argv[3]);
            y = atoi(argv[4]);
            if (argc == 7) {
                w = atoi(argv[5]);
                h = atoi(argv[6]);
                if (w < 1 || h < 1) {
                    shell_transcript_appendf_ansi(SH_ERR "gfx image: bad size %dx%d\n" SH_RST, w, h);
                    batch_set_errorlevel(2);
                    return false;
                }
            }
        }
        rc = command_load_file_psram(argv[2], "gfx image",
                                     P4_CONFIG_IMAGE_MAX_BYTES, &buf, &got);
        if (rc != 0) {
            batch_set_errorlevel(rc);
            return false;
        }
        {
            imgdec_format_t fmt = imgdec_probe(buf, got, &native_w, &native_h);

            if (fmt == IMGDEC_FMT_PNG || fmt == IMGDEC_FMT_JPEG) {
                imgdec_image_t imd;

                if (w < 1 || h < 1) {
                    gfx_bmp_fit(native_w > 0 ? native_w : 1,
                                native_h > 0 ? native_h : 1,
                                s_gfx.w, s_gfx.h, &w, &h);
                }
                if (!imgdec_decode_scaled(buf, got, w, h, &imd)) {
                    shell_transcript_appendf_ansi(SH_ERR "gfx image: decode failed (%s)\n" SH_RST, argv[2]);
                    heap_caps_free(buf);
                    batch_set_errorlevel(1);
                    return false;
                }
                heap_caps_free(buf);
                {
                    /* Point a surface at the decoded buffer (no second copy). */
                    gfx_surface_t dec = { imd.pixels, imd.width, imd.height };

                    gfx_surface_blit(&s_gfx, &dec, x, y, false, 0);
                }
                imgdec_free(&imd);
                batch_set_errorlevel(0);
                return true;
            }
        }
        if (!gfx_bmp_parse_header_ex(buf, got, &info, GFX_IMAGE_MAX_W, GFX_IMAGE_MAX_H)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: not a 24/32-bit BMP, or a PNG/JPEG that is malformed or exceeds the %dx%d decode cap (%s)\n" SH_RST,
                                          P4_CONFIG_IMGDEC_MAX_W, P4_CONFIG_IMGDEC_MAX_H,
                                          argv[2]);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        if (w < 1 || h < 1) {
            gfx_bmp_fit(info.w, info.h, s_gfx.w, s_gfx.h, &w, &h);
        }
        if (!gfx_bmp_decode_scaled_565(buf, got, &info, w, h, &img)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: decode failed (%s)\n" SH_RST, argv[2]);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        heap_caps_free(buf);
        gfx_surface_blit(&s_gfx, &img, x, y, false, 0);
        gfx_surface_free(&img);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "free")) {
        /* gfx free <slot> */
        int slot;
        char *end = NULL;

        if (argc != 3) {
            shell_transcript_appendf_ansi(SH_ERR "gfx free: usage: gfx free <slot 0..%d>\n" SH_RST,
                                          GFX_SPR_SLOTS - 1);
            batch_set_errorlevel(2);
            return false;
        }
        slot = (int)strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || slot < 0 || slot >= GFX_SPR_SLOTS) {
            shell_transcript_appendf_ansi(SH_ERR "gfx free: bad slot '%s' (0..%d)\n" SH_RST,
                                          argv[2], GFX_SPR_SLOTS - 1);
            batch_set_errorlevel(2);
            return false;
        }
        gfx_surface_free(&s_gfx_spr[slot]);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "slots")) {
        /* gfx slots: list live sprite slots (batch `for /f`-friendly). */
        int i;

        for (i = 0; i < GFX_SPR_SLOTS; i++) {
            if (s_gfx_spr[i].px != NULL) {
                shell_transcript_appendf("gfx.slot: %d %dx%d\n",
                                         i, s_gfx_spr[i].w, s_gfx_spr[i].h);
            }
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "save")) {
        /* gfx save <path>: write the canvas as a 24-bit BMP (same layout
         * `screenshot <file>` produces: headers via the shared
         * screenshot_write_bmp_headers, rows bottom-up). Same guarded
         * session + free-space precheck + partial-remove discipline as
         * the screenshot SD path. */
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;
        FILE *f = NULL;
        uint8_t headers[54];
        uint8_t *row_buf = NULL;
        uint32_t row_bytes;
        bool write_ok = true;
        int32_t y;

        if (!gfx_require_foreground()) return false;
        if (!gfx_require_open()) return false;
        if (argc != 3) {
            shell_transcript_appendf_ansi(SH_ERR "gfx save: usage: gfx save <path>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "gfx save: invalid path %s\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return false;
        }
        if (shell_sd_begin(&session) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "gfx save: SD card not present\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        {
            uint64_t needed = 54 + (uint64_t)s_gfx.w * (uint64_t)s_gfx.h * 3;
            uint64_t reclaim = storage_get_file_size(resolved);

            if (!storage_check_free_space(needed, reclaim, "gfx save")) {
                shell_sd_end(&session, "gfx save");
                batch_set_errorlevel(1);
                return false;
            }
        }
        f = fopen(resolved, "wb");
        if (f == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx save: cannot create %s\n" SH_RST, resolved);
            shell_sd_end(&session, "gfx save");
            batch_set_errorlevel(1);
            return false;
        }
        screenshot_write_bmp_headers(headers, (uint32_t)s_gfx.w, (uint32_t)s_gfx.h);
        if (fwrite(headers, 1, sizeof(headers), f) != sizeof(headers)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx save: failed to write BMP header\n" SH_RST);
            fclose(f);
            remove(resolved);
            shell_sd_end(&session, "gfx save");
            batch_set_errorlevel(1);
            return false;
        }
        row_bytes = (uint32_t)s_gfx.w * 3u;
        row_buf = p4heap_alloc_psram(row_bytes);
        if (row_buf == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx save: out of memory for row buffer\n" SH_RST);
            fclose(f);
            remove(resolved);
            shell_sd_end(&session, "gfx save");
            batch_set_errorlevel(1);
            return false;
        }
        for (y = s_gfx.h - 1; y >= 0; y--) {
            gfx_565_to_888_row(row_buf,
                               &s_gfx.px[(size_t)y * (size_t)s_gfx.w],
                               s_gfx.w);
            if (fwrite(row_buf, 1, row_bytes, f) != row_bytes) {
                write_ok = false;
                break;
            }
        }
        heap_caps_free(row_buf);
        fclose(f);
        shell_sd_end(&session, "gfx save");
        if (!write_ok) {
            shell_transcript_appendf_ansi(SH_ERR "gfx save: write failed, removing partial file\n" SH_RST);
            remove(resolved);
            batch_set_errorlevel(1);
            return false;
        }
        shell_transcript_appendf("gfx: saved %s (%u bytes)\n", resolved,
                                 (unsigned)(54 + (uint32_t)s_gfx.w * (uint32_t)s_gfx.h * 3u));
        batch_set_errorlevel(0);
        return true;
    }

    shell_transcript_appendf_ansi(SH_ERR "gfx: unknown subcommand %s\n" SH_RST, argv[1]);
    batch_set_errorlevel(2);
    return false;
}

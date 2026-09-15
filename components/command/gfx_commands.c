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
#include "tui.h"
#include "storage.h"
#include "windows.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static gfx_surface_t s_gfx = {NULL, 0, 0};
static lv_obj_t *s_gfx_canvas = NULL;

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
static void gfx_teardown(void)
{
    if (lvgl_port_lock(0)) {
        lv_obj_t *spans = windows_get_transcript_spans();

        if (s_gfx_canvas != NULL) {
            lv_obj_del(s_gfx_canvas);
            s_gfx_canvas = NULL;
        }
        if (spans != NULL) {
            lv_obj_remove_flag(spans, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    } else if (s_gfx_canvas != NULL) {
        /* Lock failed: drop the buffer now; the canvas object leaks one
         * LVGL node rather than risking a use-after-free. Loud, rare. */
        s_gfx_canvas = NULL;
        shell_transcript_appendf_ansi(SH_WARN "gfx: LVGL lock failed, display object leaked\n" SH_RST);
    }
    gfx_surface_free(&s_gfx);
    gfx_sprites_free_all();
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
        shell_transcript_appendf_ansi("Usage: gfx init|close|status|clear|pixel|line|rect|circle|hline|vline|triangle|ellipse|polygon|fill|text|show|image|load|blit|free|slots|save <args>\n");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "init")) {
        int w;
        int h;
        lv_obj_t *parent;
        lv_obj_t *spans;

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
        if (!lvgl_port_lock(0)) {
            gfx_surface_free(&s_gfx);
            shell_transcript_appendf_ansi(SH_ERR "gfx init: could not acquire LVGL lock\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        parent = windows_get_transcript();
        spans = windows_get_transcript_spans();
        if (parent == NULL) {
            lvgl_port_unlock();
            gfx_surface_free(&s_gfx);
            shell_transcript_appendf_ansi(SH_ERR "gfx init: no transcript surface\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        if (spans != NULL) lv_obj_add_flag(spans, LV_OBJ_FLAG_HIDDEN);
        s_gfx_canvas = lv_canvas_create(parent);
        lv_canvas_set_buffer(s_gfx_canvas, s_gfx.px, s_gfx.w, s_gfx.h,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_center(s_gfx_canvas);
        lv_obj_invalidate(s_gfx_canvas);
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
        lv_obj_invalidate(s_gfx_canvas);
        /* Wake the LVGL port task so the frame presents now. An invalidation
         * alone does not wake it: the port sleeps up to task_max_sleep_ms
         * (500 ms) between timers, so a 20 fps animation loop would only be
         * redrawn every few frames and the motion looks jerky. */
        lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
        lvgl_port_unlock();
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
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;
        FILE *file = NULL;
        long size = 0;
        uint8_t *buf = NULL;
        size_t got = 0;
        gfx_bmp_info_t info;
        gfx_surface_t decoded = {NULL, 0, 0};
        int slot;
        char *end = NULL;

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
        if (shell_fs_resolve_path(argv[3], resolved, sizeof(resolved)) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: invalid path %s\n" SH_RST, argv[3]);
            batch_set_errorlevel(2);
            return false;
        }
        if (shell_sd_begin(&session) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: SD card not present\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        file = fopen(resolved, "rb");
        if (file == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: cannot open %s\n" SH_RST, resolved);
            shell_sd_end(&session, "gfx load");
            batch_set_errorlevel(1);
            return false;
        }
        if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
            fseek(file, 0, SEEK_SET) != 0) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: cannot size %s\n" SH_RST, resolved);
            fclose(file);
            shell_sd_end(&session, "gfx load");
            batch_set_errorlevel(1);
            return false;
        }
        if (size < 54 || (uint64_t)size > GFX_BMP_MAX_BYTES) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: bad size %ld (max %u bytes)\n" SH_RST,
                                          size, (unsigned)GFX_BMP_MAX_BYTES);
            fclose(file);
            shell_sd_end(&session, "gfx load");
            batch_set_errorlevel(1);
            return false;
        }
        buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            buf = malloc((size_t)size);
        }
        if (buf == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: out of memory\n" SH_RST);
            fclose(file);
            shell_sd_end(&session, "gfx load");
            batch_set_errorlevel(1);
            return false;
        }
        got = fread(buf, 1, (size_t)size, file);
        fclose(file);
        shell_sd_end(&session, "gfx load");
        if (got != (size_t)size) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: short read %s\n" SH_RST, resolved);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        if (!gfx_bmp_parse_header_ex(buf, got, &info, GFX_SPR_MAX, GFX_SPR_MAX)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: not a 24/32-bit BI_RGB BMP <= %dx%d (%s)\n" SH_RST,
                                          GFX_SPR_MAX, GFX_SPR_MAX, resolved);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        if (!gfx_bmp_decode_565(buf, got, &info, &decoded)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx load: decode failed (%s)\n" SH_RST, resolved);
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

    if (shell_text_equals_ignore_case(argv[1], "image")) {
        /* gfx image <path> [x y [w h]]: decode a BMP straight to the target
         * rect and blit it onto the canvas. Defaults to the native size
         * aspect-fit to the canvas at 0,0. Reuses the single BMP decoder. */
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;
        FILE *file = NULL;
        long size = 0;
        uint8_t *buf = NULL;
        size_t got = 0;
        gfx_bmp_info_t info;
        gfx_surface_t img = {NULL, 0, 0};
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;

        if (argc != 3 && argc != 5 && argc != 7) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: usage: gfx image <path> [x y [w h]]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: invalid path %s\n" SH_RST, argv[2]);
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
        if (shell_sd_begin(&session) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: SD card not present\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        file = fopen(resolved, "rb");
        if (file == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: cannot open %s\n" SH_RST, resolved);
            shell_sd_end(&session, "gfx image");
            batch_set_errorlevel(1);
            return false;
        }
        if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: cannot size %s\n" SH_RST, resolved);
            fclose(file);
            shell_sd_end(&session, "gfx image");
            batch_set_errorlevel(1);
            return false;
        }
        if (size < 54 || (uint64_t)size > P4_CONFIG_IMAGE_MAX_BYTES) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: bad size %ld (max %u bytes)\n" SH_RST,
                                          size, (unsigned)P4_CONFIG_IMAGE_MAX_BYTES);
            fclose(file);
            shell_sd_end(&session, "gfx image");
            batch_set_errorlevel(1);
            return false;
        }
        buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) buf = malloc((size_t)size);
        if (buf == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: out of memory\n" SH_RST);
            fclose(file);
            shell_sd_end(&session, "gfx image");
            batch_set_errorlevel(1);
            return false;
        }
        got = fread(buf, 1, (size_t)size, file);
        fclose(file);
        shell_sd_end(&session, "gfx image");
        if (got != (size_t)size) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: short read %s\n" SH_RST, resolved);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        if (!gfx_bmp_parse_header_ex(buf, got, &info, GFX_IMAGE_MAX_W, GFX_IMAGE_MAX_H)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: not a 24/32-bit BI_RGB BMP (%s)\n" SH_RST, resolved);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        if (w < 1 || h < 1) {
            gfx_bmp_fit(info.w, info.h, s_gfx.w, s_gfx.h, &w, &h);
        }
        if (!gfx_bmp_decode_scaled_565(buf, got, &info, w, h, &img)) {
            shell_transcript_appendf_ansi(SH_ERR "gfx image: decode failed (%s)\n" SH_RST, resolved);
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
        row_buf = heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (row_buf == NULL) {
            row_buf = malloc(row_bytes);
        }
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

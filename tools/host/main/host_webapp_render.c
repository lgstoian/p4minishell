/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file host_webapp_render.c
 * @brief Standalone host webapp render probe (not registered in Unity).
 *
 * Drives the firmware's ONE HTML engine - dom_parse(), css_parse_block() /
 * css_compute_styles() (which calls css_apply()) and dom_render_tree() - over
 * one fixture file and prints a `tui dump /a`-style report on stdout:
 *
 *   webapp.render.begin fixture=<name> cols=80 rows=25
 *   tui.dump cols=80 from=1 to=25
 *   NNN|<normalised text row>
 *   NNN@<start>-<end>:<fg> ...
 *   tui.dump.end
 *   webapp.canvas ...            (when the page has a <canvas>)
 *   webapp.render.end fixture=<name> rows=25 runs=25
 *
 * The cell probe follows the pattern in test/main/test_html.c:525-570: it
 * records what the renderer emits instead of driving a display. No second
 * parser, stylesheet engine or renderer is introduced here.
 *
 * Canvas 2D is exercised only through the pure rasterizer API: the bitmap is
 * drawn with js_canvas_fill_rect() using colours from the ONE CSS colour parser
 * and probed as raw RGBA bytes. No pixel-to-cell sampler is duplicated and no
 * QuickJS/DOM script execution is attempted on the host; the <canvas> element
 * therefore renders its fallback child through the shared DOM renderer.
 *
 * This is a host pre-gate input for tools/host_webapp_gate.py, never hardware
 * sign-off. The caller (CMake, wired separately) supplies the markdown, ansi,
 * strutil and js_canvas headers and sources.
 */

#include "dom.h"
#include "dom_render.h"
#include "css.h"
#include "html.h"
#include "js_canvas.h"
#include "gfx.h"
#include "p4minishell_config.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEBAPP_COLS 80
#define WEBAPP_ROWS 25
#define WEBAPP_HTML_MAX (512u * 1024u)
#define DEFAULT_FG 7

/** One rendered cell as seen through the renderer's put_cell sink. */
typedef struct {
    char text[8];
    uint8_t fg;
    uint8_t bg;
} probe_cell_t;

/** File-scope page grid: 80x25 cells stay off the stack. */
typedef struct {
    probe_cell_t cells[WEBAPP_COLS * WEBAPP_ROWS];
    int emitted;
} page_probe_t;

static page_probe_t s_page;
static css_ruleset_t s_rules;
static char s_canvas_line[384];
static char s_meta_line[192];
static char s_css_line[192];
static char s_events_line[192];
static char s_cascade_line[192];
static char s_entity_line[192];
static char s_fixture_dir[512];
static js_canvas_t *s_chart_canvas = NULL;
static int s_last_native_w = 0;
static int s_last_native_h = 0;

/* Image caps mirror the firmware's browser image budget. The host probe only
 * decodes local BMP fixtures through the ONE gfx BMP decoder (the same decoder
 * imagefile delegates to); no second decoder or pixel model is introduced. */
#define HOST_IMG_MAX_BYTES (64u * 1024u)
#define HOST_IMG_MAX_W     80
#define HOST_IMG_MAX_H     40

/** Append one key=value pair to a fixed report line (bounded, deterministic). */
static void report_add(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    size_t used = strlen(buf);
    int n;

    if (used + 1 >= cap) return;
    va_start(ap, fmt);
    n = vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
    if (n < 0) buf[used] = '\0';
}

/** Start every row as an 80-cell grid with the default foreground, matching
 *  the page buffer `tui dump /a` reads on the board. */
static void probe_reset(void)
{
    for (int i = 0; i < WEBAPP_COLS * WEBAPP_ROWS; i++) {
        s_page.cells[i].text[0] = ' ';
        s_page.cells[i].text[1] = '\0';
        s_page.cells[i].fg = DEFAULT_FG;
        s_page.cells[i].bg = 16;
    }
    s_page.emitted = 0;
}

/** put_cell sink: record glyph and foreground at the 1-based cell position. */
static void probe_put_cell(void *ctx, int col, int row, const char *utf8,
                           uint8_t fg, uint8_t bg, uint8_t attr)
{
    page_probe_t *probe = (page_probe_t *)ctx;

    (void)bg;
    (void)attr;
    if (probe == NULL || utf8 == NULL) return;
    if (col < 1 || col > WEBAPP_COLS || row < 1 || row > WEBAPP_ROWS) return;
    {
        probe_cell_t *cell = &probe->cells[(row - 1) * WEBAPP_COLS + (col - 1)];
        size_t len = strlen(utf8);

        if (len >= sizeof(cell->text)) len = sizeof(cell->text) - 1;
        memcpy(cell->text, utf8, len);
        cell->text[len] = '\0';
        cell->fg = fg;
        probe->emitted++;
    }
}

/** Background sink: record the DOS bg index without touching the glyph, the
 *  same contract the live page buffer's fill_bg has. */
static void probe_fill_bg(void *ctx, int col, int row, int w, int h,
                          uint8_t bg, uint8_t alpha)
{
    page_probe_t *probe = (page_probe_t *)ctx;

    if (probe == NULL || alpha == 0 || bg > 15) return;
    for (int r = row; r < row + h && r <= WEBAPP_ROWS; r++) {
        if (r < 1) continue;
        for (int c = col; c < col + w && c <= WEBAPP_COLS; c++) {
            if (c >= 1) probe->cells[(r - 1) * WEBAPP_COLS + (c - 1)].bg = bg;
        }
    }
}

/** Append one `start-end:fg` run to a row's run buffer, with bounds checks. */
static void append_run(char *buf, size_t cap, size_t *pos,
                       int start, int end, int fg, bool first)
{
    char run[24];
    int n = snprintf(run, sizeof(run), "%s%d-%d:%d", first ? "" : " ",
                     start, end, fg);

    if (n < 0 || *pos + (size_t)n >= cap) return;
    memcpy(buf + *pos, run, (size_t)n + 1);
    *pos += (size_t)n;
}

/** Emit the same two lines per row that `tui dump [from] [to] /a` emits. */
static void print_dump(void)
{
    char line[WEBAPP_COLS * 4 + 1];
    char runs[WEBAPP_COLS * 20 + 1];

    printf("tui.dump cols=%d from=1 to=%d\n", WEBAPP_COLS, WEBAPP_ROWS);
    for (int r = 1; r <= WEBAPP_ROWS; r++) {
        size_t pos = 0;
        const probe_cell_t *row = &s_page.cells[(r - 1) * WEBAPP_COLS];
        size_t rpos = 0;
        int run_start = 1;
        int run_fg = row[0].fg;
        int run_end = 1;

        for (int c = 1; c <= WEBAPP_COLS; c++) {
            size_t len = strlen(row[c - 1].text);

            if (pos + len + 1 > sizeof(line)) break;
            memcpy(line + pos, row[c - 1].text, len);
            pos += len;
        }
        while (pos > 0 && line[pos - 1] == ' ') pos--;
        line[pos] = '\0';
        printf("%03d|%s\n", r, line);

        for (int c = 2; c <= WEBAPP_COLS; c++) {
            if ((int)row[c - 1].fg == run_fg) {
                run_end = c;
            } else {
                append_run(runs, sizeof(runs), &rpos, run_start, run_end,
                           run_fg, rpos == 0);
                run_start = c;
                run_fg = row[c - 1].fg;
                run_end = c;
            }
        }
        append_run(runs, sizeof(runs), &rpos, run_start, run_end, run_fg,
                   rpos == 0);
        printf("%03d@%s\n", r, runs);
    }
    printf("tui.dump.end\n");
}

/** Draw the page's <canvas> through the pure 2D core and record a deterministic
 *  digest of two raw pixels. This proves the canvas API reached the fixture
 *  without reimplementing the pixel-to-cell conversion in this harness. */
static void probe_canvas(dom_tree_t *tree)
{
    dom_node_t *node = dom_find_first_by_tag(tree, "canvas");
    js_canvas_t *canvas;
    js_canvas_rgba_t first;
    js_canvas_rgba_t second;
    const uint8_t *px;
    int w;
    int h;
    int half;

    s_canvas_line[0] = '\0';
    if (node == NULL) return;
    w = node->canvas_w > 0 ? node->canvas_w : 96;
    h = node->canvas_h > 0 ? node->canvas_h : 48;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > JS_CANVAS_MAX_W) w = JS_CANVAS_MAX_W;
    if (h > JS_CANVAS_MAX_H) h = JS_CANVAS_MAX_H;
    canvas = js_canvas_for_node(node, w, h);
    if (canvas == NULL) return;
    s_chart_canvas = canvas;   /* the DOM canvas sink samples this same bitmap */
    if (!js_canvas_parse_color("#ff5555", &first)) return;
    if (!js_canvas_parse_color("#55ffff", &second)) return;

    js_canvas_set_fill(canvas, first);
    half = w / 2;
    js_canvas_fill_rect(canvas, 0.0f, 0.0f, (float)half, (float)h);
    js_canvas_set_fill(canvas, second);
    js_canvas_fill_rect(canvas, (float)half, 0.0f, (float)(w - half), (float)h);
    px = js_canvas_pixels(canvas);
    if (px == NULL) return;

    (void)snprintf(s_canvas_line, sizeof(s_canvas_line),
                   "webapp.canvas id=%s w=%d h=%d pixel0=%02x%02x%02x%02x "
                   "pixel1=%02x%02x%02x%02x",
                   node->id[0] != '\0' ? node->id : "-", w, h,
                   px[0], px[1], px[2], px[3],
                   px[(size_t)half * 4u], px[(size_t)half * 4u + 1u],
                   px[(size_t)half * 4u + 2u], px[(size_t)half * 4u + 3u]);

    /* The SAME rasterizer also serves gradients, rounded rects and shadows.
     * All samples are raw RGBA bytes; no second pixel model is introduced. */
    {
        js_canvas_gradient_t gradient;

        if (js_canvas_gradient_parse(
                "linear-gradient(90deg, #ff0000 0%, #0000ff 100%)", &gradient)) {
            js_canvas_t *gc = js_canvas_new(8, 4);

            if (gc != NULL) {
                const uint8_t *gpx;

                js_canvas_fill_gradient_rect(gc, &gradient, 0, 0, 8, 4, false);
                gpx = js_canvas_pixels(gc);
                if (gpx != NULL) {
                    report_add(s_canvas_line, sizeof(s_canvas_line),
                               " grad0=%02x%02x%02x%02x grad1=%02x%02x%02x%02x",
                               gpx[0], gpx[1], gpx[2], gpx[3],
                               gpx[7u * 4u], gpx[7u * 4u + 1u],
                               gpx[7u * 4u + 2u], gpx[7u * 4u + 3u]);
                }
                js_canvas_free(gc);
            }
        }
    }
    {
        js_canvas_t *rc = js_canvas_new(8, 8);

        if (rc != NULL) {
            const uint8_t *rpx;

            js_canvas_set_fill(rc, (js_canvas_rgba_t){ 255, 255, 255, 255 });
            js_canvas_fill_rounded_rect(rc, 1, 1, 6, 6, 2);
            rpx = js_canvas_pixels(rc);
            if (rpx != NULL) {
                const uint8_t *corner = rpx + (size_t)1 * 8u * 4u + 1u * 4u;
                const uint8_t *centre = rpx + (size_t)4 * 8u * 4u + 4u * 4u;

                report_add(s_canvas_line, sizeof(s_canvas_line),
                           " round_corner=%02x round_centre=%02x",
                           corner[3], centre[3]);
            }
            js_canvas_free(rc);
        }
    }
    {
        js_canvas_t *sc = js_canvas_new(12, 12);

        if (sc != NULL) {
            const uint8_t *spx;

            js_canvas_set_shadow(sc, (js_canvas_rgba_t){ 255, 0, 0, 255 }, 2, 0, 0);
            js_canvas_set_fill(sc, (js_canvas_rgba_t){ 0, 0, 255, 255 });
            js_canvas_fill_rect(sc, 1, 1, 3, 3);
            spx = js_canvas_pixels(sc);
            if (spx != NULL) {
                const uint8_t *shadow = spx + (size_t)2 * 12u * 4u + 4u * 4u;

                report_add(s_canvas_line, sizeof(s_canvas_line),
                           " shadow=%02x%02x%02x%02x",
                           shadow[0], shadow[1], shadow[2], shadow[3]);
            }
            js_canvas_free(sc);
        }
    }
}

/** Report the ONE entity decoder on fixed probes: longest/named, legacy
 *  no-semicolon, numeric hex/decimal, clamped and C1-remapped. No second
 *  decoder is introduced - this calls html_entity_decode() directly. */
static void probe_entities(void)
{
    static const char *PROBES[] = {
        "&amp;", "&reg", "&#x41;", "&#1114112;", "&#x80;", "&hellip;"
    };

    snprintf(s_entity_line, sizeof(s_entity_line), "webapp.entity");
    for (size_t i = 0; i < sizeof(PROBES) / sizeof(PROBES[0]); i++) {
        unsigned long cp = 0;
        size_t used = 0;

        if (html_entity_decode(PROBES[i], PROBES[i] + strlen(PROBES[i]),
                               &cp, &used)) {
            report_add(s_entity_line, sizeof(s_entity_line), " %s=%04lX/%u",
                       PROBES[i], cp, (unsigned)used);
        }
    }
}

/** Report which `color` rule wins for <h1> using the SAME css_rule_compare()
 *  and css_selector_matches() the cascade uses in css_apply(). */
static void probe_cascade(const dom_tree_t *tree)
{
    dom_node_t *h1 = dom_find_first_by_tag((dom_tree_t *)tree, "h1");
    const css_rule_t *best = NULL;

    s_cascade_line[0] = '\0';
    if (h1 == NULL) return;
    for (int i = 0; i < s_rules.count; i++) {
        const css_rule_t *r = &s_rules.rules[i];

        if (strcmp(r->property, "color") != 0 ||
            !css_selector_matches(r->selector, h1)) {
            continue;
        }
        if (best == NULL || css_rule_compare(r, best) > 0) best = r;
    }
    if (best != NULL) {
        snprintf(s_cascade_line, sizeof(s_cascade_line),
                 "webapp.cascade h1_color=%s spec=%d.%d.%d imp=%d order=%d fg=%u",
                 best->selector, css_specificity_id(best),
                 css_specificity_class(best), css_specificity_type(best),
                 best->important ? 1 : 0, best->order, (unsigned)h1->fg);
    }
}

/** Recursive feature walk over the ONE parsed tree (bounded by DOM depth). */
static void count_feature_node(dom_node_t *n, int *semantic, int *controls,
                               int *grids, int *canvases, int *handlers,
                               dom_node_t **card, int depth)
{
    if (n == NULL || depth > 64) return;
    if (n->type == DOM_NODE_ELEMENT) {
        if (strcmp(n->tag, "header") == 0 || strcmp(n->tag, "nav") == 0 ||
            strcmp(n->tag, "main") == 0 || strcmp(n->tag, "section") == 0 ||
            strcmp(n->tag, "article") == 0 || strcmp(n->tag, "footer") == 0 ||
            strcmp(n->tag, "details") == 0 || strcmp(n->tag, "summary") == 0 ||
            strcmp(n->tag, "dialog") == 0) {
            (*semantic)++;
        }
        if (strcmp(n->tag, "form") == 0 || strcmp(n->tag, "input") == 0 ||
            strcmp(n->tag, "select") == 0 || strcmp(n->tag, "option") == 0 ||
            strcmp(n->tag, "button") == 0 || strcmp(n->tag, "label") == 0) {
            (*controls)++;
        }
        if (n->display == DOM_DISPLAY_GRID || n->display == DOM_DISPLAY_FLEX) (*grids)++;
        if (strcmp(n->tag, "canvas") == 0) (*canvases)++;
        if (n->onclick[0] != '\0' || n->onload[0] != '\0') (*handlers)++;
        if (*card == NULL && strcmp(n->id, "card") == 0) *card = n;
    }
    for (dom_node_t *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        count_feature_node(ch, semantic, controls, grids, canvases, handlers,
                           card, depth + 1);
    }
}

/** Count the new HTML5/CSS constructs by walking the ONE parsed tree, and
 *  report the computed visual state of the first element with id="card". */
static void probe_features(const dom_tree_t *tree)
{
    int semantic = 0;
    int controls = 0;
    int grids = 0;
    int canvases = 0;
    int handlers = 0;
    dom_node_t *card = NULL;

    s_meta_line[0] = s_css_line[0] = s_events_line[0] = '\0';
    count_feature_node(tree->root, &semantic, &controls, &grids, &canvases,
                       &handlers, &card, 0);
    (void)snprintf(s_meta_line, sizeof(s_meta_line),
                   "webapp.meta semantic=%d controls=%d grids=%d canvases=%d",
                   semantic, controls, grids, canvases);
    (void)snprintf(s_events_line, sizeof(s_events_line),
                   "webapp.events handlers=%d", handlers);
    if (card != NULL) {
        (void)snprintf(s_css_line, sizeof(s_css_line),
                       "webapp.css id=card radius=%u shadow=%d gradient=%d "
                       "opacity=%u",
                       (unsigned)card->border_radius, card->shadow_set ? 1 : 0,
                       card->gradient_set ? 1 : 0, (unsigned)card->opacity);
    }
}

/** Read a whole fixture as bytes; bounded so a malformed file cannot exhaust
 *  the host. Returns a NUL-terminated malloc() buffer or NULL. */
static char *read_html(const char *path)
{
    FILE *fh = fopen(path, "rb");
    long size;
    char *buf;
    size_t got;

    if (fh == NULL) return NULL;
    if (fseek(fh, 0, SEEK_END) != 0) {
        fclose(fh);
        return NULL;
    }
    size = ftell(fh);
    if (size < 0 || (unsigned long)size >= WEBAPP_HTML_MAX) {
        fclose(fh);
        return NULL;
    }
    rewind(fh);
    buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fh);
        return NULL;
    }
    got = fread(buf, 1, (size_t)size, fh);
    fclose(fh);
    buf[got] = '\0';
    return buf;
}

/** Portable basename for the fixture marker (host builds may cross-compile). */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');

    if (back != NULL && (slash == NULL || back > slash)) slash = back;
    return slash != NULL ? slash + 1 : path;
}

/** Resolve a DOM image source inside the fixture directory only. Absolute,
 *  parent-traversal and URL sources are refused: the probe reads no network and
 *  no path outside its fixture root. */
static bool probe_image_path(const char *src, char *out, size_t cap)
{
    if (src == NULL || src[0] == '\0' || out == NULL || cap == 0) return false;
    if (src[0] == '/' || src[0] == '\\' || strchr(src, ':') != NULL ||
        strstr(src, "..") != NULL) {
        return false;
    }
    if (snprintf(out, cap, "%s/%s", s_fixture_dir, src) >= (int)cap) {
        return false;
    }
    return true;
}

/** Read a bounded fixture asset. Oversized or unreadable files fail honestly. */
static uint8_t *probe_read_asset(const char *path, size_t *len_out)
{
    FILE *fh = fopen(path, "rb");
    uint8_t *buf;
    long size;
    size_t got;

    if (fh == NULL) return NULL;
    if (fseek(fh, 0, SEEK_END) != 0) { fclose(fh); return NULL; }
    size = ftell(fh);
    if (size < 54 || (unsigned long)size > HOST_IMG_MAX_BYTES) {
        fclose(fh);
        return NULL;
    }
    rewind(fh);
    buf = malloc((size_t)size);
    if (buf == NULL) { fclose(fh); return NULL; }
    got = fread(buf, 1, (size_t)size, fh);
    fclose(fh);
    if (got != (size_t)size) { free(buf); return NULL; }
    *len_out = got;
    return buf;
}

/** The one image sink for <img> and CSS background-image: decode the local BMP
 *  through gfx (the BMP half of the one imagefile decoder), then pack cells
 *  with dom_image_sample_rgb565 - the DOM's pure equivalent of the one TUI
 *  sampler. No second decoder, ramp or palette. */
static bool probe_load_image(void *ctx, const char *src, dom_image_t *out)
{
    char path[600];
    uint8_t *data;
    size_t len = 0;
    gfx_bmp_info_t info;
    gfx_surface_t surface;
    int nw, nh, w, h;
    uint8_t *cells;

    (void)ctx;
    memset(out, 0, sizeof(*out));
    if (!probe_image_path(src, path, sizeof(path))) return false;
    data = probe_read_asset(path, &len);
    if (data == NULL) return false;
    if (!gfx_bmp_parse_header_ex(data, len, &info, 256, 256)) {
        free(data);
        return false;
    }
    nw = info.w > 0 ? info.w : 1;
    nh = info.h > 0 ? info.h : 1;
    w = nw;
    h = nh;
    if (w > HOST_IMG_MAX_W) { h = h * HOST_IMG_MAX_W / w; w = HOST_IMG_MAX_W; }
    if (h > HOST_IMG_MAX_H) { w = w * HOST_IMG_MAX_H / h; h = HOST_IMG_MAX_H; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (!gfx_bmp_decode_scaled_565(data, len, &info, w, h, &surface)) {
        free(data);
        return false;
    }
    cells = malloc((size_t)w * (size_t)h);
    if (cells == NULL ||
        !dom_image_sample_rgb565(surface.px, surface.w, surface.h,
                                 cells, w, h)) {
        free(cells);
        gfx_surface_free(&surface);
        free(data);
        return false;
    }
    gfx_surface_free(&surface);
    free(data);
    out->width = w;
    out->height = h;
    s_last_native_w = nw;
    s_last_native_h = nh;
    out->pixels = cells;
    return true;
}

/** <canvas> bitmap: snapshot the RGBA bytes the pure 2D core produced in
 *  probe_canvas() and sample them through the SAME dom_image_sample_rgb565. */
static bool probe_load_canvas(void *ctx, dom_node_t *node, dom_image_t *out)
{
    uint8_t *rgba;
    uint16_t *rgb565;
    int w, h;

    (void)ctx;
    memset(out, 0, sizeof(*out));
    if (s_chart_canvas == NULL || node == NULL) return false;
    w = P4_CONFIG_BROWSER_IMAGE_MAX_WIDTH < 64
            ? P4_CONFIG_BROWSER_IMAGE_MAX_WIDTH : 64;
    h = P4_CONFIG_BROWSER_IMAGE_MAX_HEIGHT < 32
            ? P4_CONFIG_BROWSER_IMAGE_MAX_HEIGHT : 32;
    rgba = malloc((size_t)w * (size_t)h * 4u);
    rgb565 = malloc((size_t)w * (size_t)h * sizeof(uint16_t));
    if (rgba == NULL || rgb565 == NULL) {
        free(rgba);
        free(rgb565);
        return false;
    }
    if (!js_canvas_snapshot_rgba(s_chart_canvas, w, h, rgba, &w, &h) ||
        w < 1 || h < 1) {
        free(rgba);
        free(rgb565);
        return false;
    }
    for (int i = 0; i < w * h; i++) {
        rgb565[i] = (uint16_t)(((uint16_t)(rgba[i * 4u] >> 3) << 11) |
                               ((uint16_t)(rgba[i * 4u + 1u] >> 2) << 5) |
                               (uint16_t)(rgba[i * 4u + 2u] >> 3));
    }
    out->pixels = malloc((size_t)w * (size_t)h);
    if (out->pixels == NULL ||
        !dom_image_sample_rgb565(rgb565, w, h, out->pixels, w, h)) {
        free(out->pixels);
        out->pixels = NULL;
        free(rgba);
        free(rgb565);
        return false;
    }
    for (int i = 0; i < w * h; i++) {
        if (rgba[i * 4u + 3u] < 64) out->pixels[i] = 0;
    }
    out->width = w;
    out->height = h;
    s_last_native_w = w;
    s_last_native_h = h;
    free(rgba);
    free(rgb565);
    return true;
}

/** Print the same structured outcome the device `webapp images` facade emits. */
static void probe_report_image(void *ctx, const dom_image_report_t *r)
{
    static const char *names[] = { "ok", "resized", "missing", "fallback" };
    const char *state;
    int native_w;
    int native_h;

    (void)ctx;
    if (r == NULL) return;
    state = (int)r->state >= 0 && (int)r->state < 4
                ? names[r->state] : "unknown";
    native_w = s_last_native_w > 0 ? s_last_native_w : r->native_w;
    native_h = s_last_native_h > 0 ? s_last_native_h : r->native_h;
    if (r->state == DOM_IMAGE_OK &&
        (native_w > r->fit_w || native_h > r->fit_h)) {
        state = "resized";
    }
    printf("webapp.image kind=%s src=%s state=%s native=%dx%d fit=%dx%d alt=%s\n",
           r->kind != NULL ? r->kind : "-",
           r->src != NULL ? r->src : "-", state,
           native_w, native_h, r->fit_w, r->fit_h,
           r->alt != NULL && r->alt[0] != '\0' ? r->alt : "-");
}

int main(int argc, char **argv)
{
    char *html;
    dom_tree_t *tree;
    dom_node_t *style;
    dom_render_ops_t ops;
    const char *name;

    if (argc != 2) {
        fprintf(stderr, "usage: host_webapp_render <fixture.html>\n");
        return 2;
    }
    {
        const char *slash = strrchr(argv[1], '/');
        const char *back = strrchr(argv[1], '\\');
        const char *last = back != NULL && (slash == NULL || back > slash)
                               ? back : slash;

        if (last != NULL) {
            size_t n = (size_t)(last - argv[1]);

            if (n >= sizeof(s_fixture_dir)) n = sizeof(s_fixture_dir) - 1;
            memcpy(s_fixture_dir, argv[1], n);
            s_fixture_dir[n] = '\0';
        } else {
            snprintf(s_fixture_dir, sizeof(s_fixture_dir), ".");
        }
    }
    html = read_html(argv[1]);
    if (html == NULL) {
        fprintf(stderr, "host_webapp_render: cannot read %s\n", argv[1]);
        return 2;
    }
    probe_reset();
    memset(&s_rules, 0, sizeof(s_rules));
    s_canvas_line[0] = '\0';
    s_meta_line[0] = s_css_line[0] = s_events_line[0] = '\0';
    s_cascade_line[0] = s_entity_line[0] = '\0';
    js_canvas_clear_registry();

    tree = dom_parse(html);
    free(html);
    if (tree == NULL) {
        fprintf(stderr, "host_webapp_render: dom_parse failed\n");
        return 2;
    }

    /* The <style> block is the stylesheet source; css_compute_styles() applies
     * it through css_apply() and then runs the inline-style/inheritance pass. */
    style = dom_find_first_by_tag(tree, "style");
    if (style != NULL && style->first_child != NULL &&
            style->first_child->type == DOM_NODE_TEXT) {
        css_parse_block(style->first_child->text, &s_rules);
    }
    css_compute_styles(tree, &s_rules);

    probe_canvas(tree);
    probe_features(tree);
    probe_entities();
    probe_cascade(tree);

    memset(&ops, 0, sizeof(ops));
    ops.put_cell = probe_put_cell;
    ops.fill_bg = probe_fill_bg;
    ops.load_image = probe_load_image;
    ops.load_background = probe_load_image;
    ops.load_canvas = probe_load_canvas;
    ops.report_image = probe_report_image;
    ops.ctx = &s_page;
    ops.cols = WEBAPP_COLS;
    ops.rows = WEBAPP_ROWS;
    ops.tui = true;
    ops.base_bg = 0;
    dom_render_tree(tree, &ops);

    name = base_name(argv[1]);
    printf("webapp.render.begin fixture=%s cols=%d rows=%d\n",
           name, WEBAPP_COLS, WEBAPP_ROWS);
    print_dump();
    if (s_meta_line[0] != '\0') printf("%s\n", s_meta_line);
    if (s_css_line[0] != '\0') printf("%s\n", s_css_line);
    if (s_events_line[0] != '\0') printf("%s\n", s_events_line);
    if (s_cascade_line[0] != '\0') printf("%s\n", s_cascade_line);
    if (s_entity_line[0] != '\0') printf("%s\n", s_entity_line);
    if (s_canvas_line[0] != '\0') printf("%s\n", s_canvas_line);
    printf("webapp.render.end fixture=%s rows=%d runs=%d\n",
           name, WEBAPP_ROWS, WEBAPP_ROWS);

    dom_free(tree);
    return 0;
}

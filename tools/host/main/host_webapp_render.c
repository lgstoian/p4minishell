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
#include "js_canvas.h"

#include <stdbool.h>
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
} probe_cell_t;

/** File-scope page grid: 80x25 cells stay off the stack. */
typedef struct {
    probe_cell_t cells[WEBAPP_COLS * WEBAPP_ROWS];
    int emitted;
} page_probe_t;

static page_probe_t s_page;
static css_ruleset_t s_rules;
static char s_canvas_line[192];

/** Start every row as an 80-cell grid with the default foreground, matching
 *  the page buffer `tui dump /a` reads on the board. */
static void probe_reset(void)
{
    for (int i = 0; i < WEBAPP_COLS * WEBAPP_ROWS; i++) {
        s_page.cells[i].text[0] = ' ';
        s_page.cells[i].text[1] = '\0';
        s_page.cells[i].fg = DEFAULT_FG;
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
    html = read_html(argv[1]);
    if (html == NULL) {
        fprintf(stderr, "host_webapp_render: cannot read %s\n", argv[1]);
        return 2;
    }
    probe_reset();
    memset(&s_rules, 0, sizeof(s_rules));
    s_canvas_line[0] = '\0';
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

    memset(&ops, 0, sizeof(ops));
    ops.put_cell = probe_put_cell;
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
    if (s_canvas_line[0] != '\0') printf("%s\n", s_canvas_line);
    printf("webapp.render.end fixture=%s rows=%d runs=%d\n",
           name, WEBAPP_ROWS, WEBAPP_ROWS);

    dom_free(tree);
    return 0;
}

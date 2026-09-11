/**
 * @file plot_commands.c
 * @brief `plot` coordinate-layer verbs: scientific graphs, charts, drawings.
 *
 * A thin coordinate layer over two existing renderers — the `gfx` RGB565
 * pixel canvas (default) and the TUI cell grid (`plot tui on`) — driven by
 * one shared world-coordinate viewport (gfx_view_t). Function sampling uses
 * the existing `calc` evaluator (`calc_evaluate` over the X/T env vars);
 * data/bar files use the same guarded-SD `fopen`/`fgets` pattern as
 * `gfx load`; colors reuse the `gfx`/`draw` parsers. Nothing here owns a
 * raster engine, an expression parser, or a canvas: `gfx` keeps the canvas,
 * `draw` keeps the cell buffer, `calc` keeps the math.
 *
 * Batch notes: expressions with spaces must be quoted (`plot func "x^2+1"`;
 * `^ & | < >` are shell operators). Trig follows the current `calc` angle
 * mode (`calc /rad` for radian plots). Canvas plots never auto-show (compose
 * several, then one `gfx show`); TUI plots flush through `draw_maybe_flush`
 * (so `draw hold` coalesces them). Foreground only (shared display).
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "shell.h"
#include "batch.h"
#include "calc.h"
#include "command.h"
#include "gfx.h"
#include "tui.h"
#include "storage.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Last data source, for bare `plot auto` (0 none, 1 func, 2 polar, 3 para,
 * 4 data, 5 bar). */
enum {
    PLOT_LAST_NONE = 0,
    PLOT_LAST_FUNC,
    PLOT_LAST_POLAR,
    PLOT_LAST_PARA,
    PLOT_LAST_DATA,
    PLOT_LAST_BAR,
};

static gfx_view_t s_plot_view_win = {0};
static bool s_plot_win_set = false;
static bool s_plot_tui = false;
/* Per-target raster rect override from `plot window /rect:` (canvas pixels
 * are 0-based, TUI cells 1-based — each follows its own target). */
static int s_plot_rect_c[4] = {0, 0, 0, 0};
static bool s_plot_rect_c_set = false;
static int s_plot_rect_t[4] = {0, 0, 0, 0};
static bool s_plot_rect_t_set = false;
static int s_plot_last_kind = PLOT_LAST_NONE;
static char s_plot_last_text[256] = {0};
static char s_plot_last_text2[256] = {0}; /* para y-expr */
static uint16_t s_plot_last_color = 0xFFFF;
static int s_plot_last_samples = 0;
static bool s_plot_last_dots = false;

/* ========================================================================
 * SMALL PARSERS
 * ======================================================================== */

/** Parse a full-consumption double (no trailing junk, no empty). */
static bool plot_parse_double(const char *s, double *out)
{
    char *end = NULL;
    double v;

    if (s == NULL || *s == '\0' || out == NULL) return false;
    v = strtod(s, &end);
    if (end == s || *end != '\0') return false;
    if (!isfinite(v)) return false;
    *out = v;
    return true;
}

/** Parse a full-consumption int. */
static bool plot_parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || *s == '\0' || out == NULL) return false;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *out = (int)v;
    return true;
}

/** DOS index + RGB565 pair from one color token (defaults per verb). */
static void plot_color_pair(const char *tok, uint8_t def_dos, uint8_t *dos_out,
                            uint16_t *rgb_out)
{
    uint8_t d = (def_dos > 16) ? 16 : def_dos;

    *dos_out = draw_color_arg(tok, d);
    if (tok == NULL || *tok == '\0') {
        *rgb_out = gfx_rgb_to_565(tui_dos_color_rgb(d > 15 ? 0 : d));
    } else {
        *rgb_out = gfx_canvas_parse_color(tok, gfx_rgb_to_565(tui_dos_color_rgb(d > 15 ? 0 : d)));
    }
}

/** Evaluate @p expr with @p var bound to @p x (prior value restored).
 * @return true with a finite number in @p out. */
static bool plot_eval(const char *expr, const char *var, double x, double *out)
{
    const char *old;
    char saved[64];
    bool have_old = false;
    char xbuf[32];
    bool ok = false;

    if (expr == NULL || var == NULL || out == NULL) return false;
    old = shell_env_get(var);
    if (old != NULL && old[0] != '\0') {
        snprintf(saved, sizeof(saved), "%s", old);
        have_old = true;
    }
    snprintf(xbuf, sizeof(xbuf), "%.10g", x);
    if (shell_env_set(var, xbuf) == ESP_OK) {
        calc_value_t v;
        const char *err = NULL;

        if (calc_evaluate(expr, &v, &err) && !v.is_string && isfinite(v.num)) {
            *out = v.num;
            ok = true;
        }
    }
    if (have_old) {
        shell_env_set(var, saved);
    } else {
        shell_env_set(var, "");
    }
    return ok;
}

/* ========================================================================
 * TARGET RESOLUTION + VIEW
 * ======================================================================== */

/** Foreground + target readiness. Canvas needs an open `gfx` canvas; TUI
 * auto-enters like `draw`. @return false with EL set on refusal. */
static bool plot_require_target(bool *tui_out)
{
    if (!draw_require_foreground("plot")) return false;
    if (s_plot_tui) {
        if (!tui_is_active() && !tui_init()) {
            shell_transcript_appendf_ansi(SH_ERR "plot: cannot enter TUI mode\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        *tui_out = true;
        return true;
    }
    if (!gfx_canvas_is_open()) {
        shell_transcript_appendf_ansi(SH_ERR "plot: no canvas (gfx init <w> <h> first)\n" SH_RST);
        batch_set_errorlevel(1);
        return false;
    }
    *tui_out = false;
    return true;
}

/** Build the active view: stored world window (or -10..10 default) over the
 * stored rect override (or the full target surface). */
static void plot_build_view(gfx_view_t *v, bool tui)
{
    double xmin = -10.0;
    double xmax = 10.0;
    double ymin = -10.0;
    double ymax = 10.0;
    int px;
    int py;
    int pw;
    int ph;

    if (s_plot_win_set) {
        xmin = s_plot_view_win.xmin;
        xmax = s_plot_view_win.xmax;
        ymin = s_plot_view_win.ymin;
        ymax = s_plot_view_win.ymax;
    }
    if (tui) {
        if (s_plot_rect_t_set) {
            px = s_plot_rect_t[0];
            py = s_plot_rect_t[1];
            pw = s_plot_rect_t[2];
            ph = s_plot_rect_t[3];
        } else {
            px = 1;
            py = 1;
            pw = P4_CONFIG_TUI_COLS;
            ph = P4_CONFIG_TUI_ROWS;
        }
    } else {
        gfx_surface_t *s = gfx_canvas_surface();

        if (s_plot_rect_c_set) {
            px = s_plot_rect_c[0];
            py = s_plot_rect_c[1];
            pw = s_plot_rect_c[2];
            ph = s_plot_rect_c[3];
        } else if (s != NULL) {
            px = 0;
            py = 0;
            pw = s->w;
            ph = s->h;
        } else {
            px = 0;
            py = 0;
            pw = GFX_MAX_W;
            ph = GFX_MAX_H;
        }
    }
    gfx_view_set(v, xmin, xmax, ymin, ymax, px, py, pw, ph);
}

/* ========================================================================
 * DUAL-TARGET RASTER (canvas pixels / TUI cells)
 * ======================================================================== */

typedef struct {
    bool tui;
    gfx_surface_t *surf; /* canvas target (NULL for TUI) */
    uint16_t color;      /* canvas RGB565 */
    uint8_t fg;          /* TUI DOS fg */
    char ch;             /* TUI dot char */
} plot_out_t;

static void plot_out_init(plot_out_t *o, bool tui, uint16_t color, uint8_t fg,
                          char ch)
{
    o->tui = tui;
    o->surf = tui ? NULL : gfx_canvas_surface();
    o->color = color;
    o->fg = fg;
    o->ch = (ch == '\0') ? '*' : ch;
}

static void plot_put(plot_out_t *o, int sx, int sy)
{
    if (o->tui) {
        tui_fill(sx, sy, 1, 1, o->ch, o->fg, 16);
    } else if (o->surf != NULL) {
        gfx_surface_pixel(o->surf, sx, sy, o->color);
    }
}

/** Integer segment on either target (TUI uses a cell Bresenham). */
static void plot_seg(plot_out_t *o, int ax, int ay, int bx, int by)
{
    if (!o->tui) {
        if (o->surf != NULL) gfx_surface_line(o->surf, ax, ay, bx, by, o->color);
        return;
    }
    {
        int dx = abs(bx - ax);
        int dy = -abs(by - ay);
        int sx = (ax < bx) ? 1 : -1;
        int sy = (ay < by) ? 1 : -1;
        int err = dx + dy;
        int x = ax;
        int y = ay;

        for (;;) {
            tui_fill(x, y, 1, 1, o->ch, o->fg, 16);
            if (x == bx && y == by) break;
            {
                int e2 = 2 * err;

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
    }
}

static void plot_hline(plot_out_t *o, int x, int y, int w)
{
    if (!o->tui) {
        if (o->surf != NULL) gfx_surface_hline(o->surf, x, y, w, o->color);
    } else {
        tui_draw_line(x, y, x + w - 1, y, "single", o->fg, 16);
    }
}

static void plot_vline(plot_out_t *o, int x, int y, int h)
{
    if (!o->tui) {
        if (o->surf != NULL) gfx_surface_vline(o->surf, x, y, h, o->color);
    } else {
        tui_draw_line(x, y, x, y + h - 1, "single", o->fg, 16);
    }
}

static void plot_text(plot_out_t *o, int sx, int sy, const char *text)
{
    if (!o->tui) {
        if (o->surf != NULL) {
            gfx_surface_text(o->surf, sx, sy, text, o->color, 0, false, 1);
        }
    } else {
        tui_print_at(sx, sy, text, o->fg, 16);
    }
}

static void plot_flush_target(bool tui)
{
    if (tui) draw_maybe_flush();
}

/* ========================================================================
 * AXES + TICKS
 * ======================================================================== */

/** Draw axes at the origin (or window edges) with nice ticks + labels on
 * the active view. */
static void plot_draw_axes(const gfx_view_t *v, plot_out_t *o, int ticks)
{
    double xs = v->xmax - v->xmin;
    double ys = v->ymax - v->ymin;
    double xstep = gfx_view_nice_step(xs, ticks);
    double ystep = gfx_view_nice_step(ys, ticks);
    double yax = (0.0 >= v->ymin && 0.0 <= v->ymax) ? 0.0 : v->ymin;
    double xax = (0.0 >= v->xmin && 0.0 <= v->xmax) ? 0.0 : v->xmin;
    int ax0x;
    int ax0y;
    int ax1x;
    int ax1y;
    int ay0x;
    int ay0y;
    int ay1x;
    int ay1y;
    int i;
    double t;
    double t0;
    char label[16];
    bool labeled_origin = false;

    /* Axis rows/cols (clamped into the rect). */
    if (!gfx_view_map(v, v->xmin, yax, &ax0x, &ax0y)) {
        ax0x = v->px;
        ax0y = v->py + v->ph - 1;
    }
    if (!gfx_view_map(v, v->xmax, yax, &ax1x, &ax1y)) {
        ax1x = v->px + v->pw - 1;
        ax1y = ax0y;
    }
    if (!gfx_view_map(v, xax, v->ymin, &ay0x, &ay0y)) {
        ay0x = v->px;
        ay0y = v->py + v->ph - 1;
    }
    if (!gfx_view_map(v, xax, v->ymax, &ay1x, &ay1y)) {
        ay1x = ay0x;
        ay1y = v->py;
    }
    plot_hline(o, (ax0x < ax1x) ? ax0x : ax1x, ax0y,
               abs(ax1x - ax0x) + 1);
    plot_vline(o, ay0x, (ay0y < ay1y) ? ay0y : ay1y,
               abs(ay1y - ay0y) + 1);

    /* X ticks: small vertical marks + labels below the axis row. */
    t0 = ceil(v->xmin / xstep) * xstep;
    for (i = 0; i < 4096; i++) {
        int tx;
        int ty;

        t = t0 + (double)i * xstep;
        if (t > v->xmax) break;
        if (!gfx_view_map(v, t, yax, &tx, &ty)) continue;
        plot_vline(o, tx, ty - 1, 3);
        if (fabs(t) < xstep * 0.5) {
            if (!labeled_origin) {
                plot_text(o, tx + 1, ty + 1, "0");
                labeled_origin = true;
            }
            continue;
        }
        snprintf(label, sizeof(label), "%.4g", t);
        plot_text(o, tx + 1, ty + 1, label);
    }
    /* Y ticks: small horizontal marks + labels right of the axis col. */
    t0 = ceil(v->ymin / ystep) * ystep;
    for (i = 0; i < 4096; i++) {
        int tx;
        int ty;

        t = t0 + (double)i * ystep;
        if (t > v->ymax) break;
        if (!gfx_view_map(v, xax, t, &tx, &ty)) continue;
        plot_hline(o, tx - 1, ty, 3);
        if (fabs(t) < ystep * 0.5) {
            if (!labeled_origin) {
                plot_text(o, tx + 1, ty + 1, "0");
                labeled_origin = true;
            }
            continue;
        }
        snprintf(label, sizeof(label), "%.4g", t);
        plot_text(o, tx + 2, ty, label);
    }
}

/** Optional full-grid lines at the same nice ticks. */
static void plot_draw_grid(const gfx_view_t *v, plot_out_t *o, int ticks)
{
    double xs = v->xmax - v->xmin;
    double ys = v->ymax - v->ymin;
    double xstep = gfx_view_nice_step(xs, ticks);
    double ystep = gfx_view_nice_step(ys, ticks);
    double t0;
    double t;
    int i;
    int a;
    int b;
    int c;
    int d;

    t0 = ceil(v->xmin / xstep) * xstep;
    for (i = 0; i < 4096; i++) {
        t = t0 + (double)i * xstep;
        if (t > v->xmax) break;
        if (gfx_view_map(v, t, v->ymin, &a, &b) &&
            gfx_view_map(v, t, v->ymax, &c, &d)) {
            plot_vline(o, a, (b < d) ? b : d, abs(d - b) + 1);
        }
    }
    t0 = ceil(v->ymin / ystep) * ystep;
    for (i = 0; i < 4096; i++) {
        t = t0 + (double)i * ystep;
        if (t > v->ymax) break;
        if (gfx_view_map(v, v->xmin, t, &a, &b) &&
            gfx_view_map(v, v->xmax, t, &c, &d)) {
            plot_hline(o, (a < c) ? a : c, b, abs(c - a) + 1);
        }
    }
}

/* ========================================================================
 * SAMPLING (calc-backed)
 * ======================================================================== */

/** Sample fn points; fit_only tracks min/max, else draws a clipped polyline
 * (breaks across asymptotes/non-finite). @return points seen / EL-ish code. */
static int plot_sample_func(const gfx_view_t *v, plot_out_t *o,
                            const char *expr, const char *var,
                            double t0, double t1, int n, bool fit_only,
                            double *min_out, double *max_out)
{
    bool have_prev = false;
    int psx = 0;
    int psy = 0;
    int seen = 0;
    int i;

    if (min_out != NULL) *min_out = 0.0;
    if (max_out != NULL) *max_out = 0.0;
    for (i = 0; i < n; i++) {
        double t = (n > 1) ? t0 + (t1 - t0) * (double)i / (double)(n - 1) : t0;
        double val = 0.0;

        if (!plot_eval(expr, var, t, &val)) {
            have_prev = false;
            continue;
        }
        seen++;
        if (fit_only) {
            if (seen == 1) {
                if (min_out != NULL) *min_out = val;
                if (max_out != NULL) *max_out = val;
            } else {
                if (min_out != NULL && val < *min_out) *min_out = val;
                if (max_out != NULL && val > *max_out) *max_out = val;
            }
            continue;
        }
        {
            int sx;
            int sy;

            if (!gfx_view_map(v, t, val, &sx, &sy)) {
                have_prev = false;
                continue;
            }
            if (have_prev && abs(sy - psy) <= v->ph) {
                plot_seg(o, psx, psy, sx, sy);
            } else if (!have_prev) {
                plot_put(o, sx, sy);
            } else {
                plot_put(o, sx, sy);
            }
            have_prev = true;
            psx = sx;
            psy = sy;
        }
    }
    return seen;
}

/** Map a world y to a raster row clamped into the view rect (for TUI bar
 * columns, where out-of-window ends must still paint to the edge). */
static int plot_row_clamped(const gfx_view_t *v, double y)
{
    double yden = (double)(v->ph - 1);
    double fy = (y - v->ymin) / (v->ymax - v->ymin) * yden;
    int r = v->py + (v->ph - 1) - (int)floor(fy + 0.5);

    if (r < v->py) r = v->py;
    if (r > v->py + v->ph - 1) r = v->py + v->ph - 1;
    return r;
}

/** Polar r=f(T deg): map to x=r*cos, y=r*sin (radians in C). */
static int plot_sample_polar(const gfx_view_t *v, plot_out_t *o,
                             const char *expr, double t0, double t1, int n,
                             bool fit_only, double *min_out, double *max_out)
{
    bool have_prev = false;
    int psx = 0;
    int psy = 0;
    int seen = 0;
    int i;

    if (min_out != NULL) *min_out = 0.0;
    if (max_out != NULL) *max_out = 0.0;
    for (i = 0; i < n; i++) {
        double t = (n > 1) ? t0 + (t1 - t0) * (double)i / (double)(n - 1) : t0;
        double r = 0.0;
        double rad;
        double x;
        double y;

        if (!plot_eval(expr, "T", t, &r)) {
            have_prev = false;
            continue;
        }
        seen++;
        rad = t * M_PI / 180.0;
        x = r * cos(rad);
        y = r * sin(rad);
        if (fit_only) {
            if (seen == 1) {
                if (min_out != NULL) *min_out = (x < y) ? x : y;
                if (max_out != NULL) *max_out = (x > y) ? x : y;
            } else {
                if (min_out != NULL) {
                    if (x < *min_out) *min_out = x;
                    if (y < *min_out) *min_out = y;
                }
                if (max_out != NULL) {
                    if (x > *max_out) *max_out = x;
                    if (y > *max_out) *max_out = y;
                }
            }
            continue;
        }
        {
            int sx;
            int sy;

            if (!gfx_view_map(v, x, y, &sx, &sy)) {
                have_prev = false;
                continue;
            }
            if (have_prev && abs(sy - psy) <= v->ph) {
                plot_seg(o, psx, psy, sx, sy);
            } else {
                plot_put(o, sx, sy);
            }
            have_prev = true;
            psx = sx;
            psy = sy;
        }
    }
    return seen;
}

/** Parametric x=fx(T), y=fy(T), T in degrees. */
static int plot_sample_para(const gfx_view_t *v, plot_out_t *o,
                            const char *xexpr, const char *yexpr,
                            double t0, double t1, int n, bool fit_only,
                            double *min_out, double *max_out)
{
    bool have_prev = false;
    int psx = 0;
    int psy = 0;
    int seen = 0;
    int i;

    if (min_out != NULL) *min_out = 0.0;
    if (max_out != NULL) *max_out = 0.0;
    for (i = 0; i < n; i++) {
        double t = (n > 1) ? t0 + (t1 - t0) * (double)i / (double)(n - 1) : t0;
        double x = 0.0;
        double y = 0.0;

        if (!plot_eval(xexpr, "T", t, &x) || !plot_eval(yexpr, "T", t, &y)) {
            have_prev = false;
            continue;
        }
        seen++;
        if (fit_only) {
            if (seen == 1) {
                if (min_out != NULL) *min_out = (x < y) ? x : y;
                if (max_out != NULL) *max_out = (x > y) ? x : y;
            } else {
                if (min_out != NULL) {
                    if (x < *min_out) *min_out = x;
                    if (y < *min_out) *min_out = y;
                }
                if (max_out != NULL) {
                    if (x > *max_out) *max_out = x;
                    if (y > *max_out) *max_out = y;
                }
            }
            continue;
        }
        {
            int sx;
            int sy;

            if (!gfx_view_map(v, x, y, &sx, &sy)) {
                have_prev = false;
                continue;
            }
            if (have_prev && abs(sy - psy) <= v->ph) {
                plot_seg(o, psx, psy, sx, sy);
            } else {
                plot_put(o, sx, sy);
            }
            have_prev = true;
            psx = sx;
            psy = sy;
        }
    }
    return seen;
}

/** Fit the stored window's y-range to [lo,hi] with a 5% margin. */
static void plot_fit_y(double lo, double hi)
{
    double pad;

    if (!(lo <= hi)) return;
    if (lo == hi) {
        lo -= 1.0;
        hi += 1.0;
    }
    pad = (hi - lo) * 0.05;
    s_plot_view_win.ymin = lo - pad;
    s_plot_view_win.ymax = hi + pad;
    s_plot_win_set = true;
}

/** Remember the last data source so bare `plot auto` can refit it. */
static void plot_remember(int kind, const char *t1, const char *t2,
                          uint16_t color, int samples, bool dots)
{
    s_plot_last_kind = kind;
    if (t1 != NULL) {
        snprintf(s_plot_last_text, sizeof(s_plot_last_text), "%s", t1);
    } else {
        s_plot_last_text[0] = '\0';
    }
    if (t2 != NULL) {
        snprintf(s_plot_last_text2, sizeof(s_plot_last_text2), "%s", t2);
    } else {
        s_plot_last_text2[0] = '\0';
    }
    s_plot_last_color = color;
    s_plot_last_samples = samples;
    s_plot_last_dots = dots;
}

/* ========================================================================
 * DATA FILES (same guarded-SD pattern as `gfx load`)
 * ======================================================================== */

typedef struct {
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *f;
    bool open;
} plot_file_t;

static bool plot_file_open(const char *path, plot_file_t *pf)
{
    if (path == NULL || pf == NULL) return false;
    pf->open = false;
    pf->f = NULL;
    if (shell_fs_resolve_path(path, pf->resolved, sizeof(pf->resolved)) != ESP_OK) {
        return false;
    }
    if (shell_sd_begin(&pf->session) != ESP_OK) {
        return false;
    }
    pf->f = fopen(pf->resolved, "rb");
    if (pf->f == NULL) {
        shell_sd_end(&pf->session, "plot");
        return false;
    }
    pf->open = true;
    return true;
}

static void plot_file_close(plot_file_t *pf)
{
    if (pf == NULL || !pf->open) return;
    fclose(pf->f);
    pf->f = NULL;
    shell_sd_end(&pf->session, "plot");
    pf->open = false;
}

/** Parse one `x,y` / `x y` data line (skips blanks and #/; comments). */
static bool plot_parse_pair(const char *line, double *x, double *y)
{
    const char *p = line;
    char *end = NULL;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#' || *p == ';') {
        return false;
    }
    *x = strtod(p, &end);
    if (end == p) return false;
    p = end;
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (*p == '\0' || *p == '\n' || *p == '\r') return false;
    *y = strtod(p, &end);
    if (end == p || !isfinite(*x) || !isfinite(*y)) return false;
    return true;
}

/* ========================================================================
 * USAGE
 * ======================================================================== */

static void shell_command_plot_usage(void)
{
    shell_transcript_appendf_ansi(
        "Usage: plot tui on|off | window <xmin> <xmax> <ymin> <ymax> [/rect:x:y:w:h]\n"
        "       plot auto [func <expr> | data <file> | bar <src>] [<xmin> <xmax>]\n"
        "       plot axes [color] [/grid] [/ticks:N] | plot func <expr> [color] [/samples:N] [/auto]\n"
        "       plot polar <expr> [color] [/samples:N] [/auto] | plot para <xe> <ye> [color] [/samples:N] [/auto]\n"
        "       plot data <file> [color] [/dots] [/auto] | plot bar <file|v1,v2,..> [color] [/auto]\n"
        "       plot table <expr> /from:<a> /to:<b> /step:<s> | plot line <x1> <y1> <x2> <y2> [color]\n"
        "       plot point <x> <y> [color] | plot clear [color] | plot status\n");
}

/* ========================================================================
 * VERB
 * ======================================================================== */

bool shell_command_plot(int argc, char **argv)
{
    bool tui = false;

    if (argc < 2) {
        shell_command_plot_usage();
        batch_set_errorlevel(2);
        return false;
    }

    /* plot status: read-only state report (no display, no refusal). */
    if (shell_text_equals_ignore_case(argv[1], "status")) {
        gfx_surface_t *s = gfx_canvas_surface();
        char wbuf[96];

        if (s_plot_win_set) {
            snprintf(wbuf, sizeof(wbuf), "window: x=[%.4g, %.4g] y=[%.4g, %.4g]",
                     s_plot_view_win.xmin, s_plot_view_win.xmax,
                     s_plot_view_win.ymin, s_plot_view_win.ymax);
        } else {
            snprintf(wbuf, sizeof(wbuf), "window: (unset, defaults to -10..10)");
        }
        shell_transcript_appendf("plot: target=%s %s\n",
                                 s_plot_tui ? "tui" : "canvas", wbuf);
        if (s != NULL) {
            shell_transcript_appendf("plot: canvas %dx%d open\n", s->w, s->h);
        } else {
            shell_transcript_append_text("plot: no canvas open\n");
        }
        shell_transcript_appendf("plot: tui %s, angle=%s\n",
                                 tui_is_active() ? "active" : "idle",
                                 calc_angle_is_degrees() ? "degrees" : "radians");
        batch_set_errorlevel(0);
        return true;
    }

    /* plot tui on|off: select the render target (state only). */
    if (shell_text_equals_ignore_case(argv[1], "tui")) {
        if (argc != 3 ||
            (!shell_text_equals_ignore_case(argv[2], "on") &&
             !shell_text_equals_ignore_case(argv[2], "off"))) {
            shell_transcript_appendf_ansi(SH_ERR "plot tui: usage: plot tui on|off\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (!draw_require_foreground("plot")) return false;
        s_plot_tui = shell_text_equals_ignore_case(argv[2], "on");
        shell_transcript_appendf("plot: target=%s\n", s_plot_tui ? "tui" : "canvas");
        batch_set_errorlevel(0);
        return true;
    }

    /* plot clear [color]: wipe the active surface. */
    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        uint8_t dos = 0;
        uint16_t rgb = 0x0000;

        if (argc > 3) {
            shell_transcript_appendf_ansi(SH_ERR "plot clear: usage: plot clear [color]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (!plot_require_target(&tui)) return false;
        plot_color_pair((argc > 2) ? argv[2] : NULL, 0, &dos, &rgb);
        if (tui) {
            tui_clear();
            draw_maybe_flush();
        } else {
            gfx_surface_t *s = gfx_canvas_surface();

            if (s != NULL) gfx_surface_clear(s, rgb);
        }
        batch_set_errorlevel(0);
        return true;
    }

    /* plot window <xmin> <xmax> <ymin> <ymax> [/rect:x:y:w:h] */
    if (shell_text_equals_ignore_case(argv[1], "window")) {
        double xmin;
        double xmax;
        double ymin;
        double ymax;
        int i;

        if (argc < 6) {
            shell_transcript_appendf_ansi(SH_ERR "plot window: usage: plot window <xmin> <xmax> <ymin> <ymax> [/rect:x:y:w:h]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (!draw_require_foreground("plot")) return false;
        if (!plot_parse_double(argv[2], &xmin) ||
            !plot_parse_double(argv[3], &xmax) ||
            !plot_parse_double(argv[4], &ymin) ||
            !plot_parse_double(argv[5], &ymax) ||
            !(xmin < xmax) || !(ymin < ymax)) {
            shell_transcript_appendf_ansi(SH_ERR "plot window: need xmin<xmax and ymin<ymax\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        s_plot_view_win.xmin = xmin;
        s_plot_view_win.xmax = xmax;
        s_plot_view_win.ymin = ymin;
        s_plot_view_win.ymax = ymax;
        s_plot_win_set = true;
        for (i = 6; i < argc; i++) {
            int rx;
            int ry;
            int rw;
            int rh;

            if (strncasecmp(argv[i], "/rect:", 6) != 0 ||
                sscanf(argv[i] + 6, "%d:%d:%d:%d", &rx, &ry, &rw, &rh) != 4 ||
                rw <= 0 || rh <= 0) {
                shell_transcript_appendf_ansi(SH_ERR "plot window: bad option '%s' (want /rect:x:y:w:h)\n" SH_RST,
                                              argv[i]);
                batch_set_errorlevel(2);
                return false;
            }
            if (s_plot_tui) {
                s_plot_rect_t[0] = rx;
                s_plot_rect_t[1] = ry;
                s_plot_rect_t[2] = rw;
                s_plot_rect_t[3] = rh;
                s_plot_rect_t_set = true;
            } else {
                s_plot_rect_c[0] = rx;
                s_plot_rect_c[1] = ry;
                s_plot_rect_c[2] = rw;
                s_plot_rect_c[3] = rh;
                s_plot_rect_c_set = true;
            }
        }
        shell_transcript_appendf("plot: window x=[%.4g, %.4g] y=[%.4g, %.4g]\n",
                                 xmin, xmax, ymin, ymax);
        batch_set_errorlevel(0);
        return true;
    }

    /* plot axes [color] [/grid] [/ticks:N] */
    if (shell_text_equals_ignore_case(argv[1], "axes")) {
        gfx_view_t v;
        plot_out_t o;
        uint8_t dos = 0;
        uint16_t rgb = 0;
        bool grid = false;
        int ticks = P4_CONFIG_PLOT_TICK_TARGET;
        const char *color_tok = NULL;
        int i;

        for (i = 2; i < argc; i++) {
            if (strncasecmp(argv[i], "/grid", 5) == 0 && (argv[i][5] == '\0')) {
                grid = true;
            } else if (strncasecmp(argv[i], "/ticks:", 7) == 0) {
                if (!plot_parse_int(argv[i] + 7, &ticks) || ticks < 2 || ticks > 64) {
                    shell_transcript_appendf_ansi(SH_ERR "plot axes: bad /ticks:N (2..64)\n" SH_RST);
                    batch_set_errorlevel(2);
                    return false;
                }
            } else if (argv[i][0] == '/') {
                shell_transcript_appendf_ansi(SH_ERR "plot axes: unknown option '%s'\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return false;
            } else if (color_tok == NULL) {
                color_tok = argv[i];
            } else {
                shell_transcript_appendf_ansi(SH_ERR "plot axes: usage: plot axes [color] [/grid] [/ticks:N]\n" SH_RST);
                batch_set_errorlevel(2);
                return false;
            }
        }
        if (!plot_require_target(&tui)) return false;
        plot_color_pair(color_tok, 8, &dos, &rgb);
        plot_build_view(&v, tui);
        plot_out_init(&o, tui, rgb, dos, '+');
        if (grid) plot_draw_grid(&v, &o, ticks);
        plot_draw_axes(&v, &o, ticks);
        plot_flush_target(tui);
        batch_set_errorlevel(0);
        return true;
    }

    /* plot func <expr> [color] [/samples:N] [/auto] */
    if (shell_text_equals_ignore_case(argv[1], "func")) {
        gfx_view_t v;
        plot_out_t o;
        uint8_t dos = 0;
        uint16_t rgb = 0;
        const char *color_tok = NULL;
        int samples = 0;
        bool fit = false;
        const char *expr;
        int i;
        int seen;
        double lo = 0.0;
        double hi = 0.0;

        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "plot func: usage: plot func <expr> [color] [/samples:N] [/auto]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        expr = argv[2];
        for (i = 3; i < argc; i++) {
            if (strncasecmp(argv[i], "/samples:", 9) == 0) {
                if (!plot_parse_int(argv[i] + 9, &samples) || samples < 2 ||
                    samples > P4_CONFIG_PLOT_SAMPLES) {
                    shell_transcript_appendf_ansi(SH_ERR "plot func: bad /samples:N (2..%d)\n" SH_RST,
                                                  P4_CONFIG_PLOT_SAMPLES);
                    batch_set_errorlevel(2);
                    return false;
                }
            } else if (strcasecmp(argv[i], "/auto") == 0) {
                fit = true;
            } else if (argv[i][0] == '/') {
                shell_transcript_appendf_ansi(SH_ERR "plot func: unknown option '%s'\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return false;
            } else if (color_tok == NULL) {
                color_tok = argv[i];
            } else {
                shell_transcript_appendf_ansi(SH_ERR "plot func: usage: plot func <expr> [color] [/samples:N] [/auto]\n" SH_RST);
                batch_set_errorlevel(2);
                return false;
            }
        }
        if (!plot_require_target(&tui)) return false;
        plot_color_pair(color_tok, 15, &dos, &rgb);
        plot_build_view(&v, tui);
        if (samples <= 0) {
            samples = v.pw;
            if (samples < 2) samples = 2;
            if (samples > P4_CONFIG_PLOT_SAMPLES) samples = P4_CONFIG_PLOT_SAMPLES;
        }
        if (fit) {
            plot_out_t dummy;

            plot_out_init(&dummy, tui, rgb, dos, '*');
            seen = plot_sample_func(&v, &dummy, expr, "X", v.xmin, v.xmax,
                                    samples, true, &lo, &hi);
            if (seen == 0) {
                shell_transcript_appendf_ansi(SH_ERR "plot func: no finite values to fit\n" SH_RST);
                batch_set_errorlevel(1);
                return false;
            }
            plot_fit_y(lo, hi);
            plot_build_view(&v, tui);
        }
        plot_out_init(&o, tui, rgb, dos, '*');
        seen = plot_sample_func(&v, &o, expr, "X", v.xmin, v.xmax,
                                samples, false, NULL, NULL);
        if (seen == 0) {
            shell_transcript_appendf_ansi(SH_ERR "plot func: expression has no plottable values\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        plot_remember(PLOT_LAST_FUNC, expr, NULL, rgb, samples, false);
        plot_flush_target(tui);
        batch_set_errorlevel(0);
        return true;
    }

    /* plot polar <expr> [color] [/samples:N] [/auto] (r=f(T), T in degrees) */
    if (shell_text_equals_ignore_case(argv[1], "polar")) {
        gfx_view_t v;
        plot_out_t o;
        uint8_t dos = 0;
        uint16_t rgb = 0;
        const char *color_tok = NULL;
        int samples = 0;
        bool fit = false;
        const char *expr;
        int i;
        int seen;
        double lo = 0.0;
        double hi = 0.0;

        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "plot polar: usage: plot polar <expr> [color] [/samples:N] [/auto]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        expr = argv[2];
        for (i = 3; i < argc; i++) {
            if (strncasecmp(argv[i], "/samples:", 9) == 0) {
                if (!plot_parse_int(argv[i] + 9, &samples) || samples < 2 ||
                    samples > P4_CONFIG_PLOT_SAMPLES) {
                    shell_transcript_appendf_ansi(SH_ERR "plot polar: bad /samples:N (2..%d)\n" SH_RST,
                                                  P4_CONFIG_PLOT_SAMPLES);
                    batch_set_errorlevel(2);
                    return false;
                }
            } else if (strcasecmp(argv[i], "/auto") == 0) {
                fit = true;
            } else if (argv[i][0] == '/') {
                shell_transcript_appendf_ansi(SH_ERR "plot polar: unknown option '%s'\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return false;
            } else if (color_tok == NULL) {
                color_tok = argv[i];
            } else {
                shell_transcript_appendf_ansi(SH_ERR "plot polar: usage: plot polar <expr> [color] [/samples:N] [/auto]\n" SH_RST);
                batch_set_errorlevel(2);
                return false;
            }
        }
        if (!plot_require_target(&tui)) return false;
        plot_color_pair(color_tok, 11, &dos, &rgb);
        plot_build_view(&v, tui);
        if (samples <= 0) {
            samples = v.pw;
            if (samples < 2) samples = 2;
            if (samples > P4_CONFIG_PLOT_SAMPLES) samples = P4_CONFIG_PLOT_SAMPLES;
        }
        if (fit) {
            plot_out_t dummy;

            plot_out_init(&dummy, tui, rgb, dos, '*');
            seen = plot_sample_polar(&v, &dummy, expr, 0.0, 360.0,
                                     samples, true, &lo, &hi);
            if (seen == 0) {
                shell_transcript_appendf_ansi(SH_ERR "plot polar: no finite values to fit\n" SH_RST);
                batch_set_errorlevel(1);
                return false;
            }
            plot_fit_y(lo, hi);
            plot_build_view(&v, tui);
        }
        plot_out_init(&o, tui, rgb, dos, '*');
        seen = plot_sample_polar(&v, &o, expr, 0.0, 360.0,
                                 samples, false, NULL, NULL);
        if (seen == 0) {
            shell_transcript_appendf_ansi(SH_ERR "plot polar: expression has no plottable values\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        plot_remember(PLOT_LAST_POLAR, expr, NULL, rgb, samples, false);
        plot_flush_target(tui);
        batch_set_errorlevel(0);
        return true;
    }

    /* plot para <xexpr> <yexpr> [color] [/samples:N] [/auto] (T in degrees) */
    if (shell_text_equals_ignore_case(argv[1], "para")) {
        gfx_view_t v;
        plot_out_t o;
        uint8_t dos = 0;
        uint16_t rgb = 0;
        const char *color_tok = NULL;
        int samples = 0;
        bool fit = false;
        const char *xexpr;
        const char *yexpr;
        int i;
        int seen;
        double lo = 0.0;
        double hi = 0.0;

        if (argc < 4) {
            shell_transcript_appendf_ansi(SH_ERR "plot para: usage: plot para <xexpr> <yexpr> [color] [/samples:N] [/auto]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        xexpr = argv[2];
        yexpr = argv[3];
        for (i = 4; i < argc; i++) {
            if (strncasecmp(argv[i], "/samples:", 9) == 0) {
                if (!plot_parse_int(argv[i] + 9, &samples) || samples < 2 ||
                    samples > P4_CONFIG_PLOT_SAMPLES) {
                    shell_transcript_appendf_ansi(SH_ERR "plot para: bad /samples:N (2..%d)\n" SH_RST,
                                                  P4_CONFIG_PLOT_SAMPLES);
                    batch_set_errorlevel(2);
                    return false;
                }
            } else if (strcasecmp(argv[i], "/auto") == 0) {
                fit = true;
            } else if (argv[i][0] == '/') {
                shell_transcript_appendf_ansi(SH_ERR "plot para: unknown option '%s'\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return false;
            } else if (color_tok == NULL) {
                color_tok = argv[i];
            } else {
                shell_transcript_appendf_ansi(SH_ERR "plot para: usage: plot para <xexpr> <yexpr> [color] [/samples:N] [/auto]\n" SH_RST);
                batch_set_errorlevel(2);
                return false;
            }
        }
        if (!plot_require_target(&tui)) return false;
        plot_color_pair(color_tok, 13, &dos, &rgb);
        plot_build_view(&v, tui);
        if (samples <= 0) {
            samples = v.pw;
            if (samples < 2) samples = 2;
            if (samples > P4_CONFIG_PLOT_SAMPLES) samples = P4_CONFIG_PLOT_SAMPLES;
        }
        if (fit) {
            plot_out_t dummy;

            plot_out_init(&dummy, tui, rgb, dos, '*');
            seen = plot_sample_para(&v, &dummy, xexpr, yexpr, 0.0, 360.0,
                                    samples, true, &lo, &hi);
            if (seen == 0) {
                shell_transcript_appendf_ansi(SH_ERR "plot para: no finite values to fit\n" SH_RST);
                batch_set_errorlevel(1);
                return false;
            }
            plot_fit_y(lo, hi);
            plot_build_view(&v, tui);
        }
        plot_out_init(&o, tui, rgb, dos, '*');
        seen = plot_sample_para(&v, &o, xexpr, yexpr, 0.0, 360.0,
                                samples, false, NULL, NULL);
        if (seen == 0) {
            shell_transcript_appendf_ansi(SH_ERR "plot para: expressions have no plottable values\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        plot_remember(PLOT_LAST_PARA, xexpr, yexpr, rgb, samples, false);
        plot_flush_target(tui);
        batch_set_errorlevel(0);
        return true;
    }

    /* plot data <file> [color] [/dots] [/auto] */
    if (shell_text_equals_ignore_case(argv[1], "data")) {
        gfx_view_t v;
        plot_out_t o;
        uint8_t dos = 0;
        uint16_t rgb = 0;
        const char *color_tok = NULL;
        bool dots = false;
        bool fit = false;
        const char *path;
        int i;
        plot_file_t pf;
        char line[P4_CONFIG_PLOT_LINE_BYTES];
        bool have_prev = false;
        int psx = 0;
        int psy = 0;
        int seen = 0;
        int total = 0;
        double lo = 0.0;
        double hi = 0.0;
        bool have_range = false;

        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "plot data: usage: plot data <file> [color] [/dots] [/auto]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        path = argv[2];
        for (i = 3; i < argc; i++) {
            if (strcasecmp(argv[i], "/dots") == 0) {
                dots = true;
            } else if (strcasecmp(argv[i], "/auto") == 0) {
                fit = true;
            } else if (argv[i][0] == '/') {
                shell_transcript_appendf_ansi(SH_ERR "plot data: unknown option '%s'\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return false;
            } else if (color_tok == NULL) {
                color_tok = argv[i];
            } else {
                shell_transcript_appendf_ansi(SH_ERR "plot data: usage: plot data <file> [color] [/dots] [/auto]\n" SH_RST);
                batch_set_errorlevel(2);
                return false;
            }
        }
        if (!plot_require_target(&tui)) return false;
        if (!plot_file_open(path, &pf)) {
            shell_transcript_appendf_ansi(SH_ERR "plot data: cannot open %s\n" SH_RST, path);
            batch_set_errorlevel(1);
            return false;
        }
        plot_color_pair(color_tok, 14, &dos, &rgb);
        plot_build_view(&v, tui);
        plot_out_init(&o, tui, rgb, dos, 'o');
        while (fgets(line, sizeof(line), pf.f) != NULL) {
            double x = 0.0;
            double y = 0.0;
            int sx;
            int sy;

            if (!plot_parse_pair(line, &x, &y)) continue;
            if (total >= P4_CONFIG_PLOT_MAX_POINTS) continue;
            total++;
            if (!have_range) {
                lo = (x < y) ? x : y;
                hi = (x > y) ? x : y;
                have_range = true;
            } else {
                if (x < lo) lo = x;
                if (y < lo) lo = y;
                if (x > hi) hi = x;
                if (y > hi) hi = y;
            }
            if (fit) continue;
            if (!gfx_view_map(&v, x, y, &sx, &sy)) {
                have_prev = false;
                continue;
            }
            seen++;
            if (dots || !have_prev) {
                plot_put(&o, sx, sy);
            } else if (abs(sy - psy) <= v.ph) {
                plot_seg(&o, psx, psy, sx, sy);
            } else {
                plot_put(&o, sx, sy);
            }
            have_prev = true;
            psx = sx;
            psy = sy;
        }
        plot_file_close(&pf);
        if (fit) {
            if (!have_range) {
                shell_transcript_appendf_ansi(SH_ERR "plot data: no plottable pairs in %s\n" SH_RST, path);
                batch_set_errorlevel(1);
                return false;
            }
            plot_fit_y(lo, hi);
            plot_build_view(&v, tui);
            /* Second pass draws with the fitted window. */
            if (!plot_file_open(path, &pf)) {
                shell_transcript_appendf_ansi(SH_ERR "plot data: cannot reopen %s\n" SH_RST, path);
                batch_set_errorlevel(1);
                return false;
            }
            total = 0;
            have_prev = false;
            while (fgets(line, sizeof(line), pf.f) != NULL) {
                double x = 0.0;
                double y = 0.0;
                int sx;
                int sy;

                if (!plot_parse_pair(line, &x, &y)) continue;
                if (total >= P4_CONFIG_PLOT_MAX_POINTS) continue;
                total++;
                if (!gfx_view_map(&v, x, y, &sx, &sy)) {
                    have_prev = false;
                    continue;
                }
                seen++;
                if (dots || !have_prev) {
                    plot_put(&o, sx, sy);
                } else if (abs(sy - psy) <= v.ph) {
                    plot_seg(&o, psx, psy, sx, sy);
                } else {
                    plot_put(&o, sx, sy);
                }
                have_prev = true;
                psx = sx;
                psy = sy;
            }
            plot_file_close(&pf);
        }
        if (total == 0) {
            shell_transcript_appendf_ansi(SH_ERR "plot data: no plottable pairs in %s\n" SH_RST, path);
            batch_set_errorlevel(1);
            return false;
        }
        if (total >= P4_CONFIG_PLOT_MAX_POINTS) {
            shell_transcript_appendf("plot: note: truncated at %d points\n", P4_CONFIG_PLOT_MAX_POINTS);
        }
        plot_remember(PLOT_LAST_DATA, path, NULL, rgb, 0, dots);
        plot_flush_target(tui);
        batch_set_errorlevel(0);
        return true;
    }

    /* plot bar <file|v1,v2,..> [color] [/auto] */
    if (shell_text_equals_ignore_case(argv[1], "bar")) {
        gfx_view_t v;
        plot_out_t o;
        uint8_t dos = 0;
        uint16_t rgb = 0;
        const char *color_tok = NULL;
        bool fit = false;
        const char *src;
        int i;
        double *vals = NULL;
        int n = 0;
        int cap = 0;
        double lo = 0.0;
        double hi = 0.0;

        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "plot bar: usage: plot bar <file|v1,v2,..> [color] [/auto]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        src = argv[2];
        for (i = 3; i < argc; i++) {
            if (strcasecmp(argv[i], "/auto") == 0) {
                fit = true;
            } else if (argv[i][0] == '/') {
                shell_transcript_appendf_ansi(SH_ERR "plot bar: unknown option '%s'\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return false;
            } else if (color_tok == NULL) {
                color_tok = argv[i];
            } else {
                shell_transcript_appendf_ansi(SH_ERR "plot bar: usage: plot bar <file|v1,v2,..> [color] [/auto]\n" SH_RST);
                batch_set_errorlevel(2);
                return false;
            }
        }
        if (!plot_require_target(&tui)) return false;
        cap = P4_CONFIG_PLOT_MAX_POINTS;
        vals = heap_caps_malloc((size_t)cap * sizeof(*vals), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (vals == NULL) {
            vals = malloc((size_t)cap * sizeof(*vals));
        }
        if (vals == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "plot bar: out of memory\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        if (strchr(src, ',') != NULL) {
            /* Inline comma list. */
            const char *p = src;

            while (*p != '\0' && n < cap) {
                char *end = NULL;
                double d;

                while (*p == ' ' || *p == '\t') p++;
                if (*p == '\0') break;
                d = strtod(p, &end);
                if (end == p || !isfinite(d)) break;
                vals[n++] = d;
                p = end;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == ',') p++;
                else if (*p != '\0') break;
            }
        } else {
            /* One value per line. */
            plot_file_t pf;
            char line[P4_CONFIG_PLOT_LINE_BYTES];

            if (!plot_file_open(src, &pf)) {
                shell_transcript_appendf_ansi(SH_ERR "plot bar: cannot open %s\n" SH_RST, src);
                heap_caps_free(vals);
                batch_set_errorlevel(1);
                return false;
            }
            while (n < cap && fgets(line, sizeof(line), pf.f) != NULL) {
                const char *p = line;
                char *end = NULL;
                double d;

                while (*p == ' ' || *p == '\t') p++;
                if (*p == '\0' || *p == '\n' || *p == '\r' ||
                    *p == '#' || *p == ';') {
                    continue;
                }
                d = strtod(p, &end);
                if (end == p || !isfinite(d)) continue;
                vals[n++] = d;
            }
            plot_file_close(&pf);
        }
        if (n == 0) {
            shell_transcript_appendf_ansi(SH_ERR "plot bar: no values in '%s'\n" SH_RST, src);
            heap_caps_free(vals);
            batch_set_errorlevel(1);
            return false;
        }
        lo = 0.0;
        hi = 0.0;
        for (i = 0; i < n; i++) {
            if (vals[i] < lo) lo = vals[i];
            if (vals[i] > hi) hi = vals[i];
        }
        plot_color_pair(color_tok, 10, &dos, &rgb);
        plot_build_view(&v, tui);
        if (fit) {
            plot_fit_y(lo, hi);
            plot_build_view(&v, tui);
        }
        plot_out_init(&o, tui, rgb, dos, '#');
        {
            /* One vertical span per raster column: the column's world-x picks
             * its bar, so bars stay exact at any value count. Canvas segments
             * clip via gfx_view_line; TUI rows clamp to the rect. */
            double y0 = (0.0 >= v.ymin && 0.0 <= v.ymax) ? 0.0 : v.ymin;
            double xspan = v.xmax - v.xmin;
            int cx;

            for (cx = v.px; cx < v.px + v.pw; cx++) {
                double bx;
                int idx;

                if (!(xspan > 0.0)) break;
                bx = v.xmin + ((double)(cx - v.px) + 0.5) / (double)v.pw * xspan;
                idx = (int)((bx - v.xmin) / xspan * (double)n);
                if (idx < 0 || idx >= n) continue;
                if (!o.tui) {
                    gfx_view_line(&v, o.surf, bx, y0, bx, vals[idx], o.color);
                } else {
                    int r0 = plot_row_clamped(&v, y0);
                    int r1 = plot_row_clamped(&v, vals[idx]);
                    int rt = (r0 < r1) ? r0 : r1;

                    tui_fill(cx, rt, 1, abs(r1 - r0) + 1, '#', o.fg, 16);
                }
            }
        }
        heap_caps_free(vals);
        if (n >= cap) {
            shell_transcript_appendf("plot: note: truncated at %d values\n", cap);
        }
        plot_remember(PLOT_LAST_BAR, src, NULL, rgb, 0, false);
        plot_flush_target(tui);
        batch_set_errorlevel(0);
        return true;
    }

    /* plot table <expr> /from:<a> /to:<b> /step:<s> */
    if (shell_text_equals_ignore_case(argv[1], "table")) {
        double from = 0.0;
        double to = 0.0;
        double step = 0.0;
        bool have_from = false;
        bool have_to = false;
        bool have_step = false;
        const char *expr;
        int i;
        int rows = 0;
        char numbuf[48];

        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "plot table: usage: plot table <expr> /from:<a> /to:<b> /step:<s>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        expr = argv[2];
        for (i = 3; i < argc; i++) {
            if (strncasecmp(argv[i], "/from:", 6) == 0) {
                if (!plot_parse_double(argv[i] + 6, &from)) {
                    shell_transcript_appendf_ansi(SH_ERR "plot table: bad /from: value\n" SH_RST);
                    batch_set_errorlevel(2);
                    return false;
                }
                have_from = true;
            } else if (strncasecmp(argv[i], "/to:", 4) == 0) {
                if (!plot_parse_double(argv[i] + 4, &to)) {
                    shell_transcript_appendf_ansi(SH_ERR "plot table: bad /to: value\n" SH_RST);
                    batch_set_errorlevel(2);
                    return false;
                }
                have_to = true;
            } else if (strncasecmp(argv[i], "/step:", 6) == 0) {
                if (!plot_parse_double(argv[i] + 6, &step) || !(step > 0.0)) {
                    shell_transcript_appendf_ansi(SH_ERR "plot table: bad /step: value\n" SH_RST);
                    batch_set_errorlevel(2);
                    return false;
                }
                have_step = true;
            } else {
                shell_transcript_appendf_ansi(SH_ERR "plot table: unknown option '%s'\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return false;
            }
        }
        if (!have_from || !have_to || !have_step || !(from <= to)) {
            shell_transcript_appendf_ansi(SH_ERR "plot table: need /from: /to: /step: with from<=to and step>0\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        /* Text-only verb: no display, so background jobs may use it. */
        for (i = 0; i < P4_CONFIG_PLOT_MAX_POINTS; i++) {
            double x = from + (double)i * step;
            double y = 0.0;
            char ybuf[48];

            if (x > to) break;
            if (!plot_eval(expr, "X", x, &y)) continue;
            calc_format_number(x, numbuf, sizeof(numbuf));
            calc_format_number(y, ybuf, sizeof(ybuf));
            shell_transcript_appendf("plot.table: %s %s\n", numbuf, ybuf);
            rows++;
        }
        if (rows == 0) {
            shell_transcript_appendf_ansi(SH_ERR "plot table: expression has no values in range\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    /* plot auto [func <expr> | data <file> | bar <src>] [<xmin> <xmax>] */
    if (shell_text_equals_ignore_case(argv[1], "auto")) {
        gfx_view_t v;
        plot_out_t dummy;
        int kind = PLOT_LAST_NONE;
        const char *t1 = NULL;
        int samples = 0;
        int i = 2;
        bool have_x = false;
        double nxmin = 0.0;
        double nxmax = 0.0;
        double lo = 0.0;
        double hi = 0.0;
        int seen = 0;

        /* Optional source: auto [func|data|bar] <text...> then optional x pair. */
        if (i < argc && (shell_text_equals_ignore_case(argv[i], "func") ||
                         shell_text_equals_ignore_case(argv[i], "data") ||
                         shell_text_equals_ignore_case(argv[i], "bar"))) {
            if (shell_text_equals_ignore_case(argv[i], "func")) kind = PLOT_LAST_FUNC;
            else if (shell_text_equals_ignore_case(argv[i], "data")) kind = PLOT_LAST_DATA;
            else kind = PLOT_LAST_BAR;
            i++;
            if (i >= argc) {
                shell_transcript_appendf_ansi(SH_ERR "plot auto: usage: plot auto [func <expr> | data <file> | bar <src>] [<xmin> <xmax>]\n" SH_RST);
                batch_set_errorlevel(2);
                return false;
            }
            t1 = argv[i++];
            if (kind == PLOT_LAST_FUNC && i < argc && strncasecmp(argv[i], "/samples:", 9) == 0) {
                if (!plot_parse_int(argv[i] + 9, &samples) || samples < 2 ||
                    samples > P4_CONFIG_PLOT_SAMPLES) {
                    shell_transcript_appendf_ansi(SH_ERR "plot auto: bad /samples:N (2..%d)\n" SH_RST,
                                                  P4_CONFIG_PLOT_SAMPLES);
                    batch_set_errorlevel(2);
                    return false;
                }
                i++;
            }
        }
        /* Optional trailing xmin xmax. */
        if (i < argc) {
            if (i + 2 != argc) {
                shell_transcript_appendf_ansi(SH_ERR "plot auto: usage: plot auto [func <expr> | data <file> | bar <src>] [<xmin> <xmax>]\n" SH_RST);
                batch_set_errorlevel(2);
                return false;
            }
            if (!plot_parse_double(argv[i], &nxmin) ||
                !plot_parse_double(argv[i + 1], &nxmax) || !(nxmin < nxmax)) {
                shell_transcript_appendf_ansi(SH_ERR "plot auto: need xmin<xmax\n" SH_RST);
                batch_set_errorlevel(2);
                return false;
            }
            have_x = true;
        }
        if (kind == PLOT_LAST_NONE) {
            if (s_plot_last_kind == PLOT_LAST_NONE) {
                shell_transcript_appendf_ansi(SH_ERR "plot auto: nothing plotted yet (give a source)\n" SH_RST);
                batch_set_errorlevel(1);
                return false;
            }
            kind = s_plot_last_kind;
            t1 = s_plot_last_text;
            samples = s_plot_last_samples;
        }
        if (!draw_require_foreground("plot")) return false;
        /* Build a scratch view over the explicit or current x-range. */
        {
            double xmin = -10.0;
            double xmax = 10.0;

            if (have_x) {
                xmin = nxmin;
                xmax = nxmax;
            } else if (s_plot_win_set) {
                xmin = s_plot_view_win.xmin;
                xmax = s_plot_view_win.xmax;
            }
            gfx_view_set(&v, xmin, xmax, -1.0, 1.0, 0, 0, 64, 64);
        }
        if (samples <= 0) {
            samples = 64;
            if (samples > P4_CONFIG_PLOT_SAMPLES) samples = P4_CONFIG_PLOT_SAMPLES;
        }
        plot_out_init(&dummy, false, 0, 0, '*');
        if (kind == PLOT_LAST_FUNC) {
            seen = plot_sample_func(&v, &dummy, t1, "X", v.xmin, v.xmax,
                                    samples, true, &lo, &hi);
        } else if (kind == PLOT_LAST_DATA || kind == PLOT_LAST_BAR) {
            plot_file_t pf;
            char line[P4_CONFIG_PLOT_LINE_BYTES];
            int total = 0;
            bool have_range = false;

            if (!plot_file_open(t1, &pf)) {
                shell_transcript_appendf_ansi(SH_ERR "plot auto: cannot open %s\n" SH_RST, t1);
                batch_set_errorlevel(1);
                return false;
            }
            while (total < P4_CONFIG_PLOT_MAX_POINTS &&
                   fgets(line, sizeof(line), pf.f) != NULL) {
                double x = 0.0;
                double y = 0.0;
                const char *p = line;

                if (kind == PLOT_LAST_BAR && strchr(t1, ',') != NULL) {
                    break; /* inline lists refit below */
                }
                if (kind == PLOT_LAST_DATA) {
                    if (!plot_parse_pair(line, &x, &y)) continue;
                } else {
                    char *end = NULL;

                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == '\0' || *p == '\n' || *p == '\r' ||
                        *p == '#' || *p == ';') {
                        continue;
                    }
                    y = strtod(p, &end);
                    if (end == p || !isfinite(y)) continue;
                    x = y;
                }
                total++;
                if (!have_range) {
                    lo = (x < y) ? x : y;
                    hi = (x > y) ? x : y;
                    have_range = true;
                } else {
                    if (x < lo) lo = x;
                    if (y < lo) lo = y;
                    if (x > hi) hi = x;
                    if (y > hi) hi = y;
                }
            }
            plot_file_close(&pf);
            if (kind == PLOT_LAST_BAR && strchr(t1, ',') != NULL) {
                const char *p = t1;

                total = 0;
                have_range = false;
                while (*p != '\0' && total < P4_CONFIG_PLOT_MAX_POINTS) {
                    char *end = NULL;
                    double d;

                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == '\0') break;
                    d = strtod(p, &end);
                    if (end == p || !isfinite(d)) break;
                    total++;
                    if (!have_range) {
                        lo = (d < 0.0) ? d : 0.0;
                        hi = (d > 0.0) ? d : 0.0;
                        have_range = true;
                    } else {
                        if (d < lo) lo = d;
                        if (d > hi) hi = d;
                    }
                    p = end;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == ',') p++;
                    else if (*p != '\0') break;
                }
            }
            if (kind == PLOT_LAST_BAR && have_range) {
                if (lo > 0.0) lo = 0.0;
                if (hi < 0.0) hi = 0.0;
            }
            seen = total;
        } else {
            shell_transcript_appendf_ansi(SH_ERR "plot auto: polar/para fit via /auto on the verb\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (seen == 0 || !(lo <= hi)) {
            shell_transcript_appendf_ansi(SH_ERR "plot auto: no values to fit\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        if (have_x) {
            s_plot_view_win.xmin = nxmin;
            s_plot_view_win.xmax = nxmax;
        } else if (!s_plot_win_set) {
            s_plot_view_win.xmin = v.xmin;
            s_plot_view_win.xmax = v.xmax;
        }
        plot_fit_y(lo, hi);
        shell_transcript_appendf("plot: window x=[%.4g, %.4g] y=[%.4g, %.4g]\n",
                                 s_plot_view_win.xmin, s_plot_view_win.xmax,
                                 s_plot_view_win.ymin, s_plot_view_win.ymax);
        batch_set_errorlevel(0);
        return true;
    }

    /* plot line <x1> <y1> <x2> <y2> [color] */
    if (shell_text_equals_ignore_case(argv[1], "line")) {
        gfx_view_t v;
        plot_out_t o;
        uint8_t dos = 0;
        uint16_t rgb = 0;
        double x1;
        double y1;
        double x2;
        double y2;
        int ax;
        int ay;
        int bx;
        int by;
        bool tui = false;

        if (argc != 6 && argc != 7) {
            shell_transcript_appendf_ansi(SH_ERR "plot line: usage: plot line <x1> <y1> <x2> <y2> [color]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (!plot_parse_double(argv[2], &x1) ||
            !plot_parse_double(argv[3], &y1) ||
            !plot_parse_double(argv[4], &x2) ||
            !plot_parse_double(argv[5], &y2)) {
            shell_transcript_appendf_ansi(SH_ERR "plot line: need numeric coordinates\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (!plot_require_target(&tui)) return false;
        plot_color_pair((argc > 6) ? argv[6] : NULL, 15, &dos, &rgb);
        plot_build_view(&v, tui);
        if (!gfx_view_clip_line(&v, x1, y1, x2, y2, &ax, &ay, &bx, &by)) {
            shell_transcript_appendf("plot: line is outside the window\n");
            batch_set_errorlevel(0);
            return true;
        }
        plot_out_init(&o, tui, rgb, dos, '*');
        plot_seg(&o, ax, ay, bx, by);
        plot_flush_target(tui);
        batch_set_errorlevel(0);
        return true;
    }

    /* plot point <x> <y> [color] */
    if (shell_text_equals_ignore_case(argv[1], "point")) {
        gfx_view_t v;
        plot_out_t o;
        uint8_t dos = 0;
        uint16_t rgb = 0;
        double x;
        double y;
        int sx;
        int sy;
        bool tui = false;

        if (argc != 4 && argc != 5) {
            shell_transcript_appendf_ansi(SH_ERR "plot point: usage: plot point <x> <y> [color]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (!plot_parse_double(argv[2], &x) || !plot_parse_double(argv[3], &y)) {
            shell_transcript_appendf_ansi(SH_ERR "plot point: need numeric coordinates\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (!plot_require_target(&tui)) return false;
        plot_color_pair((argc > 4) ? argv[4] : NULL, 15, &dos, &rgb);
        plot_build_view(&v, tui);
        if (!gfx_view_map(&v, x, y, &sx, &sy)) {
            shell_transcript_appendf("plot: point is outside the window\n");
            batch_set_errorlevel(0);
            return true;
        }
        plot_out_init(&o, tui, rgb, dos, '*');
        plot_put(&o, sx, sy);
        plot_flush_target(tui);
        batch_set_errorlevel(0);
        return true;
    }

    shell_transcript_appendf_ansi(SH_ERR "plot: unknown subcommand '%s'\n" SH_RST, argv[1]);
    shell_command_plot_usage();
    batch_set_errorlevel(2);
    return false;
}

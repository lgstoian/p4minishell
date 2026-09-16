/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file gfx_view.c
 * @brief World-coordinate viewport for P4MiniShell plotting.
 *
 * Pure integer/double math, no LVGL, no I/O: `gfx_view_t` maps a math window
 * (xmin..xmax, ymin..ymax, y up) onto an integer raster rect. The rect is a
 * plain (x, y, w, h) run of integer coordinates, so the same code drives the
 * 0-based `gfx` pixel canvas and the 1-based TUI cell grid — the caller picks
 * the origin convention. Clipping is exact (Cohen-Sutherland on the integer
 * rect), so out-of-window segments never produce giant coordinates.
 * Headless-safe and unit-tested (test_gfx.c view cases).
 */

#include "gfx.h"
#include <math.h>

void gfx_view_set(gfx_view_t *v, double xmin, double xmax, double ymin,
                  double ymax, int px, int py, int pw, int ph)
{
    if (v == NULL) return;
    v->xmin = xmin;
    v->xmax = xmax;
    v->ymin = ymin;
    v->ymax = ymax;
    v->px = px;
    v->py = py;
    v->pw = pw;
    v->ph = ph;
}

/** Round-half-away to int (portable; avoids llround libm edge cases). */
static int gfx_view_round(double d)
{
    return (int)floor(d + 0.5);
}

bool gfx_view_map(const gfx_view_t *v, double x, double y, int *sx, int *sy)
{
    double fx;
    double fy;
    double xspan;
    double yspan;
    int ix;
    int iy;

    if (v == NULL || sx == NULL || sy == NULL) return false;
    xspan = v->xmax - v->xmin;
    yspan = v->ymax - v->ymin;
    if (!(xspan > 0.0) || !(yspan > 0.0)) return false;
    /* A 1-wide/tall raster is degenerate for plotting (no span to map). */
    if (v->pw <= 1 || v->ph <= 1) return false;
    if (!isfinite(x) || !isfinite(y)) return false;
    /* Edges land exactly on the first/last raster index. */
    fx = (x - v->xmin) / xspan * (double)(v->pw - 1);
    fy = (y - v->ymin) / yspan * (double)(v->ph - 1);
    ix = v->px + gfx_view_round(fx);
    /* Math y grows up, raster y grows down. */
    iy = v->py + (v->ph - 1) - gfx_view_round(fy);
    *sx = ix;
    *sy = iy;
    return ix >= v->px && ix < v->px + v->pw &&
           iy >= v->py && iy < v->py + v->ph;
}

void gfx_view_point(const gfx_view_t *v, gfx_surface_t *s, double x, double y,
                    uint16_t color)
{
    int sx;
    int sy;

    if (v == NULL || s == NULL) return;
    if (gfx_view_map(v, x, y, &sx, &sy)) {
        gfx_surface_pixel(s, sx, sy, color);
    }
}

/** Cohen-Sutherland outcode over the view's integer rect (fractional
 * coordinates; the window edges sit exactly on the border indices). */
static int gfx_view_outcode(const gfx_view_t *v, double x, double y)
{
    double fx;
    double fy;
    double xspan = v->xmax - v->xmin;
    double yspan = v->ymax - v->ymin;
    double xden = (double)(v->pw - 1);
    double yden = (double)(v->ph - 1);
    int code = 0;

    fx = (x - v->xmin) / xspan * xden;
    fy = (y - v->ymin) / yspan * yden;
    if (fx < -0.5) code |= 1;
    else if (fx > xden + 0.5) code |= 2;
    if (fy < -0.5) code |= 4;
    else if (fy > yden + 0.5) code |= 8;
    return code;
}

bool gfx_view_clip_line(const gfx_view_t *v, double x1, double y1, double x2,
                        double y2, int *ax, int *ay, int *bx, int *by)
{
    double xspan;
    double yspan;
    int c1;
    int c2;
    int guard = 0;

    if (v == NULL || ax == NULL || ay == NULL || bx == NULL || by == NULL) {
        return false;
    }
    xspan = v->xmax - v->xmin;
    yspan = v->ymax - v->ymin;
    if (!(xspan > 0.0) || !(yspan > 0.0)) return false;
    if (v->pw <= 1 || v->ph <= 1) return false;
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2)) {
        return false;
    }
    /* Clip in fractional-pixel space, then round once at the end. */
    c1 = gfx_view_outcode(v, x1, y1);
    c2 = gfx_view_outcode(v, x2, y2);
    while ((c1 | c2) != 0 && guard++ < 64) {
        double x = 0.0;
        double y = 0.0;
        double dx = x2 - x1;
        double dy = y2 - y1;
        int code;
        double fx;
        double fy;
        double xden = (double)(v->pw - 1);
        double yden = (double)(v->ph - 1);

        /* Trivial reject BEFORE clipping: endpoints sharing an outside
         * region can never cross the window, even though clipping would
         * drag them onto the boundary. */
        if ((c1 & c2) != 0) return false;
        code = (c1 != 0) ? c1 : c2;

        if ((code & 8) != 0) {           /* above: fy = yden+0.5 */
            fy = yden + 0.5;
            y = v->ymin + fy / yden * yspan;
            x = (dy != 0.0) ? x1 + dx * (y - y1) / dy : x1;
        } else if ((code & 4) != 0) {    /* below: fy = -0.5 */
            fy = -0.5;
            y = v->ymin + fy / yden * yspan;
            x = (dy != 0.0) ? x1 + dx * (y - y1) / dy : x1;
        } else if ((code & 2) != 0) {    /* right: fx = xden+0.5 */
            fx = xden + 0.5;
            x = v->xmin + fx / xden * xspan;
            y = (dx != 0.0) ? y1 + dy * (x - x1) / dx : y1;
        } else {                         /* left: fx = -0.5 */
            fx = -0.5;
            x = v->xmin + fx / xden * xspan;
            y = (dx != 0.0) ? y1 + dy * (x - x1) / dx : y1;
        }
        if (code == c1) {
            x1 = x;
            y1 = y;
            c1 = gfx_view_outcode(v, x1, y1);
        } else {
            x2 = x;
            y2 = y;
            c2 = gfx_view_outcode(v, x2, y2);
        }
        if ((c1 & c2) != 0) return false;
    }
    if ((c1 | c2) != 0) return false;
    {
        double xden = (double)(v->pw - 1);
        double yden = (double)(v->ph - 1);
        double fx1 = (x1 - v->xmin) / xspan * xden;
        double fy1 = (y1 - v->ymin) / yspan * yden;
        double fx2 = (x2 - v->xmin) / xspan * xden;
        double fy2 = (y2 - v->ymin) / yspan * yden;
        int rx0 = v->px;
        int ry0 = v->py;
        int rx1 = v->px + v->pw - 1;
        int ry1 = v->py + v->ph - 1;

        *ax = v->px + gfx_view_round(fx1);
        *ay = v->py + (v->ph - 1) - gfx_view_round(fy1);
        *bx = v->px + gfx_view_round(fx2);
        *by = v->py + (v->ph - 1) - gfx_view_round(fy2);
        /* Float-exact boundary hits can round one step outside; nudge them
         * back (sub-pixel, invisible) rather than dropping the segment. */
        if (*ax < rx0) *ax = rx0;
        if (*ax > rx1) *ax = rx1;
        if (*ay < ry0) *ay = ry0;
        if (*ay > ry1) *ay = ry1;
        if (*bx < rx0) *bx = rx0;
        if (*bx > rx1) *bx = rx1;
        if (*by < ry0) *by = ry0;
        if (*by > ry1) *by = ry1;
    }
    return true;
}

bool gfx_view_line(const gfx_view_t *v, gfx_surface_t *s, double x1, double y1,
                   double x2, double y2, uint16_t color)
{
    int ax;
    int ay;
    int bx;
    int by;

    if (v == NULL || s == NULL) return false;
    if (!gfx_view_clip_line(v, x1, y1, x2, y2, &ax, &ay, &bx, &by)) {
        return false;
    }
    gfx_surface_line(s, ax, ay, bx, by, color);
    return true;
}

double gfx_view_nice_step(double range, int ticks)
{
    double raw;
    double mag;
    double norm;

    if (!(range > 0.0) || ticks <= 0) return 1.0;
    raw = range / (double)ticks;
    mag = pow(10.0, floor(log10(raw)));
    norm = raw / mag;
    if (norm < 1.5) return 1.0 * mag;
    if (norm < 3.5) return 2.0 * mag;
    if (norm < 7.5) return 5.0 * mag;
    return 10.0 * mag;
}

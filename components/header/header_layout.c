/**
 * @file header_layout.c
 * @brief Pure layout policy for the top status bar (no LVGL, no I/O).
 */

#include "header_layout.h"

#include <stddef.h>

static void header_layout_apply_levels(header_layout_t *out,
                                       header_level_t status,
                                       header_level_t sys,
                                       const int status_w[HEADER_LEVEL_COUNT],
                                       const int sys_w[HEADER_LEVEL_COUNT])
{
    out->status_level = status;
    out->sys_level = sys;
    out->show_status = true;
    out->show_sys = true;
    out->left_w = status_w[status];
    out->right_w = sys_w[sys];
    /* Separators and the sparkline only make sense in FULL; the battery bar
     * survives SHORT (MIN is value-only). */
    out->show_sep = (sys == HEADER_LEVEL_FULL);
    out->show_cpu_graph = (sys == HEADER_LEVEL_FULL);
    out->show_battery_bar = (sys <= HEADER_LEVEL_SHORT);
}

void header_layout_compute(header_layout_t *out, int avail_w, int gap,
                           const int status_w[HEADER_LEVEL_COUNT],
                           const int sys_w[HEADER_LEVEL_COUNT],
                           int center_min)
{
    static const header_level_t k_combos[][2] = {
        { HEADER_LEVEL_FULL,  HEADER_LEVEL_FULL  },
        { HEADER_LEVEL_FULL,  HEADER_LEVEL_SHORT },
        { HEADER_LEVEL_SHORT, HEADER_LEVEL_SHORT },
        { HEADER_LEVEL_SHORT, HEADER_LEVEL_MIN   },
        { HEADER_LEVEL_MIN,   HEADER_LEVEL_MIN   },
    };
    int i;
    int left;
    int right;

    if (out == NULL) {
        return;
    }
    if (gap < 0) gap = 0;
    if (center_min < 0) center_min = 0;
    if (avail_w < 0) avail_w = 0;

    /* Default to the most compact useful layout; overwritten on a fit. */
    header_layout_apply_levels(out, HEADER_LEVEL_MIN, HEADER_LEVEL_MIN,
                               status_w, sys_w);
    out->smaller_font = false;
    out->show_center = false;
    out->center_w = 0;

    /* Pass 1: keep the notification if the side panels fit around it. */
    for (i = 0; i < (int)(sizeof(k_combos) / sizeof(k_combos[0])); i++) {
        int l = status_w[k_combos[i][0]];
        int r = sys_w[k_combos[i][1]];
        int leftover = avail_w - l - r - 2 * gap;

        if (leftover >= center_min) {
            header_layout_apply_levels(out, k_combos[i][0], k_combos[i][1],
                                       status_w, sys_w);
            out->show_center = (center_min > 0);
            out->center_w = out->show_center ? leftover : 0;
            return;
        }
    }

    /* Pass 2: the notification yields first -- keep the most detailed side
     * panels that fit with no center region. */
    for (i = 0; i < (int)(sizeof(k_combos) / sizeof(k_combos[0])); i++) {
        int l = status_w[k_combos[i][0]];
        int r = sys_w[k_combos[i][1]];

        if (avail_w - l - r - 2 * gap >= 0) {
            header_layout_apply_levels(out, k_combos[i][0], k_combos[i][1],
                                       status_w, sys_w);
            out->show_center = false;
            out->center_w = 0;
            return;
        }
    }

    /* Pass 3: even the minimum side panels do not fit -> ask the caller for a
     * smaller font. The caller re-measures and calls again; this call still
     * resolves to a safe, overlap-free layout (drop a panel only if needed). */
    out->smaller_font = true;
    left = status_w[HEADER_LEVEL_MIN];
    right = sys_w[HEADER_LEVEL_MIN];
    header_layout_apply_levels(out, HEADER_LEVEL_MIN, HEADER_LEVEL_MIN,
                               status_w, sys_w);
    out->show_center = false;
    out->center_w = 0;

    if (left + right + gap <= avail_w) {
        return;
    }
    out->show_sys = false;
    out->right_w = 0;
    out->show_sep = false;
    out->show_cpu_graph = false;
    out->show_battery_bar = false;
    if (left <= avail_w) {
        out->left_w = left;
        return;
    }
    out->show_status = false;
    out->left_w = 0;
}

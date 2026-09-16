/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file header_layout.h
 * @brief Pure, measurement-driven layout policy for the top status bar.
 *
 * The header is composed of a left status panel (WiFi/BT/USB/SD), a center
 * notification region, and a right system panel (MEM/CPU/BAT). This module
 * decides, for a given available width and a set of measured panel widths,
 * how much of each panel to show so nothing overlaps and long notifications
 * cannot push the side panels off-screen.
 *
 * It contains NO LVGL and NO I/O: callers measure real text widths and pass
 * them in, which keeps the policy headless and unit-testable
 * (test/main/test_header.c). header.c owns the widgets.
 *
 * Policy (per the approved design):
 *   - Three levels per panel: FULL -> SHORT (abbreviations) -> MIN (values
 *     only, no separators/graph/bars).
 *   - Try combinations in order (full/full, full/short, short/short,
 *     short/min, min/min). The first that leaves `center_min` for the
 *     notification wins.
 *   - If nothing fits, ask for a smaller font (`smaller_font`); the caller
 *     re-measures and runs again. If it still cannot fit, the notification
 *     yields first (show_center=false), and only as a last resort is a side
 *     panel dropped entirely (show_sys then show_status), so the screen never
 *     overlaps.
 */

#ifndef P4MINISHELL_HEADER_LAYOUT_H
#define P4MINISHELL_HEADER_LAYOUT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Content level for a side panel. */
typedef enum {
    HEADER_LEVEL_FULL = 0, /**< Long labels, separators, graph/bars. */
    HEADER_LEVEL_SHORT,    /**< Abbreviated labels, no graph. */
    HEADER_LEVEL_MIN,      /**< Values only, no separators/graph/bars/icon. */
    HEADER_LEVEL_COUNT
} header_level_t;

/** Resolved layout for one render pass. */
typedef struct {
    header_level_t status_level; /**< Left panel content level. */
    header_level_t sys_level;    /**< Right panel content level. */
    bool show_status;            /**< Left panel visible at all. */
    bool show_sys;               /**< Right panel visible at all. */
    bool show_center;            /**< Notification region visible. */
    bool show_sep;               /**< System-panel separators visible. */
    bool show_cpu_graph;         /**< CPU sparkline instead of the plain bar. */
    bool show_battery_bar;       /**< Battery bar (vs the text icon only). */
    bool smaller_font;           /**< Caller should retry with a smaller font. */
    int left_w;                  /**< Exact left-panel width (px). */
    int right_w;                 /**< Exact right-panel width (px). */
    int center_w;                /**< Exact center-region width (px, >= 0). */
} header_layout_t;

/**
 * Compute the layout.
 *
 * @param out         Receives the resolved layout (always fully written).
 * @param avail_w     Usable header width in pixels (screen minus outer pads).
 * @param gap         Flex gap between panels in pixels.
 * @param status_w    Measured left-panel widths per level (>= 0).
 * @param sys_w       Measured right-panel widths per level (>= 0).
 * @param center_min  Minimum usable notification width (px); 0 disables the
 *                    center entirely.
 */
void header_layout_compute(header_layout_t *out, int avail_w, int gap,
                           const int status_w[HEADER_LEVEL_COUNT],
                           const int sys_w[HEADER_LEVEL_COUNT],
                           int center_min);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_HEADER_LAYOUT_H */

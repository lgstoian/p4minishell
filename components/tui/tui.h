/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tui.h
 * @module tui
 * @layer leaf
 * @domain ui
 * @provides 80x25 cell buffer, draw primitives, workspace/pane/HUD
 * @status stable
 * @test test/main/test_tui.c
 * @hard-rule ai-context.md#module-layering-rules
 * @brief TUI cell buffer + drawing primitives for P4MiniShell.
 *
 * Logical grid: the live grid follows the panel aspect (tui_get_cols/rows).
 * It keeps the documented 80-column DOS grid whenever a committed cell still
 * fits it (so app frames authored for 80 columns are never clipped) and turns
 * the extra panel height into more rows; only a panel too narrow for 80
 * columns falls back to a narrower aspect grid (1-based DOS coords). The grid
 * maps to the live transcript region's pixel rect
 * (rotation/keyboard/fullscreen-aware). The buffer is heap (PSRAM preferred)
 * and flushed to an LVGL label inside the transcript container via windows TUI
 * mode.
 *
 * Architecture:
 *   - tui_init() creates the buffer and enters windows TUI mode (hides shell
 *     spangroup, shows TUI container sized to the transcript region).
 *   - tui_* draw primitives mutate the logical cells (ch, fg/bg, attrs).
 *   - tui_flush() renders the buffer to the TUI label (monospace, recolor).
 *   - tui_deinit() exits TUI mode and frees the buffer.
 *
 * Layering: leaf component, REQUIRES shell, windows, ansi, display. No
 * dependency on command/batch. CSI handling in ansi.c calls into tui when
 * windows_tui_mode_active().
 */

#ifndef P4MINISHELL_TUI_H
#define P4MINISHELL_TUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h" /* gfx_surface_t for tui_draw_image */

#ifdef __cplusplus
extern "C" {
#endif

/** TUI cell (one logical grid position). UTF-8 up to 3 bytes per cell. */
typedef struct {
    char utf8[4];   /**< UTF-8 sequence, NUL-terminated (box chars are 3 bytes). */
    uint8_t fg;     /**< DOS 0-15 foreground (16 = default). */
    uint8_t bg;     /**< DOS 0-15 background (16 = default). */
    uint8_t attr;   /**< Bitmask: 1=bold,2=dim,4=italic,8=underline. */
} tui_cell_t;

/** Initialize TUI buffer and enter windows TUI mode. @return true on success. */
bool tui_init(void);

/** Deinitialize TUI buffer and leave windows TUI mode. */
void tui_deinit(void);

/** Report whether TUI mode is active. */
bool tui_is_active(void);

/**
 * Start a fresh page for a newly launched app.
 *
 * The TUI surface is reused between apps (no destroy/create flash), so it can
 * still hold the previous app's cells - and after a panel rotation those cells
 * were authored at another pitch. `launch` calls this before running the app's
 * script so its first frame is never composited with stale content.
 */
void tui_new_app_session(void);

/**
 * True when the live surface was fitted for a different panel geometry (the
 * panel rotated since it was bound). `launch` tears such a surface down so the
 * next app starts on a fresh one instead of laying its frame out against the
 * previous orientation's pitch.
 */
bool tui_surface_needs_refit(void);

/** True when LVGL/display is up, so tui_init() can safely take the port lock.
 *  False in the headless unit-test app (the page buffer is still usable). */
bool tui_ready(void);

/** Re-resolve the TUI label font after a font switch (stale-pointer fix).
 * No-op unless TUI mode is active. */
void tui_refresh_fonts(void);

/** Live logical grid width in cells. Keeps the documented 80-column DOS grid
 *  whenever a committed cell fits it (the width is always
 *  <= P4_CONFIG_TUI_COLS, the fixed-buffer ceiling); only a panel too narrow
 *  for 80 columns reports fewer. */
int tui_get_cols(void);

/** Live logical grid height in cells (see tui_get_cols()). */
int tui_get_rows(void);

/**
 * Resolve the logical grid that fills a @p w x @p h pixel viewport with the
 * largest committed TUI cell font, keeping a usable grid (at least 40 columns
 * and 24 rows) and clamping columns to P4_CONFIG_TUI_COLS. Pure: no LVGL, no
 * allocation, so `tui status` and host tests can query any viewport.
 */
void tui_grid_resolve(int w, int h, int *cols_out, int *rows_out,
                      int *cell_w_out, int *cell_h_out);

/**
 * The logical grid an app surface lays out against, resolved the same way the
 * live layout resolves it:
 *   - while TUI mode is up, the committed grid (an app launched from another
 *     app draws into the surface that is already there);
 *   - otherwise the app viewport (panel minus the header and input row) run
 *     through tui_grid_resolve().
 * This is what the shell publishes to `launch`ed batch apps as %TUICOLS% /
 * %TUIROWS%, so an app can size its frame before it enters TUI/fullscreen.
 * Pure reads (no LVGL, no allocation), headless-safe: without a display the
 * configured 80x25 grid is reported. Out-pointers are all optional.
 */
void tui_app_grid(int *cols_out, int *rows_out, int *cell_w_out,
                  int *cell_h_out);

/** Clear entire grid (ED 2J) with default colors. */
void tui_clear(void);

/** Clear current line (EL 2K) at cursor row. */
void tui_clear_line(int mode);

/** Scroll the grid up one row (top row lost, bottom cleared, cursor kept in
 * range). No-op unless the TUI buffer is allocated. Used by the VT100
 * terminal pump (`usb userial term`) so remote output scrolls instead of
 * clamping at the last row. */
void tui_scroll_up(void);

/** Set cursor (1-based, clamped to COLS/ROWS). */
void tui_set_cursor(int row, int col);

/** Get cursor (1-based). */
void tui_get_cursor(int *row_out, int *col_out);

/** Save cursor (SCP). */
void tui_save_cursor(void);

/** Restore cursor (RCP). */
void tui_restore_cursor(void);

/** Show/hide cursor (DECTCEM). */
void tui_set_cursor_visible(bool visible);

/** Put one char at cursor, advancing (wrap). */
void tui_putc(char ch);

/** Print text at logical position (1-based). */
void tui_print_at(int col, int row, const char *text, uint8_t fg, uint8_t bg);

/** Draw box border (single/double/rounded) at x,y,w,h with optional title. */
void tui_draw_box(int x, int y, int w, int h, const char *style, uint8_t fg, uint8_t bg, const char *title);

/** Draw horizontal/vertical line at x1,y1 to x2,y2 (H/V only). */
void tui_draw_line(int x1, int y1, int x2, int y2, const char *style, uint8_t fg, uint8_t bg);

/** Fill rect at x,y,w,h with char ch. */
void tui_fill(int x, int y, int w, int h, char ch, uint8_t fg, uint8_t bg);

/** Map a 24-bit RGB color to the nearest DOS 0-15 index (pure, headless-safe).
 * Used by `draw` verbs so hex colors survive the 80x25 TUI cell model. */
uint8_t tui_rgb_to_dos(uint32_t rgb);

/** Classic CGA/DOS 16-color RGB value for a 0-15 index (pure, headless-safe).
 * Shared by `gfx` verbs so pixel colors match TUI cell colors. */
uint32_t tui_dos_color_rgb(uint8_t index);

/** Draw a `[fill...empty...]` progress bar at x,y (1-based) of width w.
 * pct is clamped 0..100; the two border cells are `[`/`]`. */
void tui_draw_bar(int x, int y, int w, int pct, char fill_ch, char empty_ch,
                  uint8_t fg, uint8_t bg);

/** Render an RGB565 image into the cell grid at x,y (1-based) spanning w x h
 * cells. Each cell is drawn as an ASCII glyph picked from a luminance ramp and
 * coloured with the nearest DOS palette entry (foreground only — the TUI label
 * has no per-cell background). Posterizes to 16 colors; the full-color viewer
 * is `view <file.bmp>`. Clipped to the grid. */
void tui_draw_image(int x, int y, int w, int h, const gfx_surface_t *img);

/** Total grid width of a bordered table: sum(widths) + 3 per column + 1
 * (pure, headless-safe; each column renders as `│<pad><cell><pad>`). */
int tui_table_total_width(int ncols, const int *widths);

/** Draw a bordered table at x,y (1-based): single-style border with
 * T-junctions, `cells` is nrows*ncols row-major (NULL cell = empty),
 * widths are per-column content widths. The first row renders bold when
 * header is true. Rows that would overflow the grid are dropped. */
void tui_draw_table(int x, int y, int ncols, const int *widths, int nrows,
                    const char *const *cells, bool header,
                    uint8_t fg, uint8_t bg);

/** Table cursor/selection extension for file-manager grids (`draw table
 * /cursor:N /sel:a,b`). cursor_data_row is 1-based over DATA rows
 * (header excluded, 0 = none, out-of-range = none); sel_mask bit i
 * selects data row i+1. Cursor rows render bright-white bold (per-span LVGL
 * backgrounds don't exist, so inverse isn't expressible — documented in
 * command.md); selected rows render bold. Borders keep the base colors. */
void tui_draw_table_ex(int x, int y, int ncols, const int *widths, int nrows,
                       const char *const *cells, bool header,
                       uint8_t fg, uint8_t bg, int cursor_data_row,
                       uint32_t sel_mask);

/** Parse a `/cursor:N` value: 1-based data-row number, 0 when unset,
 * malformed, or outside 1..n_data_rows. Pure, headless-safe. */
int tui_table_parse_cursor(const char *s, int n_data_rows);

/** Parse a `/sel:a,b,...` value into a bitmask (bit i = data row i+1).
 * Malformed tokens are ignored, valid ones clipped to 1..n_data_rows.
 * Pure, headless-safe. */
uint32_t tui_table_parse_sel(const char *s, int n_data_rows);

/** Set default fg/bg for next tui_putc/print (DOS COLOR). 16 = default. */
void tui_set_default_color(uint8_t fg, uint8_t bg);

/** Opt-in translucency of the whole TUI surface (0-255; 255 opaque).
 *  Batch apps set it from their APPINFO `alpha=`; the shell restores 255
 *  after the app returns. Applies live when TUI mode is up. */
void tui_set_background_alpha(uint8_t alpha);
uint8_t tui_get_background_alpha(void);

/** Get default fg/bg. */
void tui_get_default_color(uint8_t *fg_out, uint8_t *bg_out);

/** Save alt-screen (ESC[?1049h) and clear. */
void tui_alt_enter(void);

/** Restore alt-screen (ESC[?1049l). */
void tui_alt_leave(void);

/** Flush logical buffer to LVGL TUI label. Must run on LVGL task or via async. */
void tui_flush(void);

/** Reset the TUI frame-pacing statistics (also done on tui_init). */
void tui_frame_stats_reset(void);

/** Set the target frame rate that counts dropped frames (0 disables). */
void tui_frame_stats_set_target_fps(uint32_t fps);

/** Read-only view of the TUI frame-pacing statistics. */
const gfx_frame_stats_t *tui_frame_stats_get(void);

/** Refresh TUI surface size to live transcript region (call on rotation/keyboard). */
void tui_refresh_surface(void);

/** Hide TUI label for modal (editor) takeover. */
void tui_hide_for_modal(void);

/** Show TUI label after modal closes. */
void tui_show_after_modal(void);

/** Enter fullscreen TUI (hide header, input row, keyboard). Global. */
void tui_enter_fullscreen(void);

/** Exit fullscreen TUI (restore header, input row). Global. */
void tui_exit_fullscreen(void);

/** Report whether TUI is in fullscreen. */
bool tui_is_fullscreen(void);

/** Bound cell font info for `tui status` (0/-1 when inactive or unlaid-out).
 *  Pure reads, headless-safe. */
void tui_bound_cell_info(int *cell_w_out, int *cell_h_out, int *index_out);

/** Self-test: every SH_BOX_* pairwise distinct and decoding to its spec
 *  codepoint (catches the scrambled-double class). Returns failure count;
 *  first failing macro name via @p fail_name_out (may be NULL).
 *  Pure, headless-safe. */
int tui_selftest_palette(const char **fail_name_out);

/** Self-test: every box codepoint present in every committed cell font with
 *  matching advance (no fallback substitution). Returns failure count.
 *  Pure table reads (no display), headless-safe. */
int tui_selftest_coverage(const char **fail_name_out);

/** Self-test: bound font line heights match their cells. Returns failure
 *  count. Pure, headless-safe. */
int tui_selftest_metrics(const char **fail_name_out);

/** Run all self-test groups in order, stopping at the first failing group.
 *  Returns the failure count; first failing macro/stage name via
 *  @p fail_name_out (may be NULL). Pure, headless-safe. */
int tui_selftest_check(const char **fail_name_out);

#define TUI_MAX_LINKS 64
#define TUI_URL_MAX 512

typedef struct {
    int x, y, w, h;
    char url[TUI_URL_MAX];
} tui_link_t;

void tui_viewport_init(int page_rows);
void tui_viewport_scroll_to(int row);
void tui_viewport_scroll_by(int delta);

/* ==== Page snapshots (the browser back/forward cache) ====
 * A snapshot is an opaque PSRAM blob holding the rendered page: the cell grid,
 * the link regions and the address bar. Restoring one is a memcpy, so going back
 * or forward to a cached page is instant and costs no network, parse or render
 * work. The blob carries its own total size and grid dimensions: a snapshot
 * taken under a different grid, or a truncated/corrupted blob, is rejected
 * rather than blitted into the wrong shape or read past its allocation. */
typedef struct {
    int32_t cols;
    int32_t rows;
    int32_t link_count;
    uint32_t link_offset;      /* bytes from the header to the link array */
    uint32_t total_bytes;      /* whole blob size, for restore bounds checks */
    char address[256];
} tui_page_shot_t;

/** Capture the current page. @return an owned PSRAM blob (release with
 *  tui_page_shot_release()), or NULL when no page is up or on OOM. */
void *tui_page_shot_capture(void);

/** Restore a captured page. @return false when the blob is NULL, the viewport
 *  is not initialised, or the snapshot's grid does not match the current one. */
bool tui_page_shot_restore(const void *blob);

/** Release a blob returned by tui_page_shot_capture(). NULL-safe. */
void tui_page_shot_release(void *blob);
int tui_viewport_get_offset(void);
int tui_viewport_get_page_rows(void);

void tui_page_set_cell(int col, int row, const char *utf8, uint8_t fg, uint8_t bg, uint8_t attr);
/** Read one cell of the logical page (1-based col/row). Copies the cell UTF-8
 *  into @p utf8_out (bounded by @p out_size) and reports fg/bg/attr when the
 *  pointers are non-NULL. Returns false on an out-of-range coordinate or when
 *  no page is active. Pure read, headless-safe: used by the browser render
 *  tests and any host-side page assertion. */
bool tui_page_get_cell(int col, int row, char *utf8_out, size_t out_size,
                       uint8_t *fg_out, uint8_t *bg_out, uint8_t *attr_out);
void tui_page_clear(void);
/** Paint a CSS background rectangle behind the cell grid (1-based cells).
 *  Real LVGL rectangles, so text and spaces stay untouched; no-op headless.
 *  @p alpha is 0-255 (255 opaque): CSS `rgba()` backgrounds keep their alpha
 *  and `transparent` (0) draws nothing. */
void tui_page_bg_rect(int col, int row, int w, int h, uint8_t bg,
                      uint8_t alpha);
void tui_page_render(void);

/** Copy the current pointer selection to the shell clipboard. */
bool tui_selection_copy(void);
void tui_selection_clear(void);

/* ==== Multitasking app switching (one input seam, one frame renderer) ====
 * The TUI never owns sessions. main registers the one switch helper (the
 * mstack cycle) and modal registers the one view (these functions), so the
 * edge-swipe gesture reuses the same policy as keyboard Alt+Tab. */

/** Register the app-switch handler called by the bottom-left edge swipe.
 *  @p handler receives the cycle step (+1 forward, -1 back); NULL disables. */
/** Register the app-switch handler invoked by the bottom-left edge swipe.
 *  The handler is the existing modal session-cycle primitive; it returns
 *  whether a switch actually happened (the TUI ignores the result). */
void tui_set_app_switch_handler(bool (*handler)(int step));

/** Draw the 80x25 CP437 app switcher through the ONE tui frame renderer and
 *  schedule its one-shot hide. @p names are in from-top order; @p focus is
 *  the from-top index of the highlighted session. */
void tui_app_switcher_render(const char *const *names, int count, int focus);

/** Blit an image into the browser PAGE buffer as cell-ramp glyphs (1-based
 *  cells). Shares the sampler with tui_draw_image(); used for emoji pack
 *  glyphs so a page shows colour emoji on the next tui_page_render(). */
void tui_page_draw_image(int col, int row, int w, int h,
                         const gfx_surface_t *img);

/** Called when a tap lands on a registered link region. The TUI never runs
 *  commands itself; the owner (the browser) decides what a link means. */
void tui_set_link_callback(void (*callback)(const char *url));

void tui_link_register(int x, int y, int w, int h, const char *url);
bool tui_link_hit_test(int x, int y, const char **url_out);
void tui_link_clear(void);

/** Number of tappable regions the last page render registered. */
int tui_link_count(void);

/** Command registered for region @p index (NULL when out of range). Lets
 *  `browser links` list them and `browser link <n>` run one without touch. */
const char *tui_link_command_at(int index);

void tui_address_bar_set(const char *text);
void tui_address_bar_render(void);

void tui_status_bar_set(const char *text);
void tui_status_bar_render(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_TUI_H */

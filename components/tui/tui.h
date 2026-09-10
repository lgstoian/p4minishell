/**
 * @file tui.h
 * @brief TUI cell buffer + drawing primitives for P4MiniShell.
 *
 * Logical grid P4_CONFIG_TUI_COLS x ROWS (default 80x25, 1-based DOS coords)
 * maps to the live transcript region's pixel rect (rotation/keyboard-aware).
 * The buffer is heap (PSRAM preferred) and flushed to an LVGL label inside the
 * transcript container via windows TUI mode.
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

/** Re-resolve the TUI label font after a font switch (stale-pointer fix).
 * No-op unless TUI mode is active. */
void tui_refresh_fonts(void);

/** Clear entire grid (ED 2J) with default colors. */
void tui_clear(void);

/** Clear current line (EL 2K) at cursor row. */
void tui_clear_line(int mode);

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

/** Set default fg/bg for next tui_putc/print (DOS COLOR). 16 = default. */
void tui_set_default_color(uint8_t fg, uint8_t bg);

/** Get default fg/bg. */
void tui_get_default_color(uint8_t *fg_out, uint8_t *bg_out);

/** Save alt-screen (ESC[?1049h) and clear. */
void tui_alt_enter(void);

/** Restore alt-screen (ESC[?1049l). */
void tui_alt_leave(void);

/** Flush logical buffer to LVGL TUI label. Must run on LVGL task or via async. */
void tui_flush(void);

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

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_TUI_H */

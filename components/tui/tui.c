/**
 * @file tui.c
 * @brief TUI cell buffer implementation.
 */

#include "tui.h"
#include "windows.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TUI_TAG "tui"

static tui_cell_t *s_cells = NULL; /* ROWS*COLS */
static int s_rows = P4_CONFIG_TUI_ROWS;
static int s_cols = P4_CONFIG_TUI_COLS;
static int s_cur_row = 1;
static int s_cur_col = 1;
static int s_saved_row = 1;
static int s_saved_col = 1;
static bool s_cursor_visible = true;
static bool s_active = false;
static bool s_alt_saved = false;
static tui_cell_t *s_alt_cells = NULL;
static int s_alt_cur_row = 1;
static int s_alt_cur_col = 1;
static uint8_t s_def_fg = 16;
static uint8_t s_def_bg = 16;
static lv_obj_t *s_tui_label = NULL;
static lv_obj_t *s_tui_container = NULL;

static inline tui_cell_t *cell_at(int row, int col)
{
    if (row < 1) row = 1;
    if (row > s_rows) row = s_rows;
    if (col < 1) col = 1;
    if (col > s_cols) col = s_cols;
    return &s_cells[(row - 1) * s_cols + (col - 1)];
}

static void tui_alloc_cells(void)
{
    size_t total = (size_t)s_rows * (size_t)s_cols;
    size_t bytes = total * sizeof(tui_cell_t);
    s_cells = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_cells) s_cells = malloc(bytes);
    if (s_cells) memset(s_cells, 0, bytes);
}

bool tui_init(void)
{
    if (s_active) return true;
    s_rows = P4_CONFIG_TUI_ROWS;
    s_cols = P4_CONFIG_TUI_COLS;
    tui_alloc_cells();
    if (!s_cells) return false;
    s_cur_row = 1;
    s_cur_col = 1;
    s_saved_row = 1;
    s_saved_col = 1;
    s_def_fg = 16;
    s_def_bg = 16;
    /* Enter windows TUI mode (create container/label). */
    lvgl_port_lock(0);
    lv_obj_t *surf = windows_enter_tui_mode();
    if (!surf) {
        lvgl_port_unlock();
        free(s_cells);
        s_cells = NULL;
        if (s_cells && heap_caps_get_allocated_size(s_cells) > 0) heap_caps_free(s_cells);
        return false;
    }
    s_tui_container = surf;
    s_tui_label = lv_label_create(surf);
    lv_obj_set_width(s_tui_label, LV_PCT(100));
    lv_obj_set_height(s_tui_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_tui_label, windows_get_terminal_font(), 0);
    lv_obj_set_style_bg_opa(s_tui_label, LV_OPA_TRANSP, 0);
    lv_label_set_recolor(s_tui_label, true);
    lv_label_set_long_mode(s_tui_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_all(s_tui_label, 0, 0);
    lvgl_port_unlock();
    s_active = true;
    tui_clear();
    tui_flush();
    return true;
}

void tui_deinit(void)
{
    if (!s_active) return;
    lvgl_port_lock(0);
    if (s_tui_label) { lv_obj_del(s_tui_label); s_tui_label = NULL; }
    windows_exit_tui_mode();
    s_tui_container = NULL;
    lvgl_port_unlock();
    if (s_cells) {
        heap_caps_free(s_cells);
        s_cells = NULL;
    }
    if (s_alt_cells) {
        heap_caps_free(s_alt_cells);
        s_alt_cells = NULL;
    }
    s_active = false;
}

bool tui_is_active(void) { return s_active; }

static void tui_cell_set(tui_cell_t *cell, const char *utf8, uint8_t fg, uint8_t bg)
{
    if (!cell || !utf8) return;
    strncpy(cell->utf8, utf8, sizeof(cell->utf8) - 1);
    cell->utf8[sizeof(cell->utf8) - 1] = '\0';
    cell->fg = fg;
    cell->bg = bg;
    cell->attr = 0;
}

void tui_clear(void)
{
    if (!s_cells) return;
    for (int r = 1; r <= s_rows; r++) {
        for (int c = 1; c <= s_cols; c++) {
            tui_cell_t *cell = cell_at(r, c);
            tui_cell_set(cell, " ", s_def_fg, s_def_bg);
        }
    }
    s_cur_row = 1;
    s_cur_col = 1;
}

void tui_clear_line(int mode)
{
    if (!s_cells) return;
    if (mode == 2) { /* entire line */
        for (int c = 1; c <= s_cols; c++) {
            tui_cell_t *cell = cell_at(s_cur_row, c);
            tui_cell_set(cell, " ", s_def_fg, s_def_bg);
        }
    } else if (mode == 0) { /* from cursor to end */
        for (int c = s_cur_col; c <= s_cols; c++) {
            tui_cell_t *cell = cell_at(s_cur_row, c);
            tui_cell_set(cell, " ", s_def_fg, s_def_bg);
        }
    } else if (mode == 1) { /* from start to cursor */
        for (int c = 1; c <= s_cur_col; c++) {
            tui_cell_t *cell = cell_at(s_cur_row, c);
            tui_cell_set(cell, " ", s_def_fg, s_def_bg);
        }
    }
}

void tui_set_cursor(int row, int col)
{
    if (row < 1) row = 1;
    if (row > s_rows) row = s_rows;
    if (col < 1) col = 1;
    if (col > s_cols) col = s_cols;
    s_cur_row = row;
    s_cur_col = col;
}

void tui_get_cursor(int *row_out, int *col_out)
{
    if (row_out) *row_out = s_cur_row;
    if (col_out) *col_out = s_cur_col;
}

void tui_save_cursor(void) { s_saved_row = s_cur_row; s_saved_col = s_cur_col; }
void tui_restore_cursor(void) { s_cur_row = s_saved_row; s_cur_col = s_saved_col; }
void tui_set_cursor_visible(bool visible) { s_cursor_visible = visible; }

void tui_putc(char ch)
{
    if (!s_cells) return;
    if (ch == '\n') {
        s_cur_row++;
        s_cur_col = 1;
        if (s_cur_row > s_rows) s_cur_row = s_rows;
        return;
    }
    if (ch == '\r') { s_cur_col = 1; return; }
    tui_cell_t *cell = cell_at(s_cur_row, s_cur_col);
    char tmp[2] = {ch, '\0'};
    tui_cell_set(cell, tmp, s_def_fg, s_def_bg);
    s_cur_col++;
    if (s_cur_col > s_cols) {
        s_cur_col = 1;
        s_cur_row++;
        if (s_cur_row > s_rows) s_cur_row = s_rows;
    }
}

void tui_print_at(int col, int row, const char *text, uint8_t fg, uint8_t bg)
{
    if (!s_cells || !text) return;
    if (row < 1) row = 1;
    if (row > s_rows) row = s_rows;
    if (col < 1) col = 1;
    if (col > s_cols) return;
    int r = row, c = col;
    for (const char *p = text; *p; ) {
        if (c > s_cols) { c = 1; r++; if (r > s_rows) break; }
        tui_cell_t *cell = cell_at(r, c);
        // handle UTF-8: copy one codepoint
        char tmp[5] = {0};
        int len = 1;
        unsigned char ch = (unsigned char)*p;
        if ((ch & 0xE0) == 0xC0) len = 2;
        else if ((ch & 0xF0) == 0xE0) len = 3;
        else if ((ch & 0xF8) == 0xF0) len = 4;
        if ((int)strlen(p) < len) len = 1;
        strncpy(tmp, p, len);
        tmp[len] = '\0';
        tui_cell_set(cell, tmp, fg, bg);
        p += len;
        c++;
    }
}

void tui_draw_box(int x, int y, int w, int h, const char *style, uint8_t fg, uint8_t bg, const char *title)
{
    if (!s_cells) return;
    if (w < 3) w = 3;
    if (h < 3) h = 3;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x + w - 1 > s_cols) w = s_cols - x + 1;
    if (y + h - 1 > s_rows) h = s_rows - y + 1;

    const char *tl = SH_BOX_TL, *tr = SH_BOX_TR, *bl = SH_BOX_BL, *br = SH_BOX_BR, *hch = SH_BOX_H, *vch = SH_BOX_V;
    if (style && strcasecmp(style, "double") == 0) {
        tl = SH_BOX_TL2; tr = SH_BOX_TR2; bl = SH_BOX_BL2; br = SH_BOX_BR2; hch = SH_BOX_H2; vch = SH_BOX_V2;
    } else if (style && strcasecmp(style, "rounded") == 0) {
        tl = SH_BOX_TLR; tr = SH_BOX_TRR; bl = SH_BOX_BLR; br = SH_BOX_BRR;
    }

    /* Top/bottom */
    for (int c = x; c < x + w; c++) {
        tui_cell_t *top = cell_at(y, c);
        tui_cell_t *bot = cell_at(y + h - 1, c);
        const char *ch = hch;
        if (c == x) ch = tl;
        else if (c == x + w - 1) ch = tr;
        tui_cell_set(top, ch, fg, bg);
        const char *bch = hch;
        if (c == x) bch = bl;
        else if (c == x + w - 1) bch = br;
        tui_cell_set(bot, bch, fg, bg);
    }
    /* Sides */
    for (int r = y + 1; r < y + h - 1; r++) {
        tui_cell_t *left = cell_at(r, x);
        tui_cell_t *right = cell_at(r, x + w - 1);
        tui_cell_set(left, vch, fg, bg);
        tui_cell_set(right, vch, fg, bg);
    }
    /* Title centered on top border */
    if (title && title[0]) {
        int tlen = (int)strlen(title);
        int tx = x + (w - tlen - 2) / 2;
        if (tx < x + 1) tx = x + 1;
        tui_print_at(tx, y, " ", fg, bg);
        tui_print_at(tx + 1, y, title, fg, bg);
        tui_print_at(tx + 1 + tlen, y, " ", fg, bg);
    }
    /* Clear interior */
    for (int r = y + 1; r < y + h - 1; r++) {
        for (int c = x + 1; c < x + w - 1; c++) {
            tui_cell_t *cell = cell_at(r, c);
            tui_cell_set(cell, " ", fg, bg);
        }
    }
}

void tui_draw_line(int x1, int y1, int x2, int y2, const char *style, uint8_t fg, uint8_t bg)
{
    if (!s_cells) return;
    const char *hch = SH_BOX_H;
    const char *vch = SH_BOX_V;
    if (style && strcasecmp(style, "double") == 0) {
        hch = SH_BOX_H2; vch = SH_BOX_V2;
    } else if (style && strcasecmp(style, "heavy") == 0) {
        hch = SH_BOX_HL; vch = SH_BOX_VL;
    }
    if (y1 == y2) { /* horizontal */
        int y = y1;
        int x_start = x1 < x2 ? x1 : x2;
        int x_end = x1 < x2 ? x2 : x1;
        for (int c = x_start; c <= x_end; c++) {
            tui_cell_t *cell = cell_at(y, c);
            tui_cell_set(cell, hch, fg, bg);
        }
    } else if (x1 == x2) { /* vertical */
        int x = x1;
        int y_start = y1 < y2 ? y1 : y2;
        int y_end = y1 < y2 ? y2 : y1;
        for (int r = y_start; r <= y_end; r++) {
            tui_cell_t *cell = cell_at(r, x);
            tui_cell_set(cell, vch, fg, bg);
        }
    }
}

void tui_fill(int x, int y, int w, int h, char ch, uint8_t fg, uint8_t bg)
{
    if (!s_cells) return;
    char tmp[2] = {ch, '\0'};
    for (int r = y; r < y + h && r <= s_rows; r++) {
        for (int c = x; c < x + w && c <= s_cols; c++) {
            tui_cell_t *cell = cell_at(r, c);
            tui_cell_set(cell, tmp, fg, bg);
        }
    }
}

void tui_set_default_color(uint8_t fg, uint8_t bg) { s_def_fg = fg; s_def_bg = bg; }
void tui_get_default_color(uint8_t *fg_out, uint8_t *bg_out) { if (fg_out) *fg_out = s_def_fg; if (bg_out) *bg_out = s_def_bg; }

void tui_alt_enter(void)
{
    if (s_alt_saved) return;
    size_t total = (size_t)s_rows * s_cols * sizeof(tui_cell_t);
    s_alt_cells = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!s_alt_cells) s_alt_cells = malloc(total);
    if (s_alt_cells && s_cells) {
        memcpy(s_alt_cells, s_cells, total);
        s_alt_cur_row = s_cur_row;
        s_alt_cur_col = s_cur_col;
        s_alt_saved = true;
        tui_clear();
        tui_flush();
    }
}

void tui_alt_leave(void)
{
    if (!s_alt_saved || !s_alt_cells || !s_cells) return;
    size_t total = (size_t)s_rows * s_cols * sizeof(tui_cell_t);
    memcpy(s_cells, s_alt_cells, total);
    s_cur_row = s_alt_cur_row;
    s_cur_col = s_alt_cur_col;
    heap_caps_free(s_alt_cells);
    s_alt_cells = NULL;
    s_alt_saved = false;
    tui_flush();
}

void tui_flush(void)
{
    if (!s_cells || !s_tui_label) return;
    // Build recolor buffer: each fg run emits "#RRGGBB " + utf8 + "#" ; rows separated by "\n"
    // Cap: rows*(cols* (7+3+1) +1) worst ~ rows*cols*12
    size_t cap = (size_t)s_rows * (size_t)(s_cols * 12 + 1) + 1;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(cap);
    if (!buf) return;
    size_t pos = 0;
    for (int r = 1; r <= s_rows; r++) {
        int c = 1;
        while (c <= s_cols) {
            tui_cell_t *cell = cell_at(r, c);
            uint8_t fg = cell->fg;
            // coalesce same fg
            int run = 1;
            while (c + run <= s_cols) {
                tui_cell_t *n = cell_at(r, c + run);
                if (n->fg != fg) break;
                run++;
            }
            // Emit color tag if not default
            if (fg < 16) {
                uint32_t col = ansi_get_palette_color((ansi_color_index_t)fg);
                if (col == 0 && fg != 0) col = ansi_get_palette_color(ANSI_COLOR_WHITE);
                int n = snprintf(buf + pos, cap - pos, "#%06X ", (unsigned)(col & 0xFFFFFF));
                if (n > 0) pos += (size_t)n;
            }
            for (int k = 0; k < run; k++) {
                tui_cell_t *cc = cell_at(r, c + k);
                const char *utf8 = cc->utf8[0] ? cc->utf8 : " ";
                size_t ulen = strlen(utf8);
                if (pos + ulen < cap) {
                    memcpy(buf + pos, utf8, ulen);
                    pos += ulen;
                }
            }
            if (fg < 16) {
                if (pos + 1 < cap) buf[pos++] = '#';
            }
            c += run;
        }
        if (r < s_rows && pos + 1 < cap) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    lvgl_port_lock(0);
    lv_label_set_text(s_tui_label, buf);
    lvgl_port_unlock();
    heap_caps_free(buf);
}

void tui_refresh_surface(void)
{
    if (!s_active) return;
    lvgl_port_lock(0);
    windows_refresh_tui_surface();
    lvgl_port_unlock();
    tui_flush();
}

void tui_hide_for_modal(void)
{
    if (!s_active || !s_tui_label) return;
    lvgl_port_lock(0);
    lv_obj_add_flag(s_tui_label, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void tui_show_after_modal(void)
{
    if (!s_active || !s_tui_label) return;
    lvgl_port_lock(0);
    lv_obj_remove_flag(s_tui_label, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
    tui_flush();
}

void tui_enter_fullscreen(void)
{
    lvgl_port_lock(0);
    windows_set_fullscreen(true);
    if (!s_active) {
        lvgl_port_unlock();
        tui_init();
        lvgl_port_lock(0);
        windows_set_fullscreen(true);
    }
    windows_refresh_tui_surface();
    lvgl_port_unlock();
    tui_flush();
}

void tui_exit_fullscreen(void)
{
    lvgl_port_lock(0);
    windows_set_fullscreen(false);
    lvgl_port_unlock();
    if (s_active) tui_flush();
}

bool tui_is_fullscreen(void)
{
    return windows_is_fullscreen();
}

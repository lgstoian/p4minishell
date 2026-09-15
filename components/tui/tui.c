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

/** Re-resolve the TUI label font after a `font set/size` switch. */
void tui_refresh_fonts(void)
{
    if (!s_active || s_tui_label == NULL) {
        return;
    }
    if (!lvgl_port_lock(0)) {
        return;
    }
    lv_obj_set_style_text_font(s_tui_label, windows_get_terminal_font(), 0);
    lvgl_port_unlock();
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

void tui_scroll_up(void)
{
    if (!s_cells) return;
    size_t row_bytes = (size_t)s_cols * sizeof(tui_cell_t);
    memmove(s_cells, s_cells + s_cols, row_bytes * (size_t)(s_rows - 1));
    for (int c = 1; c <= s_cols; c++) {
        tui_cell_set(cell_at(s_rows, c), " ", s_def_fg, s_def_bg);
    }
    if (s_cur_row > 1) s_cur_row--;
    if (s_cur_row < 1) s_cur_row = 1;
    if (s_cur_row > s_rows) s_cur_row = s_rows;
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

/* Classic CGA/DOS 16-color RGB values (0-7 dim, 8-15 bright). */
static const uint32_t s_dos_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

uint32_t tui_dos_color_rgb(uint8_t index)
{
    return s_dos_rgb[index & 15];
}

uint8_t tui_rgb_to_dos(uint32_t rgb)
{
    uint8_t best = 7;
    uint32_t best_d = (uint32_t)-1;
    int r = (int)((rgb >> 16) & 0xFF);
    int g = (int)((rgb >> 8) & 0xFF);
    int b = (int)(rgb & 0xFF);
    int i;

    for (i = 0; i < 16; i++) {
        int dr = r - (int)((s_dos_rgb[i] >> 16) & 0xFF);
        int dg = g - (int)((s_dos_rgb[i] >> 8) & 0xFF);
        int db = b - (int)(s_dos_rgb[i] & 0xFF);
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);

        if (d < best_d) {
            best_d = d;
            best = (uint8_t)i;
        }
    }
    return best;
}

void tui_draw_image(int x, int y, int w, int h, const gfx_surface_t *img)
{
    /* The TUI label renders only the foreground colour (LVGL recolor has no
     * per-span background), so a pixel is drawn as an ASCII glyph chosen from a
     * luminance ramp, coloured with the nearest DOS palette entry as fg. */
    static const char k_ramp[] = " .:-=+*#%@";
    const int ramp_last = (int)(sizeof(k_ramp) - 2);
    int cy;

    if (s_cells == NULL || img == NULL || img->px == NULL) return;
    if (w < 1 || h < 1) return;

    for (cy = 0; cy < h; cy++) {
        int row = y + cy;
        int sy;
        int cx;

        if (row < 1) continue;
        if (row > s_rows) break;
        sy = (int)((int64_t)cy * img->h / h);
        if (sy >= img->h) sy = img->h - 1;
        for (cx = 0; cx < w; cx++) {
            int col = x + cx;
            int sx;
            uint16_t px;
            uint32_t rgb;
            int r;
            int g;
            int b;
            int lum;
            int level;
            char ch[2];

            if (col < 1) continue;
            if (col > s_cols) break;
            sx = (int)((int64_t)cx * img->w / w);
            if (sx >= img->w) sx = img->w - 1;
            px = img->px[(size_t)sy * (size_t)img->w + (size_t)sx];
            /* RGB565 -> RGB888. */
            r = ((px >> 11) & 0x1F) << 3;
            g = ((px >> 5) & 0x3F) << 2;
            b = (px & 0x1F) << 3;
            rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

            lum = (r * 30 + g * 59 + b * 11) / 100; /* 0..255 luma */
            level = lum * ramp_last / 255;
            if (level < 0) level = 0;
            if (level > ramp_last) level = ramp_last;
            ch[0] = k_ramp[level];
            ch[1] = '\0';
            tui_print_at(col, row, ch, tui_rgb_to_dos(rgb), 16);
        }
    }
}

void tui_draw_bar(int x, int y, int w, int pct, char fill_ch, char empty_ch,
                  uint8_t fg, uint8_t bg)
{
    char tmp[2];
    int inner;
    int filled;
    int i;

    if (!s_cells) return;
    if (w < 3) w = 3;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (y > s_rows) return;
    if (x + w - 1 > s_cols) w = s_cols - x + 1;
    if (w < 3) return;

    inner = w - 2;
    filled = (inner * pct + 50) / 100;

    tui_cell_set(cell_at(y, x), "[", fg, bg);
    for (i = 0; i < inner; i++) {
        tmp[0] = (i < filled) ? fill_ch : empty_ch;
        tmp[1] = '\0';
        tui_cell_set(cell_at(y, x + 1 + i), tmp, fg, bg);
    }
    tui_cell_set(cell_at(y, x + w - 1), "]", fg, bg);
}

int tui_table_total_width(int ncols, const int *widths)
{
    int total = 1; /* leading border */
    int i;

    if (ncols < 1 || widths == NULL) return 0;
    for (i = 0; i < ncols; i++) {
        int w = widths[i] < 1 ? 1 : widths[i];
        total += w + 3; /* pad + content + pad + border */
    }
    return total;
}

/** Write one table border row: left/mid/right junctions joined by fills. */
static void tui_table_border(int row, int x, int total_w, const int *stops,
                             int nstops, const char *left, const char *mid,
                             const char *right, const char *fill,
                             uint8_t fg, uint8_t bg)
{
    int c;
    int s = 0;

    tui_cell_set(cell_at(row, x), left, fg, bg);
    for (c = 1; c < total_w - 1; c++) {
        if (s < nstops && x + c == stops[s]) {
            tui_cell_set(cell_at(row, x + c), mid, fg, bg);
            s++;
        } else {
            tui_cell_set(cell_at(row, x + c), fill, fg, bg);
        }
    }
    tui_cell_set(cell_at(row, x + total_w - 1), right, fg, bg);
}

/** Copy at most max_bytes of src without splitting a UTF-8 sequence. */
static size_t tui_utf8_trunc(const char *src, size_t max_bytes)
{
    size_t len = strlen(src);

    if (len <= max_bytes) return len;
    len = max_bytes;
    while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    return len;
}

void tui_draw_table(int x, int y, int ncols, const int *widths, int nrows,
                    const char *const *cells, bool header,
                    uint8_t fg, uint8_t bg)
{
    tui_draw_table_ex(x, y, ncols, widths, nrows, cells, header, fg, bg,
                      0, 0u);
}

int tui_table_parse_cursor(const char *s, int n_data_rows)
{
    long v;
    char *end = NULL;

    if (s == NULL || n_data_rows < 1) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return 0;
    v = strtol(s, &end, 10);
    if (end == s) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return 0;
    if (v < 1 || v > n_data_rows) return 0;
    return (int)v;
}

uint32_t tui_table_parse_sel(const char *s, int n_data_rows)
{
    uint32_t mask = 0u;

    if (s == NULL || n_data_rows < 1) return 0u;
    while (*s != '\0') {
        long v;
        char *end = NULL;

        while (*s == ' ' || *s == '\t' || *s == ',') s++;
        if (*s == '\0') break;
        v = strtol(s, &end, 10);
        if (end == s) break; /* malformed tail: keep bits parsed so far */
        if (v >= 1 && v <= n_data_rows && v <= 32) {
            mask |= (1u << (v - 1));
        }
        s = end;
    }
    return mask;
}

void tui_draw_table_ex(int x, int y, int ncols, const int *widths, int nrows,
                       const char *const *cells, bool header,
                       uint8_t fg, uint8_t bg, int cursor_data_row,
                       uint32_t sel_mask)
{
    /* Column border x-stops (absolute grid coords) for the T-junctions. */
    int stops[P4_CONFIG_TUI_COLS];
    int total_w;
    int r;
    int c;

    if (!s_cells || cells == NULL) return;
    if (ncols < 1 || nrows < 1 || widths == NULL) return;
    if (ncols > P4_CONFIG_TUI_COLS) ncols = P4_CONFIG_TUI_COLS;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    total_w = tui_table_total_width(ncols, widths);
    if (x + total_w - 1 > s_cols) return; /* table either fits or it doesn't */

    stops[0] = x;
    for (c = 0; c < ncols; c++) {
        int w = widths[c] < 1 ? 1 : widths[c];
        stops[c + 1] = stops[c] + w + 3;
    }

    tui_table_border(y, x, total_w, &stops[1], ncols - 1,
                     SH_BOX_TL, SH_BOX_T, SH_BOX_TR, SH_BOX_H, fg, bg);
    for (r = 0; r < nrows; r++) {
        int row = y + 1 + r * 2;
        int sep_row = row + 1;

        if (row > s_rows) break;
        /* Cursor row renders bright-white bold content; selected rows
         * render bold. Borders keep base colors so the grid stays
         * readable. (Per-span LVGL backgrounds don't exist, so a true
         * inverse bar isn't expressible — see command.md.) */
        bool is_cursor = (cursor_data_row >= 1 && r == cursor_data_row);
        bool is_sel = (r >= 1 && r <= 32 && (sel_mask & (1u << (r - 1))) != 0u);
        uint8_t rfg = is_cursor ? 15 : fg;
        uint8_t rbg = bg;
        bool rbold = is_sel || is_cursor;
        tui_cell_set(cell_at(row, x), SH_BOX_V, fg, bg);
        for (c = 0; c < ncols; c++) {
            int w = widths[c] < 1 ? 1 : widths[c];
            const char *text = cells[r * ncols + c];
            char buf[P4_CONFIG_TUI_COLS + 1];
            size_t copy;

            if (text == NULL) text = "";
            copy = tui_utf8_trunc(text, (size_t)w);
            if (copy > sizeof(buf) - 1) copy = sizeof(buf) - 1;
            memcpy(buf, text, copy);
            buf[copy] = '\0';
            tui_cell_set(cell_at(row, stops[c] + 1), " ", rfg, rbg);
            {
                /* One cell per codepoint (never split UTF-8 across cells). */
                const char *p = buf;
                int col = stops[c] + 2;

                while (*p != '\0' && col < stops[c] + 2 + w) {
                    char tmp[5] = {0};
                    int len = 1;
                    unsigned char ch = (unsigned char)*p;
                    tui_cell_t *cell;

                    if ((ch & 0xE0) == 0xC0) len = 2;
                    else if ((ch & 0xF0) == 0xE0) len = 3;
                    else if ((ch & 0xF8) == 0xF0) len = 4;
                    if ((int)strlen(p) < len) len = 1;
                    memcpy(tmp, p, (size_t)len);
                    cell = cell_at(row, col);
                    tui_cell_set(cell, tmp, rfg, rbg);
                    if ((header && r == 0) || rbold) cell->attr |= 1; /* bold header/selection */
                    p += len;
                    col++;
                }
                for (; col < stops[c] + 2 + w; col++) {
                    tui_cell_set(cell_at(row, col), " ", rfg, rbg);
                }
            }
            tui_cell_set(cell_at(row, stops[c] + 2 + w), " ", rfg, rbg);
            tui_cell_set(cell_at(row, stops[c] + 3 + w), SH_BOX_V, fg, bg);
        }
        /* Separator under every row: header gets the same single rule. */
        if (sep_row > s_rows) break;
        if (r == nrows - 1) {
            tui_table_border(sep_row, x, total_w, &stops[1], ncols - 1,
                             SH_BOX_BL, SH_BOX_B, SH_BOX_BR, SH_BOX_H, fg, bg);
        } else {
            tui_table_border(sep_row, x, total_w, &stops[1], ncols - 1,
                             SH_BOX_L, SH_BOX_X, SH_BOX_R, SH_BOX_H, fg, bg);
        }
    }
}

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
    // Build recolor buffer: each fg run emits "#rrggbb " + utf8 + " #";
    // `#` cells split colored runs (close/escape/reopen), so budget 16
    // bytes per cell worst case. Rows separated by "\n".
    size_t cap = (size_t)s_rows * (size_t)(s_cols * 16 + 1) + 1;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(cap);
    if (!buf) return;
    size_t pos = 0;
    for (int r = 1; r <= s_rows; r++) {
        int c = 1;
        while (c <= s_cols) {
            tui_cell_t *cell = cell_at(r, c);
            /* Effective fg: bold on a stock color renders bright (recolor
             * markup has no bold/italic/underline; dim/italic/underline keep
             * their base color). No cell writer sets attrs yet — forward-
             * compatible for Markdown TUI targeting. */
            uint8_t fg = cell->fg;
            if ((cell->attr & 1) != 0 && fg < 8) {
                fg += 8;
            }
            /* Coalesce same effective fg (attrs beyond bold ride along;
             * only the lead cell's attr mattered above). */
            int run = 1;
            while (c + run <= s_cols) {
                tui_cell_t *n = cell_at(r, c + run);
                uint8_t nfg = n->fg;
                if (((n->attr & 1) != 0) && nfg < 8) {
                    nfg += 8;
                }
                if (nfg != fg) break;
                run++;
            }
            /* Emit one fg run. A `#` cell inside a COLORED run must split
             * it: in TEXT_INPUT the first `#` always closes the color, so
             * close, emit the cell untagged (`##` is the WAIT-state literal
             * escape), and reopen. Untagged runs (fg 16) just double `#`. */
            bool tagged = (fg < 16);
            uint32_t col = 0;

            if (tagged) {
                col = ansi_get_palette_color((ansi_color_index_t)fg);
                if (col == 0 && fg != 0) col = ansi_get_palette_color(ANSI_COLOR_WHITE);
                int n = snprintf(buf + pos, cap - pos, "#%06x ", (unsigned)(col & 0xFFFFFF));
                if (n < 0) break;
                if ((size_t)n >= cap - pos) { pos = cap - 1; break; }
                pos += (size_t)n;
            }
            for (int k = 0; k < run; k++) {
                tui_cell_t *cc = cell_at(r, c + k);
                const char *utf8 = cc->utf8[0] ? cc->utf8 : " ";

                if (tagged && strchr(utf8, '#') != NULL) {
                    int n;

                    if (pos + 1 < cap) buf[pos++] = '#'; /* close */
                    for (const char *q = utf8; *q != '\0' && pos + 2 < cap; q++) {
                        buf[pos++] = *q;
                        if (*q == '#') buf[pos++] = '#'; /* WAIT escape */
                    }
                    n = snprintf(buf + pos, cap - pos, "#%06x ", (unsigned)(col & 0xFFFFFF));
                    if (n < 0) break;
                    if ((size_t)n >= cap - pos) { pos = cap - 1; break; }
                    pos += (size_t)n;
                    continue;
                }
                for (const char *q = utf8; *q != '\0' && pos + 2 < cap; q++) {
                    buf[pos++] = *q;
                    if (*q == '#') buf[pos++] = '#'; /* WAIT escape */
                }
            }
            if (tagged) {
                if (pos + 2 < cap) { buf[pos++] = ' '; buf[pos++] = '#'; }
            }
            c += run;
        }
        if (r < s_rows && pos + 1 < cap) buf[pos++] = '\n';
    }
    if (pos >= cap) pos = cap - 1;
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

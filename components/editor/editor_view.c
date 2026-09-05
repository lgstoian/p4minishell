/**
 * @file editor_view.c
 * @brief LVGL surface for the P4MiniShell editor.
 *
 * Renders an editor_doc_t as syntax-coloured rows inside the scrollable
 * editor surface (the transcript container, aliased by the window manager in
 * editor mode). A dedicated span group renders one row per document line, a
 * block cursor with a blink timer marks the caret, and a background overlay
 * highlights the active selection. The view is modal: while open it owns the
 * transcript region and the input row becomes a status bar.
 *
 * The editor owns a small "prompt mode" (Find / Replace / Go-to-Line /
 * Save As / Quit confirmation) rendered into the status bar, mirroring the
 * DOS EDIT search menus. USB and OSK keys are routed through the public key
 * handlers; serial lines are routed through command.c.
 *
 * Thread safety: every function here must run on the LVGL task.
 */

#include "editor_view.h"
#include "editor.h"
#include "windows.h"
#include "keyboard.h"
#include "ansi.h"
#include "shell.h"
#include "modal.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "p4minishell_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define EDITOR_VIEW_TAG  P4_CONFIG_SHELL_TAG

/* Session event bits. MODAL_EVENT_* live in modal.h; editor keeps SAVE. */
#define EDITOR_EVENT_SAVE   (1 << 2)

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

/** Inline prompt modes shown in the editor's status bar. */
typedef enum {
    EDITOR_PROMPT_NONE = 0,          /**< No prompt active */
    EDITOR_PROMPT_FIND,              /**< "Find: <text>" */
    EDITOR_PROMPT_REPLACE_FIND,      /**< "Replace: <find>" */
    EDITOR_PROMPT_REPLACE_WITH,      /**< "With: <replacement>" */
    EDITOR_PROMPT_GOTO,              /**< "Go to line: <n>" */
    EDITOR_PROMPT_SAVE_AS,           /**< "Save As: <path>" */
    EDITOR_PROMPT_QUIT_CONFIRM,      /**< "Quit without saving? (Y/N)" */
} editor_prompt_t;

static struct {
    bool open;
    editor_doc_t *doc;
    editor_control_t *control;
    lv_obj_t *surface;          /* Scrollable container (the transcript region) */
    lv_obj_t *spans;            /* Dedicated span group child of the surface */
    lv_obj_t *cursor;           /* Block cursor child of the surface */
    lv_timer_t *cursor_timer;   /* Cursor blink timer */
    bool cursor_visible;
    lv_obj_t *sel_layer;        /* Selection overlay container (child of surface) */
    lv_obj_t *current_line;     /* Current-line highlight rect (behind the text) */
    lv_obj_t *status;           /* Status bar label (owned by windows) */
    bool touch_selecting;       /* Drag-select gesture in progress */
    bool shift_held;            /* USB Shift modifier on the current key */

    /* Prompt / find / replace state. */
    editor_prompt_t prompt;
    char prompt_buf[P4_CONFIG_EDITOR_PROMPT_BYTES];
    size_t prompt_len;
    char find_str[P4_CONFIG_EDITOR_FIND_BYTES];
    size_t find_len;
    char replace_str[P4_CONFIG_EDITOR_FIND_BYTES];
    size_t replace_len;
    bool replace_armed;         /* Enter repeats the last replace */

    /* Per-row display width cache (cumulative pixel offsets per column). */
    size_t width_row;
    lv_coord_t *widths;
    size_t width_count;
} s_editor_view = {
    .open = false,
    .doc = NULL,
    .control = NULL,
    .surface = NULL,
    .spans = NULL,
    .cursor = NULL,
    .cursor_timer = NULL,
    .cursor_visible = true,
    .sel_layer = NULL,
    .current_line = NULL,
    .status = NULL,
    .touch_selecting = false,
    .shift_held = false,
    .prompt = EDITOR_PROMPT_NONE,
    .prompt_len = 0,
    .find_len = 0,
    .replace_len = 0,
    .replace_armed = false,
    .width_row = (size_t)-1,
    .widths = NULL,
    .width_count = 0,
};

/* Forward declarations (mutual recursion between rendering and editing). */
static void editor_rebuild(void);
static void editor_update_cursor(void);
static void editor_update_current_line(void);
static void editor_build_selection(void);
static void editor_ensure_cursor_visible(void);
static void editor_status_default(void);
static void editor_layout_update(void);
static void editor_touch_event_cb(lv_event_t *event);
static void editor_quit_signal(void);

/* ========================================================================
 * GEOMETRY
 * ======================================================================== */

static lv_coord_t editor_line_height(void)
{
    const lv_font_t *font = windows_get_terminal_font();
    if (font != NULL) {
        lv_coord_t lh = lv_font_get_line_height(font);
        if (lh > 0) {
            return lh + 2; /* a little breathing room between rows */
        }
    }
    return P4_CONFIG_EDITOR_LINE_HEIGHT;
}

/** Width of one monospace terminal cell (fallback for proportional fonts). */
static lv_coord_t editor_cell_width(void)
{
    const lv_font_t *font = windows_get_terminal_font();
    uint32_t w = font != NULL ? lv_font_get_glyph_width(font, 'M', '\0') : 8;
    return (lv_coord_t)w;
}

/** Pixel width of the line-number gutter (0 when line numbers are off). The
 *  gutter occupies P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS digits plus one
 *  trailing space. */
static lv_coord_t editor_gutter_width(void)
{
#if P4_CONFIG_EDITOR_LINE_NUMBERS
    return (lv_coord_t)((P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS + 1) *
                        (size_t)editor_cell_width());
#else
    return 0;
#endif
}

/** Drop the cached width table (must run after any content change). */
static void editor_width_invalidate(void)
{
    s_editor_view.width_row = (size_t)-1;
}

/**
 * Build the cumulative display-width table for @p row: widths[col] is the
 * pixel offset of byte column @p col. Tabs expand to the next tab stop so
 * the caret lands on a visual tab stop exactly as it does in DOS EDIT.
 */
static void editor_width_cache(size_t row)
{
    editor_doc_t *doc = s_editor_view.doc;
    const lv_font_t *font = windows_get_terminal_font();
    size_t len;
    size_t i;
    size_t cell = 0;
    lv_coord_t acc = 0;
    lv_coord_t *w;
    const char *text;

    if (doc == NULL) {
        return;
    }
    if (s_editor_view.widths != NULL && s_editor_view.width_row == row) {
        return;
    }
    len = editor_doc_line_length(doc, row);
    w = realloc(s_editor_view.widths, (len + 1) * sizeof(lv_coord_t));
    if (w == NULL) {
        return;
    }
    s_editor_view.widths = w;
    s_editor_view.width_row = row;
    s_editor_view.width_count = len;
    w[0] = 0;
    if (len == 0) {
        return;
    }

    text = editor_doc_line_text(doc, row);
    for (i = 0; i < len; i++) {
        lv_coord_t cw;
        if (text[i] == '\t') {
            size_t to_stop = P4_CONFIG_EDITOR_TAB_WIDTH -
                             (cell % P4_CONFIG_EDITOR_TAB_WIDTH);
            cw = (lv_coord_t)(to_stop * (size_t)editor_cell_width());
            cell += to_stop;
        } else {
            cw = font != NULL ? (lv_coord_t)lv_font_get_glyph_width(
                                   font, (uint32_t)(uint8_t)text[i], '\0')
                              : editor_cell_width();
            cell++;
        }
        acc += cw;
        w[i + 1] = acc;
    }
}

/** Display pixel offset of @p col within @p row (clamped to the line). */
static lv_coord_t editor_measure_prefix(const editor_doc_t *doc, size_t row, size_t col)
{
    editor_width_cache(row);
    if (s_editor_view.widths != NULL && s_editor_view.width_row == row) {
        if (col > s_editor_view.width_count) {
            col = s_editor_view.width_count;
        }
        return s_editor_view.widths[col];
    }
    return 0;
}

/** Map a pixel x within a row to the nearest byte column (binary search). */
static size_t editor_column_from_x(const editor_doc_t *doc, size_t row, lv_coord_t x)
{
    size_t lo, hi;
    size_t len;

    if (doc == NULL) {
        return 0;
    }
    if (x <= 0) {
        return 0;
    }
    editor_width_cache(row);
    if (s_editor_view.widths == NULL || s_editor_view.width_row != row) {
        return 0;
    }
    len = s_editor_view.width_count;
    if (x >= s_editor_view.widths[len]) {
        return len;
    }
    /* First column whose offset is >= x. */
    lo = 0;
    hi = len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_editor_view.widths[mid] < x) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return 0;
    }
    if ((x - s_editor_view.widths[lo - 1]) < (s_editor_view.widths[lo] - x)) {
        return lo - 1;
    }
    return lo;
}

/* ========================================================================
 * SPAN RENDERING (one row per document line, no wrapping)
 * ======================================================================== */

/* Deferred rebuild trampoline: builds spans on a fresh LVGL handler pass. */
static void editor_rebuild_cb(void *user_data)
{
    (void)user_data;
    editor_rebuild();
}

/** Delete every span currently held by @p group. */
static void editor_clear_spans(lv_obj_t *group)
{
    uint32_t count = lv_spangroup_get_span_count(group);
    uint32_t i;

    for (i = 0; i < count; i++) {
        lv_span_t *span = lv_spangroup_get_child(group, 0);
        if (span != NULL) {
            lv_spangroup_delete_span(group, span);
        }
    }
}

/** Append one coloured run of @p row to the span group. */
static void editor_add_run(const char *text, size_t len,
                           lv_color_t color)
{
    lv_span_t *span;
    lv_style_t *style;
    char *buf;

    if (len == 0) {
        return;
    }
    buf = malloc(len + 1);
    if (buf == NULL) {
        return;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    span = lv_spangroup_add_span(s_editor_view.spans);
    if (span == NULL) {
        free(buf);
        return;
    }
    lv_span_set_text(span, buf);
    style = lv_span_get_style(span);
    lv_style_set_text_color(style, color);
    lv_style_set_text_font(style, windows_get_terminal_font());
    free(buf);
}

/** Render one document row as syntax-coloured spans plus a line break. */
static void editor_render_row(editor_doc_t *doc, size_t row)
{
    const char *text = editor_doc_line_text(doc, row);
    size_t len = editor_doc_line_length(doc, row);
    editor_syntax_run_t runs[64];
    size_t run_count = 0;
    size_t covered = 0;
    size_t i;

#if P4_CONFIG_EDITOR_LINE_NUMBERS
    /* Line-number gutter: a muted, right-aligned "N " prefix before the
     * content, rendered as part of the same visual row. */
    {
        char gutter[P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS + 2];
        size_t g = editor_format_line_number(row, P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS,
                                             gutter, sizeof(gutter));
        editor_add_run(gutter, g,
                       lv_color_hex(ansi_get_palette_color(ANSI_COLOR_BRIGHT_BLACK)));
    }
#endif

    if (len == 0) {
        return; /* The '\n' separator still advances the row height. */
    }

    if (doc->syntax == EDITOR_SYNTAX_BATCH && P4_CONFIG_EDITOR_SYNTAX_BATCH) {
        run_count = editor_lex_batch(text, len, runs, 64);
    }
    if (run_count == 0) {
        runs[0].start = 0;
        runs[0].length = len;
        runs[0].color = ANSI_COLOR_BRIGHT_WHITE;
        run_count = 1;
    }

    for (i = 0; i < run_count; i++) {
        size_t start = runs[i].start;
        size_t rlen = runs[i].length;
        lv_color_t color = lv_color_hex(ansi_get_palette_color(runs[i].color));

        if (start > len) {
            start = len;
        }
        if (start + rlen > len) {
            rlen = len - start;
        }
        /* Any gap before this run (e.g. a run buffer that filled early) is
         * rendered as plain text so no byte of the line is ever dropped. */
        if (covered < start) {
            editor_add_run(text + covered, start - covered,
                           lv_color_hex(ansi_get_palette_color(ANSI_COLOR_BRIGHT_WHITE)));
        }
        if (rlen > 0) {
            editor_add_run(text + start, rlen, color);
            covered = start + rlen;
        }
    }
    if (covered < len) {
        editor_add_run(text + covered, len - covered,
                       lv_color_hex(ansi_get_palette_color(ANSI_COLOR_BRIGHT_WHITE)));
    }
}

/** Rebuild the whole editor content (spans, selection, cursor). */
static void editor_rebuild(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t count;
    size_t row;
    lv_coord_t lh = editor_line_height();

    if (s_editor_view.spans == NULL || doc == NULL) {
        return;
    }

    editor_width_invalidate();
    editor_clear_spans(s_editor_view.spans);

    count = editor_doc_line_count(doc);
    for (row = 0; row < count; row++) {
        editor_render_row(doc, row);
        if (row + 1 < count) {
            lv_span_t *sep = lv_spangroup_add_span(s_editor_view.spans);
            if (sep != NULL) {
                lv_style_t *style = lv_span_get_style(sep);
                lv_span_set_text(sep, "\n");
                lv_style_set_text_color(style, lv_color_hex(
                    ansi_get_palette_color(ANSI_COLOR_BRIGHT_WHITE)));
                lv_style_set_text_font(style, windows_get_terminal_font());
            }
        }
    }

    /* Size the span group to exactly its row height so the container sees a
     * taller-than-itself child and scrolls through it (never LV_SIZE_CONTENT:
     * see the transcript container notes in windows.c). */
    lv_obj_set_height(s_editor_view.spans, (lv_coord_t)(count * (size_t)lh));
    if (s_editor_view.sel_layer != NULL) {
        lv_obj_set_size(s_editor_view.sel_layer,
                        lv_obj_get_width(s_editor_view.surface),
                        (lv_coord_t)(count * (size_t)lh));
    }
    if (s_editor_view.current_line != NULL) {
        lv_obj_set_width(s_editor_view.current_line,
                         lv_obj_get_width(s_editor_view.surface));
    }
    lv_obj_update_layout(s_editor_view.surface);

    editor_update_cursor();
    editor_update_current_line();
    editor_build_selection();
    editor_ensure_cursor_visible();
    editor_status_default();
}

/* ========================================================================
 * CURSOR + SELECTION OVERLAY
 * ======================================================================== */

static void editor_cursor_blink_cb(lv_timer_t *timer)
{
    (void)timer;
    s_editor_view.cursor_visible = !s_editor_view.cursor_visible;
    if (s_editor_view.cursor != NULL) {
        if (s_editor_view.cursor_visible) {
            lv_obj_remove_flag(s_editor_view.cursor, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_editor_view.cursor, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/** Position the block cursor and restart its blink phase. */
static void editor_update_cursor(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    lv_coord_t lh = editor_line_height();
    lv_coord_t x, y;

    if (s_editor_view.cursor == NULL || doc == NULL) {
        return;
    }

    /* The text starts after the line-number gutter, so the caret sits on the
     * document column, past the gutter. */
    x = editor_gutter_width() + editor_measure_prefix(doc,
                editor_doc_cursor_row(doc), editor_doc_cursor_col(doc));
    y = (lv_coord_t)(editor_doc_cursor_row(doc) * (size_t)lh);
    lv_obj_set_pos(s_editor_view.cursor, x, y);

    s_editor_view.cursor_visible = true;
    if (s_editor_view.cursor_timer != NULL) {
        lv_timer_reset(s_editor_view.cursor_timer);
    }
    lv_obj_remove_flag(s_editor_view.cursor, LV_OBJ_FLAG_HIDDEN);
}

/** Reposition the current-line highlight behind the cursor row. */
static void editor_update_current_line(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    lv_coord_t lh = editor_line_height();

    if (s_editor_view.current_line == NULL || doc == NULL) {
        return;
    }
    lv_obj_set_y(s_editor_view.current_line,
                 (lv_coord_t)(editor_doc_cursor_row(doc) * (size_t)lh));
}

/** Rebuild the selection background overlay from the document selection. */
static void editor_build_selection(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    lv_coord_t lh = editor_line_height();
    size_t s_row, s_col, e_row, e_col;
    size_t row;
    uint32_t child_count;
    uint32_t i;

    if (s_editor_view.sel_layer == NULL) {
        return;
    }

    child_count = lv_obj_get_child_cnt(s_editor_view.sel_layer);
    for (i = 0; i < child_count; i++) {
        lv_obj_delete(lv_obj_get_child(s_editor_view.sel_layer, 0));
    }

    if (doc == NULL || !editor_doc_has_selection(doc)) {
        lv_obj_add_flag(s_editor_view.sel_layer, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_editor_view.sel_layer, LV_OBJ_FLAG_HIDDEN);

    editor_doc_selection_bounds(doc, &s_row, &s_col, &e_row, &e_col);
    for (row = s_row; row <= e_row; row++) {
        size_t len = editor_doc_line_length(doc, row);
        size_t c0 = (row == s_row) ? s_col : 0;
        size_t c1 = (row == e_row) ? e_col : len;
        lv_coord_t x0, x1;

        if (c0 > len) c0 = len;
        if (c1 > len) c1 = len;
        if (c1 < c0) c1 = c0;

        /* The selection spans document columns; shift it past the gutter. */
        x0 = editor_gutter_width() + editor_measure_prefix(doc, row, c0);
        x1 = editor_gutter_width() + editor_measure_prefix(doc, row, c1);

        {
            lv_obj_t *rect = lv_obj_create(s_editor_view.sel_layer);
            lv_obj_remove_flag(rect, LV_OBJ_FLAG_SCROLLABLE |
                                     LV_OBJ_FLAG_CLICKABLE |
                                     LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_set_style_bg_color(rect,
                lv_color_hex(P4_CONFIG_EDITOR_SELECTION_COLOR), 0);
            lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(rect, 0, 0);
            lv_obj_set_style_radius(rect, 0, 0);
            lv_obj_set_pos(rect, x0, (lv_coord_t)(row * (size_t)lh));
            lv_obj_set_size(rect, x1 - x0, lh);
        }
    }
}

/** Scroll the surface so the cursor row is always on screen. */
static void editor_ensure_cursor_visible(void)
{
    lv_obj_t *surface = s_editor_view.surface;
    editor_doc_t *doc = s_editor_view.doc;
    lv_coord_t lh = editor_line_height();
    lv_coord_t scroll_y;
    lv_coord_t view_h;
    lv_coord_t cy;

    if (surface == NULL || doc == NULL) {
        return;
    }
    lv_obj_update_layout(surface);
    scroll_y = lv_obj_get_scroll_y(surface);
    view_h = lv_obj_get_height(surface);
    cy = (lv_coord_t)(editor_doc_cursor_row(doc) * (size_t)lh);

    if (cy < scroll_y) {
        lv_obj_scroll_to_y(surface, cy, LV_ANIM_OFF);
    } else if (cy + lh > scroll_y + view_h) {
        lv_obj_scroll_to_y(surface, cy + lh - view_h, LV_ANIM_OFF);
    }
}

/* ========================================================================
 * STATUS BAR + PROMPT
 * ======================================================================== */

/** Paint a literal status-bar string (never a format). */
static void editor_status_raw(const char *text)
{
    if (s_editor_view.status != NULL && text != NULL) {
        lv_label_set_text(s_editor_view.status, text);
    }
}

/** Paint the status bar (printf-style; the result is a literal string). */
static void editor_status(const char *format, ...)
{
    va_list args;
    char *buf;

    if (s_editor_view.status == NULL) {
        return;
    }
    buf = malloc(P4_CONFIG_SD_PATH_BYTES + 96);
    if (buf == NULL) {
        return;
    }
    va_start(args, format);
    vsnprintf(buf, P4_CONFIG_SD_PATH_BYTES + 96, format, args);
    va_end(args);
    editor_status_raw(buf);
    free(buf);
}

/** Render the status bar (or the active prompt) on the input-row label. */
static void editor_status_default(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    char *buf = malloc(P4_CONFIG_SD_PATH_BYTES + 96);

    if (doc == NULL || buf == NULL) {
        free(buf);
        return;
    }

    if (s_editor_view.prompt != EDITOR_PROMPT_NONE) {
        const char *label = "?";
        switch (s_editor_view.prompt) {
        case EDITOR_PROMPT_FIND:         label = "Find"; break;
        case EDITOR_PROMPT_REPLACE_FIND: label = "Replace"; break;
        case EDITOR_PROMPT_REPLACE_WITH: label = "With"; break;
        case EDITOR_PROMPT_GOTO:         label = "Go to line"; break;
        case EDITOR_PROMPT_SAVE_AS:      label = "Save As"; break;
        case EDITOR_PROMPT_QUIT_CONFIRM: label = "Quit without saving"; break;
        default: break;
        }
        s_editor_view.prompt_buf[s_editor_view.prompt_len] = '\0';
        if (s_editor_view.prompt == EDITOR_PROMPT_QUIT_CONFIRM) {
            snprintf(buf, P4_CONFIG_SD_PATH_BYTES + 96, "%s?  (Y/N)",
                     label);
        } else {
            snprintf(buf, P4_CONFIG_SD_PATH_BYTES + 96, "%s: %s_",
                     label, s_editor_view.prompt_buf);
        }
        editor_status_raw(buf);
        free(buf);
        return;
    }

    {
        const char *name = doc->path[0] != '\0' ? doc->path : "(unnamed)";
        snprintf(buf, P4_CONFIG_SD_PATH_BYTES + 96, "%s  %s  Ln %u, Col %u  %s",
                 name,
                 doc->modified ? "*" : " ",
                 (unsigned)(editor_doc_cursor_row(doc) + 1),
                 (unsigned)(editor_doc_cursor_col(doc) + 1),
                 doc->overwrite ? "OVR" : "INS");
        editor_status_raw(buf);
    }
    free(buf);
}

/** Enter an inline prompt (clearing any previous one). */
static void editor_prompt_begin(editor_prompt_t type)
{
    s_editor_view.prompt = type;
    s_editor_view.prompt_len = 0;
    s_editor_view.prompt_buf[0] = '\0';

    if (type == EDITOR_PROMPT_SAVE_AS && s_editor_view.doc != NULL) {
        /* Pre-fill with the current path so it is easy to adjust. */
        const char *cur = s_editor_view.doc->path;
        size_t n = cur != NULL ? strlen(cur) : 0;
        if (n >= P4_CONFIG_EDITOR_PROMPT_BYTES) {
            n = P4_CONFIG_EDITOR_PROMPT_BYTES - 1;
        }
        if (n > 0) {
            memcpy(s_editor_view.prompt_buf, cur, n);
            s_editor_view.prompt_len = n;
            s_editor_view.prompt_buf[n] = '\0';
        }
    }
    editor_status_default();
}

/** Cancel the active prompt and restore the ordinary status line. */
static void editor_prompt_cancel(void)
{
    s_editor_view.prompt = EDITOR_PROMPT_NONE;
    s_editor_view.replace_armed = false;
    editor_status_default();
}

/** Handle one printable character while a prompt is active. */
static bool editor_prompt_handle_char(char ch)
{
    editor_prompt_t p = s_editor_view.prompt;

    if (p == EDITOR_PROMPT_NONE) {
        return false;
    }

    if (p == EDITOR_PROMPT_QUIT_CONFIRM) {
        if (ch == 'y' || ch == 'Y') {
            s_editor_view.prompt = EDITOR_PROMPT_NONE;
            editor_quit_signal();
        } else if (ch == 'n' || ch == 'N') {
            s_editor_view.prompt = EDITOR_PROMPT_NONE;
            editor_status("quit cancelled");
        }
        return true;
    }

    if (ch == '\b') {
        if (s_editor_view.prompt_len > 0) {
            s_editor_view.prompt_len--;
        }
        s_editor_view.prompt_buf[s_editor_view.prompt_len] = '\0';
        editor_status_default();
        return true;
    }

    if (p == EDITOR_PROMPT_GOTO) {
        if (ch >= '0' && ch <= '9' &&
            s_editor_view.prompt_len < P4_CONFIG_EDITOR_PROMPT_BYTES - 1) {
            s_editor_view.prompt_buf[s_editor_view.prompt_len++] = ch;
            s_editor_view.prompt_buf[s_editor_view.prompt_len] = '\0';
        }
        editor_status_default();
        return true;
    }

    /* Find/Replace strings are copied into a smaller buffer
     * (P4_CONFIG_EDITOR_FIND_BYTES); cap the prompt at that length so a long
     * search cannot overflow find_str / replace_str. */
    {
        size_t max_len = P4_CONFIG_EDITOR_PROMPT_BYTES - 1;
        if (p == EDITOR_PROMPT_FIND ||
            p == EDITOR_PROMPT_REPLACE_FIND ||
            p == EDITOR_PROMPT_REPLACE_WITH) {
            if (max_len > P4_CONFIG_EDITOR_FIND_BYTES - 1) {
                max_len = P4_CONFIG_EDITOR_FIND_BYTES - 1;
            }
        }
        if (ch >= 0x20 && ch <= 0x7E && s_editor_view.prompt_len < max_len) {
            s_editor_view.prompt_buf[s_editor_view.prompt_len++] = ch;
            s_editor_view.prompt_buf[s_editor_view.prompt_len] = '\0';
        }
    }
    editor_status_default();
    return true;
}

/* ========================================================================
 * FIND / REPLACE / GOTO / SAVE AS / QUIT
 * ======================================================================== */

static void editor_find_next(bool advance)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t row, col;

    if (doc == NULL || s_editor_view.find_len == 0) {
        editor_status("no search");
        return;
    }

    row = editor_doc_cursor_row(doc);
    col = editor_doc_cursor_col(doc);
    if (advance) {
        if (col + 1 <= editor_doc_line_length(doc, row)) {
            col++;
        } else if (row + 1 < editor_doc_line_count(doc)) {
            row++;
            col = 0;
        } else {
            row = 0;
            col = 0;
        }
    }

    if (editor_doc_find_next(doc, s_editor_view.find_str, s_editor_view.find_len,
                             row, col, P4_CONFIG_EDITOR_FIND_CASE_SENSITIVE,
                             true, &row, &col)) {
        doc->cursor_row = row;
        doc->cursor_col = col;
        editor_layout_update();
        editor_status("found at Ln %u, Col %u",
                      (unsigned)(row + 1), (unsigned)(col + 1));
    } else {
        editor_status("not found: %s", s_editor_view.find_str);
    }
}

static void editor_replace_next(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t row, col;

    if (doc == NULL || s_editor_view.find_len == 0) {
        editor_status("no search");
        return;
    }

    if (editor_doc_replace_next(doc, s_editor_view.find_str, s_editor_view.find_len,
                                s_editor_view.replace_str, s_editor_view.replace_len,
                                P4_CONFIG_EDITOR_FIND_CASE_SENSITIVE,
                                &row, &col)) {
        s_editor_view.replace_armed = true;
        editor_rebuild();
        editor_status("replaced at Ln %u, Col %u  (Enter: next)",
                      (unsigned)(row + 1), (unsigned)(col + 1));
    } else {
        s_editor_view.replace_armed = false;
        editor_status("no more matches");
    }
}

static void editor_goto_line(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    unsigned long line;

    s_editor_view.prompt_buf[s_editor_view.prompt_len] = '\0';
    line = strtoul(s_editor_view.prompt_buf, NULL, 10);
    s_editor_view.prompt = EDITOR_PROMPT_NONE;

    if (line == 0) {
        line = 1;
    }
    if (line > editor_doc_line_count(doc)) {
        line = editor_doc_line_count(doc);
    }
    if (line > 0) {
        doc->cursor_row = line - 1;
        doc->cursor_col = 0;
        editor_layout_update();
        editor_status("Ln %lu", line);
    } else {
        editor_status_default();
    }
}

static void editor_save_as(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    editor_control_t *ctl = s_editor_view.control;

    s_editor_view.prompt_buf[s_editor_view.prompt_len] = '\0';
    s_editor_view.prompt = EDITOR_PROMPT_NONE;
    if (doc == NULL || ctl == NULL) {
        return;
    }
    if (s_editor_view.prompt_len == 0) {
        editor_status("save as: no path");
        return;
    }
    snprintf(ctl->save_as_path, sizeof(ctl->save_as_path), "%s",
             s_editor_view.prompt_buf);
    ctl->save_requested = true;
    if (ctl->event_group != NULL) {
        xEventGroupSetBits((EventGroupHandle_t)ctl->event_group, EDITOR_EVENT_SAVE);
    }
    editor_status("saving to %s...", s_editor_view.prompt_buf);
}

/** Commit the active prompt (Enter). */
static void editor_prompt_commit(void)
{
    switch (s_editor_view.prompt) {
    case EDITOR_PROMPT_FIND:
        if (s_editor_view.prompt_len > 0) {
            memcpy(s_editor_view.find_str, s_editor_view.prompt_buf,
                   s_editor_view.prompt_len + 1);
            s_editor_view.find_len = s_editor_view.prompt_len;
            s_editor_view.prompt = EDITOR_PROMPT_NONE;
            editor_find_next(false);
        }
        break;
    case EDITOR_PROMPT_REPLACE_FIND:
        if (s_editor_view.prompt_len > 0) {
            memcpy(s_editor_view.find_str, s_editor_view.prompt_buf,
                   s_editor_view.prompt_len + 1);
            s_editor_view.find_len = s_editor_view.prompt_len;
            s_editor_view.prompt = EDITOR_PROMPT_REPLACE_WITH;
            s_editor_view.prompt_len = 0;
            s_editor_view.prompt_buf[0] = '\0';
            editor_status_default();
        }
        break;
    case EDITOR_PROMPT_REPLACE_WITH:
        memcpy(s_editor_view.replace_str, s_editor_view.prompt_buf,
               s_editor_view.prompt_len + 1);
        s_editor_view.replace_len = s_editor_view.prompt_len;
        s_editor_view.prompt = EDITOR_PROMPT_NONE;
        editor_replace_next();
        break;
    case EDITOR_PROMPT_GOTO:
        editor_goto_line();
        break;
    case EDITOR_PROMPT_SAVE_AS:
        editor_save_as();
        break;
    default:
        s_editor_view.prompt = EDITOR_PROMPT_NONE;
        break;
    }
}

/** Signal the worker task to quit (Esc or confirmed quit). */
static void editor_quit_signal(void)
{
    editor_control_t *ctl = s_editor_view.control;
    if (ctl != NULL) {
        ctl->quit_requested = true;
        if (ctl->event_group != NULL) {
            xEventGroupSetBits((EventGroupHandle_t)ctl->event_group, MODAL_EVENT_CLOSE_REQUEST);
        }
    }
}

/* ========================================================================
 * KEY HANDLING
 * ======================================================================== */

/** Reposition the cursor and selection without rebuilding the text. */
static void editor_layout_update(void)
{
    editor_update_cursor();
    editor_update_current_line();
    editor_build_selection();
    editor_ensure_cursor_visible();
    editor_status_default();
}

/** Move the cursor one page up / down (DOS EDIT PageUp/PageDown). */
static void editor_page_move(int delta)
{
    editor_doc_t *doc = s_editor_view.doc;
    lv_coord_t lh = editor_line_height();
    size_t lines;
    size_t i;

    if (doc == NULL || s_editor_view.surface == NULL) {
        return;
    }
    lines = (size_t)(lv_obj_get_height(s_editor_view.surface) / lh);
    if (lines < 1) {
        lines = 1;
    }
    for (i = 0; i < lines; i++) {
        if (delta > 0) {
            if (editor_doc_cursor_row(doc) + 1 < editor_doc_line_count(doc)) {
                editor_doc_cursor_down(doc);
            } else {
                break;
            }
        } else {
            if (editor_doc_cursor_row(doc) > 0) {
                editor_doc_cursor_up(doc);
            } else {
                break;
            }
        }
    }
}

static void editor_move_cursor(editor_key_t key)
{
    editor_doc_t *doc = s_editor_view.doc;
    bool sh = s_editor_view.shift_held;

    if (doc == NULL) {
        return;
    }

    if (sh && !doc->selection_active) {
        editor_doc_selection_begin(doc);
    }

    switch (key) {
    case EDITOR_KEY_LEFT:      editor_doc_cursor_left(doc); break;
    case EDITOR_KEY_RIGHT:     editor_doc_cursor_right(doc); break;
    case EDITOR_KEY_UP:        editor_doc_cursor_up(doc); break;
    case EDITOR_KEY_DOWN:      editor_doc_cursor_down(doc); break;
    case EDITOR_KEY_HOME:      editor_doc_cursor_home(doc); break;
    case EDITOR_KEY_END:       editor_doc_cursor_end(doc); break;
    case EDITOR_KEY_WORD_LEFT: editor_doc_cursor_word_left(doc); break;
    case EDITOR_KEY_WORD_RIGHT: editor_doc_cursor_word_right(doc); break;
    case EDITOR_KEY_DOC_HOME:  editor_doc_cursor_doc_home(doc); break;
    case EDITOR_KEY_DOC_END:   editor_doc_cursor_doc_end(doc); break;
    case EDITOR_KEY_PAGE_UP:   editor_page_move(-1); break;
    case EDITOR_KEY_PAGE_DOWN: editor_page_move(1); break;
    default:
        break;
    }

    if (sh) {
        editor_doc_selection_extend(doc);
    } else {
        editor_doc_selection_clear(doc);
    }
    editor_layout_update();
}

/** Route a request-to-save to the worker (Save As for unnamed buffers). */
static void editor_request_save(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    editor_control_t *ctl = s_editor_view.control;

    if (doc == NULL || ctl == NULL) {
        return;
    }
    if (doc->path[0] == '\0') {
        editor_prompt_begin(EDITOR_PROMPT_SAVE_AS);
        return;
    }
    ctl->save_requested = true;
    if (ctl->event_group != NULL) {
        xEventGroupSetBits((EventGroupHandle_t)ctl->event_group, EDITOR_EVENT_SAVE);
    }
    editor_status("saving...");
}

static void editor_apply_key(editor_key_t key, char ch)
{
    editor_doc_t *doc = s_editor_view.doc;

    if (doc == NULL) {
        return;
    }

    /* While an inline prompt is active, Enter / Backspace / Esc drive the
     * prompt instead of the document. */
    if (s_editor_view.prompt != EDITOR_PROMPT_NONE) {
        if (key == EDITOR_KEY_NEWLINE) {
            editor_prompt_commit();
            return;
        }
        if (key == EDITOR_KEY_BACKSPACE) {
            editor_prompt_handle_char('\b');
            return;
        }
        if (key == EDITOR_KEY_QUIT) {
            editor_prompt_cancel();
            return;
        }
    }

    switch (key) {
    case EDITOR_KEY_INSERT:
        if (ch >= 0x20 && ch <= 0x7E) {
            if (doc->selection_active) {
                editor_doc_selection_delete(doc);
            }
            editor_doc_undo_mark(doc);
            editor_doc_insert_char(doc, ch);
            editor_rebuild();
        }
        return;
    case EDITOR_KEY_NEWLINE:
        /* After a Replace, Enter repeats the replacement (DOS EDIT Change). */
        if (s_editor_view.replace_armed) {
            editor_replace_next();
            return;
        }
        if (doc->selection_active) {
            editor_doc_selection_delete(doc);
        }
        editor_doc_newline(doc);
        editor_rebuild();
        return;
    case EDITOR_KEY_BACKSPACE:
        editor_doc_backspace(doc);
        editor_rebuild();
        return;
    case EDITOR_KEY_DELETE:
        editor_doc_delete(doc);
        editor_rebuild();
        return;
    case EDITOR_KEY_TAB:
        editor_doc_tab(doc);
        editor_rebuild();
        return;
    case EDITOR_KEY_DELETE_LINE:
        editor_doc_delete_line(doc);
        editor_rebuild();
        return;
    case EDITOR_KEY_DELETE_EOL:
        editor_doc_delete_to_eol(doc);
        editor_rebuild();
        return;
    case EDITOR_KEY_OVERWRITE:
        editor_doc_toggle_overwrite(doc);
        editor_status_default();
        return;
    case EDITOR_KEY_LEFT:
    case EDITOR_KEY_RIGHT:
    case EDITOR_KEY_UP:
    case EDITOR_KEY_DOWN:
    case EDITOR_KEY_HOME:
    case EDITOR_KEY_END:
    case EDITOR_KEY_WORD_LEFT:
    case EDITOR_KEY_WORD_RIGHT:
    case EDITOR_KEY_DOC_HOME:
    case EDITOR_KEY_DOC_END:
    case EDITOR_KEY_PAGE_UP:
    case EDITOR_KEY_PAGE_DOWN:
        editor_move_cursor(key);
        return;
    case EDITOR_KEY_SELECT_ALL:
        editor_doc_select_all(doc);
        editor_layout_update();
        return;
    case EDITOR_KEY_COPY:
        if (editor_doc_selection_copy(doc)) {
            editor_status("selection copied");
        }
        return;
    case EDITOR_KEY_CUT:
        if (editor_doc_selection_cut(doc)) {
            editor_rebuild();
            editor_status("selection cut");
        }
        return;
    case EDITOR_KEY_PASTE:
        editor_doc_paste(doc);
        editor_rebuild();
        return;
    case EDITOR_KEY_UNDO:
        editor_doc_undo(doc);
        editor_rebuild();
        return;
    case EDITOR_KEY_REDO:
        editor_doc_redo(doc);
        editor_rebuild();
        return;
    case EDITOR_KEY_SAVE:
        editor_request_save();
        return;
    case EDITOR_KEY_SAVE_AS:
        editor_prompt_begin(EDITOR_PROMPT_SAVE_AS);
        return;
    case EDITOR_KEY_FIND:
        editor_prompt_begin(EDITOR_PROMPT_FIND);
        return;
    case EDITOR_KEY_FIND_NEXT:
        editor_find_next(true);
        return;
    case EDITOR_KEY_REPLACE:
        editor_prompt_begin(EDITOR_PROMPT_REPLACE_FIND);
        return;
    case EDITOR_KEY_GOTO_LINE:
        editor_prompt_begin(EDITOR_PROMPT_GOTO);
        return;
    case EDITOR_KEY_QUIT:
        /* Prompt-active Esc is intercepted above; here it is a plain quit. */
        editor_view_set_quit_requested();
        return;
    default:
        return;
    }
}

static void editor_apply_ascii(char ch)
{
    editor_doc_t *doc = s_editor_view.doc;

    if (doc == NULL) {
        return;
    }
    if (editor_prompt_handle_char(ch)) {
        return;
    }
    if (doc->selection_active) {
        editor_doc_selection_delete(doc);
    }
    editor_doc_undo_mark(doc);
    editor_doc_insert_char(doc, ch);
    editor_rebuild();
}

/* ========================================================================
 * PUBLIC KEY HANDLERS
 * ======================================================================== */

bool editor_view_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii)
{
    editor_key_t key;

    s_editor_view.shift_held = (modifiers & 0x02) || (modifiers & 0x20);
    key = editor_key_from_usb(key_code, modifiers, ascii);

    if (key == EDITOR_KEY_NONE) {
        if (ascii >= 0x20 && ascii <= 0x7E) {
            editor_apply_ascii(ascii);
            return true;
        }
        return false;
    }

    editor_apply_key(key, ascii);
    return true;
}

bool editor_view_handle_osk(const char *label)
{
    if (label == NULL) {
        return false;
    }

    /* LVGL symbols used by the on-screen keyboard maps. */
    if (strcmp(label, LV_SYMBOL_BACKSPACE) == 0) {
        if (s_editor_view.prompt != EDITOR_PROMPT_NONE) {
            editor_prompt_handle_char('\b');
        } else {
            editor_apply_key(EDITOR_KEY_BACKSPACE, 0);
        }
        return true;
    }
    if (strcmp(label, LV_SYMBOL_NEW_LINE) == 0 || strcmp(label, "Enter") == 0) {
        if (s_editor_view.prompt != EDITOR_PROMPT_NONE) {
            editor_prompt_commit();
        } else if (s_editor_view.replace_armed) {
            editor_replace_next();
        } else {
            editor_apply_key(EDITOR_KEY_NEWLINE, 0);
        }
        return true;
    }
    if (strcmp(label, LV_SYMBOL_OK) == 0) {
        editor_apply_key(EDITOR_KEY_NEWLINE, 0);
        return true;
    }
    if (strcmp(label, LV_SYMBOL_LEFT) == 0) {
        editor_apply_key(EDITOR_KEY_LEFT, 0);
        return true;
    }
    if (strcmp(label, LV_SYMBOL_RIGHT) == 0) {
        editor_apply_key(EDITOR_KEY_RIGHT, 0);
        return true;
    }
    if (strcmp(label, LV_SYMBOL_UP) == 0) {
        editor_apply_key(EDITOR_KEY_UP, 0);
        return true;
    }
    if (strcmp(label, LV_SYMBOL_DOWN) == 0) {
        editor_apply_key(EDITOR_KEY_DOWN, 0);
        return true;
    }
    if (strcmp(label, LV_SYMBOL_CLOSE) == 0 || strcmp(label, LV_SYMBOL_KEYBOARD) == 0) {
        /* Hide button: hide the OSK, stay in the editor. */
        keyboard_hide();
        return true;
    }

    /* Custom nav labels on the editor keyboard page. */
    if (strcmp(label, "Tab") == 0) {
        editor_apply_key(EDITOR_KEY_TAB, 0);
        return true;
    }
    if (strcmp(label, "Del") == 0) {
        editor_apply_key(EDITOR_KEY_DELETE, 0);
        return true;
    }
    if (strcmp(label, "Ins") == 0) {
        editor_apply_key(EDITOR_KEY_OVERWRITE, 0);
        return true;
    }
    if (strcmp(label, "Home") == 0) {
        editor_apply_key(EDITOR_KEY_HOME, 0);
        return true;
    }
    if (strcmp(label, "End") == 0) {
        editor_apply_key(EDITOR_KEY_END, 0);
        return true;
    }
    if (strcmp(label, "PgUp") == 0) {
        editor_apply_key(EDITOR_KEY_PAGE_UP, 0);
        return true;
    }
    if (strcmp(label, "PgDn") == 0) {
        editor_apply_key(EDITOR_KEY_PAGE_DOWN, 0);
        return true;
    }
    if (strcmp(label, "Save") == 0) {
        editor_apply_key(EDITOR_KEY_SAVE, 0);
        return true;
    }
    if (strcmp(label, "SaveAs") == 0) {
        editor_apply_key(EDITOR_KEY_SAVE_AS, 0);
        return true;
    }
    if (strcmp(label, "Find") == 0) {
        editor_apply_key(EDITOR_KEY_FIND, 0);
        return true;
    }
    if (strcmp(label, "Rep") == 0) {
        editor_apply_key(EDITOR_KEY_REPLACE, 0);
        return true;
    }
    if (strcmp(label, "Goto") == 0) {
        editor_apply_key(EDITOR_KEY_GOTO_LINE, 0);
        return true;
    }
    if (strcmp(label, "Undo") == 0) {
        editor_apply_key(EDITOR_KEY_UNDO, 0);
        return true;
    }
    if (strcmp(label, "Redo") == 0) {
        editor_apply_key(EDITOR_KEY_REDO, 0);
        return true;
    }
    if (strcmp(label, "Next") == 0) {
        editor_apply_key(EDITOR_KEY_FIND_NEXT, 0);
        return true;
    }
    if (strcmp(label, "Copy") == 0) {
        editor_apply_key(EDITOR_KEY_COPY, 0);
        return true;
    }
    if (strcmp(label, "Cut") == 0) {
        editor_apply_key(EDITOR_KEY_CUT, 0);
        return true;
    }
    if (strcmp(label, "Paste") == 0) {
        editor_apply_key(EDITOR_KEY_PASTE, 0);
        return true;
    }
    if (strcmp(label, "SelAll") == 0) {
        editor_apply_key(EDITOR_KEY_SELECT_ALL, 0);
        return true;
    }
    if (strcmp(label, "WdL") == 0) {
        editor_apply_key(EDITOR_KEY_WORD_LEFT, 0);
        return true;
    }
    if (strcmp(label, "WdR") == 0) {
        editor_apply_key(EDITOR_KEY_WORD_RIGHT, 0);
        return true;
    }
    if (strcmp(label, "DocH") == 0) {
        editor_apply_key(EDITOR_KEY_DOC_HOME, 0);
        return true;
    }
    if (strcmp(label, "DocE") == 0) {
        editor_apply_key(EDITOR_KEY_DOC_END, 0);
        return true;
    }
    if (strcmp(label, "DelLn") == 0) {
        editor_apply_key(EDITOR_KEY_DELETE_LINE, 0);
        return true;
    }
    if (strcmp(label, "DelE") == 0) {
        editor_apply_key(EDITOR_KEY_DELETE_EOL, 0);
        return true;
    }
    if (strcmp(label, "Quit") == 0 || strcmp(label, "Exit") == 0) {
        editor_apply_key(EDITOR_KEY_QUIT, 0);
        return true;
    }
    if (strcmp(label, "abc") == 0 || strcmp(label, "ABC") == 0 ||
        strcmp(label, "1#") == 0 || strcmp(label, "Nav") == 0 ||
        strcmp(label, "Nav1") == 0 || strcmp(label, "Nav2") == 0) {
        /* Mode-switch buttons: main.c's keyboard callback owns them. */
        return true;
    }

    /* A single printable character. */
    if (label[0] != '\0' && label[1] == '\0') {
        editor_apply_ascii(label[0]);
        return true;
    }

    return false;
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

bool editor_view_open(editor_doc_t *doc, editor_control_t *control)
{
    lv_obj_t *surface;
    lv_obj_t *shell_spans;

    if (s_editor_view.open) {
        editor_view_close();
    }
    if (doc == NULL) {
        ESP_LOGW(EDITOR_VIEW_TAG, "editor_view_open: NULL doc");
        return false;
    }

    surface = windows_enter_editor_mode();
    if (surface == NULL) {
        ESP_LOGW(EDITOR_VIEW_TAG, "editor_view_open: windows_enter_editor_mode failed");
        return false;
    }
    s_editor_view.open = true;
    s_editor_view.doc = doc;
    s_editor_view.control = control;
    s_editor_view.surface = surface;

    /* Hide the shell's own transcript spans so the editor's span group owns
     * the container (its content is retained and re-shown on close). */
    shell_spans = windows_get_transcript_spans();
    if (shell_spans != NULL) {
        lv_obj_add_flag(shell_spans, LV_OBJ_FLAG_HIDDEN);
    }

    /* Dedicated span group holding the document, one row per line. */
    s_editor_view.spans = lv_spangroup_create(surface);
    if (s_editor_view.spans == NULL) {
        editor_view_close();
        return false;
    }
    lv_obj_set_width(s_editor_view.spans, LV_PCT(100));
    lv_obj_set_height(s_editor_view.spans, 0);
    lv_obj_clear_flag(s_editor_view.spans, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_editor_view.spans, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_editor_view.spans, 0, 0);
    lv_obj_set_style_pad_all(s_editor_view.spans, 0, 0);
    lv_obj_set_style_text_font(s_editor_view.spans,
                               windows_get_terminal_font(), 0);
    /* Make the span group's row pitch equal editor_line_height() so the
     * cursor, selection overlay, and touch mapping stay aligned with the
     * rendered rows. */
    {
        const lv_font_t *font = windows_get_terminal_font();
        lv_coord_t font_lh = font != NULL ? lv_font_get_line_height(font) : 16;
        lv_coord_t lh = editor_line_height();
        lv_coord_t line_space = lh - font_lh;
        if (line_space < 0) {
            line_space = 0;
        }
        lv_obj_set_style_text_line_space(s_editor_view.spans, line_space, 0);
    }

    /* Selection overlay container (sits on top of the spans). */
    s_editor_view.sel_layer = lv_obj_create(surface);
    if (s_editor_view.sel_layer == NULL) {
        editor_view_close();
        return false;
    }
    lv_obj_set_pos(s_editor_view.sel_layer, 0, 0);
    lv_obj_set_size(s_editor_view.sel_layer, lv_obj_get_width(surface), 0);
    lv_obj_remove_flag(s_editor_view.sel_layer,
                       LV_OBJ_FLAG_SCROLLABLE |
                       LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_opa(s_editor_view.sel_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_editor_view.sel_layer, 0, 0);
    lv_obj_add_flag(s_editor_view.sel_layer, LV_OBJ_FLAG_HIDDEN);

    /* Block cursor. */
    s_editor_view.cursor = lv_obj_create(surface);
    if (s_editor_view.cursor == NULL) {
        editor_view_close();
        return false;
    }
    lv_obj_remove_flag(s_editor_view.cursor,
                       LV_OBJ_FLAG_SCROLLABLE |
                       LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_color(s_editor_view.cursor,
                              windows_get_color(WINDOWS_COLOR_TEXT), 0);
    lv_obj_set_style_bg_opa(s_editor_view.cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_editor_view.cursor, 0, 0);
    lv_obj_set_style_radius(s_editor_view.cursor, 0, 0);
    lv_obj_set_size(s_editor_view.cursor, editor_cell_width(),
                    editor_line_height());
    s_editor_view.cursor_visible = true;
    s_editor_view.cursor_timer = lv_timer_create(editor_cursor_blink_cb,
                                                 P4_CONFIG_EDITOR_CURSOR_BLINK_MS,
                                                 NULL);

    /* Current-line highlight: a full-width bar behind the cursor row. It is
     * moved to the very back so the text, selection, and cursor draw above
     * it. */
#if P4_CONFIG_EDITOR_CURRENT_LINE
    s_editor_view.current_line = lv_obj_create(surface);
    if (s_editor_view.current_line == NULL) {
        editor_view_close();
        return false;
    }
    lv_obj_remove_flag(s_editor_view.current_line,
                       LV_OBJ_FLAG_SCROLLABLE |
                       LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_color(s_editor_view.current_line,
        lv_color_hex(P4_CONFIG_EDITOR_CURRENT_LINE_COLOR), 0);
    lv_obj_set_style_bg_opa(s_editor_view.current_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_editor_view.current_line, 0, 0);
    lv_obj_set_style_radius(s_editor_view.current_line, 0, 0);
    lv_obj_set_pos(s_editor_view.current_line, 0, 0);
    lv_obj_set_size(s_editor_view.current_line, lv_obj_get_width(surface),
                    editor_line_height());
    lv_obj_move_to_index(s_editor_view.current_line, 0);
#endif

    /* Status bar (input row becomes status in editor mode). */
    s_editor_view.status = windows_get_editor_status();
    editor_status_default();

    /* Touch input on the editor surface (tap to place caret, drag to select). */
    lv_obj_add_event_cb(surface, editor_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(surface, editor_touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(surface, editor_touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(surface, editor_touch_event_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* Deferred content build on a later LVGL pass (synchronous fallback so
     * the surface is never left blank when the async queue is full). */
    if (lv_async_call(editor_rebuild_cb, NULL) != LV_RESULT_OK) {
        editor_rebuild();
    }

    /* The on-screen keyboard stays visible and drives the editor. Unbind the
     * shell input line so LVGL's default keyboard handler does not type into
     * the hidden textarea; every OSK button routes through our callback. */
    keyboard_bind_textarea(NULL);

    return true;
}

void editor_view_close(void)
{
    if (!s_editor_view.open) {
        return;
    }

    if (s_editor_view.cursor_timer != NULL) {
        lv_timer_delete(s_editor_view.cursor_timer);
        s_editor_view.cursor_timer = NULL;
    }

    /* A worker session owns the document this view renders. Wake it BEFORE
     * detaching the control block so an external teardown (display rotation)
     * never leaves the worker blocked waiting for a quit that no longer has
     * a view to deliver. In the normal quit path this just re-raises an
     * already-consumed quit bit, which is harmless. */
    if (s_editor_view.control != NULL && s_editor_view.control->event_group != NULL) {
        xEventGroupSetBits((EventGroupHandle_t)s_editor_view.control->event_group,
                           MODAL_EVENT_CLOSE_REQUEST);
    }

    /* Detach the document first so any straggler async callback that runs
     * after this returns sees a NULL doc and no-ops instead of touching the
     * worker-owned document that is about to be freed. */
    s_editor_view.doc = NULL;
    s_editor_view.control = NULL;

    if (s_editor_view.cursor != NULL) {
        lv_obj_delete(s_editor_view.cursor);
        s_editor_view.cursor = NULL;
    }
    if (s_editor_view.current_line != NULL) {
        lv_obj_delete(s_editor_view.current_line);
        s_editor_view.current_line = NULL;
    }
    if (s_editor_view.sel_layer != NULL) {
        lv_obj_delete(s_editor_view.sel_layer);
        s_editor_view.sel_layer = NULL;
    }
    if (s_editor_view.spans != NULL) {
        lv_obj_delete(s_editor_view.spans);
        s_editor_view.spans = NULL;
    }

    /* Restore the shell's own transcript spans so the shell output returns. */
    {
        lv_obj_t *shell_spans = windows_get_transcript_spans();
        if (shell_spans != NULL) {
            lv_obj_remove_flag(shell_spans, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Re-paint the shell transcript and jump back to its newest output. */
    windows_scroll_transcript_to_end();

    free(s_editor_view.widths);
    s_editor_view.widths = NULL;
    s_editor_view.width_count = 0;
    s_editor_view.width_row = (size_t)-1;

    /* Restore the shell input line as the OSK target. */
    keyboard_bind_textarea(windows_get_input_line());

    windows_exit_editor_mode();

    s_editor_view.open = false;
    s_editor_view.surface = NULL;
    s_editor_view.status = NULL;
    s_editor_view.touch_selecting = false;
    s_editor_view.prompt = EDITOR_PROMPT_NONE;
    s_editor_view.replace_armed = false;
}

bool editor_view_is_open(void)
{
    return s_editor_view.open;
}

void editor_view_notify_saved(bool ok)
{
    if (!s_editor_view.open) {
        return;
    }
    if (s_editor_view.doc != NULL) {
        if (ok) {
            s_editor_view.doc->modified = false;
        }
        editor_status(ok ? "saved" : "save failed");
    }
}

void editor_view_notify_saved_cb(void *user_data)
{
    editor_view_notify_saved((intptr_t)user_data != 0);
}

void editor_view_scroll_by(int32_t pixels)
{
    if (s_editor_view.surface == NULL) {
        return;
    }
    lv_obj_scroll_by_bounded(s_editor_view.surface, 0, pixels, LV_ANIM_OFF);
}

void editor_view_set_quit_requested(void)
{
    /* Esc: confirm before discarding unsaved changes (DOS EDIT behaviour). */
    if (s_editor_view.doc != NULL && s_editor_view.doc->modified) {
        editor_prompt_begin(EDITOR_PROMPT_QUIT_CONFIRM);
        return;
    }
    editor_quit_signal();
}

void editor_view_set_save_requested(void)
{
    if (s_editor_view.control != NULL) {
        s_editor_view.control->save_requested = true;
        if (s_editor_view.control->event_group != NULL) {
            xEventGroupSetBits((EventGroupHandle_t)s_editor_view.control->event_group,
                               EDITOR_EVENT_SAVE);
        }
    }
}

/* ========================================================================
 * TOUCH INPUT
 * ======================================================================== */

static lv_coord_t editor_surface_x(void)
{
    lv_coord_t x = 0;
    lv_obj_t *o = s_editor_view.surface;
    while (o != NULL) {
        x += lv_obj_get_x(o);
        o = lv_obj_get_parent(o);
    }
    return x;
}

static lv_coord_t editor_surface_y(void)
{
    lv_coord_t y = 0;
    lv_obj_t *o = s_editor_view.surface;
    while (o != NULL) {
        y += lv_obj_get_y(o);
        o = lv_obj_get_parent(o);
    }
    /* Content scrolls inside the surface; the touch point must be
     * translated back into unscrolled content coordinates. */
    if (s_editor_view.surface != NULL) {
        y -= lv_obj_get_scroll_y(s_editor_view.surface);
    }
    return y;
}

/* Map a touch point (in surface content coordinates) to a (row, col) pair,
 * clamping into the document. */
static void editor_touch_to_cell(size_t *row_out, size_t *col_out)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;
    lv_coord_t lh = editor_line_height();
    lv_coord_t rel_x, rel_y;
    size_t row;
    lv_coord_t pad_left = 0;
    lv_coord_t pad_top = 0;

    lv_indev_get_point(indev, &point);
    rel_x = point.x - editor_surface_x();
    rel_y = point.y - editor_surface_y();

    if (s_editor_view.surface != NULL) {
        pad_left = lv_obj_get_style_pad_left(s_editor_view.surface, 0);
        pad_top = lv_obj_get_style_pad_top(s_editor_view.surface, 0);
    }
    rel_x -= pad_left;
    rel_y -= pad_top;
    /* Skip the line-number gutter when mapping x back to a document column. */
    rel_x -= editor_gutter_width();
    if (rel_x < 0) rel_x = 0;
    if (rel_y < 0) rel_y = 0;

    row = (size_t)(rel_y / lh);
    if (s_editor_view.doc != NULL && row >= editor_doc_line_count(s_editor_view.doc)) {
        row = editor_doc_line_count(s_editor_view.doc) - 1;
    }
    if (row_out != NULL) {
        *row_out = row;
    }
    if (col_out != NULL) {
        *col_out = editor_column_from_x(s_editor_view.doc, row, rel_x);
    }
}

static void editor_touch_press(lv_event_t *event)
{
    size_t row, col;
    editor_doc_t *doc = s_editor_view.doc;

    (void)event;
    if (doc == NULL) {
        return;
    }
    editor_touch_to_cell(&row, &col);

    s_editor_view.touch_selecting = false;
    doc->cursor_row = row;
    doc->cursor_col = col;
    editor_doc_selection_clear(doc);
    editor_layout_update();
}

static void editor_touch_drag(lv_event_t *event)
{
    size_t row, col;

    (void)event;
    if (!s_editor_view.touch_selecting || s_editor_view.doc == NULL) {
        return;
    }
    editor_touch_to_cell(&row, &col);

    s_editor_view.doc->cursor_row = row;
    s_editor_view.doc->cursor_col = col;
    editor_doc_selection_extend(s_editor_view.doc);
    editor_layout_update();
}

/* Dispatcher for touch input on the editor surface. */
static void editor_touch_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    switch (code) {
    case LV_EVENT_PRESSED:
        editor_touch_press(event);
        break;
    case LV_EVENT_PRESSING:
        editor_touch_drag(event);
        break;
    case LV_EVENT_RELEASED:
        s_editor_view.touch_selecting = false;
        break;
    case LV_EVENT_LONG_PRESSED:
        editor_touch_press(event);
        s_editor_view.touch_selecting = true;
        break;
    default:
        break;
    }
}

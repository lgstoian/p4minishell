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
#include "font.h"
#include "markdown.h"
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

/* Session event bits live once in editor.h (shared by the worker and this view). */

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
    EDITOR_PROMPT_OPEN,              /**< "Open: <path>" */
    EDITOR_PROMPT_OPEN_CONFIRM,      /**< "Open without saving? (Y/N)" */
    EDITOR_PROMPT_QUIT_CONFIRM,      /**< "Quit without saving? (Y/N)" */
} editor_prompt_t;

static struct {
    bool open;
    editor_doc_t *doc;
    editor_control_t *control;
    lv_obj_t *surface;          /* Scrollable container (the transcript region) */
    lv_obj_t *spacer;           /* Full-height invisible child: keeps the
                                 * scroll range correct when only a window of
                                 * rows is materialized as spans */
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
    bool preview;               /* Rendered Markdown preview (read-only) */
    bool find_case;             /* Find/replace case sensitivity (session) */
    bool wrap;                  /* Word-wrap long rows (session) */
    size_t *wrap_counts;        /* Visual chunks per doc row (wrap on) */
    size_t wrap_count_rows;
    lv_coord_t wrap_px_used;    /* Wrap width the cache was built for */

    /* Per-row display width cache (cumulative pixel offsets per column). */
    size_t width_row;
    lv_coord_t *widths;
    size_t width_count;

    /* Virtualized rendering: the first document row currently materialized as
     * spans. At most P4_CONFIG_EDITOR_RENDER_ROWS rows exist at once; a scroll
     * or cursor jump shifts the window. */
    size_t render_first;
    bool render_busy;           /* Reentrancy guard for the scroll handler */
} s_editor_view = {
    .open = false,
    .doc = NULL,
    .control = NULL,
    .surface = NULL,
    .spacer = NULL,
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
    .preview = false,
    .find_case = P4_CONFIG_EDITOR_FIND_CASE_SENSITIVE,
    .wrap = false,
    .wrap_counts = NULL,
    .wrap_count_rows = 0,
    .wrap_px_used = 0,
    .width_row = (size_t)-1,
    .widths = NULL,
    .width_count = 0,
    .render_first = 0,
    .render_busy = false,
};

/* Forward declarations (mutual recursion between rendering and editing). */
static void editor_rebuild(void);
static void editor_render_spans(void);
static void editor_update_cursor(void);
static void editor_update_current_line(void);
static void editor_build_selection(void);
static void editor_ensure_cursor_visible(void);
static void editor_status(const char *format, ...);
static void editor_status_default(void);
static void editor_layout_update(void);
static void editor_touch_event_cb(lv_event_t *event);
static void editor_scroll_event_cb(lv_event_t *event);
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
    w = editor_mem_realloc(s_editor_view.widths, (len + 1) * sizeof(lv_coord_t));
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
 * WORD WRAP (explicit chunking with \n separators)
 * ========================================================================
 * When wrap is on, long rows are split into visual chunks of wrap_px
 * pixels, emitted with separator spans so the on-screen rows always match
 * this math exactly (independent of LVGL's span layout). Mappings below
 * translate between document (row, col) and absolute visual rows. The
 * per-row chunk counts are cached at rebuild; mappings trigger a rebuild
 * of the cache when the wrap width changed (e.g. keyboard show/hide).
 */

static lv_coord_t editor_wrap_px(void)
{
    lv_coord_t content;
    lv_coord_t gutter;

    if (!s_editor_view.wrap || s_editor_view.surface == NULL) {
        return 0;
    }
    content = lv_obj_get_content_width(s_editor_view.surface);
    gutter = editor_gutter_width();
    if (content - gutter < 80) {
        return 0;
    }
    return content - gutter;
}

/** First byte col past @p start that still fits in @p budget_px (widths
 * must be cached for @p row). Always advances >= 1 col when text remains. */
static size_t editor_fit_cols(size_t row, size_t start, lv_coord_t budget_px)
{
    size_t len;
    size_t end;

    if (s_editor_view.widths == NULL || s_editor_view.width_row != row) {
        return start;
    }
    len = s_editor_view.width_count;
    if (start >= len) {
        return len;
    }
    end = start;
    while (end < len &&
           s_editor_view.widths[end + 1] - s_editor_view.widths[start] < budget_px) {
        end++;
    }
    if (end == start) {
        end++;
    }
    return end;
}

/** Byte columns [start, end) of chunk @p chunk within @p row. */
static void editor_chunk_cols(const editor_doc_t *doc, size_t row, size_t chunk,
                              size_t *start_out, size_t *end_out)
{
    lv_coord_t wrap_px = editor_wrap_px();
    size_t len;
    size_t start = 0;
    size_t end;
    size_t k;

    if (start_out != NULL) {
        *start_out = 0;
    }
    if (end_out != NULL) {
        *end_out = 0;
    }
    if (doc == NULL || row >= editor_doc_line_count(doc) || wrap_px <= 0) {
        if (doc != NULL && row < editor_doc_line_count(doc)) {
            if (end_out != NULL) {
                *end_out = editor_doc_line_length(doc, row);
            }
        }
        return;
    }
    len = editor_doc_line_length(doc, row);
    editor_width_cache(row);
    if (s_editor_view.widths == NULL || s_editor_view.width_row != row) {
        if (end_out != NULL) {
            *end_out = len;
        }
        return;
    }
    /* Walk chunks greedily by pixel width; every chunk takes >= 1 col
     * (shared fit rule with the render emitter). */
    for (k = 0; k <= chunk; k++) {
        end = editor_fit_cols(row, start, wrap_px);
        if (end >= len) {
            end = len;
            if (k == chunk) {
                if (start_out != NULL) {
                    *start_out = start;
                }
                if (end_out != NULL) {
                    *end_out = end;
                }
            }
            return;
        }
        if (k == chunk) {
            if (start_out != NULL) {
                *start_out = start;
            }
            if (end_out != NULL) {
                *end_out = end;
            }
            return;
        }
        start = end;
    }
    /* Past the last chunk: clamp to the tail. */
    if (start_out != NULL) {
        *start_out = start;
    }
    if (end_out != NULL) {
        *end_out = len;
    }
}

/** Number of visual chunks of @p row (>= 1). */
static size_t editor_row_chunks(const editor_doc_t *doc, size_t row)
{
    lv_coord_t wrap_px = editor_wrap_px();
    size_t len;
    size_t start = 0;
    size_t chunks = 0;

    if (doc == NULL || row >= editor_doc_line_count(doc) || wrap_px <= 0) {
        return 1;
    }
    len = editor_doc_line_length(doc, row);
    if (len == 0) {
        return 1;
    }
    editor_width_cache(row);
    if (s_editor_view.widths == NULL || s_editor_view.width_row != row) {
        return 1;
    }
    while (start < len) {
        start = editor_fit_cols(row, start, wrap_px);
        chunks++;
    }
    return chunks > 0 ? chunks : 1;
}

/** Rebuild the per-row chunk-count cache (call from editor_rebuild). */
static void editor_wrap_rebuild(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t count;
    size_t row;

    editor_mem_free(s_editor_view.wrap_counts);
    s_editor_view.wrap_counts = NULL;
    s_editor_view.wrap_count_rows = 0;
    s_editor_view.wrap_px_used = s_editor_view.wrap ? editor_wrap_px() : 0;
    if (doc == NULL || !s_editor_view.wrap || s_editor_view.wrap_px_used <= 0) {
        return;
    }
    count = editor_doc_line_count(doc);
    if (count == 0) {
        return;
    }
    s_editor_view.wrap_counts = editor_mem_alloc(count * sizeof(size_t));
    if (s_editor_view.wrap_counts == NULL) {
        return;
    }
    s_editor_view.wrap_count_rows = count;
    for (row = 0; row < count; row++) {
        s_editor_view.wrap_counts[row] = editor_row_chunks(doc, row);
    }
}

/** Chunk index holding byte @p col within @p row (0 when not wrapping). */
static size_t editor_chunk_of(const editor_doc_t *doc, size_t row, size_t col)
{
    size_t len;
    size_t start = 0;
    size_t end = 0;
    size_t k = 0;

    if (doc == NULL || row >= editor_doc_line_count(doc) ||
        !s_editor_view.wrap || editor_wrap_px() <= 0) {
        return 0;
    }
    len = editor_doc_line_length(doc, row);
    if (col > len) {
        col = len;
    }
    while (true) {
        editor_chunk_cols(doc, row, k, &start, &end);
        if (col < end || end >= len) {
            break;
        }
        k++;
    }
    return k;
}

/** Absolute visual row of document (row, col): chunk base + chunk index. */
static size_t editor_visual_row(const editor_doc_t *doc, size_t row, size_t col)
{
    size_t v = 0;
    size_t r;

    if (doc == NULL) {
        return 0;
    }
    if (row >= editor_doc_line_count(doc)) {
        row = editor_doc_line_count(doc);
    }
    /* Prefix sums from the cache when it matches this document shape. */
    if (s_editor_view.wrap && s_editor_view.wrap_counts != NULL &&
        s_editor_view.wrap_count_rows == editor_doc_line_count(doc) &&
        s_editor_view.wrap_px_used == editor_wrap_px()) {
        for (r = 0; r < row; r++) {
            v += s_editor_view.wrap_counts[r];
        }
    } else if (s_editor_view.wrap) {
        for (r = 0; r < row; r++) {
            size_t n = editor_row_chunks(doc, r);
            v += n;
        }
    } else {
        return row;
    }
    if (row >= editor_doc_line_count(doc)) {
        return v;
    }
    /* Chunk index of col within the row. */
    return v + editor_chunk_of(doc, row, col);
}

/** Inverse: absolute visual row -> (doc row, chunk). Clamps to the doc. */
static void editor_visual_to_doc(size_t vrow, size_t *row_out, size_t *chunk_out)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t count;
    size_t row = 0;
    size_t n;

    if (row_out != NULL) {
        *row_out = 0;
    }
    if (chunk_out != NULL) {
        *chunk_out = 0;
    }
    if (doc == NULL) {
        return;
    }
    count = editor_doc_line_count(doc);
    if (count == 0) {
        return;
    }
    if (!s_editor_view.wrap) {
        if (vrow >= count) {
            vrow = count - 1;
        }
        if (row_out != NULL) {
            *row_out = vrow;
        }
        return;
    }
    for (row = 0; row < count; row++) {
        if (s_editor_view.wrap_counts != NULL &&
            s_editor_view.wrap_count_rows == count &&
            s_editor_view.wrap_px_used == editor_wrap_px()) {
            n = s_editor_view.wrap_counts[row];
        } else {
            n = editor_row_chunks(doc, row);
        }
        if (vrow < n) {
            if (row_out != NULL) {
                *row_out = row;
            }
            if (chunk_out != NULL) {
                *chunk_out = vrow;
            }
            return;
        }
        vrow -= n;
    }
    if (row_out != NULL) {
        *row_out = count - 1;
    }
    if (chunk_out != NULL) {
        size_t last = count - 1;
        size_t ln;
        if (s_editor_view.wrap_counts != NULL &&
            s_editor_view.wrap_count_rows == count &&
            s_editor_view.wrap_px_used == editor_wrap_px()) {
            ln = s_editor_view.wrap_counts[last];
        } else {
            ln = editor_row_chunks(doc, last);
        }
        *chunk_out = ln > 0 ? ln - 1 : 0;
    }
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

/** Append one styled run of @p row to the span group. */
static void editor_add_run(const char *text, size_t len,
                           ansi_color_index_t color, unsigned attrs)
{
    lv_span_t *span;
    lv_style_t *style;
    char *buf;

    if (len == 0) {
        return;
    }
    buf = editor_mem_alloc(len + 1);
    if (buf == NULL) {
        return;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    span = lv_spangroup_add_span(s_editor_view.spans);
    if (span == NULL) {
        editor_mem_free(buf);
        return;
    }
    lv_span_set_text(span, buf);
    style = lv_span_get_style(span);
    /* Editor surfaces are terminal-role; attrs select TTF variants. */
    font_span_style(style, FONT_ROLE_TERMINAL, attrs, (int)color,
                    ansi_get_palette_color(color));
    editor_mem_free(buf);
}

/* Chunked span emission state (one editor_render_row at a time). When
 * wrapping, content runs are split at chunk bounds with "\n" separators
 * plus gutter pads, so visual rows always match editor_chunk_cols. */
static bool s_emit_wrap;
static size_t s_emit_row;
static lv_coord_t s_emit_remain;
static size_t s_emit_pad;

static void editor_emit_break(void)
{
    char pad[32];
    size_t n = s_emit_pad;

    editor_add_run("\n", 1, ANSI_COLOR_BRIGHT_WHITE, 0);
    if (n > sizeof(pad) - 1) {
        n = sizeof(pad) - 1;
    }
    if (n > 0) {
        memset(pad, ' ', n);
        editor_add_run(pad, n, ANSI_COLOR_BRIGHT_BLACK, 0);
    }
}

/** Emit [start, start+len) of @p text, splitting at wrap chunk bounds. */
static void editor_emit_span(const char *text, size_t start, size_t len,
                             ansi_color_index_t color, unsigned attrs)
{
    size_t off = 0;

    if (!s_emit_wrap || len == 0) {
        if (len > 0) {
            editor_add_run(text + start, len, color, attrs);
        }
        return;
    }
    if (s_editor_view.widths == NULL ||
        s_editor_view.width_row != s_emit_row) {
        /* Widths lost (should not happen mid-render): emit whole. */
        editor_add_run(text + start, len, color, attrs);
        return;
    }
    while (off < len) {
        size_t cur = start + off;
        size_t fit_end = editor_fit_cols(s_emit_row, cur, s_emit_remain);
        if (fit_end > start + len) {
            fit_end = start + len;
        }
        if (fit_end <= cur) {
            fit_end = cur + 1;
        }
        editor_add_run(text + cur, fit_end - cur, color, attrs);
        s_emit_remain -=
            (s_editor_view.widths[fit_end] - s_editor_view.widths[cur]);
        off = fit_end - start;
        if (off < len) {
            editor_emit_break();
            s_emit_remain = editor_wrap_px();
        }
    }
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
    bool wrapping = false;

#if P4_CONFIG_EDITOR_LINE_NUMBERS
    /* Line-number gutter: a muted, right-aligned "N " prefix before the
     * content, rendered as part of the same visual row. */
    {
        char gutter[P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS + 2];
        size_t g = editor_format_line_number(row, P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS,
                                             gutter, sizeof(gutter));
        editor_add_run(gutter, g, ANSI_COLOR_BRIGHT_BLACK, 0);
    }
#endif

    if (len == 0) {
        return; /* The '\n' separator still advances the row height. */
    }

    /* Wrap setup: chunk emission state for this row (widths stay cached
     * for the whole row since nothing else measures mid-render). The
     * continuation gutter pad matches the number-run width above. */
    wrapping = s_editor_view.wrap && editor_wrap_px() > 0 && len > 0;
    if (wrapping) {
        editor_width_cache(row);
        wrapping = s_editor_view.widths != NULL && s_editor_view.width_row == row;
    }
    s_emit_wrap = wrapping;
    if (wrapping) {
        s_emit_row = row;
        s_emit_remain = editor_wrap_px();
#if P4_CONFIG_EDITOR_LINE_NUMBERS
        {
            char gutter[P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS + 2];
            s_emit_pad = editor_format_line_number(
                row, P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS,
                gutter, sizeof(gutter));
        }
#else
        s_emit_pad = 0;
#endif
    }

    /* Lexers fill color only; attrs default plain (the MD lexer sets both). */
    memset(runs, 0, sizeof(runs));

    if (doc->syntax == EDITOR_SYNTAX_BATCH && P4_CONFIG_EDITOR_SYNTAX_BATCH) {
        run_count = editor_lex_batch(text, len, runs, 64);
    } else if (doc->syntax == EDITOR_SYNTAX_MARKDOWN) {
        run_count = editor_lex_markdown(text, len, runs, 64);
    } else if (doc->syntax == EDITOR_SYNTAX_JSON) {
        run_count = editor_lex_json(text, len, runs, 64);
    }
    if (run_count == 0) {
        runs[0].start = 0;
        runs[0].length = len;
        runs[0].color = ANSI_COLOR_BRIGHT_WHITE;
        runs[0].attrs = 0;
        run_count = 1;
    }

    for (i = 0; i < run_count; i++) {
        size_t start = runs[i].start;
        size_t rlen = runs[i].length;
        ansi_color_index_t color = runs[i].color;
        unsigned attrs = runs[i].attrs;

        if (start > len) {
            start = len;
        }
        if (start + rlen > len) {
            rlen = len - start;
        }
        /* Any gap before this run (e.g. a run buffer that filled early) is
         * rendered as plain text so no byte of the line is ever dropped. */
        if (covered < start) {
            editor_emit_span(text, covered, start - covered,
                             ANSI_COLOR_BRIGHT_WHITE, 0);
        }
        if (rlen > 0) {
            editor_emit_span(text, start, rlen, color, attrs);
            covered = start + rlen;
        }
    }
    if (covered < len) {
        editor_emit_span(text, covered, len - covered,
                         ANSI_COLOR_BRIGHT_WHITE, 0);
    }
}

/** True when the document is large enough to virtualize row rendering. */
static bool editor_windowing_active(void)
{
    editor_doc_t *doc = s_editor_view.doc;

    /* Wrapping changes the row pitch (visual rows), so keep the simple
     * full-render path while wrap is on; wrap is a per-session toggle and off
     * by default. */
    return doc != NULL && !s_editor_view.wrap &&
           editor_doc_line_count(doc) > P4_CONFIG_EDITOR_RENDER_ROWS;
}

/** First row to materialize so @p focus_row sits inside with an overscan. */
static size_t editor_render_first_for(size_t focus_row, size_t total)
{
    size_t win = P4_CONFIG_EDITOR_RENDER_ROWS;
    size_t margin = win / 4;
    size_t first;

    if (total <= win) {
        return 0;
    }
    first = focus_row > margin ? focus_row - margin : 0;
    if (first + win > total) {
        first = total - win;
    }
    return first;
}

/** Append a row separator span to the span group. */
static void editor_add_separator(void)
{
    lv_span_t *sep = lv_spangroup_add_span(s_editor_view.spans);
    if (sep != NULL) {
        lv_style_t *style = lv_span_get_style(sep);
        lv_span_set_text(sep, "\n");
        lv_style_set_text_color(style, lv_color_hex(
            ansi_get_palette_color(ANSI_COLOR_BRIGHT_WHITE)));
        lv_style_set_text_font(style, windows_get_terminal_font());
    }
}

/** Materialize the current row window as spans, positioned at its document
 *  y offset so the cursor/selection overlays and touch mapping (which work in
 *  document coordinates) stay aligned. */
static void editor_render_spans(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t total;
    size_t first;
    size_t last;
    size_t row;
    lv_coord_t lh = editor_line_height();

    if (s_editor_view.spans == NULL || doc == NULL) {
        return;
    }
    total = editor_doc_line_count(doc);
    first = s_editor_view.render_first;
    if (first > total) {
        first = total;
    }
    s_editor_view.render_first = first;
    last = first + P4_CONFIG_EDITOR_RENDER_ROWS;
    if (last > total) {
        last = total;
    }

    editor_clear_spans(s_editor_view.spans);
    for (row = first; row < last; row++) {
        editor_render_row(doc, row);
        if (row + 1 < last) {
            editor_add_separator();
        }
    }

    /* When virtualized, the spans child covers only its window and is placed
     * at the matching document row; the full-height spacer (not the spans)
     * establishes the scroll range. A small document renders fully at 0. */
    if (editor_windowing_active()) {
        lv_obj_set_y(s_editor_view.spans, (lv_coord_t)(first * (size_t)lh));
        lv_obj_set_height(s_editor_view.spans,
                          (lv_coord_t)((last - first) * (size_t)lh));
    } else {
        lv_obj_set_y(s_editor_view.spans, 0);
        lv_obj_set_height(s_editor_view.spans,
                          (lv_coord_t)(total * (size_t)lh));
    }
}

/** Rebuild the whole editor content (spans, selection, cursor). */
static void editor_rebuild(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t count;
    lv_coord_t lh = editor_line_height();

    if (s_editor_view.spans == NULL || doc == NULL) {
        return;
    }

    editor_width_invalidate();

    /* Wrap chunk cache first: render_row and all mappings agree on it. */
    editor_wrap_rebuild();

    count = editor_doc_line_count(doc);
    s_editor_view.render_first = editor_windowing_active()
        ? editor_render_first_for(editor_doc_cursor_row(doc), count)
        : 0;

    s_editor_view.render_busy = true;
    editor_render_spans();

    /* Full-height invisible spacer: keeps the scroll range equal to the whole
     * document even though only a window of rows is materialized. */
    if (s_editor_view.spacer != NULL) {
        lv_obj_set_height(s_editor_view.spacer,
                          (lv_coord_t)(count * (size_t)lh));
    }
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
    s_editor_view.render_busy = false;

    editor_update_cursor();
    editor_update_current_line();
    editor_build_selection();
    editor_ensure_cursor_visible();
    editor_status_default();
}

/** Shift the render window when the cursor moves outside it (Go-to-Line,
 *  PageDown at a window edge, ...). Called from editor_layout_update. */
static void editor_render_follow_cursor(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t total;
    size_t row;
    size_t first;
    size_t win;

    if (s_editor_view.spans == NULL || doc == NULL || s_editor_view.preview ||
        !editor_windowing_active()) {
        return;
    }
    total = editor_doc_line_count(doc);
    row = editor_doc_cursor_row(doc);
    first = s_editor_view.render_first;
    win = P4_CONFIG_EDITOR_RENDER_ROWS;
    if (row >= first && row < first + win) {
        return;
    }
    s_editor_view.render_first = editor_render_first_for(row, total);
    s_editor_view.render_busy = true;
    editor_render_spans();
    s_editor_view.render_busy = false;
}

/** Surface scroll: shift the window when the visible top nears either edge of
 *  the materialized window, so touch-drag scrolling keeps showing text. */
static void editor_scroll_event_cb(lv_event_t *event)
{
    editor_doc_t *doc = s_editor_view.doc;
    lv_obj_t *surface = s_editor_view.surface;
    size_t total;
    size_t visible_top;
    size_t first;
    size_t win;
    size_t want;
    lv_coord_t lh;
    lv_coord_t scroll_y;

    (void)event;
    if (s_editor_view.render_busy || s_editor_view.preview ||
        surface == NULL || doc == NULL || s_editor_view.spans == NULL ||
        !editor_windowing_active()) {
        return;
    }
    lh = editor_line_height();
    if (lh <= 0) {
        return;
    }
    total = editor_doc_line_count(doc);
    scroll_y = lv_obj_get_scroll_y(surface);
    if (scroll_y < 0) {
        scroll_y = 0;
    }
    visible_top = (size_t)(scroll_y / lh);
    first = s_editor_view.render_first;
    win = P4_CONFIG_EDITOR_RENDER_ROWS;

    /* Rebuild only when the visible top pushes into the first/last quarter. */
    if (visible_top < first + win / 4 || visible_top + win / 4 > first + win) {
        size_t focus = visible_top + win / 8;
        if (focus > total) {
            focus = total;
        }
        want = editor_render_first_for(focus, total);
        if (want != first) {
            s_editor_view.render_first = want;
            s_editor_view.render_busy = true;
            editor_render_spans();
            s_editor_view.render_busy = false;
        }
    }
}

/* ========================================================================
 * MARKDOWN PREVIEW (read-only rendered view, same surface)
 * ========================================================================
 * Ctrl+P toggles between source spans and a rendered preview. Preview feeds
 * the joined document through markdown_render_doc() and reuses the editor
 * span machinery via an ANSI segment bridge (bold/italic/underline/strike
 * + TTF variants, same as the transcript). Editing keys exit preview;
 * navigation, toggle, and quit keep working. Gutter/cursor/selection hide
 * in preview (positions belong to source rows).
 */

#define EDITOR_PREVIEW_MAX_BYTES (96 * 1024)

/* ANSI segment bridge: SGR runs become editor spans. */
static void editor_preview_segment(const char *text, const ansi_state_t *state,
                                   void *user_data)
{
    lv_obj_t *group = (lv_obj_t *)user_data;
    lv_span_t *span;
    lv_style_t *style;
    ansi_color_index_t color = ANSI_COLOR_BRIGHT_WHITE;
    unsigned attrs = 0;

    if (group == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    if (state != NULL) {
        if (state->fg_index >= 0) {
            color = (ansi_color_index_t)state->fg_index;
        }
        attrs = (unsigned)state->attrs;
    }
    span = lv_spangroup_add_span(group);
    if (span == NULL) {
        return;
    }
    lv_span_set_text(span, text);
    style = lv_span_get_style(span);
    font_span_style(style, FONT_ROLE_TERMINAL, attrs, (int)color,
                    ansi_get_palette_color(color));
}

static void editor_preview_show(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t count;
    size_t total = 0;
    size_t row;
    char *joined = NULL;
    char *rendered = NULL;
    size_t off = 0;

    if (doc == NULL || s_editor_view.spans == NULL) {
        return;
    }
    count = editor_doc_line_count(doc);
    for (row = 0; row < count; row++) {
        total += editor_doc_line_length(doc, row) + 1;
        if (total > EDITOR_PREVIEW_MAX_BYTES) {
            editor_status("too large to preview");
            return;
        }
    }
    joined = editor_mem_alloc(total + 1);
    rendered = editor_mem_alloc(total * 2 + 64);
    if (joined == NULL || rendered == NULL) {
        editor_mem_free(joined);
        editor_mem_free(rendered);
        editor_status("out of memory for preview");
        return;
    }
    for (row = 0; row < count; row++) {
        const char *text = editor_doc_line_text(doc, row);
        size_t len = editor_doc_line_length(doc, row);
        if (text != NULL && len > 0) {
            memcpy(joined + off, text, len);
            off += len;
        }
        joined[off++] = '\n';
    }
    joined[off] = '\0';
    markdown_render_doc(joined, rendered, total * 2 + 64);
    editor_mem_free(joined);

    /* Preview replaces the source spans; reset any virtualized window and
     * give the group the full source height (preview is capped at 96 KB). */
    s_editor_view.render_first = 0;
    lv_obj_set_y(s_editor_view.spans, 0);
    editor_clear_spans(s_editor_view.spans);
    ansi_process_text(rendered, editor_preview_segment, s_editor_view.spans);
    editor_mem_free(rendered);
    lv_obj_set_height(s_editor_view.spans,
                      (lv_coord_t)(count * (size_t)editor_line_height()));

    /* Preview owns the surface: hide source chrome. */
    if (s_editor_view.cursor != NULL) {
        lv_obj_add_flag(s_editor_view.cursor, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_editor_view.current_line != NULL) {
        lv_obj_add_flag(s_editor_view.current_line, LV_OBJ_FLAG_HIDDEN);
    }
    editor_doc_selection_clear(doc);
    editor_build_selection();
    s_editor_view.preview = true;
    /* Scroll back to the top for the fresh render. */
    lv_obj_scroll_to_y(s_editor_view.surface, 0, LV_ANIM_OFF);
    editor_status("preview (Ctrl+P to edit)");
}

static void editor_preview_exit(void)
{
    if (!s_editor_view.preview) {
        return;
    }
    s_editor_view.preview = false;
    editor_rebuild();
    editor_status_default();
}

static void editor_preview_toggle(void)
{
    editor_doc_t *doc = s_editor_view.doc;

    if (doc == NULL) {
        return;
    }
    if (s_editor_view.preview) {
        editor_preview_exit();
        return;
    }
    /* Preview renders Markdown; other syntaxes stay in source view. */
    if (doc->syntax != EDITOR_SYNTAX_MARKDOWN) {
        editor_status("preview needs a Markdown file");
        return;
    }
    /* Immediate feedback: the render (TTF variant load over SD) can take
     * tens of seconds after heavy transcript use (O6 latency tail). */
    editor_status("rendering preview...");
    editor_preview_show();
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
     * document column, past the gutter. With wrap, the caret sits inside
     * its visual chunk: x relative to the chunk start, y at the chunk. */
    if (s_editor_view.wrap && editor_wrap_px() > 0) {
        size_t row = editor_doc_cursor_row(doc);
        size_t col = editor_doc_cursor_col(doc);
        size_t vrow = editor_visual_row(doc, row, col);
        size_t k = editor_chunk_of(doc, row, col);
        size_t cs = 0;
        size_t ce = 0;
        editor_chunk_cols(doc, row, k, &cs, &ce);
        x = editor_gutter_width() + editor_measure_prefix(doc, row, col) -
            editor_measure_prefix(doc, row, cs);
        y = (lv_coord_t)(vrow * (size_t)lh);
    } else {
        x = editor_gutter_width() + editor_measure_prefix(doc,
                    editor_doc_cursor_row(doc), editor_doc_cursor_col(doc));
        y = (lv_coord_t)(editor_doc_cursor_row(doc) * (size_t)lh);
    }
    lv_obj_set_pos(s_editor_view.cursor, x, y);

    s_editor_view.cursor_visible = true;
    if (s_editor_view.cursor_timer != NULL) {
        lv_timer_reset(s_editor_view.cursor_timer);
    }
    lv_obj_remove_flag(s_editor_view.cursor, LV_OBJ_FLAG_HIDDEN);
}

/** Live-update the cursor blink period of an open editor (0 = steady:
 * timer deleted, cursor forced visible). No-op unless open. */
void editor_view_set_blink_ms(uint32_t blink_ms)
{
    /* Check `open` under the lock: testing it before taking the lock races a
     * close on the LVGL task and can create a cursor timer after
     * editor_view_close() already ran, orphaning it. */
    if (!lvgl_port_lock(0)) {
        return;
    }
    if (!s_editor_view.open) {
        lvgl_port_unlock();
        return;
    }
    if (blink_ms == 0) {
        if (s_editor_view.cursor_timer != NULL) {
            lv_timer_delete(s_editor_view.cursor_timer);
            s_editor_view.cursor_timer = NULL;
        }
        s_editor_view.cursor_visible = true;
        if (s_editor_view.cursor != NULL) {
            lv_obj_remove_flag(s_editor_view.cursor, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_editor_view.cursor_timer != NULL) {
        lv_timer_set_period(s_editor_view.cursor_timer, blink_ms);
    } else {
        s_editor_view.cursor_timer = lv_timer_create(editor_cursor_blink_cb,
                                                     blink_ms, NULL);
    }
    lvgl_port_unlock();
}

/** Reposition the current-line highlight behind the cursor row. */
static void editor_update_current_line(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    lv_coord_t lh = editor_line_height();

    if (s_editor_view.current_line == NULL || doc == NULL) {
        return;
    }
    /* Preview hides this bar; every rebuild restores it. */
    lv_obj_remove_flag(s_editor_view.current_line, LV_OBJ_FLAG_HIDDEN);
    if (s_editor_view.wrap && editor_wrap_px() > 0) {
        /* Single visual line under the caret (not the whole doc row). */
        lv_obj_set_y(s_editor_view.current_line,
                     (lv_coord_t)(editor_visual_row(
                                      doc, editor_doc_cursor_row(doc),
                                      editor_doc_cursor_col(doc)) *
                                  (size_t)lh));
    } else {
        lv_obj_set_y(s_editor_view.current_line,
                     (lv_coord_t)(editor_doc_cursor_row(doc) * (size_t)lh));
    }
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
    if (s_editor_view.wrap && editor_wrap_px() > 0) {
        /* Wrapped: one rect per visual segment overlapped by the range. */
        size_t row;
        for (row = s_row; row <= e_row; row++) {
            size_t len = editor_doc_line_length(doc, row);
            size_t c0 = (row == s_row) ? s_col : 0;
            size_t c1 = (row == e_row) ? e_col : len;
            size_t k = 0;
            if (c0 > len) c0 = len;
            if (c1 > len) c1 = len;
            if (c1 < c0) c1 = c0;
            if (c0 >= c1) {
                continue;
            }
            while (true) {
                size_t cs = 0;
                size_t ce = 0;
                size_t a;
                size_t b;
                lv_coord_t x0;
                lv_coord_t x1;
                lv_obj_t *rect;
                editor_chunk_cols(doc, row, k, &cs, &ce);
                if (cs >= c1) {
                    break;
                }
                a = (c0 > cs) ? c0 : cs;
                b = (c1 < ce) ? c1 : ce;
                if (b > a) {
                    x0 = editor_gutter_width() +
                         editor_measure_prefix(doc, row, a) -
                         editor_measure_prefix(doc, row, cs);
                    x1 = editor_gutter_width() +
                         editor_measure_prefix(doc, row, b) -
                         editor_measure_prefix(doc, row, cs);
                    rect = lv_obj_create(s_editor_view.sel_layer);
                    lv_obj_remove_flag(rect, LV_OBJ_FLAG_SCROLLABLE |
                                             LV_OBJ_FLAG_CLICKABLE |
                                             LV_OBJ_FLAG_CLICK_FOCUSABLE);
                    lv_obj_set_style_bg_color(rect,
                        lv_color_hex(P4_CONFIG_EDITOR_SELECTION_COLOR), 0);
                    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_width(rect, 0, 0);
                    lv_obj_set_style_radius(rect, 0, 0);
                    {
                        size_t vr = editor_visual_row(doc, row, a);
                        lv_obj_set_pos(rect, x0, (lv_coord_t)(vr * (size_t)lh));
                    }
                    lv_obj_set_size(rect, x1 - x0, lh);
                }
                if (ce >= len) {
                    break;
                }
                k++;
            }
        }
        return;
    }
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
    if (s_editor_view.wrap && editor_wrap_px() > 0) {
        cy = (lv_coord_t)(editor_visual_row(doc, editor_doc_cursor_row(doc),
                                            editor_doc_cursor_col(doc)) *
                          (size_t)lh);
    } else {
        cy = (lv_coord_t)(editor_doc_cursor_row(doc) * (size_t)lh);
    }

    if (cy < scroll_y) {
        lv_obj_scroll_to_y(surface, cy, LV_ANIM_OFF);
    } else if (cy + lh > scroll_y + view_h) {
        lv_obj_scroll_to_y(surface, cy + lh - view_h, LV_ANIM_OFF);
    }

    /* Horizontal follow (wrap off, long rows): keep the caret's x in view
     * so wrapped-off lines stay reachable without touch-dragging. */
    if (!s_editor_view.wrap) {
        lv_coord_t scroll_x = lv_obj_get_scroll_x(surface);
        lv_coord_t view_w = lv_obj_get_width(surface);
        lv_coord_t cx = editor_gutter_width() +
                        editor_measure_prefix(doc, editor_doc_cursor_row(doc),
                                              editor_doc_cursor_col(doc));
        if (cx < scroll_x) {
            lv_obj_scroll_to_x(surface, cx, LV_ANIM_OFF);
        } else if (cx + editor_cell_width() > scroll_x + view_w) {
            lv_obj_scroll_to_x(surface, cx + editor_cell_width() - view_w,
                               LV_ANIM_OFF);
        }
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
        case EDITOR_PROMPT_OPEN:         label = "Open"; break;
        case EDITOR_PROMPT_OPEN_CONFIRM: label = "Open without saving"; break;
        case EDITOR_PROMPT_QUIT_CONFIRM: label = "Quit without saving"; break;
        default: break;
        }
        s_editor_view.prompt_buf[s_editor_view.prompt_len] = '\0';
        if (s_editor_view.prompt == EDITOR_PROMPT_QUIT_CONFIRM ||
            s_editor_view.prompt == EDITOR_PROMPT_OPEN_CONFIRM) {
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
        const char *syntax = "txt";
        switch (doc->syntax) {
        case EDITOR_SYNTAX_BATCH: syntax = "bat"; break;
        case EDITOR_SYNTAX_MARKDOWN: syntax = "md"; break;
        case EDITOR_SYNTAX_JSON: syntax = "json"; break;
        default: break;
        }
        snprintf(buf, P4_CONFIG_SD_PATH_BYTES + 96, "%s  %s  Ln %u, Col %u  %s %s%s%s%s%s",
                 name,
                 doc->modified ? "*" : " ",
                 (unsigned)(editor_doc_cursor_row(doc) + 1),
                 (unsigned)(editor_doc_cursor_col(doc) + 1),
                 doc->overwrite ? "OVR" : "INS",
                 syntax,
                 doc->crlf ? " CRLF" : "",
                 doc->readonly ? " RO" : "",
                 s_editor_view.preview ? "  PREVIEW" : "",
                 s_editor_view.wrap ? "  WRAP" : "");
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

    if ((type == EDITOR_PROMPT_SAVE_AS || type == EDITOR_PROMPT_OPEN) &&
        s_editor_view.doc != NULL) {
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
    /* Text-entry prompts (and Y/N confirms) need the letters page even when the
     * session was navigating; commit/cancel restore the nav page so the touch
     * keyboard follows the context. No-op when the OSK is hidden (USB). */
    if (type != EDITOR_PROMPT_NONE) {
        keyboard_set_mode(KEYBOARD_MODE_TEXT_LOWER);
    }
    editor_status_default();
}

/** Cancel the active prompt and restore the ordinary status line. */
static void editor_prompt_cancel(void)
{
    s_editor_view.prompt = EDITOR_PROMPT_NONE;
    s_editor_view.replace_armed = false;
    keyboard_set_mode(KEYBOARD_MODE_NAV);
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

    if (p == EDITOR_PROMPT_OPEN_CONFIRM) {
        if (ch == 'y' || ch == 'Y') {
            /* Discard guard is accepted by the user: start the path prompt. */
            editor_prompt_begin(EDITOR_PROMPT_OPEN);
        } else if (ch == 'n' || ch == 'N') {
            s_editor_view.prompt = EDITOR_PROMPT_NONE;
            editor_status("open cancelled");
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
                             row, col, s_editor_view.find_case,
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
                                 s_editor_view.find_case,
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

static void editor_replace_all(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    size_t count;

    if (doc == NULL || s_editor_view.find_len == 0) {
        editor_status("no search");
        return;
    }

    count = editor_doc_replace_all(doc, s_editor_view.find_str, s_editor_view.find_len,
                                   s_editor_view.replace_str, s_editor_view.replace_len,
                                   s_editor_view.find_case);
    if (count > 0) {
        s_editor_view.replace_armed = false;
        editor_rebuild();
        editor_status("replaced %u match%s", (unsigned)count, count == 1 ? "" : "es");
    } else {
        editor_status("no matches");
    }
}

static void editor_case_toggle(void)
{
    s_editor_view.find_case = !s_editor_view.find_case;
    editor_status("case: %s", s_editor_view.find_case ? "sensitive" : "insensitive");
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

/** Route a file switch to the worker (loads the file and swaps the view). */
static void editor_request_open(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    editor_control_t *ctl = s_editor_view.control;

    if (doc == NULL || ctl == NULL) {
        return;
    }
    if (doc->modified) {
        /* A discard guard (same shape as the quit confirm). */
        editor_prompt_begin(EDITOR_PROMPT_OPEN_CONFIRM);
        return;
    }
    editor_prompt_begin(EDITOR_PROMPT_OPEN);
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
    case EDITOR_PROMPT_OPEN:
        if (s_editor_view.prompt_len == 0) {
            s_editor_view.prompt = EDITOR_PROMPT_NONE;
            editor_status("open: no path");
            break;
        }
        if (s_editor_view.control == NULL) {
            s_editor_view.prompt = EDITOR_PROMPT_NONE;
            editor_status("open failed");
            break;
        }
        snprintf(s_editor_view.control->open_path,
                 sizeof(s_editor_view.control->open_path), "%s",
                 s_editor_view.prompt_buf);
        s_editor_view.control->open_requested = true;
        if (s_editor_view.control->event_group != NULL) {
            xEventGroupSetBits((EventGroupHandle_t)s_editor_view.control->event_group,
                               EDITOR_EVENT_OPEN);
        }
        s_editor_view.prompt = EDITOR_PROMPT_NONE;
        editor_status("opening %s...", s_editor_view.prompt_buf);
        break;
    default:
        s_editor_view.prompt = EDITOR_PROMPT_NONE;
        break;
    }

    /* A finished prompt returns the touch keyboard to navigation; a prompt
     * that chained (Replace find -> With) keeps the letters page. */
    if (s_editor_view.prompt == EDITOR_PROMPT_NONE) {
        keyboard_set_mode(KEYBOARD_MODE_NAV);
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
    /* A cursor jump can leave the materialized window behind (Go-to-Line,
     * page move, arrow at a window edge): shift it before scrolling. */
    editor_render_follow_cursor();
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
    /* Wrapped: step visual rows (keeps the column); unwrapped: doc rows. */
    if (s_editor_view.wrap && editor_wrap_px() > 0) {
        size_t vrow = editor_visual_row(doc, editor_doc_cursor_row(doc),
                                        editor_doc_cursor_col(doc));
        size_t want_col = editor_doc_cursor_col(doc);
        size_t nrow;
        size_t nchunk;
        if (delta > 0) {
            vrow += lines;
        } else if (vrow >= lines) {
            vrow -= lines;
        } else {
            vrow = 0;
        }
        editor_visual_to_doc(vrow, &nrow, &nchunk);
        doc->cursor_row = nrow;
        {
            /* Keep the column when landing inside a chunk. */
            size_t cs = 0;
            size_t ce = 0;
            size_t len = editor_doc_line_length(doc, nrow);
            editor_chunk_cols(doc, nrow, nchunk, &cs, &ce);
            if (want_col < cs) {
                doc->cursor_col = cs;
            } else if (want_col > ce) {
                doc->cursor_col = ce;
            } else {
                doc->cursor_col = want_col;
            }
            if (doc->cursor_col > len) {
                doc->cursor_col = len;
            }
        }
        return;
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

/** Step one visual row up/down keeping the column (wrap mode). */
static void editor_visual_step(editor_doc_t *doc, int delta)
{
    size_t vrow;
    size_t want_col;
    size_t nrow;
    size_t nchunk;

    if (doc == NULL) {
        return;
    }
    vrow = editor_visual_row(doc, editor_doc_cursor_row(doc),
                             editor_doc_cursor_col(doc));
    want_col = editor_doc_cursor_col(doc);
    if (delta > 0) {
        vrow++;
    } else if (vrow > 0) {
        vrow--;
    } else {
        return;
    }
    editor_visual_to_doc(vrow, &nrow, &nchunk);
    {
        size_t cs = 0;
        size_t ce = 0;
        size_t len = editor_doc_line_length(doc, nrow);
        doc->cursor_row = nrow;
        editor_chunk_cols(doc, nrow, nchunk, &cs, &ce);
        if (want_col < cs) {
            doc->cursor_col = cs;
        } else if (want_col > ce) {
            doc->cursor_col = ce;
        } else {
            doc->cursor_col = want_col;
        }
        if (doc->cursor_col > len) {
            doc->cursor_col = len;
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
    case EDITOR_KEY_UP:
        if (s_editor_view.wrap && editor_wrap_px() > 0) {
            editor_visual_step(doc, -1);
        } else {
            editor_doc_cursor_up(doc);
        }
        break;
    case EDITOR_KEY_DOWN:
        if (s_editor_view.wrap && editor_wrap_px() > 0) {
            editor_visual_step(doc, +1);
        } else {
            editor_doc_cursor_down(doc);
        }
        break;
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
    if (doc->readonly) {
        editor_status("read-only: use Save As for a new path");
        return;
    }
    ctl->save_requested = true;
    if (ctl->event_group != NULL) {
        xEventGroupSetBits((EventGroupHandle_t)ctl->event_group,
                           EDITOR_EVENT_SAVE);
    }
    editor_status("saving...");
}

/** Route a reload to the worker (re-reads the file, resets undo). */
static void editor_request_reload(void)
{
    editor_doc_t *doc = s_editor_view.doc;
    editor_control_t *ctl = s_editor_view.control;

    if (doc == NULL || ctl == NULL) {
        return;
    }
    if (doc->path[0] == '\0') {
        editor_status("nothing to reload (unnamed buffer)");
        return;
    }
    ctl->reload_requested = true;
    if (ctl->event_group != NULL) {
        xEventGroupSetBits((EventGroupHandle_t)ctl->event_group,
                           EDITOR_EVENT_RELOAD);
    }
    editor_status("reloading...");
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

    if (key == EDITOR_KEY_PREVIEW) {
        editor_preview_toggle();
        return;
    }

    /* Preview is read-only: navigation scrolls the rendered view, edits are
     * swallowed, quit flows through to its normal handler below. */
    if (s_editor_view.preview && key != EDITOR_KEY_QUIT) {
        lv_coord_t page;
        switch (key) {
        case EDITOR_KEY_PAGE_UP:
            page = lv_obj_get_height(s_editor_view.surface);
            lv_obj_scroll_by(s_editor_view.surface, 0, page > 0 ? page : 200,
                             LV_ANIM_OFF);
            return;
        case EDITOR_KEY_PAGE_DOWN:
            page = lv_obj_get_height(s_editor_view.surface);
            lv_obj_scroll_by(s_editor_view.surface, 0, page > 0 ? -page : -200,
                             LV_ANIM_OFF);
            return;
        case EDITOR_KEY_UP:
            lv_obj_scroll_by(s_editor_view.surface, 0,
                             editor_line_height(), LV_ANIM_OFF);
            return;
        case EDITOR_KEY_DOWN:
            lv_obj_scroll_by(s_editor_view.surface, 0,
                             -editor_line_height(), LV_ANIM_OFF);
            return;
        default:
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
    case EDITOR_KEY_RELOAD:
        editor_request_reload();
        return;
    case EDITOR_KEY_OPEN:
        editor_request_open();
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
    case EDITOR_KEY_REPLACE_ALL:
        editor_replace_all();
        return;
    case EDITOR_KEY_CASE_TOGGLE:
        editor_case_toggle();
        return;
    case EDITOR_KEY_COMMENT: {
        size_t changed = editor_doc_comment_toggle(doc);
        if (changed > 0) {
            editor_rebuild();
            editor_status("commented %u line%s", (unsigned)changed,
                          changed == 1 ? "" : "s");
        } else if (doc->syntax != EDITOR_SYNTAX_BATCH &&
                   doc->syntax != EDITOR_SYNTAX_JSON &&
                   doc->syntax != EDITOR_SYNTAX_MARKDOWN) {
            editor_status("comment toggle needs batch/json/md");
        } else {
            editor_status("nothing to comment");
        }
        return;
    }
    case EDITOR_KEY_MATCH_JUMP:
        if (editor_doc_match_jump(doc)) {
            editor_layout_update();
        } else {
            editor_status("no match");
        }
        return;
    case EDITOR_KEY_WRAP_TOGGLE:
        s_editor_view.wrap = !s_editor_view.wrap;
        editor_rebuild();
        editor_status("wrap %s", s_editor_view.wrap ? "on" : "off");
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

/** One row of the on-screen keyboard nav/edit command tables. */
typedef struct {
    const char *label;
    editor_key_t key;
} editor_osk_entry_t;

/** Every named action label on the editor Nav/User pages. The single source
 * for OSK buttons, the `keyboard_test.py` page screenshots, and unit tests. */
static const editor_osk_entry_t k_editor_osk_actions[] = {
    {"Tab", EDITOR_KEY_TAB},
    {"Del", EDITOR_KEY_DELETE},
    {"Ins", EDITOR_KEY_OVERWRITE},
    {"Home", EDITOR_KEY_HOME},
    {"End", EDITOR_KEY_END},
    {"PgUp", EDITOR_KEY_PAGE_UP},
    {"PgDn", EDITOR_KEY_PAGE_DOWN},
    {"Save", EDITOR_KEY_SAVE},
    {"SaveAs", EDITOR_KEY_SAVE_AS},
    {"Open", EDITOR_KEY_OPEN},
    {"Find", EDITOR_KEY_FIND},
    {"Replace", EDITOR_KEY_REPLACE},
    {"Rep", EDITOR_KEY_REPLACE},
    {"ReplAll", EDITOR_KEY_REPLACE_ALL},
    {"All", EDITOR_KEY_REPLACE_ALL},
    {"Case", EDITOR_KEY_CASE_TOGGLE},
    {"Goto", EDITOR_KEY_GOTO_LINE},
    {"Undo", EDITOR_KEY_UNDO},
    {"Redo", EDITOR_KEY_REDO},
    {"Next", EDITOR_KEY_FIND_NEXT},
    {"Preview", EDITOR_KEY_PREVIEW},
    {"Prev", EDITOR_KEY_PREVIEW},
    {"Copy", EDITOR_KEY_COPY},
    {"Cut", EDITOR_KEY_CUT},
    {"Paste", EDITOR_KEY_PASTE},
    {"SelAll", EDITOR_KEY_SELECT_ALL},
    {"WordL", EDITOR_KEY_WORD_LEFT},
    {"WordR", EDITOR_KEY_WORD_RIGHT},
    {"DocTop", EDITOR_KEY_DOC_HOME},
    {"DocBot", EDITOR_KEY_DOC_END},
    {"DelLine", EDITOR_KEY_DELETE_LINE},
    {"DelEOL", EDITOR_KEY_DELETE_EOL},
    {"Quit", EDITOR_KEY_QUIT},
    {"Exit", EDITOR_KEY_QUIT},
    {"Comment", EDITOR_KEY_COMMENT},
    {"Match", EDITOR_KEY_MATCH_JUMP},
    {"Wrap", EDITOR_KEY_WRAP_TOGGLE},
    {"Reload", EDITOR_KEY_RELOAD},
};

bool editor_osk_key_from_label(const char *label, editor_key_t *out)
{
    size_t i;

    if (label == NULL || out == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(k_editor_osk_actions) / sizeof(k_editor_osk_actions[0]); i++) {
        if (strcmp(label, k_editor_osk_actions[i].label) == 0) {
            *out = k_editor_osk_actions[i].key;
            return true;
        }
    }
    return false;
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

    /* Named action labels from the editor Nav/Edit OSK pages (single table
     * above). LVGL symbols and mode buttons are handled by their own branches;
     * everything else here runs the mapped editor key. */
    {
        editor_key_t mapped = EDITOR_KEY_NONE;

        if (editor_osk_key_from_label(label, &mapped) && mapped != EDITOR_KEY_NONE) {
            editor_apply_key(mapped, 0);
            return true;
        }
    }
    if (strcmp(label, "abc") == 0 || strcmp(label, "ABC") == 0 ||
        strcmp(label, "1#") == 0 || strcmp(label, "Nav") == 0 ||
        strcmp(label, "Nav1") == 0 || strcmp(label, "Nav2") == 0 ||
        strcmp(label, "Edit") == 0) {
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
    s_editor_view.preview = false;
    s_editor_view.find_case = P4_CONFIG_EDITOR_FIND_CASE_SENSITIVE;
    s_editor_view.wrap = false;
    editor_mem_free(s_editor_view.wrap_counts);
    s_editor_view.wrap_counts = NULL;
    s_editor_view.wrap_count_rows = 0;
    s_editor_view.wrap_px_used = 0;

    /* Hide the shell's own transcript spans so the editor's span group owns
     * the container (its content is retained and re-shown on close). */
    shell_spans = windows_get_transcript_spans();
    if (shell_spans != NULL) {
        lv_obj_add_flag(shell_spans, LV_OBJ_FLAG_HIDDEN);
    }

    /* Full-height invisible spacer: establishes the scroll range for the whole
     * document even when only a window of rows is rendered as spans. Created
     * first so it stays behind every other child. */
    s_editor_view.spacer = lv_obj_create(surface);
    if (s_editor_view.spacer == NULL) {
        editor_view_close();
        return false;
    }
    lv_obj_set_pos(s_editor_view.spacer, 0, 0);
    lv_obj_set_width(s_editor_view.spacer, LV_PCT(100));
    lv_obj_set_height(s_editor_view.spacer, 0);
    lv_obj_remove_flag(s_editor_view.spacer,
                       LV_OBJ_FLAG_SCROLLABLE |
                       LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_opa(s_editor_view.spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_editor_view.spacer, 0, 0);
    lv_obj_set_style_pad_all(s_editor_view.spacer, 0, 0);

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
    /* Blink period 0 = steady cursor: no timer, stays visible. */
    if (P4_CONFIG_CURSOR_BLINK_MS > 0) {
        s_editor_view.cursor_timer = lv_timer_create(editor_cursor_blink_cb,
                                                     P4_CONFIG_CURSOR_BLINK_MS,
                                                     NULL);
    } else {
        s_editor_view.cursor_timer = NULL;
    }

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
    /* Scroll: shift the materialized row window so touch scrolling keeps text. */
    lv_obj_add_event_cb(surface, editor_scroll_event_cb, LV_EVENT_SCROLL, NULL);

    s_editor_view.render_first = 0;
    s_editor_view.render_busy = false;

    /* Deferred content build on a later LVGL pass (synchronous fallback so
     * the surface is never left blank when the async queue is full). */
    if (lv_async_call(editor_rebuild_cb, NULL) != LV_RESULT_OK) {
        editor_rebuild();
    }

    /* The on-screen keyboard stays visible and drives the editor. Unbind the
     * shell input line so LVGL's default keyboard handler does not type into
     * the hidden textarea; every OSK button routes through our callback. Open
     * on the navigation page so the editor's buttons are reachable by touch
     * immediately (abc returns to letters to type). */
    keyboard_bind_textarea(NULL);
    keyboard_set_mode(KEYBOARD_MODE_NAV);

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
    if (s_editor_view.spacer != NULL) {
        lv_obj_delete(s_editor_view.spacer);
        s_editor_view.spacer = NULL;
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

    editor_mem_free(s_editor_view.widths);
    s_editor_view.widths = NULL;
    s_editor_view.width_count = 0;
    s_editor_view.width_row = (size_t)-1;

    editor_mem_free(s_editor_view.wrap_counts);
    s_editor_view.wrap_counts = NULL;
    s_editor_view.wrap_count_rows = 0;
    s_editor_view.wrap = false;

    /* Restore the shell input line as the OSK target and return the touch
     * keyboard to the letters page (it opened on the navigation page). */
    keyboard_bind_textarea(windows_get_input_line());
    keyboard_set_mode(KEYBOARD_MODE_TEXT_LOWER);

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

void editor_view_get_state(editor_view_state_t *out)
{
    editor_doc_t *doc;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->open = s_editor_view.open;
    out->preview = s_editor_view.preview;
    out->wrap = s_editor_view.wrap;
    doc = s_editor_view.doc;
    if (doc != NULL) {
        out->modified = doc->modified;
        out->readonly = doc->readonly;
        out->cursor_row = doc->cursor_row;
        out->cursor_col = doc->cursor_col;
        out->line_count = doc->line_count;
        snprintf(out->path, sizeof(out->path), "%s", doc->path);
    }
}

/** Whether the rendered Markdown preview is showing (read-only). */
bool editor_view_is_preview(void)
{
    return s_editor_view.open && s_editor_view.preview;
}

/** Rebuild open editor spans with current fonts after a font switch.
 * No-op unless open. Takes the port lock (caller is the worker). */
void editor_view_refresh_fonts(void)
{
    if (!s_editor_view.open) {
        return;
    }
    if (!lvgl_port_lock(0)) {
        return;
    }
    editor_rebuild();
    lvgl_port_unlock();
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

void editor_view_notify_reloaded(bool ok)
{
    if (!s_editor_view.open) {
        return;
    }
    if (ok) {
        if (!lvgl_port_lock(0)) {
            return;
        }
        editor_rebuild();
        lvgl_port_unlock();
        editor_status("reloaded");
    } else {
        editor_status("reload failed");
    }
}

void editor_view_notify_reloaded_cb(void *user_data)
{
    editor_view_notify_reloaded((intptr_t)user_data != 0);
}

void editor_view_notify_opened(bool ok)
{
    if (!s_editor_view.open) {
        return;
    }
    if (!ok) {
        editor_status("open failed");
        return;
    }
    /* The worker swapped a new document into the same object. Drop caches
     * that describe the old file (wrap, widths, preview) and rebuild; the
     * status bar picks up the new path via editor_layout_update(). */
    s_editor_view.prompt = EDITOR_PROMPT_NONE;
    s_editor_view.replace_armed = false;
    s_editor_view.preview = false;
    s_editor_view.touch_selecting = false;
    editor_mem_free(s_editor_view.wrap_counts);
    s_editor_view.wrap_counts = NULL;
    s_editor_view.wrap_count_rows = 0;
    s_editor_view.wrap_px_used = 0;
    editor_mem_free(s_editor_view.widths);
    s_editor_view.widths = NULL;
    s_editor_view.width_row = (size_t)-1;
    s_editor_view.width_count = 0;
    editor_rebuild();
    editor_status("opened");
}

void editor_view_notify_opened_cb(void *user_data)
{
    editor_view_notify_opened((intptr_t)user_data != 0);
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

    if (s_editor_view.wrap && editor_wrap_px() > 0 &&
        s_editor_view.doc != NULL) {
        /* Wrapped: y addresses a visual row; map to (doc row, chunk) and
         * resolve x inside that chunk, clamped to its columns. */
        size_t vrow = (size_t)(rel_y / lh);
        size_t drow = 0;
        size_t chunk = 0;
        size_t cs = 0;
        size_t ce = 0;
        size_t col;
        editor_visual_to_doc(vrow, &drow, &chunk);
        editor_chunk_cols(s_editor_view.doc, drow, chunk, &cs, &ce);
        col = editor_column_from_x(s_editor_view.doc, drow, rel_x);
        if (col < cs) {
            col = cs;
        }
        if (col > ce) {
            col = ce;
        }
        if (row_out != NULL) {
            *row_out = drow;
        }
        if (col_out != NULL) {
            *col_out = col;
        }
        return;
    }

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

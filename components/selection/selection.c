/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file selection.c
 * @brief Shared text-selection core: range model, text assembly, commit sink.
 *
 * Hardware-free: no LVGL, no shell. The surfaces register a cell-text provider
 * and the shell installs the one clipboard commit sink; everything here is
 * pure C, so test/main/test_selection.c exercises it directly.
 */

#include "selection.h"

#include "p4heap.h"
#include "p4minishell_config.h"

#include <string.h>

/* The clipboard cannot hold more than this, so a selection never assembles
 * more than this either: the temporary buffer stays small even when a surface
 * hands in an unbounded column bound (the transcript's row range). */
#define SEL_TEXT_MAX_BYTES   (P4_CONFIG_CLIPBOARD_BYTES + 1)

static sel_commit_fn s_commit = NULL;

/* ========================================================================
 * RANGE MODEL
 * ======================================================================== */

void sel_range_init(sel_range_t *range, int row, int col)
{
    if (range == NULL) {
        return;
    }
    range->ar = row;
    range->ac = col;
    range->fr = row;
    range->fc = col;
}

void sel_range_focus(sel_range_t *range, int row, int col)
{
    if (range == NULL) {
        return;
    }
    range->fr = row;
    range->fc = col;
}

void sel_range_bounds(const sel_range_t *range, sel_bounds_t *out)
{
    if (out == NULL) {
        return;
    }
    if (range == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    out->r0 = (range->ar < range->fr) ? range->ar : range->fr;
    out->r1 = (range->ar > range->fr) ? range->ar : range->fr;
    out->c0 = (range->ac < range->fc) ? range->ac : range->fc;
    out->c1 = (range->ac > range->fc) ? range->ac : range->fc;
}

bool sel_bounds_contains(const sel_bounds_t *bounds, int row, int col)
{
    if (bounds == NULL) {
        return false;
    }
    return row >= bounds->r0 && row <= bounds->r1 &&
           col >= bounds->c0 && col <= bounds->c1;
}

bool sel_bounds_is_anchor(const sel_bounds_t *bounds)
{
    if (bounds == NULL) {
        return true;
    }
    return bounds->r0 == bounds->r1 && bounds->c0 == bounds->c1;
}

int sel_cell_order(int r0, int c0, int r1, int c1)
{
    if (r0 != r1) {
        return (r0 < r1) ? -1 : 1;
    }
    if (c0 != c1) {
        return (c0 < c1) ? -1 : 1;
    }
    return 0;
}

/* ========================================================================
 * COPY-FROM-PROVIDER
 * ======================================================================== */

size_t sel_assembly_from_cells(sel_cell_text_fn get, void *ctx,
                               const sel_bounds_t *bounds,
                               char *out, size_t out_size)
{
    size_t pos = 0;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (get == NULL || bounds == NULL || out_size < 2) {
        return 0;
    }

    for (int r = bounds->r0; r <= bounds->r1; r++) {
        size_t line = pos;

        for (int c = bounds->c0; c <= bounds->c1; c++) {
            char cell[8] = { 0 };

            if (!get(ctx, r, c, cell, sizeof(cell))) {
                continue;
            }
            cell[sizeof(cell) - 1] = '\0';
            for (size_t i = 0; cell[i] != '\0'; i++) {
                if (pos + 2 > out_size) {
                    goto full;
                }
                out[pos++] = cell[i];
            }
        }
        /* Trim trailing blanks on the copied row (a grid pads its rows). */
        while (pos > line && (out[pos - 1] == ' ' || out[pos - 1] == '\t')) {
            pos--;
        }
        if (pos + 2 > out_size) {
            break;
        }
        out[pos++] = '\n';
    }
full:
    /* Drop the final row separator: the copied text has no trailing '\n'. */
    if (pos > 0 && out[pos - 1] == '\n') {
        pos--;
    }
    out[pos] = '\0';
    return pos;
}

void sel_set_commit(sel_commit_fn commit)
{
    s_commit = commit;
}

bool sel_commit(const char *text)
{
    if (s_commit == NULL || text == NULL || text[0] == '\0') {
        return false;
    }
    s_commit(text);
    return true;
}

bool sel_copy_cells(sel_cell_text_fn get, void *ctx, const sel_bounds_t *bounds)
{
    char *out;
    size_t len;

    if (get == NULL || bounds == NULL) {
        return false;
    }
    out = p4heap_alloc_psram(SEL_TEXT_MAX_BYTES);
    if (out == NULL) {
        return false;
    }
    len = sel_assembly_from_cells(get, ctx, bounds, out, SEL_TEXT_MAX_BYTES);
    if (len == 0 || !sel_commit(out)) {
        p4heap_free(out);
        return false;
    }
    p4heap_free(out);
    return true;
}

/* ========================================================================
 * CHARACTER / ROW HELPERS
 * ======================================================================== */

/** Byte length of the UTF-8 sequence starting at @p s (1 when malformed). */
static size_t sel_utf8_len(const char *s)
{
    unsigned char c;

    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    c = (unsigned char)s[0];
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return ((unsigned char)s[1] & 0xC0) == 0x80 ? 2 : 1;
    }
    if ((c & 0xF0) == 0xE0) {
        return (((unsigned char)s[1] & 0xC0) == 0x80 &&
                ((unsigned char)s[2] & 0xC0) == 0x80) ? 3 : 1;
    }
    if ((c & 0xF8) == 0xF0) {
        return (((unsigned char)s[1] & 0xC0) == 0x80 &&
                ((unsigned char)s[2] & 0xC0) == 0x80 &&
                ((unsigned char)s[3] & 0xC0) == 0x80) ? 4 : 1;
    }
    return 1;
}

int sel_line_count(const char *text)
{
    int lines = 1;

    if (text == NULL) {
        return 0;
    }
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            lines++;
        }
    }
    return lines;
}

int sel_char_count(const char *text)
{
    int chars = 0;

    if (text == NULL) {
        return 0;
    }
    for (const char *p = text; *p != '\0'; ) {
        size_t len = sel_utf8_len(p);

        if (len == 0) {
            break;
        }
        p += len;
        chars++;
    }
    return chars;
}

int sel_row_start_index(const char *text, int row)
{
    int chars = 0;
    int line = 1;
    const char *p;

    if (text == NULL || row < 1) {
        return -1;
    }
    if (row == 1) {
        return 0;
    }
    /* The start of row N is the char right after the (N-1)th newline. */
    for (p = text; *p != '\0'; ) {
        size_t len = sel_utf8_len(p);

        if (len == 0) {
            break;
        }
        if (*p == '\n') {
            line++;
            if (line == row) {
                return chars + 1;
            }
        }
        p += len;
        chars++;
    }
    return -1;
}

bool sel_text_span_from_range(const char *text, const sel_range_t *range,
                              int *first_out, int *last_out)
{
    int anchor;
    int focus;
    int first;
    int last;

    if (text == NULL || range == NULL || first_out == NULL ||
        last_out == NULL) {
        return false;
    }
    anchor = sel_row_col_to_index(text, range->ar, range->ac);
    focus = sel_row_col_to_index(text, range->fr, range->fc);
    if (anchor < 0 || focus < 0) {
        return false;
    }
    /* Reading order, not the drag direction: a backward drag selects the same
     * text an equivalent forward one would. */
    if (sel_cell_order(range->ar, range->ac, range->fr, range->fc) <= 0) {
        first = anchor;
        last = focus + 1;
    } else {
        first = focus;
        last = anchor + 1;
    }
    if (last <= first) {
        return false;
    }
    *first_out = first;
    *last_out = last;
    return true;
}

bool sel_text_range_contains(const char *text, const sel_range_t *range,
                             int index)
{
    int first = 0;
    int last = 0;

    if (!sel_text_span_from_range(text, range, &first, &last)) {
        return false;
    }
    return index >= first && index < last;
}

int sel_row_col_to_index(const char *text, int row, int col)
{
    int start;
    int index;
    const char *p;

    if (text == NULL || row < 1 || col < 1) {
        return -1;
    }
    start = sel_row_start_index(text, row);
    if (start < 0) {
        return -1;
    }
    index = start;
    p = text + sel_char_byte_offset(text, start);
    /* Advance (col - 1) glyphs, stopping at the end of the row. */
    for (int step = 1; step < col; step++) {
        size_t len;

        if (*p == '\0' || *p == '\n') {
            break;
        }
        len = sel_utf8_len(p);
        if (len == 0) {
            break;
        }
        p += len;
        index++;
    }
    return index;
}

bool sel_index_to_row_col(const char *text, int index, int *row_out,
                          int *col_out)
{
    int row = 1;
    int col = 1;
    const char *p;

    if (text == NULL || index < 0) {
        return false;
    }
    p = text;
    for (int i = 0; i < index; i++) {
        size_t len = sel_utf8_len(p);

        if (len == 0) {
            return false;
        }
        if (*p == '\n') {
            row++;
            col = 1;
        } else {
            col++;
        }
        p += len;
    }
    if (row_out != NULL) {
        *row_out = row;
    }
    if (col_out != NULL) {
        *col_out = col;
    }
    return true;
}

size_t sel_char_byte_offset(const char *text, int index)
{
    size_t off = 0;

    if (text == NULL || index < 0) {
        return 0;
    }
    for (int i = 0; i < index; i++) {
        size_t len = sel_utf8_len(text + off);

        if (len == 0) {
            break;
        }
        off += len;
    }
    return off;
}

size_t sel_utf8_glyph(const char *s, char *out, size_t out_size)
{
    size_t len;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (s == NULL) {
        return 0;
    }
    len = sel_utf8_len(s);
    if (len == 0) {
        return 0;
    }
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return len;
}

/* ========================================================================
 * CONTIGUOUS TEXT SPAN
 * ======================================================================== */

bool sel_copy_text_span(const char *text, int first, int last)
{
    char *out;
    size_t pos = 0;
    size_t b0;
    size_t b1;
    const char *p;
    const char *end;

    if (text == NULL || first < 0 || last < first) {
        return false;
    }
    b0 = sel_char_byte_offset(text, first);
    b1 = sel_char_byte_offset(text, last);
    if (b1 <= b0) {
        return false;
    }
    out = p4heap_alloc_psram(SEL_TEXT_MAX_BYTES);
    if (out == NULL) {
        return false;
    }
    out[0] = '\0';
    p = text + b0;
    end = text + b1;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t raw = (nl != NULL) ? (size_t)(nl - p) : (size_t)(end - p);
        size_t len = raw;

        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
            len--;
        }
        for (size_t i = 0; i < len && pos + 2 < SEL_TEXT_MAX_BYTES; i++) {
            out[pos++] = p[i];
        }
        if (pos + 2 < SEL_TEXT_MAX_BYTES) {
            out[pos++] = '\n';
        }
        p += raw + ((nl != NULL) ? 1u : 0u);
    }
    if (pos > 0 && out[pos - 1] == '\n') {
        pos--;
    }
    out[pos] = '\0';
    if (pos == 0 || !sel_commit(out)) {
        p4heap_free(out);
        return false;
    }
    p4heap_free(out);
    return true;
}

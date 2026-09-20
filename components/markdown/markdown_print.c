/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file markdown_print.c
 * @brief Print-to-file layout for the writerdeck `markdown export ... print`.
 *
 * Turns plain text (the ANSI-stripped Markdown render) into fixed-size pages:
 * @c cols columns by @c rows lines, each with a `title   Page N` header and a
 * form feed between pages. Wrapping is measured in DISPLAY cells (ASCII 1,
 * CJK 2, combining 0) so wide text aligns, tabs expand to spaces, and a
 * multibyte codepoint is never split. Over-wide words are hard-broken. The
 * output is a plain-text "PDF-less" document suitable for sharing over
 * `httpd` or printing from a host. Pure and bounded.
 *
 * Truncation contract: the return value is the byte count the document needs
 * (excluding the NUL); a value >= the caller's buffer size means truncation.
 */

#include "markdown.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char *out;      /**< destination, or NULL for measure-only. */
    size_t cap;
    size_t len;
    bool overflow;
    int lines;      /**< newlines emitted (count/emit share one path). */
} print_writer_t;

static void pw_raw(print_writer_t *w, const char *s, size_t n)
{
    if (w->out != NULL && !w->overflow) {
        if (w->len + n + 1 > w->cap) {
            w->overflow = true;
        } else {
            memcpy(w->out + w->len, s, n);
        }
    }
    w->len += n;
}

static void pw_str(print_writer_t *w, const char *s)
{
    if (s != NULL) {
        pw_raw(w, s, strlen(s));
    }
}

static void pw_char(print_writer_t *w, char c)
{
    if (c == '\n') {
        w->lines++;
    }
    pw_raw(w, &c, 1);
}

/* ========================================================================
 * DISPLAY WIDTH (shared rules with markdown.c's markdown_display_width)
 * ======================================================================== */

/** Decode one codepoint. Returns bytes consumed (0 on NUL/invalid). */
static size_t print_utf8_decode(const char *s, size_t avail, unsigned long *cp_out)
{
    unsigned char lead;
    size_t len;
    unsigned long cp;
    size_t i;

    if (s == NULL || avail == 0 || s[0] == '\0') {
        return 0;
    }
    lead = (unsigned char)s[0];
    if (lead < 0x80) {
        if (cp_out != NULL) {
            *cp_out = lead;
        }
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        len = 2;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        len = 3;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        len = 4;
        cp = lead & 0x07;
    } else {
        return 0;
    }
    if (len > avail) {
        return 0;
    }
    for (i = 1; i < len; i++) {
        unsigned char cont = (unsigned char)s[i];
        if ((cont & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    if (cp_out != NULL) {
        *cp_out = cp;
    }
    return len;
}

/** Cells for a codepoint: 0 control/combining, 2 wide (CJK), else 1. */
static int print_cp_cells(unsigned long cp)
{
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
        return 0;
    }
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) ||
        cp == 0xFEFF) {
        return 0;
    }
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

/** Cells for [s, s+n), expanding tabs to the next 4-cell stop. */
static int print_cells(const char *s, size_t n)
{
    size_t i = 0;
    int col = 0;

    while (i < n) {
        if (s[i] == '\t') {
            col += 4 - (col % 4);
            i++;
            continue;
        }
        {
            unsigned long cp = 0;
            size_t len = print_utf8_decode(s + i, n - i, &cp);
            if (len == 0) {
                len = 1;
                cp = (unsigned char)s[i];
            }
            col += print_cp_cells(cp);
            i += len;
        }
    }
    return col;
}

/** Bounded byte length of the largest prefix of [s, s+n) fitting @p max_cells. */
static size_t print_prefix_cells(const char *s, size_t n, int max_cells)
{
    size_t i = 0;
    int col = 0;

    if (max_cells <= 0) {
        return 0;
    }
    while (i < n) {
        int cells;
        size_t len;

        if (s[i] == '\t') {
            cells = 4 - (col % 4);
            len = 1;
        } else {
            unsigned long cp = 0;
            len = print_utf8_decode(s + i, n - i, &cp);
            if (len == 0) {
                len = 1;
                cp = (unsigned char)s[i];
            }
            cells = print_cp_cells(cp);
        }
        if (col + cells > max_cells) {
            break;
        }
        col += cells;
        i += len;
    }
    return i;
}

/* ========================================================================
 * WRAPPING (count and emit share this one path, so page math never drifts)
 * ======================================================================== */

/** Emit [s, s+n) wrapped to @p cols cells (blank line -> one newline). In
 *  measure-only mode (@c out NULL) it still counts the produced lines. */
static void print_wrap_emit(print_writer_t *w, const char *s, size_t n, int cols)
{
    size_t pos = 0;

    if (n == 0) {
        pw_char(w, '\n');
        return;
    }
    while (pos < n) {
        size_t i = pos;
        size_t last_space = (size_t)-1;
        size_t next_pos;
        int col = 0;

        while (i < n) {
            int cells;
            size_t len;

            if (s[i] == '\t') {
                cells = 4 - (col % 4);
                len = 1;
            } else {
                unsigned long cp = 0;
                len = print_utf8_decode(s + i, n - i, &cp);
                if (len == 0) {
                    len = 1;
                    cp = (unsigned char)s[i];
                }
                cells = print_cp_cells(cp);
            }
            if (col + cells > cols && col > 0) {
                break;
            }
            if (s[i] == ' ' || s[i] == '\t') {
                last_space = i;
            }
            col += cells;
            i += len;
        }
        if (i < n && last_space != (size_t)-1 && last_space > pos) {
            next_pos = last_space + 1; /* wrap before the space */
            i = last_space;
        } else {
            next_pos = i;
        }
        if (i <= pos) {
            /* A single codepoint wider than cols: always make progress. */
            unsigned long cp = 0;
            size_t len = print_utf8_decode(s + pos, n - pos, &cp);
            i = pos + (len == 0 ? 1 : len);
            next_pos = i;
        }
        /* Emit [pos, i) with tabs expanded and codepoints kept whole. */
        {
            size_t k = pos;
            int c = 0;
            while (k < i) {
                if (s[k] == '\t') {
                    int t = 4 - (c % 4);
                    while (t-- > 0) {
                        pw_char(w, ' ');
                    }
                    c += 4 - (c % 4);
                    k++;
                } else {
                    unsigned long cp = 0;
                    size_t len = print_utf8_decode(s + k, i - k, &cp);
                    if (len == 0) {
                        len = 1;
                        cp = (unsigned char)s[k];
                    }
                    pw_raw(w, s + k, len);
                    c += print_cp_cells(cp);
                    k += len;
                }
            }
        }
        pw_char(w, '\n');
        pos = next_pos;
        while (pos < n && (s[pos] == ' ' || s[pos] == '\t')) {
            pos++;
        }
    }
}

/** Wrapped line count for one source line (uses the emit path in count mode). */
static int print_wrapped_count(const char *s, size_t n, int cols)
{
    print_writer_t counter;

    counter.out = NULL;
    counter.cap = 0;
    counter.len = 0;
    counter.overflow = false;
    counter.lines = 0;
    print_wrap_emit(&counter, s, n, cols);
    return counter.lines;
}

/* ========================================================================
 * PAGE HEADER + ENTRY POINT
 * ======================================================================== */

/** `title` left-aligned + `Page N` right-aligned, then a rule line. */
static void print_page_header(print_writer_t *w, const char *title, int page,
                              int cols)
{
    char pbuf[24];
    const char *name = (title != NULL && title[0] != '\0') ? title : "P4MiniShell";
    int pnum = snprintf(pbuf, sizeof(pbuf), "Page %d", page);
    size_t name_n;
    int name_cells;
    int i;

    if (pnum < 0) {
        pnum = 0;
    }
    /* Keep the page number itself within the column budget. */
    {
        size_t keep = print_prefix_cells(pbuf, (size_t)pnum, cols);
        pnum = (int)keep;
    }
    name_n = print_prefix_cells(name, strlen(name),
                                cols - pnum - 1 > 0 ? cols - pnum - 1 : 0);
    pw_raw(w, name, name_n);
    name_cells = print_cells(name, name_n);
    for (i = name_cells; i < cols - pnum - 1; i++) {
        pw_char(w, ' ');
    }
    pw_char(w, ' ');
    pw_str(w, pbuf);
    pw_char(w, '\n');
    for (i = 0; i < cols; i++) {
        pw_char(w, '-');
    }
    pw_char(w, '\n');
}

size_t markdown_render_print(const char *text, const char *title,
                             int cols, int rows,
                             char *out, size_t out_size)
{
    print_writer_t w;
    const char *p;
    int page = 1;
    int used;

    /* No source, or no destination at all: nothing to lay out. */
    if (text == NULL || out == NULL || out_size == 0) {
        if (out != NULL && out_size > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    w.out = out;
    w.cap = out_size;
    w.len = 0;
    w.overflow = false;
    w.lines = 0;
    out[0] = '\0';
    if (cols < 8) {
        cols = 8;
    }
    if (rows < 4) {
        rows = 4;
    }

    print_page_header(&w, title, page, cols);
    used = 2; /* header + rule */

    for (p = text; *p != '\0'; ) {
        const char *nl = strchr(p, '\n');
        size_t n = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        int need;

        if (n > 0 && p[n - 1] == '\r') {
            n--; /* CRLF and bare CR are not printable columns */
        }
        need = print_wrapped_count(p, n, cols);
        if (used + need > rows) {
            pw_char(&w, '\f'); /* form feed: explicit page boundary */
            pw_char(&w, '\n');
            page++;
            print_page_header(&w, title, page, cols);
            used = 2;
        }
        print_wrap_emit(&w, p, n, cols);
        used += need;
        p = (nl != NULL) ? nl + 1 : p + n;
        /* The page is complete: stop so the required size is exact (the old
         * count-only continuation undercounted the truncated output). */
        if (used >= rows) {
            break;
        }
    }
    pw_char(&w, '\n');

    if (w.out != NULL && w.cap > 0) {
        if (w.overflow || w.len >= w.cap) {
            w.out[w.cap - 1] = '\0';
        } else {
            w.out[w.len] = '\0';
        }
    }
    return w.len;
}

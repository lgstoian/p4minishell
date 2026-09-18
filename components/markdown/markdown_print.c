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
 * form feed between pages. Long lines are word-wrapped; over-wide words are
 * hard-broken. The output is a plain-text "PDF-less" document suitable for
 * sharing over `httpd` or printing from a host. Pure and bounded.
 */

#include "markdown.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    bool overflow;
} print_writer_t;

static void pw_raw(print_writer_t *w, const char *s, size_t n)
{
    if (w->len + n + 1 > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->out + w->len, s, n);
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
    pw_raw(w, &c, 1);
}

/** Write a `title` + right-aligned `Page N` header line, then a rule line. */
static void print_page_header(print_writer_t *w, const char *title,
                              int page, int cols)
{
    char pbuf[24];
    const char *name = (title != NULL && title[0] != '\0') ? title : "P4MiniShell";
    int name_len = (int)strlen(name);
    int pnum = snprintf(pbuf, sizeof(pbuf), "Page %d", page);
    int i;

    if (name_len > cols) {
        name_len = cols;
    }
    pw_raw(w, name, (size_t)name_len);
    /* Pad so the page number ends at the right margin. */
    for (i = name_len; i < cols - pnum - 1; i++) {
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

/** Number of wrapped lines a source line [s, s+n) produces at @p cols. */
static int print_wrapped_count(const char *s, size_t n, int cols)
{
    size_t pos = 0;
    int count = 0;

    if (n == 0) {
        return 1;
    }
    while (pos < n) {
        size_t le = pos;
        int width = 0;
        size_t last_space = (size_t)-1;
        while (le < n && width < cols) {
            width += (s[le] == '\t') ? 4 : 1;
            if (s[le] == ' ' || s[le] == '\t') {
                last_space = le;
            }
            le++;
        }
        if (le < n && last_space != (size_t)-1 && last_space >= pos) {
            le = last_space;
        }
        pos = le;
        while (pos < n && (s[pos] == ' ' || s[pos] == '\t')) {
            pos++;
        }
        count++;
    }
    return count;
}

/** Emit a source line wrapped to @p cols (blank line -> one newline). */
static void print_wrap_emit(print_writer_t *w, const char *s, size_t n, int cols)
{
    size_t pos = 0;

    if (n == 0) {
        pw_char(w, '\n');
        return;
    }
    while (pos < n && !w->overflow) {
        size_t le = pos;
        int width = 0;
        size_t last_space = (size_t)-1;
        while (le < n && width < cols) {
            width += (s[le] == '\t') ? 4 : 1;
            if (s[le] == ' ' || s[le] == '\t') {
                last_space = le;
            }
            le++;
        }
        if (le < n && last_space != (size_t)-1 && last_space >= pos) {
            le = last_space;
        }
        pw_raw(w, s + pos, le - pos);
        pw_char(w, '\n');
        pos = le;
        while (pos < n && (s[pos] == ' ' || s[pos] == '\t')) {
            pos++;
        }
    }
}

size_t markdown_render_print(const char *text, const char *title,
                             int cols, int rows,
                             char *out, size_t out_size)
{
    print_writer_t w = { out, out_size, 0, false };
    const char *p;
    int page = 1;
    int used;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (cols < 8) {
        cols = 8;
    }
    if (rows < 4) {
        rows = 4;
    }
    if (text == NULL) {
        text = "";
    }

    print_page_header(&w, title, page, cols);
    used = 2; /* header + rule */

    for (p = text; *p != '\0' && !w.overflow; ) {
        const char *nl = strchr(p, '\n');
        size_t n = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        int need = print_wrapped_count(p, n, cols);

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
    }
    pw_char(&w, '\n');

    out[w.len < out_size ? w.len : out_size - 1] = '\0';
    return w.len;
}

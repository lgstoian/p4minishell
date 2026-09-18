/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file markdown_html.c
 * @brief HTML serializer for the CommonMark-ish subset (writerdeck export).
 *
 * `markdown export <src> <out> html` uses this to turn a `.md` file into a
 * self-contained HTML fragment for sharing over `httpd`. It is a distinct
 * output format from the ANSI renderer in markdown.c (which stays the single
 * implementation for on-device rendering); this serializer has its own
 * bounded writer and block/inline handling and never touches that path.
 *
 * Supported: ATX headings, fenced code, blockquotes, unordered/ordered lists,
 * `---` rules, blank-line paragraphs, and inline **bold** / *italic* /
 * ~~strike~~ / `code` / [text](url). Everything else is escaped verbatim.
 */

#include "markdown.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Bounded writer: tracks truncation instead of overflowing. */
typedef struct {
    char *out;
    size_t cap;
    size_t len;
    bool overflow;
} html_writer_t;

static void html_raw(html_writer_t *w, const char *s, size_t n)
{
    if (w->len + n + 1 > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->out + w->len, s, n);
    w->len += n;
}

static void html_str(html_writer_t *w, const char *s)
{
    if (s != NULL) {
        html_raw(w, s, strlen(s));
    }
}

static void html_fmt(html_writer_t *w, const char *fmt, ...)
{
    char buf[128];
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) {
        html_raw(w, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
}

/** Append @p s with `<>&"` escaped. */
static void html_escape(html_writer_t *w, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        switch (s[i]) {
        case '<': html_str(w, "&lt;"); break;
        case '>': html_str(w, "&gt;"); break;
        case '&': html_str(w, "&amp;"); break;
        case '"': html_str(w, "&quot;"); break;
        default:  html_raw(w, s + i, 1); break;
        }
    }
}

/* ---- inline ---- */

/** Find @p needle in [p, end). Returns pointer or NULL. */
static const char *html_find(const char *p, const char *end, const char *needle)
{
    size_t nl = strlen(needle);
    const char *q;

    if (nl == 0) {
        return NULL;
    }
    for (q = p; q + nl <= end; q++) {
        if (memcmp(q, needle, nl) == 0) {
            return q;
        }
    }
    return NULL;
}

/** Emit [p, end) as inline HTML (escaped, with simple emphasis). */
static void html_inline(html_writer_t *w, const char *p, const char *end)
{
    while (p < end && !w->overflow) {
        /* Code span (opaque). */
        if (*p == '`') {
            const char *close = html_find(p + 1, end, "`");
            if (close != NULL) {
                html_str(w, "<code>");
                html_escape(w, p + 1, (size_t)(close - (p + 1)));
                html_str(w, "</code>");
                p = close + 1;
                continue;
            }
        }
        /* Bold, then strike, then italic (longer markers first). */
        {
            static const char *const marks[][2] = {
                { "**", "strong" }, { "__", "strong" },
                { "~~", "del" }, { "*", "em" }, { "_", "em" },
            };
            size_t m;
            bool matched = false;
            for (m = 0; m < sizeof(marks) / sizeof(marks[0]); m++) {
                size_t ml = strlen(marks[m][0]);
                if ((size_t)(end - p) < ml || memcmp(p, marks[m][0], ml) != 0) {
                    continue;
                }
                const char *close = html_find(p + ml, end, marks[m][0]);
                if (close == NULL || close == p + ml) {
                    continue;
                }
                html_fmt(w, "<%s>", marks[m][1]);
                html_escape(w, p + ml, (size_t)(close - (p + ml)));
                html_fmt(w, "</%s>", marks[m][1]);
                p = close + ml;
                matched = true;
                break;
            }
            if (matched) {
                continue;
            }
        }
        /* Link [text](url). */
        if (*p == '[') {
            const char *rb = html_find(p + 1, end, "](");
            if (rb != NULL) {
                const char *url = rb + 2;
                const char *rp = html_find(url, end, ")");
                if (rp != NULL) {
                    html_str(w, "<a href=\"");
                    html_escape(w, url, (size_t)(rp - url));
                    html_str(w, "\">");
                    html_escape(w, p + 1, (size_t)(rb - (p + 1)));
                    html_str(w, "</a>");
                    p = rp + 1;
                    continue;
                }
            }
        }
        html_escape(w, p, 1);
        p++;
    }
}

/* ---- block ---- */

/** True when [p,end) is a `---`/`***`/`___` rule (3+ of one marker). */
static bool html_is_hr(const char *p, const char *end)
{
    char c = *p;
    size_t count = 0;

    if (c != '-' && c != '*' && c != '_') {
        return false;
    }
    for (; p < end; p++) {
        if (*p == c) {
            count++;
        } else if (*p != ' ' && *p != '\t') {
            return false;
        }
    }
    return count >= 3;
}

/** True when [p,end) opens a fence; copies the marker run char. */
static bool html_is_fence(const char *p, const char *end, char *mark)
{
    size_t n = 0;

    while (p + n < end && p[n] == *p && (*p == '`' || *p == '~')) {
        n++;
    }
    if (n >= 3 && (*p == '`' || *p == '~')) {
        *mark = *p;
        return true;
    }
    return false;
}

size_t markdown_render_html(const char *md, char *out, size_t out_size)
{
    html_writer_t w = { out, out_size, 0, false };
    const char *p;
    const char *end;
    char fence = 0;
    bool in_code = false;
    const char *code_open = NULL;
    bool in_list = false;
    bool in_quote = false;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (md == NULL) {
        return 0;
    }
    end = md + strlen(md);

    /* Close any open container before switching block kind. */
#define HTML_CLOSE_LIST()  do { if (in_list)  { html_str(&w, "</ul>\n"); in_list = false; } } while (0)
#define HTML_CLOSE_QUOTE() do { if (in_quote) { html_str(&w, "</blockquote>\n"); in_quote = false; } } while (0)

    for (p = md; p < end && !w.overflow; ) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl != NULL ? nl : end;
        const char *line = p;
        const char *trim = line;
        size_t tlen;

        p = nl != NULL ? nl + 1 : end;

        /* Fenced code: copy verbatim (escaped) until the closing fence. */
        if (in_code) {
            char mark;
            if (html_is_fence(trim, line_end, &mark) && mark == fence) {
                html_str(&w, "</code></pre>\n");
                in_code = false;
                code_open = NULL;
            } else {
                html_escape(&w, line, (size_t)(line_end - line));
                html_str(&w, "\n");
            }
            continue;
        }

        while (trim < line_end && (*trim == ' ' || *trim == '\t')) {
            trim++;
        }
        tlen = (size_t)(line_end - trim);

        if (tlen == 0) {
            HTML_CLOSE_LIST();
            HTML_CLOSE_QUOTE();
            continue;
        }

        /* Opening fence. */
        {
            char mark;
            if (html_is_fence(trim, line_end, &mark) && tlen >= 3) {
                HTML_CLOSE_LIST();
                HTML_CLOSE_QUOTE();
                html_str(&w, "<pre><code>");
                in_code = true;
                fence = mark;
                code_open = trim;
                continue;
            }
        }

        /* Heading. */
        if (*trim == '#') {
            int level = 0;
            const char *h = trim;
            while (h < line_end && *h == '#' && level < 6) {
                level++;
                h++;
            }
            if (h < line_end && (*h == ' ' || *h == '\t')) {
                HTML_CLOSE_LIST();
                HTML_CLOSE_QUOTE();
                while (h < line_end && (*h == ' ' || *h == '\t')) {
                    h++;
                }
                /* Trim a trailing hashes run. */
                {
                    const char *he = line_end;
                    while (he > h && (he[-1] == ' ' || he[-1] == '#')) {
                        he--;
                    }
                    html_fmt(&w, "<h%d>", level);
                    html_inline(&w, h, he);
                    html_fmt(&w, "</h%d>\n", level);
                }
                continue;
            }
        }

        /* Horizontal rule. */
        if (html_is_hr(trim, line_end)) {
            HTML_CLOSE_LIST();
            HTML_CLOSE_QUOTE();
            html_str(&w, "<hr>\n");
            continue;
        }

        /* Blockquote. */
        if (*trim == '>') {
            const char *q = trim + 1;
            while (q < line_end && (*q == ' ' || *q == '\t')) {
                q++;
            }
            if (!in_quote) {
                HTML_CLOSE_LIST();
                html_str(&w, "<blockquote>\n");
                in_quote = true;
            }
            html_inline(&w, q, line_end);
            html_str(&w, "\n");
            continue;
        }

        /* Unordered list. */
        if ((*trim == '-' || *trim == '*' || *trim == '+') &&
            trim + 1 < line_end && (trim[1] == ' ' || trim[1] == '\t')) {
            if (!in_list) {
                HTML_CLOSE_QUOTE();
                html_str(&w, "<ul>\n");
                in_list = true;
            }
            html_str(&w, "<li>");
            html_inline(&w, trim + 2, line_end);
            html_str(&w, "</li>\n");
            continue;
        }

        /* Paragraph. */
        HTML_CLOSE_LIST();
        HTML_CLOSE_QUOTE();
        html_str(&w, "<p>");
        html_inline(&w, trim, line_end);
        html_str(&w, "</p>\n");
    }

    if (in_code) {
        html_str(&w, "</code></pre>\n");
    }
    HTML_CLOSE_LIST();
    HTML_CLOSE_QUOTE();
#undef HTML_CLOSE_LIST
#undef HTML_CLOSE_QUOTE

    out[w.len < out_size ? w.len : out_size - 1] = '\0';
    (void)code_open;
    return w.len;
}

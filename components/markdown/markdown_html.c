/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file markdown_html.c
 * @brief HTML serializer for the CommonMark-ish subset (writerdeck export).
 *
 * `markdown export <src> <out> html` uses this to turn a `.md` file into a
 * standalone reader page for sharing over `httpd`. This serializer is a
 * distinct output format from the ANSI renderer in markdown.c (which stays
 * the single implementation for on-device rendering); it has its own bounded
 * writer and block/inline handling and never touches that path. The page
 * wrapper only adds the `<!DOCTYPE html>` shell and reader CSS around the
 * body produced here.
 *
 * Supported: ATX headings (indent <= 3, level 1-6), fenced code (with a
 * `language-*` class), nested blockquotes, unordered/ordered lists (nested,
 * task-list checkboxes), GFM tables with alignment, `---` rules, inline
 * **bold** / *italic* / ~~strike~~ / `code` (multi-backtick) / [text](url) /
 * ![alt](url) images, `<autolinks>` and bare URLs, backslash escapes, and
 * CommonMark flanking rules so `2 * 3`, `foo_bar`, and `a * b * c` pass
 * through untouched. Unsafe URL schemes (javascript/data/vbscript/file) are
 * never linked. Everything else is escaped verbatim.
 *
 * Truncation contract: each entry point returns the number of bytes the
 * document *needs* (excluding the NUL). When the return value is >= the
 * caller's buffer size the output was truncated and is not a complete
 * document; callers must detect and report that (see md_commands.c).
 */

#include "markdown.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
/* ---- bounded writer ----
 * Counts every byte that should be written; copies into @c out only while it
 * fits. On overflow it keeps counting (so the return value is the required
 * size) and stops copying, leaving a valid prefix plus a NUL. */
typedef struct {
    char *out;      /**< destination, or NULL for measure-only. */
    size_t cap;     /**< destination size including NUL. */
    size_t len;     /**< bytes that should be written (excl. NUL). */
    bool overflow;  /**< destination was too small. */
} html_writer_t;

static void html_raw(html_writer_t *w, const char *s, size_t n)
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

/** Append @p s with `<>&"'` escaped (single quotes kept inert in attributes). */
static void html_escape(html_writer_t *w, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        switch (s[i]) {
        case '<': html_str(w, "&lt;"); break;
        case '>': html_str(w, "&gt;"); break;
        case '&': html_str(w, "&amp;"); break;
        case '"': html_str(w, "&quot;"); break;
        case '\'': html_str(w, "&#39;"); break;
        default:  html_raw(w, s + i, 1); break;
        }
    }
}

/** NUL-terminate and return the required byte count (excluding the NUL). A
 *  return value >= the destination size means the output was truncated. */
static size_t html_finish(html_writer_t *w)
{
    if (w->out != NULL && w->cap > 0) {
        if (w->overflow || w->len >= w->cap) {
            w->out[w->cap - 1] = '\0';
        } else {
            w->out[w->len] = '\0';
        }
    }
    return w->len;
}

/* ========================================================================
 * UTF-8 + CHARACTER CLASSES (inline emphasis needs CommonMark flanking)
 * ======================================================================== */

/** Decode one codepoint. Returns bytes consumed (0 on NUL/invalid). */
static size_t html_utf8_decode(const char *s, size_t avail, unsigned long *cp_out)
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

static bool html_is_space_cp(unsigned long cp)
{
    return cp == ' ' || cp == '\t';
}

static bool html_is_punct_cp(unsigned long cp)
{
    return cp < 0x80 && ispunct((int)cp) != 0;
}

/* Codepoint at byte offset (0 on end/invalid). */
static unsigned long html_cp_at(const char *p, const char *end)
{
    unsigned long cp = 0;
    if (p < end) {
        html_utf8_decode(p, (size_t)(end - p), &cp);
    }
    return cp;
}

/* Byte length of the codepoint starting at p (1 if invalid, 0 at end). */
static size_t html_cp_len(const char *p, const char *end)
{
    unsigned long cp = 0;
    size_t len;
    if (p >= end) {
        return 0;
    }
    len = html_utf8_decode(p, (size_t)(end - p), &cp);
    return len > 0 ? len : 1;
}

/* Codepoint ending exactly at q (q > start), or 0. */
static unsigned long html_prev_cp(const char *start, const char *q)
{
    const char *r;

    if (q <= start) {
        return 0;
    }
    r = q - 1;
    while (r > start && ((unsigned char)*r & 0xC0) == 0x80) {
        r--;
    }
    return html_cp_at(r, q);
}

/* Flanking test for a delimiter run at q (run bytes already matched). */
static void html_flank(const char *start, const char *q, const char *end,
                       size_t run, bool underscore,
                       bool *can_open, bool *can_close)
{
    unsigned long prev = html_prev_cp(start, q);
    unsigned long next = 0;
    const char *after = q + run;

    if (after < end) {
        html_utf8_decode(after, (size_t)(end - after), &next);
    }
    if (underscore) {
        bool left_ok = (prev == 0 || html_is_space_cp(prev) || html_is_punct_cp(prev));
        bool right_ok = (next == 0 || html_is_space_cp(next) || html_is_punct_cp(next));
        *can_open = left_ok && next != 0 && !html_is_space_cp(next);
        *can_close = right_ok && prev != 0 && !html_is_space_cp(prev);
    } else {
        *can_open = next != 0 && !html_is_space_cp(next);
        *can_close = prev != 0 && !html_is_space_cp(prev);
    }
}

/* Skip a backtick code span starting at p (p points at `). Returns the byte
 * after the closing run, or NULL when unterminated. */
static const char *html_skip_code_span(const char *p, const char *end)
{
    size_t open_len = 0;
    const char *q = p;

    while (q < end && *q == '`') {
        open_len++;
        q++;
    }
    while (q < end) {
        if (*q == '`') {
            size_t close_len = 0;
            const char *r = q;
            while (r < end && *r == '`') {
                close_len++;
                r++;
            }
            if (close_len == open_len) {
                return r;
            }
            q = r;
        } else {
            q += html_cp_len(q, end);
        }
    }
    return NULL;
}

/* ---- emphasis delimiters ---- */
typedef struct {
    const char *text;
    size_t len;
    const char *open;
    const char *close;
    bool underscore;
} html_delim_t;

static const html_delim_t HTML_DELIMS[] = {
    { "**", 2, "<strong>", "</strong>", false },
    { "__", 2, "<strong>", "</strong>", true },
    { "~~", 2, "<del>", "</del>", false },
    { "*", 1, "<em>", "</em>", false },
    { "_", 1, "<em>", "</em>", true },
};

#define HTML_NEST_MAX 8

/* Find the matching closer for a delimiter run (nesting-aware, skipping code
 * spans). base is the scan-region start (for prev computation). */
static const char *html_find_closer(const char *base, const char *p,
                                    const char *end, const html_delim_t *d)
{
    int nest = 1;
    const char *q = p;

    while (q < end) {
        if (*q == '`') {
            const char *after = html_skip_code_span(q, end);
            if (after == NULL) {
                return NULL;
            }
            q = after;
            continue;
        }
        if ((size_t)(end - q) >= d->len && memcmp(q, d->text, d->len) == 0) {
            bool can_open = false;
            bool can_close = false;
            html_flank(base, q, end, d->len, d->underscore, &can_open, &can_close);
            if (can_close && !(can_open && nest > 1)) {
                nest--;
                if (nest == 0) {
                    return q;
                }
            } else if (can_open) {
                nest++;
            }
            q += d->len;
            continue;
        }
        q += html_cp_len(q, end);
    }
    return NULL;
}

/* ---- URL safety ---- */

/** Case-insensitive prefix test. */
static bool html_ci_prefix(const char *s, size_t n, const char *pfx)
{
    size_t i;
    size_t pl = strlen(pfx);

    if (n < pl) {
        return false;
    }
    for (i = 0; i < pl; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != pfx[i]) {
            return false;
        }
    }
    return true;
}

/** Reject `javascript:`/`data:`/`vbscript:`/`file:` URLs (and controls). */
static bool html_url_is_safe(const char *u, size_t n)
{
    char scheme[16];
    size_t sl = 0;

    while (n > 0 && (unsigned char)u[0] <= 0x20) {
        u++;
        n--;
    }
    if (n == 0 || !((u[0] >= 'a' && u[0] <= 'z') ||
                    (u[0] >= 'A' && u[0] <= 'Z'))) {
        return true;
    }
    while (sl < n && sl < sizeof(scheme) - 1) {
        char c = u[sl];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            scheme[sl++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        } else {
            break;
        }
    }
    if (sl >= n || u[sl] != ':') {
        return true; /* no scheme: relative/anchor URL */
    }
    scheme[sl] = '\0';
    if (strcmp(scheme, "javascript") == 0 || strcmp(scheme, "data") == 0 ||
        strcmp(scheme, "vbscript") == 0 || strcmp(scheme, "file") == 0) {
        return false;
    }
    return true;
}

/* ---- inline ---- */

static void html_inline(html_writer_t *w, const char *p, const char *end,
                        int depth);

/* Parse [text](url) at p ('['). Returns the byte after the construct, or
 * NULL when it is not a link (caller emits the literal text). Emits an
 * <a>; unsafe URLs are refused so the text stays inert. */
static const char *html_parse_link(html_writer_t *w, const char *p,
                                   const char *end, int depth)
{
    const char *q = p + 1;
    int nest = 1;
    const char *text_end = NULL;
    const char *url;
    const char *url_end;
    const char *url_close;

    while (q < end) {
        if (*q == '`') {
            const char *after = html_skip_code_span(q, end);
            if (after == NULL) {
                return NULL;
            }
            q = after;
            continue;
        }
        if (*q == '[') {
            nest++;
        } else if (*q == ']') {
            nest--;
            if (nest == 0) {
                text_end = q;
                break;
            }
        }
        q += html_cp_len(q, end);
    }
    if (text_end == NULL || text_end + 1 >= end || text_end[1] != '(') {
        return NULL;
    }
    url = text_end + 2;
    while (url < end && (*url == ' ' || *url == '\t' || *url == '<')) {
        url++;
    }
    url_end = url;
    while (url_end < end && *url_end != ')' && *url_end != ' ' && *url_end != '\t') {
        url_end++;
    }
    url_close = url_end;
    while (url_close < end && (*url_close == ' ' || *url_close == '\t')) {
        url_close++;
    }
    if (url_close < end && *url_close == '>') {
        url_close++;
        while (url_close < end && (*url_close == ' ' || *url_close == '\t')) {
            url_close++;
        }
    }
    if (url_close >= end || *url_close != ')') {
        return NULL;
    }
    {
        const char *ue = url_end;
        if (ue > url && ue[-1] == '>') {
            ue--;
        }
        if (!html_url_is_safe(url, (size_t)(ue - url))) {
            return NULL; /* refuse to link; caller escapes the raw text */
        }
        html_str(w, "<a href=\"");
        html_escape(w, url, (size_t)(ue - url));
        html_str(w, "\">");
        html_inline(w, p + 1, text_end, depth + 1);
        html_str(w, "</a>");
    }
    return url_close + 1;
}

/* Parse ![alt](url) at p ('!'). Returns the byte after the construct, or
 * NULL. The alt text is escaped verbatim (no inline markup). */
static const char *html_parse_image(html_writer_t *w, const char *p,
                                    const char *end)
{
    const char *q = p + 2;
    int nest = 1;
    const char *alt_end = NULL;
    const char *url;
    const char *url_end;
    const char *url_close;

    while (q < end) {
        if (*q == '`') {
            const char *after = html_skip_code_span(q, end);
            if (after == NULL) {
                return NULL;
            }
            q = after;
            continue;
        }
        if (*q == '[') {
            nest++;
        } else if (*q == ']') {
            nest--;
            if (nest == 0) {
                alt_end = q;
                break;
            }
        }
        q += html_cp_len(q, end);
    }
    if (alt_end == NULL || alt_end + 1 >= end || alt_end[1] != '(') {
        return NULL;
    }
    url = alt_end + 2;
    while (url < end && (*url == ' ' || *url == '\t' || *url == '<')) {
        url++;
    }
    url_end = url;
    while (url_end < end && *url_end != ')' && *url_end != ' ' && *url_end != '\t') {
        url_end++;
    }
    url_close = url_end;
    while (url_close < end && (*url_close == ' ' || *url_close == '\t')) {
        url_close++;
    }
    if (url_close < end && *url_close == '>') {
        url_close++;
        while (url_close < end && (*url_close == ' ' || *url_close == '\t')) {
            url_close++;
        }
    }
    if (url_close >= end || *url_close != ')') {
        return NULL;
    }
    {
        const char *ue = url_end;
        if (ue > url && ue[-1] == '>') {
            ue--;
        }
        if (!html_url_is_safe(url, (size_t)(ue - url))) {
            return NULL;
        }
        html_str(w, "<img src=\"");
        html_escape(w, url, (size_t)(ue - url));
        html_str(w, "\" alt=\"");
        html_escape(w, p + 2, (size_t)(alt_end - (p + 2)));
        html_str(w, "\">");
    }
    return url_close + 1;
}

/* Emit `<a>` for `<scheme:...>` / `<user@host>`, or false when not one. */
static bool html_autolink(html_writer_t *w, const char *inner, size_t n)
{
    size_t i;
    bool has_colon = false;
    bool has_at = false;

    if (n == 0) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = inner[i];
        if (c == ':') {
            has_colon = true;
        } else if (c == '@') {
            has_at = true;
        } else if (c == ' ' || c == '\t' || c == '"' || c == '<' || c == '>') {
            return false;
        }
    }
    if (has_colon) {
        if (!html_url_is_safe(inner, n)) {
            return false;
        }
        html_str(w, "<a href=\"");
        html_escape(w, inner, n);
        html_str(w, "\">");
        html_escape(w, inner, n);
        html_str(w, "</a>");
        return true;
    }
    if (has_at) {
        html_str(w, "<a href=\"mailto:");
        html_escape(w, inner, n);
        html_str(w, "\">");
        html_escape(w, inner, n);
        html_str(w, "</a>");
        return true;
    }
    return false;
}

/* Turn a bare http(s):// or www. run into a link. Returns the byte after it,
 * or NULL when p does not start one. */
static const char *html_bare_url(html_writer_t *w, const char *p,
                                 const char *end)
{
    size_t avail = (size_t)(end - p);
    const char *e;
    bool www = false;
    size_t off;

    if (html_ci_prefix(p, avail, "http://")) {
        off = 7;
    } else if (html_ci_prefix(p, avail, "https://")) {
        off = 8;
    } else if (html_ci_prefix(p, avail, "www.")) {
        www = true;
        off = 4;
    } else {
        return NULL;
    }
    e = p + off;
    while (e < end) {
        unsigned char c = (unsigned char)*e;
        if (c == ' ' || c == '\t' || c == '<' || c == '>' || c == '"' ||
            c == '`' || c == '\n' || c == '\r') {
            break;
        }
        e++;
    }
    /* Trim trailing sentence punctuation and unbalanced ')'. */
    while (e > p + off) {
        char c = e[-1];
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') {
            e--;
            continue;
        }
        if (c == ')') {
            int bal = 0;
            const char *t;
            for (t = p; t < e; t++) {
                if (*t == '(') {
                    bal++;
                } else if (*t == ')') {
                    bal--;
                }
            }
            if (bal < 0) {
                e--;
                continue;
            }
        }
        break;
    }
    if (e <= p + off) {
        return NULL;
    }
    html_str(w, "<a href=\"");
    if (www) {
        html_str(w, "http://");
    }
    html_escape(w, p, (size_t)(e - p));
    html_str(w, "\">");
    html_escape(w, p, (size_t)(e - p));
    html_str(w, "</a>");
    return e;
}

/** Emit [p, end) as inline HTML. Recurses for nested emphasis and link text. */
static void html_inline(html_writer_t *w, const char *p, const char *end,
                        int depth)
{
    const char *base = p;

    while (p < end) {
        /* Backslash escape: literal next ASCII punctuation. */
        if (*p == '\\' && p + 1 < end && html_is_punct_cp((unsigned char)p[1])) {
            html_escape(w, p + 1, 1);
            p += 2;
            continue;
        }
        /* Code span (multi-backtick, opaque). */
        if (*p == '`') {
            const char *after = html_skip_code_span(p, end);
            if (after != NULL) {
                size_t open_len = 0;
                const char *q = p;
                const char *inner_end;
                while (q < end && *q == '`') {
                    open_len++;
                    q++;
                }
                inner_end = after - open_len;
                /* Strip one surrounding space pair per CommonMark. */
                if (inner_end - q >= 2 && *q == ' ' && inner_end[-1] == ' ') {
                    q++;
                    inner_end--;
                }
                html_str(w, "<code>");
                html_escape(w, q, (size_t)(inner_end - q));
                html_str(w, "</code>");
                p = after;
                continue;
            }
        }
        /* Image. */
        if (*p == '!' && p + 1 < end && p[1] == '[') {
            const char *after = html_parse_image(w, p, end);
            if (after != NULL) {
                p = after;
                continue;
            }
        }
        /* Link. */
        if (*p == '[') {
            const char *after = html_parse_link(w, p, end, depth);
            if (after != NULL) {
                p = after;
                continue;
            }
        }
        /* Angle-bracket autolink. */
        if (*p == '<') {
            const char *gt = memchr(p + 1, '>', (size_t)(end - (p + 1)));
            if (gt != NULL &&
                html_autolink(w, p + 1, (size_t)(gt - (p + 1)))) {
                p = gt + 1;
                continue;
            }
        }
        /* Bare URL. */
        if (*p == 'h' || *p == 'H' || *p == 'w' || *p == 'W') {
            const char *after = html_bare_url(w, p, end);
            if (after != NULL) {
                p = after;
                continue;
            }
        }
        /* Emphasis delimiters (longest first via table order). */
        if (depth < HTML_NEST_MAX) {
            bool matched = false;
            size_t i;
            for (i = 0; i < sizeof(HTML_DELIMS) / sizeof(HTML_DELIMS[0]); i++) {
                const html_delim_t *d = &HTML_DELIMS[i];
                if ((size_t)(end - p) >= d->len && memcmp(p, d->text, d->len) == 0) {
                    bool can_open = false;
                    bool can_close = false;
                    html_flank(base, p, end, d->len, d->underscore,
                               &can_open, &can_close);
                    (void)can_close;
                    if (can_open) {
                        const char *closer = html_find_closer(base, p + d->len,
                                                              end, d);
                        if (closer != NULL) {
                            html_str(w, d->open);
                            html_inline(w, p + d->len, closer, depth + 1);
                            html_str(w, d->close);
                            p = closer + d->len;
                            matched = true;
                            break;
                        }
                    }
                }
            }
            if (matched) {
                continue;
            }
        }
        /* Plain codepoint. */
        {
            size_t len = html_cp_len(p, end);
            html_escape(w, p, len);
            p += len;
        }
    }
}

/* ========================================================================
 * BLOCK LAYER
 * ======================================================================== */

#define HTML_TABLE_ROWS_MAX 64
#define HTML_TABLE_COLS_MAX 16
#define HTML_LIST_MAX 8

/* Table scratch is heap-allocated on first use (PSRAM-first) and reused for
 * the life of the firmware. A static array here would cost ~8 KB of internal
 * DIRAM, which starved the command-worker task creation at boot (the worker
 * stack comes from the same bank). The renderer runs on the single command
 * worker, so the buffer is not shared; see bugs.md C4 for the reentrancy
 * note. Freed only when allocation fails (the caller falls back to
 * paragraphs). */
typedef struct {
    const char *cells[HTML_TABLE_ROWS_MAX][HTML_TABLE_COLS_MAX][2];
    const char *starts[HTML_TABLE_ROWS_MAX];
    const char *ends[HTML_TABLE_ROWS_MAX];
} html_table_scratch_t;

static html_table_scratch_t *s_html_table_scratch;

/** Lazily obtain the table scratch (PSRAM first, internal fallback). */
static html_table_scratch_t *html_table_scratch(void)
{
    if (s_html_table_scratch == NULL) {
        s_html_table_scratch =
            (html_table_scratch_t *)heap_caps_malloc(sizeof(*s_html_table_scratch),
                                                     MALLOC_CAP_SPIRAM);
        if (s_html_table_scratch == NULL) {
            s_html_table_scratch =
                (html_table_scratch_t *)heap_caps_malloc(sizeof(*s_html_table_scratch),
                                                         MALLOC_CAP_DEFAULT);
        }
    }
    return s_html_table_scratch;
}

typedef struct {
    size_t indent;
    bool ordered;
    bool li_open;
} html_list_lvl_t;

/** Reader CSS for the standalone page: narrow measure, serif body, and
 * readable code/table/pre list/task styling with a dark-mode companion.
 * Embedded so the exported file shares over `httpd` with no extra assets. */
static const char *const HTML_READER_CSS =
    "body{margin:0;background:#f6f3ec;color:#1e1e1e;"
    "font:18px/1.65 Georgia,'Times New Roman',serif}"
    "main{max-width:42em;margin:2em auto;padding:0 1.2em}"
    "h1,h2,h3,h4,h5,h6{line-height:1.25;margin:1.2em 0 .5em}"
    "p,ul,ol,blockquote,pre,table{margin:.8em 0}"
    "ul,ol{padding-left:1.6em}"
    "li.task{list-style:none}"
    "li.task input{margin:0 .4em 0 -1.4em}"
    "a{color:#0b5fa5}"
    "img{max-width:100%;height:auto}"
    "blockquote{border-left:3px solid #c9c2b4;margin-left:0;padding-left:1em;color:#555}"
    "code{font-family:ui-monospace,Consolas,monospace;font-size:.9em;"
    "background:#ece7da;padding:.1em .3em;border-radius:4px}"
    "pre{background:#ece7da;padding:1em;overflow-x:auto;border-radius:6px}"
    "pre code{background:none;padding:0}"
    "table{border-collapse:collapse;width:100%}"
    "th,td{border:1px solid #c9c2b4;padding:.35em .6em;text-align:left}"
    "th{background:#ece7da}"
    "hr{border:none;border-top:1px solid #c9c2b4;margin:1.6em 0}"
    "@media (prefers-color-scheme:dark){"
    "body{background:#1a1a1a;color:#e8e6e3}"
    "a{color:#7db8ff}"
    "blockquote{border-color:#555;color:#bbb}"
    "th,td{border-color:#444}"
    "th{background:#2a2a2a}"
    "code,pre{background:#2a2a2a}}";

/** True when [p,end) opens a fence; sets the marker char and info string. */
static bool html_match_fence(const char *p, const char *end, char *mark_out,
                             const char **info_out, size_t *info_len)
{
    const char *q = p;
    char mark;
    size_t n = 0;

    while (q < end && (*q == ' ' || *q == '\t')) {
        q++;
    }
    if (q >= end || (*q != '`' && *q != '~')) {
        return false;
    }
    mark = *q;
    while (q < end && *q == mark) {
        n++;
        q++;
    }
    if (n < 3) {
        return false;
    }
    if (mark_out != NULL) {
        *mark_out = mark;
    }
    if (info_out != NULL) {
        *info_out = q;
    }
    if (info_len != NULL) {
        *info_len = (size_t)(end - q);
    }
    return true;
}

/** Emit `<pre><code class="language-x">` using the first info-string token. */
static void html_emit_fence_open(html_writer_t *w, const char *info, size_t n)
{
    char lang[32];
    size_t li = 0;
    size_t i = 0;

    while (i < n && (info[i] == ' ' || info[i] == '\t')) {
        i++;
    }
    while (i < n && li < sizeof(lang) - 1) {
        char c = info[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            lang[li++] = c;
            i++;
        } else {
            break;
        }
    }
    lang[li] = '\0';
    if (li > 0) {
        html_str(w, "<pre><code class=\"language-");
        html_str(w, lang);
        html_str(w, "\">");
    } else {
        html_str(w, "<pre><code>");
    }
}

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

/** ATX heading: up to 3 leading spaces, 1-6 `#`, then space/EOL. 0 = no. */
static int html_match_heading(const char *p, const char *end,
                              const char **content)
{
    int indent = 0;
    int level = 0;
    const char *q = p;

    while (q < end && *q == ' ' && indent < 4) {
        indent++;
        q++;
    }
    if (indent >= 4) {
        return 0;
    }
    while (q < end && *q == '#') {
        level++;
        q++;
    }
    if (level < 1 || level > 6) {
        return 0;
    }
    if (q < end && *q != ' ' && *q != '\t') {
        return 0;
    }
    while (q < end && (*q == ' ' || *q == '\t')) {
        q++;
    }
    *content = q;
    return level;
}

/** Trim trailing spaces/tabs plus a closing `#` run preceded by whitespace. */
static const char *html_block_end_trim(const char *q, const char *end)
{
    const char *r = end;

    while (r > q && (r[-1] == ' ' || r[-1] == '\t')) {
        r--;
    }
    {
        const char *h = r;
        while (h > q && h[-1] == '#') {
            h--;
        }
        if (h != r) {
            const char *s = h;
            while (s > q && (s[-1] == ' ' || s[-1] == '\t')) {
                s--;
            }
            if (s > q) {
                r = s;
            }
        }
    }
    return r;
}

/** Nested blockquote depth: count leading `>` markers (<=3 spaces each). */
static size_t html_quote_depth(const char *p, const char *end,
                               const char **content)
{
    size_t depth = 0;
    const char *q = p;

    while (q < end) {
        int sp = 0;
        const char *s = q;
        while (s < end && *s == ' ' && sp < 3) {
            s++;
            sp++;
        }
        if (s < end && *s == '>') {
            depth++;
            q = s + 1;
            if (q < end && (*q == ' ' || *q == '\t')) {
                q++;
            }
            continue;
        }
        break;
    }
    *content = q;
    return depth;
}

typedef struct {
    const char *content;
    size_t indent;
    bool ordered;
} html_list_item_t;

/** List item: indent + (-|+|*|digits[.)]) + space. Returns false when no. */
static bool html_match_list(const char *p, const char *end,
                            html_list_item_t *out)
{
    const char *q = p;
    size_t indent = 0;

    while (q < end && (*q == ' ' || *q == '\t')) {
        indent += (*q == '\t') ? 4 : 1;
        q++;
    }
    if (q < end && (*q == '-' || *q == '+' || *q == '*')) {
        if (q + 1 >= end || (q[1] != ' ' && q[1] != '\t')) {
            return false;
        }
        out->ordered = false;
        out->indent = indent;
        out->content = q + 2;
        while (out->content < end && (*out->content == ' ' || *out->content == '\t')) {
            out->content++;
        }
        return true;
    }
    if (q < end && *q >= '0' && *q <= '9') {
        const char *d = q;
        while (d < end && *d >= '0' && *d <= '9') {
            d++;
        }
        if (d >= end || (*d != '.' && *d != ')')) {
            return false;
        }
        d++;
        if (d >= end || (*d != ' ' && *d != '\t')) {
            return false;
        }
        out->ordered = true;
        out->indent = indent;
        out->content = d;
        while (out->content < end && (*out->content == ' ' || *out->content == '\t')) {
            out->content++;
        }
        return true;
    }
    return false;
}

/** Split a table row on unescaped `|` (honoring code spans) into cells. */
static int html_split_row(const char *p, const char *end,
                          const char *cells[][2], int cap)
{
    int n = 0;
    const char *q = p;
    const char *cell_start;

    {
        const char *t = q;
        while (t < end && (*t == ' ' || *t == '\t')) {
            t++;
        }
        if (t < end && *t == '|') {
            q = t + 1;
        }
    }
    cell_start = q;
    while (q < end && n < cap) {
        if (*q == '`') {
            const char *after = html_skip_code_span(q, end);
            q = (after != NULL) ? after : q + 1;
            continue;
        }
        if (*q == '|') {
            cells[n][0] = cell_start;
            cells[n][1] = q;
            n++;
            cell_start = q + 1;
        }
        q += html_cp_len(q, end);
    }
    {
        const char *t = cell_start;
        const char *te = q;
        while (t < te && (*t == ' ' || *t == '\t')) {
            t++;
        }
        while (te > t && (te[-1] == ' ' || te[-1] == '\t')) {
            te--;
        }
        if (t < te || n == 0) {
            if (n < cap) {
                cells[n][0] = cell_start;
                cells[n][1] = q;
                n++;
            }
        }
    }
    return n;
}

/** Separator row: fills align[] (0 left, 1 center, 2 right). 0 = invalid. */
static int html_parse_table_sep(const char *p, const char *end, int align[],
                                int cap)
{
    const char *cells[HTML_TABLE_COLS_MAX][2];
    int n = html_split_row(p, end, cells, HTML_TABLE_COLS_MAX);
    int i;

    if (n == 0 || n > cap) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        const char *a = cells[i][0];
        const char *b = cells[i][1];
        bool left = false;
        bool right = false;
        while (a < b && (*a == ' ' || *a == '\t')) {
            a++;
        }
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) {
            b--;
        }
        if (a < b && *a == ':') {
            left = true;
            a++;
        }
        if (b > a && b[-1] == ':') {
            right = true;
            b--;
        }
        if (b <= a) {
            return 0;
        }
        while (a < b) {
            if (*a != '-') {
                return 0;
            }
            a++;
        }
        align[i] = left ? (right ? 1 : 0) : (right ? 2 : 0);
    }
    return n;
}

static const char *html_align_name(int a)
{
    return a == 1 ? "center" : (a == 2 ? "right" : "left");
}

/** Emit one `<th>`/`<td>` with its inline content. */
static void html_emit_cell(html_writer_t *w, const char *a, const char *b,
                           const char *tag, int align)
{
    while (a < b && (*a == ' ' || *a == '\t')) {
        a++;
    }
    while (b > a && (b[-1] == ' ' || b[-1] == '\t')) {
        b--;
    }
    html_str(w, "<");
    html_str(w, tag);
    html_str(w, " align=\"");
    html_str(w, html_align_name(align));
    html_str(w, "\">");
    html_inline(w, a, b, 0);
    html_str(w, "</");
    html_str(w, tag);
    html_str(w, ">");
}

/** Render buffered table rows; falls back to paragraphs when malformed. */
static void html_flush_table(html_writer_t *w, int n)
{
    html_table_scratch_t *s = html_table_scratch();
    int ncols;
    int align[HTML_TABLE_COLS_MAX];
    int r;
    int c;

    if (n > HTML_TABLE_ROWS_MAX) {
        n = HTML_TABLE_ROWS_MAX;
    }
    if (s != NULL && n >= 2) {
        ncols = html_split_row(s->starts[0], s->ends[0],
                               s->cells[0], HTML_TABLE_COLS_MAX);
        if (ncols > 0 && ncols <= HTML_TABLE_COLS_MAX &&
            html_parse_table_sep(s->starts[1], s->ends[1],
                                 align, ncols) == ncols) {
            bool ok = true;
            for (r = 2; r < n; r++) {
                if (html_split_row(s->starts[r], s->ends[r],
                                   s->cells[r], HTML_TABLE_COLS_MAX) != ncols) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                html_str(w, "<table>\n<thead>\n<tr>");
                for (c = 0; c < ncols; c++) {
                    html_emit_cell(w, s->cells[0][c][0],
                                   s->cells[0][c][1], "th", align[c]);
                }
                html_str(w, "</tr>\n</thead>\n<tbody>\n");
                for (r = 2; r < n; r++) {
                    html_str(w, "<tr>");
                    for (c = 0; c < ncols; c++) {
                        html_emit_cell(w, s->cells[r][c][0],
                                       s->cells[r][c][1], "td", align[c]);
                    }
                    html_str(w, "</tr>\n");
                }
                html_str(w, "</tbody>\n</table>\n");
                return;
            }
        }
    }
    /* Malformed/short (or no scratch memory): emit each buffered line as a
     * paragraph. Without scratch, re-derive the line bounds from the source
     * via the raw line pointers cached below. */
    if (s == NULL) {
        return;
    }
    for (r = 0; r < n; r++) {
        const char *a = s->starts[r];
        const char *b = s->ends[r];
        while (a < b && (*a == ' ' || *a == '\t')) {
            a++;
        }
        html_str(w, "<p>");
        html_inline(w, a, b, 0);
        html_str(w, "</p>\n");
    }
}

static bool html_line_has_pipe(const char *p, const char *end)
{
    while (p < end) {
        if (*p == '`') {
            const char *after = html_skip_code_span(p, end);
            p = (after != NULL) ? after : p + 1;
            continue;
        }
        if (*p == '|') {
            return true;
        }
        p += html_cp_len(p, end);
    }
    return false;
}

static void html_close_li(html_writer_t *w, html_list_lvl_t *lvl)
{
    if (lvl->li_open) {
        lvl->li_open = false;
        html_str(w, "</li>\n");
    }
}

static void html_close_lists(html_writer_t *w, html_list_lvl_t *list, int *depth)
{
    while (*depth > 0) {
        (*depth)--;
        html_close_li(w, &list[*depth]);
        html_str(w, list[*depth].ordered ? "</ol>\n" : "</ul>\n");
    }
}

static void html_close_quotes(html_writer_t *w, int *depth)
{
    while (*depth > 0) {
        (*depth)--;
        html_str(w, "</blockquote>\n");
    }
}

/** Emit a list item, opening/closing nested levels via an indent stack. */
static void html_emit_list_item(html_writer_t *w, html_list_lvl_t *list,
                                int *depth, const html_list_item_t *item,
                                const char *line_end)
{
    const char *content = item->content;

    /* Close deeper levels, then a level whose kind changed at this indent. */
    while (*depth > 0 && item->indent < list[*depth - 1].indent) {
        (*depth)--;
        html_close_li(w, &list[*depth]);
        html_str(w, list[*depth].ordered ? "</ol>\n" : "</ul>\n");
    }
    if (*depth > 0 && item->indent == list[*depth - 1].indent &&
        list[*depth - 1].ordered != item->ordered) {
        (*depth)--;
        html_close_li(w, &list[*depth]);
        html_str(w, list[*depth].ordered ? "</ol>\n" : "</ul>\n");
    }
    if (*depth == 0 || item->indent > list[*depth - 1].indent) {
        if (*depth < HTML_LIST_MAX) {
            /* Parent <li> stays open so the nested list sits inside it. */
            list[*depth].indent = item->indent;
            list[*depth].ordered = item->ordered;
            list[*depth].li_open = false;
            (*depth)++;
            html_str(w, item->ordered ? "<ol>\n" : "<ul>\n");
        } else {
            /* Too deep to nest: render the text inline in the parent item. */
            html_inline(w, content, line_end, 0);
            html_str(w, "\n");
            return;
        }
    } else {
        /* Same level: close the previous sibling before opening this one. */
        html_close_li(w, &list[*depth - 1]);
    }
    /* Detect a task-list checkbox before choosing the opening tag. */
    bool is_task = false;
    bool checked = false;
    if (content + 3 <= line_end && content[0] == '[' && content[2] == ']' &&
        (content[1] == ' ' || content[1] == 'x' || content[1] == 'X') &&
        (content + 3 == line_end || content[3] == ' ' || content[3] == '\t')) {
        is_task = true;
        checked = (content[1] != ' ');
        content += 3;
        while (content < line_end && (*content == ' ' || *content == '\t')) {
            content++;
        }
    }
    html_str(w, is_task ? "<li class=\"task\">" : "<li>");
    if (is_task) {
        html_str(w, checked ? "<input type=\"checkbox\" disabled checked> "
                            : "<input type=\"checkbox\" disabled> ");
    }
    html_inline(w, content, line_end, 0);
    list[*depth - 1].li_open = true;
}

/** Render the document body (fragment). Shared by both entry points. */
static void html_render_body(html_writer_t *w, const char *md)
{
    const char *p;
    const char *end;
    char fence = 0;
    bool in_code = false;
    int quote_depth = 0;
    html_list_lvl_t list[HTML_LIST_MAX];
    int list_depth = 0;
    int tbl_n = 0;

    end = md + strlen(md);

    /* Ensure the (heap/PSRAM) table scratch exists before buffering rows;
     * if allocation ever fails, tables degrade to paragraphs. */
    (void)html_table_scratch();

    for (p = md; p < end; ) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl != NULL ? nl : end;
        const char *line = p;
        const char *trim = line;
        size_t tlen;

        p = nl != NULL ? nl + 1 : end;

        /* Fenced code: copy verbatim (escaped) until the closing fence. */
        if (in_code) {
            char mark;
            if (html_match_fence(trim, line_end, &mark, NULL, NULL) &&
                mark == fence) {
                html_str(w, "</code></pre>\n");
                in_code = false;
            } else {
                html_escape(w, line, (size_t)(line_end - line));
                html_str(w, "\n");
            }
            continue;
        }

        while (trim < line_end && (*trim == ' ' || *trim == '\t')) {
            trim++;
        }
        tlen = (size_t)(line_end - trim);

        /* Buffer table rows; flush before the first non-pipe line. */
        if (tbl_n > 0 && !html_line_has_pipe(trim, line_end)) {
            html_flush_table(w, tbl_n);
            tbl_n = 0;
        }
        if (tlen > 0 && html_line_has_pipe(trim, line_end) &&
            s_html_table_scratch != NULL) {
            if (tbl_n == HTML_TABLE_ROWS_MAX) {
                html_flush_table(w, tbl_n);
                tbl_n = 0;
            }
            s_html_table_scratch->starts[tbl_n] = line;
            s_html_table_scratch->ends[tbl_n] = line_end;
            tbl_n++;
            continue;
        }

        if (tlen == 0) {
            html_close_lists(w, list, &list_depth);
            html_close_quotes(w, &quote_depth);
            continue;
        }

        /* Opening fence. */
        {
            char mark;
            const char *info = NULL;
            size_t info_len = 0;
            if (html_match_fence(trim, line_end, &mark, &info, &info_len)) {
                html_close_lists(w, list, &list_depth);
                html_close_quotes(w, &quote_depth);
                html_emit_fence_open(w, info, info_len);
                in_code = true;
                fence = mark;
                continue;
            }
        }

        /* Heading. */
        {
            const char *content = NULL;
            int level = html_match_heading(trim, line_end, &content);
            if (level > 0) {
                const char *ce = html_block_end_trim(content, line_end);
                html_close_lists(w, list, &list_depth);
                html_close_quotes(w, &quote_depth);
                html_fmt(w, "<h%d>", level);
                html_inline(w, content, ce, 0);
                html_fmt(w, "</h%d>\n", level);
                continue;
            }
        }

        /* Horizontal rule. */
        if (html_is_hr(trim, line_end)) {
            html_close_lists(w, list, &list_depth);
            html_close_quotes(w, &quote_depth);
            html_str(w, "<hr>\n");
            continue;
        }

        /* Blockquote (nested by depth). */
        {
            const char *qc = NULL;
            size_t qd = html_quote_depth(line, line_end, &qc);
            if (qd > 0) {
                html_close_lists(w, list, &list_depth);
                while ((size_t)quote_depth < qd) {
                    html_str(w, "<blockquote>\n");
                    quote_depth++;
                }
                while ((size_t)quote_depth > qd) {
                    quote_depth--;
                    html_str(w, "</blockquote>\n");
                }
                html_str(w, "<p>");
                html_inline(w, qc, line_end, 0);
                html_str(w, "</p>\n");
                continue;
            }
        }

        /* List item (ordered/unordered, nested). */
        {
            html_list_item_t item;
            if (html_match_list(line, line_end, &item)) {
                html_close_quotes(w, &quote_depth);
                html_emit_list_item(w, list, &list_depth, &item, line_end);
                continue;
            }
        }

        /* Paragraph. */
        html_close_lists(w, list, &list_depth);
        html_close_quotes(w, &quote_depth);
        html_str(w, "<p>");
        html_inline(w, trim, line_end, 0);
        html_str(w, "</p>\n");
    }

    if (tbl_n > 0) {
        html_flush_table(w, tbl_n);
    }
    if (in_code) {
        html_str(w, "</code></pre>\n");
    }
    html_close_lists(w, list, &list_depth);
    html_close_quotes(w, &quote_depth);
}

size_t markdown_render_html(const char *md, char *out, size_t out_size)
{
    html_writer_t w;

    if (md == NULL) {
        return 0;
    }
    w.out = (out != NULL && out_size > 0) ? out : NULL;
    w.cap = (out != NULL && out_size > 0) ? out_size : 0;
    w.len = 0;
    w.overflow = false;
    if (w.out != NULL) {
        w.out[0] = '\0';
    }
    html_render_body(&w, md);
    return html_finish(&w);
}

size_t markdown_render_html_page(const char *md, const char *title,
                                 char *out, size_t out_size)
{
    html_writer_t w;
    const char *name;

    if (md == NULL) {
        return 0;
    }
    w.out = (out != NULL && out_size > 0) ? out : NULL;
    w.cap = (out != NULL && out_size > 0) ? out_size : 0;
    w.len = 0;
    w.overflow = false;
    if (w.out != NULL) {
        w.out[0] = '\0';
    }
    name = (title != NULL && title[0] != '\0') ? title : "P4MiniShell";
    html_str(&w, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
                 "<meta charset=\"utf-8\">\n"
                 "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                 "<title>");
    html_escape(&w, name, strlen(name));
    html_str(&w, "</title>\n<style>\n");
    html_str(&w, HTML_READER_CSS);
    html_str(&w, "\n</style>\n</head>\n<body>\n<main>\n");
    html_render_body(&w, md);
    html_str(&w, "</main>\n</body>\n</html>\n");
    return html_finish(&w);
}

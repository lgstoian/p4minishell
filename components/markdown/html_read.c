/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file html_read.c
 * @brief HTML reading: render an HTML document to ANSI SGR (html.h).
 *
 * Single-pass, bounded subset reader (no allocation). It is the inverse of
 * markdown_html.c and is intentionally the ONLY HTML reader in the tree: the
 * viewer, `type`, and the editor preview all call html_render_ansi() and then
 * reuse markdown_strip_ansi() for plain surfaces. Own bounded writer and
 * UTF-8 encoder, matching the other document renderers; no shell/batch deps.
 */

#include "html.h"

#include <stdbool.h>
#include <string.h>

/* ========================================================================
 * BOUNDED WRITER (measure-only when out == NULL; NUL-terminates)
 * ======================================================================== */

typedef struct {
    char *out;      /**< destination, or NULL for measure-only. */
    size_t cap;     /**< destination size including NUL. */
    size_t len;     /**< bytes that should be written (excl. NUL). */
    bool overflow;  /**< destination was too small. */
} hw_t;

static void hw_raw(hw_t *w, const char *s, size_t n)
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

static void hw_str(hw_t *w, const char *s)
{
    if (s != NULL) {
        hw_raw(w, s, strlen(s));
    }
}

static void hw_ch(hw_t *w, char c)
{
    hw_raw(w, &c, 1);
}

/** NUL-terminate and return the required byte count (excl. NUL). A return
 *  >= out_size means truncation. */
static size_t hw_finish(hw_t *w)
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
 * UTF-8 ENCODE (for decoded numeric entities; no decoder needed here)
 * ======================================================================== */

size_t html_utf8_encode(unsigned long cp, char *buf)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        buf[0] = '?';
        return 1;
    }
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ========================================================================
 * ENTITIES
 *
 * One flash-resident table, one decoder. The table is `static const`, so on
 * the device it lives in flash (not in the scarce internal-DRAM budget).
 * Matching is longest-first across the whole table, and the handful of legacy
 * entities that HTML allows without a trailing ';' are flagged individually
 * instead of getting a second decoder. Numeric references are clamped to
 * Unicode and remapped through the HTML5 C1/Windows-1252 table below. The DOM
 * parser and the ANSI reader both call html_entity_decode(), so there is one
 * entity table in the tree.
 * ======================================================================== */

typedef struct {
    const char *name;
    unsigned long cp;
    bool legacy_nosemi;     /**< HTML legacy set: '&amp' may omit the ';' */
} html_entity_t;

static const html_entity_t HTML_ENTITIES[] = {
    { "amp", '&', true },       { "lt", '<', true },        { "gt", '>', true },
    { "quot", '"', true },      { "apos", '\'', true },     { "nbsp", ' ', true },
    { "copy", 0xA9, true },     { "reg", 0xAE, true },
    { "trade", 0x2122, false }, { "hellip", 0x2026, false },
    { "mdash", 0x2014, false }, { "ndash", 0x2013, false },
    { "lsquo", 0x2018, false }, { "rsquo", 0x2019, false },
    { "ldquo", 0x201C, false }, { "rdquo", 0x201D, false },
    { "laquo", 0xAB, false },   { "raquo", 0xBB, false },
    { "bull", 0x2022, false },  { "middot", 0xB7, false },
    { "deg", 0xB0, false },     { "plusmn", 0xB1, false },
    { "times", 0xD7, false },   { "divide", 0xF7, false },
    { "frac12", 0xBD, false },  { "frac14", 0xBC, false },
    { "frac34", 0xBE, false },  { "micro", 0xB5, false },
    { "sect", 0xA7, false },    { "para", 0xB6, false },
    { "euro", 0x20AC, false },  { "pound", 0xA3, false },
    { "yen", 0xA5, false },     { "cent", 0xA2, false },
    { "not", 0xAC, false },     { "shy", 0xAD, false },
};

/** HTML5 numeric-reference overrides for the C1 range 0x80..0x9F. A missing
 *  entry is an invalid reference and becomes U+FFFD rather than a control
 *  glyph the 12x20 cell font cannot draw. */
static const unsigned long HTML_C1_REMAP[0x20] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

/** True for a codepoint the renderer treats as collapsible whitespace. */
static bool html_cp_is_space(unsigned long cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == 0xA0;
}

/** Clamp/remap a decoded numeric codepoint to something drawable. */
static unsigned long html_numeric_cp(unsigned long cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0xFFFD;
    }
    if (cp >= 0x80 && cp <= 0x9F) {
        return HTML_C1_REMAP[cp - 0x80];
    }
    if (cp == 0xA0) {
        return ' ';             /* one collapsible cell, like the named form */
    }
    if (cp < 0x20 || cp == 0x7F) {
        return 0xFFFD;
    }
    return cp;
}

/**
 * Match an entity at @p p (which points at '&'). On success writes the
 * decoded codepoint to @p cp_out and the bytes consumed (including '&' and an
 * optional ';') to @p consumed_out. Named matching is longest-first; legacy
 * entities without a semicolon are accepted when the next byte cannot extend
 * the name. Returns false when there is no valid entity here.
 */
bool html_entity_decode(const char *p, const char *end, unsigned long *cp_out,
                        size_t *consumed_out)
{
    const char *q = p + 1;
    size_t avail;

    if (p == NULL || cp_out == NULL || consumed_out == NULL || q >= end) {
        return false;
    }
    avail = (size_t)(end - q);
    if (avail > 32) {
        avail = 32;             /* one bounded span, never scan the whole page */
    }

    /* Numeric reference: &#123; or &#x7B;. The semicolon is optional at the
     * end of a text run, matching what browsers do for truncated markup. */
    if (*q == '#') {
        unsigned long cp = 0;
        size_t i = 1;
        bool hex = false;
        bool any = false;
        bool overflow = false;

        if (i < avail && (q[i] == 'x' || q[i] == 'X')) {
            hex = true;
            i++;
        }
        for (; i < avail; i++) {
            char c = q[i];
            int d;

            if (hex) {
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
            } else {
                if (c < '0' || c > '9') break;
                d = c - '0';
            }
            any = true;
            if (cp > 0x10FFFFul / (unsigned long)(hex ? 16 : 10)) {
                overflow = true;
            } else {
                cp = cp * (unsigned long)(hex ? 16 : 10) + (unsigned long)d;
            }
        }
        if (!any) {
            return false;
        }
        {
            size_t used = i;
            bool semi = (i < avail && q[i] == ';');

            if (semi) used++;
            *cp_out = overflow ? 0xFFFD : html_numeric_cp(cp);
            *consumed_out = 1u + used;   /* '&' plus the q bytes we took */
            return true;
        }
    }

    /* Named: scan the whole table and keep the longest full match. */
    {
        const html_entity_t *best = NULL;
        size_t best_len = 0;
        size_t k;

        for (k = 0; k < sizeof(HTML_ENTITIES) / sizeof(HTML_ENTITIES[0]); k++) {
            const char *name = HTML_ENTITIES[k].name;
            size_t len = strlen(name);

            if (len > avail || len <= best_len || strncmp(q, name, len) != 0) {
                continue;
            }
            {
                const char *after = q + len;
                bool semi = (after < q + avail && *after == ';');

                if (!semi && !HTML_ENTITIES[k].legacy_nosemi) {
                    continue;       /* a non-legacy entity needs its ';' */
                }
                if (!semi && after < end &&
                    ((*after >= 'a' && *after <= 'z') ||
                     (*after >= 'A' && *after <= 'Z') ||
                     (*after >= '0' && *after <= '9') || *after == '=')) {
                    /* '&copycat' is not '&copy' + 'cat': leave it alone. */
                    continue;
                }
                best = &HTML_ENTITIES[k];
                best_len = len;
            }
        }
        if (best != NULL) {
            size_t used = best_len;

            if (q + used < end && q[used] == ';') used++;
            *cp_out = best->cp;
            *consumed_out = 1u + used;
            return true;
        }
    }
    return false;
}

/* ========================================================================
 * TAG SCANNING
 * ======================================================================== */

typedef struct {
    char name[24];      /**< lowercased element name ("" for comments/decls) */
    bool closing;       /**< starts with '</' */
    bool comment;       /**< <!-- ... --> */
    bool decl;          /**< <!doctype ...> or <?...> */
    char href[512];     /**< href attribute (a), "" when absent */
    char alt[128];      /**< alt attribute (img), "" when absent */
    char input_type[16];/**< type attribute (input/button) */
    char value[128];    /**< value attribute (form controls) */
    bool checked;       /**< checked/selected attribute */
} html_tag_t;

/** ASCII lower for tag names. */
static char html_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/** Copy an attribute value (quoted or bare) into @p dst, bounded. */
static void html_attr_value(const char *v, const char *vend, char *dst, size_t dst_size)
{
    size_t n = (size_t)(vend - v);
    size_t i;
    size_t o = 0;

    if (dst_size == 0) {
        return;
    }
    for (i = 0; i < n && o + 1 < dst_size; i++) {
        dst[o++] = v[i];
    }
    dst[o] = '\0';
}

/**
 * Scan one tag at @p p (pointing at '<'). Returns the pointer just past the
 * tag, or @p end when unterminated. Fills @p tag.
 */
static const char *html_scan_tag(const char *p, const char *end, html_tag_t *tag)
{
    const char *q;

    memset(tag, 0, sizeof(*tag));

    if (p + 3 < end && strncmp(p, "<!--", 4) == 0) {
        const char *close = strstr(p + 4, "-->");
        tag->comment = true;
        return (close != NULL) ? close + 3 : end;
    }
    if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
        const char *gt = memchr(p + 1, '>', (size_t)(end - (p + 1)));
        tag->decl = true;
        return (gt != NULL) ? gt + 1 : end;
    }

    q = p + 1;
    if (q < end && *q == '/') {
        tag->closing = true;
        q++;
    }
    /* Name. */
    {
        size_t o = 0;
        while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                           (*q >= '0' && *q <= '9'))) {
            if (o + 1 < sizeof(tag->name)) {
                tag->name[o++] = html_lower(*q);
            }
            q++;
        }
        tag->name[o] = '\0';
    }
    if (tag->name[0] == '\0') {
        /* Not a recognizable tag (e.g. lone '<'); treat as text by returning p+1. */
        return p + 1;
    }
    /* Attributes until '>'. */
    while (q < end && *q != '>') {
        const char *aname = q;
        size_t alen = 0;
        char attr[16];

        while (q < end && *q != '>' && *q != '=' && *q != ' ' && *q != '\t' &&
               *q != '\n' && *q != '\r' && *q != '/') {
            q++;
        }
        alen = (size_t)(q - aname);
        if (alen == 0 || alen >= sizeof(attr)) {
            if (q < end) {
                q++;
            }
            continue;
        }
        {
            size_t i;
            for (i = 0; i < alen; i++) {
                attr[i] = html_lower(aname[i]);
            }
            attr[alen] = '\0';
        }
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) {
            q++;
        }
        if (q < end && *q == '=') {
            q++;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) {
                q++;
            }
            if (q < end && (*q == '"' || *q == '\'')) {
                char quote = *q++;
                const char *v = q;
                while (q < end && *q != quote) {
                    q++;
                }
                if (strcmp(attr, "href") == 0) {
                    html_attr_value(v, q, tag->href, sizeof(tag->href));
                } else if (strcmp(attr, "alt") == 0) {
                    html_attr_value(v, q, tag->alt, sizeof(tag->alt));
                } else if (strcmp(attr, "type") == 0) {
                    html_attr_value(v, q, tag->input_type, sizeof(tag->input_type));
                } else if (strcmp(attr, "value") == 0) {
                    html_attr_value(v, q, tag->value, sizeof(tag->value));
                } else if (strcmp(attr, "checked") == 0 || strcmp(attr, "selected") == 0) {
                    tag->checked = true;
                }
                if (q < end) {
                    q++; /* closing quote */
                }
            } else {
                const char *v = q;
                while (q < end && *q != '>' && *q != ' ' && *q != '\t' &&
                       *q != '\n' && *q != '\r') {
                    q++;
                }
                if (strcmp(attr, "href") == 0) {
                    html_attr_value(v, q, tag->href, sizeof(tag->href));
                } else if (strcmp(attr, "alt") == 0) {
                    html_attr_value(v, q, tag->alt, sizeof(tag->alt));
                } else if (strcmp(attr, "type") == 0) {
                    html_attr_value(v, q, tag->input_type, sizeof(tag->input_type));
                } else if (strcmp(attr, "value") == 0) {
                    html_attr_value(v, q, tag->value, sizeof(tag->value));
                } else if (strcmp(attr, "checked") == 0 || strcmp(attr, "selected") == 0) {
                    tag->checked = true;
                }
            }
        } else if (strcmp(attr, "checked") == 0 || strcmp(attr, "selected") == 0) {
            tag->checked = true;   /* valueless boolean attribute */
        }
    }
    return (q < end && *q == '>') ? q + 1 : end;
}

/** True for elements whose entire content is skipped. */
static bool html_is_skipped(const char *name)
{
    return strcmp(name, "head") == 0 || strcmp(name, "script") == 0 ||
           strcmp(name, "style") == 0 || strcmp(name, "title") == 0;
}

/** Skip to just past the matching `</name>` (case-insensitive), or @p end. */
static const char *html_skip_element(const char *p, const char *end, const char *name)
{
    size_t nlen = strlen(name);
    const char *q = p;

    while (q < end) {
        const char *lt = memchr(q, '<', (size_t)(end - q));
        if (lt == NULL) {
            return end;
        }
        if (lt + 2 + nlen <= end && lt[1] == '/') {
            size_t i;
            bool match = true;
            for (i = 0; i < nlen; i++) {
                if (html_lower(lt[2 + i]) != name[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                const char *gt = memchr(lt + 2 + nlen, '>', (size_t)(end - (lt + 2 + nlen)));
                return (gt != NULL) ? gt + 1 : end;
            }
        }
        q = lt + 1;
    }
    return end;
}

/* ========================================================================
 * RENDER STATE + EMISSION
 * ======================================================================== */

#define HTML_LIST_MAX 8

typedef struct {
    hw_t w;
    bool line_started;
    bool pending_space;
    int quote_depth;
    int heading;                 /* 0 none, 1..6 */
    bool in_pre;
    bool in_code;                /* inline <code> (not in pre) */
    bool in_cell;
    bool in_a;
    bool sgr;                    /* any SGR emitted (close with a reset) */
    char url[512];
    int list_kind[HTML_LIST_MAX];  /* 0 = ul, 1 = ol */
    int list_count[HTML_LIST_MAX];
    int list_depth;
} html_ctx_t;

static void hw_sgr(html_ctx_t *c, const char *seq)
{
    hw_str(&c->w, seq);
    c->sgr = true;
}

/** Start-of-line prefix (blockquote bars). */
static void html_line_start(html_ctx_t *c)
{
    if (!c->line_started) {
        int i;
        for (i = 0; i < c->quote_depth; i++) {
            hw_str(&c->w, "\xE2\x94\x82 "); /* U+2502 vertical bar */
        }
        c->line_started = true;
    }
}

static void html_newline(html_ctx_t *c)
{
    hw_ch(&c->w, '\n');
    c->line_started = false;
    c->pending_space = false;
}

/** Flush a pending inter-word space (never at line start). */
static void html_flush_space(html_ctx_t *c)
{
    if (c->pending_space && c->line_started) {
        hw_ch(&c->w, ' ');
    }
    c->pending_space = false;
}

static void html_emit_cp(html_ctx_t *c, unsigned long cp)
{
    char buf[4];
    size_t n;

    html_line_start(c);
    html_flush_space(c);
    n = html_utf8_encode(cp, buf);
    hw_raw(&c->w, buf, n);
}

static void html_text(html_ctx_t *c, const char *s, size_t n)
{
    size_t i = 0;

    if (c->in_pre) {
        for (i = 0; i < n; i++) {
            char ch = s[i];
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                html_newline(c);
                continue;
            }
            html_line_start(c);
            hw_ch(&c->w, ch);
        }
        return;
    }
    while (i < n) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '&') {
            unsigned long cp = 0;
            size_t consumed = 0;
            if (html_entity_decode(s + i, s + n, &cp, &consumed)) {
                if (html_cp_is_space(cp)) {
                    c->pending_space = true;
                } else {
                    html_emit_cp(c, cp);
                }
                i += consumed;
                continue;
            }
            html_line_start(c);
            html_flush_space(c);
            hw_ch(&c->w, '&');
            i++;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
            if (c->line_started) {
                c->pending_space = true;
            }
            i++;
            continue;
        }
        html_line_start(c);
        html_flush_space(c);
        hw_ch(&c->w, (char)ch);
        i++;
    }
}

/* ========================================================================
 * TAG HANDLERS
 * ======================================================================== */

static bool html_is_block(const char *name)
{
    static const char *BLOCKS[] = {
        "p", "div", "section", "article", "header", "footer", "main",
        "nav", "aside", "figure", "figcaption", "form", "fieldset",
        "details", "summary", "dialog", "caption", "legend",
    };
    size_t i;
    for (i = 0; i < sizeof(BLOCKS) / sizeof(BLOCKS[0]); i++) {
        if (strcmp(name, BLOCKS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void html_open_tag(html_ctx_t *c, const html_tag_t *tag)
{
    const char *name = tag->name;

    if (html_is_block(name)) {
        if (c->line_started) {
            html_newline(c);
        }
        return;
    }
    if (strcmp(name, "br") == 0) {
        html_newline(c);
        return;
    }
    if (strcmp(name, "hr") == 0) {
        int i;
        if (c->line_started) {
            html_newline(c);
        }
        html_line_start(c);
        for (i = 0; i < 40; i++) {
            hw_str(&c->w, "\xE2\x94\x80"); /* U+2500 horizontal rule */
        }
        html_newline(c);
        return;
    }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
        if (c->line_started) {
            html_newline(c);
        }
        html_line_start(c);
        hw_sgr(c, "\x1b[1m");
        if (name[1] == '1' || name[1] == '2') {
            hw_sgr(c, "\x1b[4m");
        }
        c->heading = name[1] - '0';
        return;
    }
    if (strcmp(name, "pre") == 0) {
        if (c->line_started) {
            html_newline(c);
        }
        c->in_pre = true;
        hw_sgr(c, "\x1b[2m");
        return;
    }
    if (strcmp(name, "code") == 0 && !c->in_pre) {
        hw_sgr(c, "\x1b[2m");
        c->in_code = true;
        return;
    }
    if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
        if (c->line_started) {
            html_newline(c);
        }
        if (c->list_depth < HTML_LIST_MAX) {
            c->list_kind[c->list_depth] = (name[0] == 'o') ? 1 : 0;
            c->list_count[c->list_depth] = 1;
            c->list_depth++;
        }
        return;
    }
    if (strcmp(name, "li") == 0) {
        int depth = (c->list_depth > 0) ? c->list_depth - 1 : 0;
        int i;
        if (c->line_started) {
            html_newline(c);
        }
        html_line_start(c);
        for (i = 0; i < depth; i++) {
            hw_str(&c->w, "  ");
        }
        if (depth < HTML_LIST_MAX && c->list_kind[depth] == 1) {
            char num[16];
            int v = c->list_count[depth];
            size_t k = 0;
            char tmp[16];
            int t = 0;
            if (v <= 0) {
                v = 1;
            }
            while (v > 0 && t < (int)sizeof(tmp)) {
                tmp[t++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (t > 0 && k + 2 < sizeof(num)) {
                num[k++] = tmp[--t];
            }
            num[k++] = '.';
            num[k++] = ' ';
            num[k] = '\0';
            hw_str(&c->w, num);
            c->list_count[depth]++;
        } else {
            hw_str(&c->w, "- ");
        }
        return;
    }
    if (strcmp(name, "blockquote") == 0) {
        if (c->line_started) {
            html_newline(c);
        }
        c->quote_depth++;
        return;
    }
    if (strcmp(name, "table") == 0) {
        if (c->line_started) {
            html_newline(c);
        }
        return;
    }
    if (strcmp(name, "tr") == 0) {
        if (c->line_started) {
            html_newline(c);
        }
        return;
    }
    if (strcmp(name, "th") == 0 || strcmp(name, "td") == 0) {
        html_line_start(c);
        hw_str(&c->w, "| ");
        c->in_cell = true;
        return;
    }
    if (strcmp(name, "a") == 0) {
        if (tag->href[0] != '\0') {
            size_t i;
            for (i = 0; i + 1 < sizeof(c->url) && tag->href[i] != '\0'; i++) {
                c->url[i] = tag->href[i];
            }
            c->url[i] = '\0';
            c->in_a = true;
            hw_sgr(c, "\x1b[4m\x1b[36m");
        }
        return;
    }
    if (strcmp(name, "input") == 0) {
        html_line_start(c);
        html_flush_space(c);
        if (strcmp(tag->input_type, "checkbox") == 0 ||
            strcmp(tag->input_type, "radio") == 0) {
            hw_str(&c->w, tag->checked ? "[x]" : "[ ]");
        } else {
            const char *shown = tag->value[0] ? tag->value : tag->alt;

            hw_ch(&c->w, '[');
            hw_str(&c->w, shown[0] ? shown : tag->input_type);
            hw_ch(&c->w, ']');
        }
        return;
    }
    if (strcmp(name, "button") == 0) {
        html_line_start(c);
        hw_str(&c->w, "[ ");
        return;
    }
    if (strcmp(name, "select") == 0) {
        html_line_start(c);
        hw_ch(&c->w, '[');
        return;
    }
    if (strcmp(name, "option") == 0 && tag->checked) {
        hw_sgr(c, "\x1b[1m");   /* the selected option is the bold one */
        return;
    }
    if (strcmp(name, "img") == 0) {
        html_line_start(c);
        html_flush_space(c);
        if (tag->alt[0] != '\0') {
            hw_ch(&c->w, '[');
            hw_str(&c->w, tag->alt);
            hw_ch(&c->w, ']');
        } else {
            hw_str(&c->w, "[image]");
        }
        return;
    }
    if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0) {
        hw_sgr(c, "\x1b[1m");
        return;
    }
    if (strcmp(name, "i") == 0 || strcmp(name, "em") == 0) {
        hw_sgr(c, "\x1b[3m");
        return;
    }
    if (strcmp(name, "u") == 0 || strcmp(name, "ins") == 0) {
        hw_sgr(c, "\x1b[4m");
        return;
    }
    if (strcmp(name, "s") == 0 || strcmp(name, "strike") == 0 ||
        strcmp(name, "del") == 0) {
        hw_sgr(c, "\x1b[9m");
        return;
    }
    /* Transparent inline wrappers (span/font/sub/sup/small/big/mark/...) and
     * unknown elements add no output. */
}

static void html_close_tag(html_ctx_t *c, const html_tag_t *tag)
{
    const char *name = tag->name;

    if (html_is_block(name)) {
        if (c->line_started) {
            html_newline(c);
        }
        return;
    }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
        hw_sgr(c, "\x1b[0m");
        c->heading = 0;
        html_newline(c);
        return;
    }
    if (strcmp(name, "pre") == 0) {
        hw_sgr(c, "\x1b[0m");
        c->in_pre = false;
        html_newline(c);
        return;
    }
    if (strcmp(name, "code") == 0 && c->in_code) {
        hw_sgr(c, "\x1b[22m");
        c->in_code = false;
        return;
    }
    if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
        if (c->list_depth > 0) {
            c->list_depth--;
        }
        if (c->line_started) {
            html_newline(c);
        }
        return;
    }
    if (strcmp(name, "blockquote") == 0) {
        if (c->line_started) {
            html_newline(c);
        }
        if (c->quote_depth > 0) {
            c->quote_depth--;
        }
        return;
    }
    if (strcmp(name, "table") == 0) {
        if (c->line_started) {
            html_newline(c);
        }
        return;
    }
    if (strcmp(name, "tr") == 0) {
        if (c->in_cell) {
            hw_str(&c->w, " |");
            c->in_cell = false;
        }
        html_newline(c);
        return;
    }
    if (strcmp(name, "th") == 0 || strcmp(name, "td") == 0) {
        hw_ch(&c->w, ' ');
        c->in_cell = false;
        return;
    }
    if (strcmp(name, "a") == 0) {
        if (c->in_a) {
            hw_sgr(c, "\x1b[39m\x1b[24m");
            if (c->url[0] != '\0' && c->url[0] != '#') {
                hw_str(&c->w, " (");
                hw_str(&c->w, c->url);
                hw_ch(&c->w, ')');
            }
            c->in_a = false;
            c->url[0] = '\0';
        }
        return;
    }
    if (strcmp(name, "button") == 0) {
        hw_str(&c->w, " ]");
        return;
    }
    if (strcmp(name, "select") == 0) {
        hw_ch(&c->w, ']');
        return;
    }
    if (strcmp(name, "option") == 0) {
        hw_sgr(c, "\x1b[22m");
        return;
    }
    if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0) {
        hw_sgr(c, "\x1b[22m");
        return;
    }
    if (strcmp(name, "i") == 0 || strcmp(name, "em") == 0) {
        hw_sgr(c, "\x1b[23m");
        return;
    }
    if (strcmp(name, "u") == 0 || strcmp(name, "ins") == 0) {
        hw_sgr(c, "\x1b[24m");
        return;
    }
    if (strcmp(name, "s") == 0 || strcmp(name, "strike") == 0 ||
        strcmp(name, "del") == 0) {
        hw_sgr(c, "\x1b[29m");
        return;
    }
}

/* ========================================================================
 * PUBLIC ENTRY
 * ======================================================================== */

size_t html_render_ansi(const char *html, char *out, size_t out_size)
{
    html_ctx_t c;
    const char *p;
    const char *end;

    if (html == NULL) {
        return 0;
    }
    memset(&c, 0, sizeof(c));
    c.w.out = (out != NULL && out_size > 0) ? out : NULL;
    c.w.cap = (out != NULL && out_size > 0) ? out_size : 0;
    c.w.len = 0;
    c.w.overflow = false;
    if (c.w.out != NULL) {
        c.w.out[0] = '\0';
    }

    p = html;
    end = html + strlen(html);
    while (p < end) {
        if (*p == '<') {
            html_tag_t tag;
            const char *after = html_scan_tag(p, end, &tag);

            if (tag.comment || tag.decl) {
                p = after;
                continue;
            }
            if (tag.name[0] == '\0') {
                /* Lone '<': render it literally. */
                html_text(&c, "<", 1);
                p = after;
                continue;
            }
            if (!tag.closing && html_is_skipped(tag.name)) {
                p = html_skip_element(after, end, tag.name);
                continue;
            }
            if (tag.closing) {
                html_close_tag(&c, &tag);
            } else {
                html_open_tag(&c, &tag);
            }
            p = after;
            continue;
        }
        {
            const char *lt = memchr(p, '<', (size_t)(end - p));
            size_t n = (lt != NULL) ? (size_t)(lt - p) : (size_t)(end - p);
            html_text(&c, p, n);
            p = (lt != NULL) ? lt : end;
        }
    }

    /* Close any colour left open (mirrors markdown.c), then NUL-terminate. */
    if (c.sgr) {
        hw_str(&c.w, "\x1b[0m");
    }
    return hw_finish(&c.w);
}

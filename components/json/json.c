/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file json.c
 * @brief Shared JSON reader (see include/json.h).
 *
 * The scanner below is the firmware's single recursive-descent JSON
 * implementation, moved here verbatim from components/command so that
 * command-layer and library consumers share it. Validation and pretty-print
 * are the original code paths and are behaviourally unchanged.
 *
 * Walk mode is additive bookkeeping layered on the same descent: a path
 * buffer and array-index stack are maintained while walking, and each scalar
 * emits an event. With `visit == NULL` that bookkeeping is inert, so the
 * validate/pretty output is byte-identical to before the move.
 */

#include "json.h"

#include <stdio.h>
#include <string.h>

/** Maximum object/array nesting tracked by the path buffer. */
#define JSON_MAX_DEPTH 64

/** Dotted path buffer. Bounds the visitor contract; longer paths truncate. */
#define JSON_PATH_BYTES 512

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    size_t line;
    size_t col;
    char error[128];

    /* ---- walk-mode state (all inert when visit == NULL) ---- */
    json_visit_fn visit;
    void *visit_ctx;
    char path[JSON_PATH_BYTES];  /**< current dotted path */
    int index[JSON_MAX_DEPTH];   /**< per-depth array element counter */
    int depth;
} json_parser_t;

static void json_fail(json_parser_t *p, const char *what)
{
    snprintf(p->error, sizeof(p->error), "%s at line %u col %u",
             what, (unsigned)p->line, (unsigned)p->col);
}

static char json_peek(json_parser_t *p)
{
    if (p->pos >= p->len) {
        return '\0';
    }
    return p->src[p->pos];
}

static void json_advance(json_parser_t *p)
{
    if (p->pos >= p->len) {
        return;
    }
    if (p->src[p->pos] == '\n') {
        p->line++;
        p->col = 1;
    } else {
        p->col++;
    }
    p->pos++;
}

static void json_skip_ws(json_parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            json_advance(p);
        } else {
            break;
        }
    }
}

/* ========================================================================
 * PATH BOOKKEEPING (walk mode only)
 * ======================================================================== */

/** Append ".seg" to the current path, truncating rather than overflowing. */
static void json_path_push(json_parser_t *p, const char *seg, size_t seg_len)
{
    size_t at = strlen(p->path);

    if (seg_len >= JSON_PATH_BYTES - at - 1) {
        seg_len = (JSON_PATH_BYTES - at - 1);
    }
    if (seg_len == 0) {
        return;
    }
    if (at + 1 < JSON_PATH_BYTES) {
        p->path[at++] = '.';
        seg_len = (seg_len > (JSON_PATH_BYTES - at - 1)) ? (JSON_PATH_BYTES - at - 1) : seg_len;
    }
    memcpy(p->path + at, seg, seg_len);
    p->path[at + seg_len] = '\0';
}

/** Append ".seg" where seg is a decimal integer (array index). */
static void json_path_push_index(json_parser_t *p, int value)
{
    char tmp[12];
    int n = snprintf(tmp, sizeof(tmp), "%d", value);

    if (n > 0) {
        json_path_push(p, tmp, (size_t)n);
    }
}

/** Drop the last path segment written by the matching push. */
static void json_path_pop(json_parser_t *p)
{
    char *dot = strrchr(p->path, '.');

    if (dot != NULL) {
        *dot = '\0';
    }
}

/* ========================================================================
 * SCANNER
 * ======================================================================== */

/* Forward declarations (mutual recursion value <-> array/object). */
static bool json_parse_value(json_parser_t *p, char *out, size_t out_size,
                             size_t *out_len, int indent);

static bool json_out_str(json_parser_t *p, char *out, size_t out_size,
                         size_t *out_len, const char *s, size_t n)
{
    /* NULL out = validate/count mode: track length without storing. */
    if (out == NULL) {
        *out_len += n;
        return true;
    }
    if (*out_len + n >= out_size) {
        json_fail(p, "output too large");
        return false;
    }
    memcpy(out + *out_len, s, n);
    *out_len += n;
    return true;
}

static bool json_out_indent(json_parser_t *p, char *out, size_t out_size,
                            size_t *out_len, int indent)
{
    int i;

    for (i = 0; i < indent; i++) {
        if (!json_out_str(p, out, out_size, out_len, "  ", 2)) {
            return false;
        }
    }
    return true;
}

/** Parse "..." with escapes; echoes the raw literal (validates shape).
 *  On return the consumed span is [start, p->pos). */
static bool json_parse_string_span(json_parser_t *p, size_t *start_out,
                                   char *out, size_t out_size, size_t *out_len)
{
    size_t start;

    if (json_peek(p) != '"') {
        json_fail(p, "expected string");
        return false;
    }
    start = p->pos;
    json_advance(p);
    while (p->pos < p->len) {
        char c = json_peek(p);
        if (c == '"') {
            json_advance(p);
            if (start_out != NULL) {
                *start_out = start;
            }
            return json_out_str(p, out, out_size, out_len,
                                p->src + start, p->pos - start);
        }
        if (c == '\\') {
            json_advance(p);
            c = json_peek(p);
            /* \" \\ \/ \b \f \n \r \t \uXXXX */
            if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' ||
                c == 'n' || c == 'r' || c == 't') {
                json_advance(p);
            } else if (c == 'u') {
                int k;
                json_advance(p);
                for (k = 0; k < 4; k++) {
                    c = json_peek(p);
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F'))) {
                        json_fail(p, "bad \\u escape");
                        return false;
                    }
                    json_advance(p);
                }
            } else if (c == '\0') {
                break;
            } else {
                json_fail(p, "bad escape");
                return false;
            }
            continue;
        }
        if (c == '\n' || c == '\0') {
            break;
        }
        if ((unsigned char)c < 0x20) {
            json_fail(p, "unescaped control in string");
            return false;
        }
        json_advance(p);
    }
    json_fail(p, "unterminated string");
    return false;
}

static bool json_parse_string(json_parser_t *p, char *out, size_t out_size,
                              size_t *out_len)
{
    return json_parse_string_span(p, NULL, out, out_size, out_len);
}

static bool json_parse_number(json_parser_t *p, char *out, size_t out_size,
                              size_t *out_len)
{
    size_t start = p->pos;

    if (json_peek(p) == '-') {
        json_advance(p);
    }
    if (json_peek(p) == '0') {
        json_advance(p);
    } else if (json_peek(p) >= '1' && json_peek(p) <= '9') {
        while (json_peek(p) >= '0' && json_peek(p) <= '9') {
            json_advance(p);
        }
    } else {
        json_fail(p, "bad number");
        return false;
    }
    if (json_peek(p) == '.') {
        json_advance(p);
        if (!(json_peek(p) >= '0' && json_peek(p) <= '9')) {
            json_fail(p, "bad fraction");
            return false;
        }
        while (json_peek(p) >= '0' && json_peek(p) <= '9') {
            json_advance(p);
        }
    }
    if (json_peek(p) == 'e' || json_peek(p) == 'E') {
        json_advance(p);
        if (json_peek(p) == '+' || json_peek(p) == '-') {
            json_advance(p);
        }
        if (!(json_peek(p) >= '0' && json_peek(p) <= '9')) {
            json_fail(p, "bad exponent");
            return false;
        }
        while (json_peek(p) >= '0' && json_peek(p) <= '9') {
            json_advance(p);
        }
    }
    return json_out_str(p, out, out_size, out_len,
                        p->src + start, p->pos - start);
}

static bool json_parse_literal(json_parser_t *p, char *out, size_t out_size,
                               size_t *out_len, const char *word)
{
    size_t n = strlen(word);

    if (p->pos + n > p->len || strncmp(p->src + p->pos, word, n) != 0) {
        json_fail(p, "bad literal");
        return false;
    }
    p->pos += n;
    p->col += n;
    return json_out_str(p, out, out_size, out_len, word, n);
}

static bool json_parse_array(json_parser_t *p, char *out, size_t out_size,
                             size_t *out_len, int indent)
{
    bool first = true;
    int elem = 0;

    json_advance(p); /* [ */
    if (indent > JSON_MAX_DEPTH) {
        json_fail(p, "too deeply nested");
        return false;
    }
    json_skip_ws(p);
    if (json_peek(p) == ']') {
        json_advance(p);
        return json_out_str(p, out, out_size, out_len, "[]", 2);
    }
    if (!json_out_str(p, out, out_size, out_len, "[\n", 2)) {
        return false;
    }
    while (true) {
        json_skip_ws(p);
        if (!json_out_indent(p, out, out_size, out_len, indent + 1)) {
            return false;
        }
        /* Walk mode: give this element its own ".N" path segment. */
        if (p->visit != NULL && indent < JSON_MAX_DEPTH) {
            json_path_push_index(p, elem);
        }
        if (!json_parse_value(p, out, out_size, out_len, indent + 1)) {
            return false;
        }
        if (p->visit != NULL && indent < JSON_MAX_DEPTH) {
            json_path_pop(p);
        }
        elem++;
        json_skip_ws(p);
        if (json_peek(p) == ',') {
            json_advance(p);
            if (!json_out_str(p, out, out_size, out_len, ",\n", 2)) {
                return false;
            }
            first = false;
            continue;
        }
        if (json_peek(p) == ']') {
            json_advance(p);
            if (!json_out_str(p, out, out_size, out_len, "\n", 1)) {
                return false;
            }
            if (!json_out_indent(p, out, out_size, out_len, indent)) {
                return false;
            }
            return json_out_str(p, out, out_size, out_len, "]", 1);
        }
        json_fail(p, first ? "expected ]" : "expected , or ]");
        return false;
    }
}

static bool json_parse_object(json_parser_t *p, char *out, size_t out_size,
                              size_t *out_len, int indent)
{
    bool first = true;

    json_advance(p); /* { */
    if (indent > JSON_MAX_DEPTH) {
        json_fail(p, "too deeply nested");
        return false;
    }
    json_skip_ws(p);
    if (json_peek(p) == '}') {
        json_advance(p);
        return json_out_str(p, out, out_size, out_len, "{}", 2);
    }
    if (!json_out_str(p, out, out_size, out_len, "{\n", 2)) {
        return false;
    }
    while (true) {
        size_t kstart = 0;

        json_skip_ws(p);
        if (json_peek(p) != '"') {
            json_fail(p, "expected string key");
            return false;
        }
        if (!json_out_indent(p, out, out_size, out_len, indent + 1)) {
            return false;
        }
        /* Walk mode: capture the key text (without quotes) as a path segment.
         * Reuses the same string scanner; no second key reader exists. */
        if (p->visit != NULL) {
            if (!json_parse_string_span(p, &kstart, out, out_size, out_len)) {
                return false;
            }
        } else if (!json_parse_string(p, out, out_size, out_len)) {
            return false;
        }
        if (p->visit != NULL) {
            char key[JSON_PATH_BYTES];

            if (kstart + 1 < p->pos) {
                size_t klen = p->pos - kstart - 2;  /* drop both quotes */

                if (klen >= sizeof(key)) {
                    klen = sizeof(key) - 1;
                }
                memcpy(key, p->src + kstart + 1, klen);
                key[klen] = '\0';
            } else {
                key[0] = '\0';
            }
            json_path_push(p, key, strlen(key));
        }
        json_skip_ws(p);
        if (json_peek(p) != ':') {
            json_fail(p, "expected :");
            return false;
        }
        json_advance(p);
        if (!json_out_str(p, out, out_size, out_len, ": ", 2)) {
            return false;
        }
        json_skip_ws(p);
        if (!json_parse_value(p, out, out_size, out_len, indent + 1)) {
            return false;
        }
        if (p->visit != NULL) {
            json_path_pop(p);
        }
        json_skip_ws(p);
        if (json_peek(p) == ',') {
            json_advance(p);
            if (!json_out_str(p, out, out_size, out_len, ",\n", 2)) {
                return false;
            }
            first = false;
            continue;
        }
        if (json_peek(p) == '}') {
            json_advance(p);
            if (!json_out_str(p, out, out_size, out_len, "\n", 1)) {
                return false;
            }
            if (!json_out_indent(p, out, out_size, out_len, indent)) {
                return false;
            }
            return json_out_str(p, out, out_size, out_len, "}", 1);
        }
        json_fail(p, first ? "expected }" : "expected , or }");
        return false;
    }
}

/** Emit one scalar event (walk mode only). Returns false to abort the walk. */
static bool json_emit(json_parser_t *p, size_t start)
{
    if (p->visit == NULL) {
        return true;
    }
    /* A string value is passed without its surrounding quotes so that the
     * contract matches the path (the caller still unescapes escapes). */
    if (p->pos > start + 1 && p->src[start] == '"' && p->src[p->pos - 1] == '"') {
        return p->visit(p->path, p->src + start + 1, p->pos - start - 2, p->visit_ctx);
    }
    return p->visit(p->path, p->src + start, p->pos - start, p->visit_ctx);
}

static bool json_parse_value(json_parser_t *p, char *out, size_t out_size,
                             size_t *out_len, int indent)
{
    char c;
    size_t vstart;

    json_skip_ws(p);
    c = json_peek(p);
    if (c == '{') {
        return json_parse_object(p, out, out_size, out_len, indent);
    }
    if (c == '[') {
        return json_parse_array(p, out, out_size, out_len, indent);
    }

    vstart = p->pos;
    if (c == '"') {
        if (!json_parse_string(p, out, out_size, out_len)) {
            return false;
        }
    } else if (c == 't') {
        if (!json_parse_literal(p, out, out_size, out_len, "true")) {
            return false;
        }
    } else if (c == 'f') {
        if (!json_parse_literal(p, out, out_size, out_len, "false")) {
            return false;
        }
    } else if (c == 'n') {
        if (!json_parse_literal(p, out, out_size, out_len, "null")) {
            return false;
        }
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        if (!json_parse_number(p, out, out_size, out_len)) {
            return false;
        }
    } else if (c == '\0') {
        json_fail(p, "unexpected end");
        return false;
    } else {
        json_fail(p, "unexpected character");
        return false;
    }
    return json_emit(p, vstart);
}

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

/** Shared prologue: reject bad args and prime line/col. */
static bool json_begin(json_parser_t *p, const char *text, size_t len)
{
    if (text == NULL) {
        return false;
    }
    memset(p, 0, sizeof(*p));
    p->src = text;
    p->len = len;
    p->line = 1;
    p->col = 1;
    return true;
}

/** Run the descent to the first value and reject trailing data. */
static bool json_descent(json_parser_t *p, char *out, size_t out_size,
                         size_t *out_len)
{
    bool ok = json_parse_value(p, out, out_size, out_len, 0);

    if (ok && p->visit == NULL) {
        json_skip_ws(p);
        if (p->pos < p->len) {
            json_fail(p, "trailing data");
            ok = false;
        }
    }
    return ok;
}

bool json_validate_text(const char *text, size_t len, char *err, size_t err_size)
{
    json_parser_t parser;
    size_t out_len = 0;
    bool ok;

    if (text == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "null input");
        }
        return false;
    }
    if (!json_begin(&parser, text, len)) {
        return false;
    }
    ok = json_descent(&parser, NULL, 0, &out_len);
    if (!ok && err != NULL && err_size > 0) {
        snprintf(err, err_size, "%s", parser.error);
    }
    return ok;
}

size_t json_pretty_text(const char *text, size_t len, char *out, size_t out_size,
                        char *err, size_t err_size)
{
    json_parser_t parser;
    size_t out_len = 0;

    if (text == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    if (!json_begin(&parser, text, len)) {
        return 0;
    }
    if (!json_descent(&parser, out, out_size, &out_len)) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "%s", parser.error);
        }
        return 0;
    }
    if (out_len + 1 < out_size) {
        out[out_len++] = '\n';
    }
    out[out_len < out_size ? out_len : out_size - 1] = '\0';
    return out_len;
}

bool json_walk(const char *text, size_t len, json_visit_fn visit, void *ctx,
               char *err, size_t err_size)
{
    json_parser_t parser;
    size_t out_len = 0;
    bool ok;

    if (text == NULL || visit == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "null input");
        }
        return false;
    }
    if (!json_begin(&parser, text, len)) {
        return false;
    }
    parser.visit = visit;
    parser.visit_ctx = ctx;
    ok = json_descent(&parser, NULL, 0, &out_len);
    if (!ok && err != NULL && err_size > 0) {
        snprintf(err, err_size, "%s", parser.error);
    }
    return ok;
}

/* ========================================================================
 * ESCAPE DECODING
 * ======================================================================== */

/** UTF-8 encode one codepoint; returns bytes written (1..4). */
static size_t json_utf8_encode(unsigned long cp, char *out)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        out[0] = '?';
        return 1;
    }
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/** Read 4 hex digits at @p s; returns false when they are not all hex. */
static bool json_hex4(const char *s, unsigned long *out)
{
    unsigned long cp = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c = s[i];
        int d;

        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return false;
        }
        cp = cp * 16 + (unsigned long)d;
    }
    *out = cp;
    return true;
}

size_t json_unescape_span(const char *src, size_t len, char *out, size_t out_size)
{
    size_t i = 0;
    size_t n = 0;

    if (src == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    while (i < len) {
        char c = src[i];

        if (c != '\\' || i + 1 >= len) {
            if (n + 1 >= out_size) {
                break;
            }
            out[n++] = c;
            i++;
            continue;
        }
        i++;  /* consume the backslash */
        c = src[i];
        switch (c) {
        case '"': case '\\': case '/':
            if (n + 1 >= out_size) { i = len; break; }
            out[n++] = c;
            i++;
            break;
        case 'b': if (n + 1 >= out_size) { i = len; break; } out[n++] = '\b'; i++; break;
        case 'f': if (n + 1 >= out_size) { i = len; break; } out[n++] = '\f'; i++; break;
        case 'n': if (n + 1 >= out_size) { i = len; break; } out[n++] = '\n'; i++; break;
        case 'r': if (n + 1 >= out_size) { i = len; break; } out[n++] = '\r'; i++; break;
        case 't': if (n + 1 >= out_size) { i = len; break; } out[n++] = '\t'; i++; break;
        case 'u': {
            unsigned long cp;

            if (i + 5 > len || !json_hex4(src + i + 1, &cp)) {
                /* Malformed: pass the 'u' through literally rather than
                 * dropping data, so the text stays displayable. */
                if (n + 1 >= out_size) { i = len; break; }
                out[n++] = 'u';
                i++;
                break;
            }
            i += 5;
            /* Surrogate pair: a low surrogate must follow a high one. */
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                i + 6 <= len && src[i] == '\\' && src[i + 1] == 'u') {
                unsigned long lo;

                if (json_hex4(src + i + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (n + 4 >= out_size) { i = len; break; }
            n += json_utf8_encode(cp, out + n);
            break;
        }
        default:
            /* Unknown escape: keep both characters, do not silently drop. */
            if (n + 2 >= out_size) { i = len; break; }
            out[n++] = '\\';
            out[n++] = c;
            i++;
            break;
        }
    }
    out[n] = '\0';
    return n;
}
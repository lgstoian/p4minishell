/**
 * @file markdown.c
 * @brief Markdown rendering (CommonMark-ish subset) to ANSI SGR.
 *
 * Self-contained (no shell/batch deps; own UTF-8 decoder) so batch,
 * storage, and command components can all use it without cycles.
 * Budgets: single pass, O(1) stack besides caller buffers; tables buffer
 * one table (capped) before aligning.
 */

#include "markdown.h"

#include <stdlib.h>
#include <string.h>

static bool s_markdown_auto = true;

void markdown_set_auto(bool on)
{
    s_markdown_auto = on;
}

bool markdown_get_auto(void)
{
    return s_markdown_auto;
}

/* ========================================================================
 * UTF-8 + DISPLAY WIDTH
 * ======================================================================== */

/** Decode one codepoint. Returns bytes consumed (0 on NUL/invalid). */
static size_t md_utf8_decode(const char *s, size_t avail, unsigned long *cp_out)
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

size_t markdown_display_width(const char *s)
{
    size_t width = 0;
    size_t i = 0;
    size_t n;

    if (s == NULL) {
        return 0;
    }
    n = strlen(s);
    while (i < n) {
        unsigned long cp = 0;
        size_t len = md_utf8_decode(s + i, n - i, &cp);
        if (len == 0) {
            i++;
            continue;
        }
        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
            /* Control: zero width. */
        } else if ((cp >= 0x300 && cp <= 0x36F) || /* combining */
                   (cp >= 0x200B && cp <= 0x200F) || /* zero-width */
                   cp == 0xFEFF) {
            /* Zero width. */
        } else if ((cp >= 0x1100 && cp <= 0x115F) || /* Hangul Jamo */
                   (cp >= 0x2E80 && cp <= 0x303E) || /* CJK roots/punct */
                   (cp >= 0x3041 && cp <= 0x33FF) || /* Hiragana/Katakana/CJK compat */
                   (cp >= 0x3400 && cp <= 0x4DBF) || /* Ext A */
                   (cp >= 0x4E00 && cp <= 0x9FFF) || /* Unified */
                   (cp >= 0xA000 && cp <= 0xA4CF) || /* Yi */
                   (cp >= 0xAC00 && cp <= 0xD7AF) || /* Hangul */
                   (cp >= 0xF900 && cp <= 0xFAFF) || /* Compat ideographs */
                   (cp >= 0xFE30 && cp <= 0xFE4F) || /* CJK compat forms */
                   (cp >= 0xFF00 && cp <= 0xFF60) || /* Fullwidth */
                   (cp >= 0x20000 && cp <= 0x3FFFD)) { /* Ext B+ */
            width += 2;
        } else {
            width += 1;
        }
        i += len;
    }
    return width;
}

/* ========================================================================
 * BOUNDED WRITER (never splits UTF-8/SGR, always closes SGR + NUL-terms)
 * ======================================================================== */

typedef struct {
    char *out;
    size_t cap;   /* total buffer size */
    size_t len;   /* bytes used (excl NUL) */
    bool sgr;     /* an SGR sequence is open */
    bool changed; /* any markup emitted */
} md_writer_t;

static bool md_putn(md_writer_t *w, const char *s, size_t n)
{
    size_t i = 0;
    /* Copy whole UTF-8 sequences only; stop before a partial one. */
    while (i < n) {
        unsigned long cp = 0;
        size_t len = md_utf8_decode(s + i, n - i, &cp);
        if (len == 0) {
            len = 1;
        }
        if (w->len + len + 5 >= w->cap) {
            return false;
        }
        memcpy(w->out + w->len, s + i, len);
        w->len += len;
        i += len;
    }
    return true;
}

static bool md_puts(md_writer_t *w, const char *s)
{
    return md_putn(w, s, strlen(s));
}

/* Emit a raw SGR introducer (caller guarantees space via the +5 headroom). */
static void md_sgr(md_writer_t *w, const char *seq)
{
    size_t n = strlen(seq);
    if (w->len + n + 5 >= w->cap) {
        return;
    }
    memcpy(w->out + w->len, seq, n);
    w->len += n;
    w->sgr = true;
    w->changed = true;
}

static void md_finish(md_writer_t *w)
{
    if (w->cap == 0) {
        return;
    }
    if (w->sgr && w->len + 5 < w->cap) {
        memcpy(w->out + w->len, "\x1b[0m", 4);
        w->len += 4;
    }
    w->out[w->len < w->cap ? w->len : w->cap - 1] = '\0';
}

/* ========================================================================
 * INLINE PARSER (flanking-safe emphasis, code, links)
 * ======================================================================== */

#include <ctype.h>

#define MD_NEST_MAX 8

/* Character classes for flanking (ASCII punct/space; multibyte = word). */
static bool md_is_space_cp(unsigned long cp)
{
    return cp == ' ' || cp == '\t';
}

static bool md_is_punct_cp(unsigned long cp)
{
    return cp < 0x80 && ispunct((int)cp) != 0;
}

/* Codepoint at byte offset (0 on end/invalid). */
static unsigned long md_cp_at(const char *p, const char *end)
{
    unsigned long cp = 0;
    if (p < end) {
        md_utf8_decode(p, (size_t)(end - p), &cp);
    }
    return cp;
}

/* Byte length of the codepoint starting at p (1 if invalid, 0 at end). */
static size_t md_cp_len(const char *p, const char *end)
{
    unsigned long cp = 0;
    size_t len;
    if (p >= end) {
        return 0;
    }
    len = md_utf8_decode(p, (size_t)(end - p), &cp);
    return len > 0 ? len : 1;
}

/* Skip a backtick code span starting at p (p points at `). Returns the
 * byte after the closing run, or NULL when unterminated. */
static const char *md_skip_code_span(const char *p, const char *end)
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
            q += md_cp_len(q, end);
        }
    }
    return NULL;
}

/* Delimiter descriptor for emphasis parsing. */
typedef struct {
    const char *text;
    size_t text_len;
    const char *open_sgr;
    const char *close_sgr;
    bool underscore;
} md_delim_t;

static const md_delim_t MD_DELIMS[] = {
    { "**", 2, "\x1b[1m", "\x1b[22m", false },
    { "__", 2, "\x1b[1m", "\x1b[22m", true },
    { "~~", 2, "\x1b[9m", "\x1b[29m", false },
    { "*", 1, "\x1b[3m", "\x1b[23m", false },
    { "_", 1, "\x1b[3m", "\x1b[23m", true },
};

/* Forward declaration. */
static const char *md_parse_inline(const char *p, const char *end,
                                   const char *stop, size_t stop_len,
                                   md_writer_t *w, int depth);

/* Codepoint ending exactly at q (q > start), or 0. */
static unsigned long md_prev_cp(const char *start, const char *q)
{
    const char *r;

    if (q <= start) {
        return 0;
    }
    r = q - 1;
    while (r > start && ((unsigned char)*r & 0xC0) == 0x80) {
        r--;
    }
    return md_cp_at(r, q);
}

/* Flanking test for a delimiter run at q (run bytes already matched). */
static void md_flank(const char *start, const char *q, const char *end,
                     size_t run, bool underscore,
                     bool *can_open, bool *can_close)
{
    unsigned long prev = md_prev_cp(start, q);
    unsigned long next = 0;
    const char *after = q + run;

    if (after < end) {
        md_utf8_decode(after, (size_t)(end - after), &next);
    }
    if (underscore) {
        bool left_ok = (prev == 0 || md_is_space_cp(prev) || md_is_punct_cp(prev));
        bool right_ok = (next == 0 || md_is_space_cp(next) || md_is_punct_cp(next));
        *can_open = left_ok && next != 0 && !md_is_space_cp(next);
        *can_close = right_ok && prev != 0 && !md_is_space_cp(prev);
    } else {
        *can_open = next != 0 && !md_is_space_cp(next);
        *can_close = prev != 0 && !md_is_space_cp(prev);
    }
}

/* Find the matching closer for a delimiter run (nesting-aware, skipping
 * code spans). base is the scan-region start (for prev computation).
 * Returns the closer position or NULL. */
static const char *md_find_closer(const char *base, const char *p,
                                  const char *end, const md_delim_t *d)
{
    int nest = 1;
    const char *q = p;

    while (q < end) {
        if (*q == '`') {
            const char *after = md_skip_code_span(q, end);
            if (after == NULL) {
                return NULL;
            }
            q = after;
            continue;
        }
        if ((size_t)(end - q) >= d->text_len &&
            memcmp(q, d->text, d->text_len) == 0) {
            bool can_open = false;
            bool can_close = false;
            md_flank(base, q, end, d->text_len, d->underscore,
                     &can_open, &can_close);
            if (can_close && !(can_open && nest > 1)) {
                nest--;
                if (nest == 0) {
                    return q;
                }
            } else if (can_open) {
                nest++;
            }
            q += d->text_len;
            continue;
        }
        q += md_cp_len(q, end);
    }
    return NULL;
}

/* Parse a [text](url) link starting at p ('['). Returns end of construct
 * or NULL. Emits underlined cyan text + " (url)". */
static const char *md_parse_link(const char *p, const char *end,
                                 md_writer_t *w, int depth)
{
    const char *q = p + 1;
    int nest = 1;
    const char *text_end = NULL;
    const char *url;
    const char *url_end;
    const char *url_close;

    while (q < end) {
        if (*q == '`') {
            const char *after = md_skip_code_span(q, end);
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
        q += md_cp_len(q, end);
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
        md_sgr(w, "\x1b[4m\x1b[36m");
        md_parse_inline(p + 1, text_end, NULL, 0, w, depth + 1);
        md_sgr(w, "\x1b[39m\x1b[24m");
        md_puts(w, " (");
        md_putn(w, url, (size_t)(ue - url));
        md_puts(w, ")");
        return url_close + 1;
    }
}

/* Recursive inline parser. Parses [p, end) until stop delimiter (or end),
 * emitting to w. Returns position after the stop (or end). */
static const char *md_parse_inline(const char *p, const char *end,
                                   const char *stop, size_t stop_len,
                                   md_writer_t *w, int depth)
{
    const char *base = p;

    while (p < end) {
        size_t i;
        /* Stop delimiter? */
        if (stop != NULL && (size_t)(end - p) >= stop_len &&
            memcmp(p, stop, stop_len) == 0) {
            return p + stop_len;
        }
        /* Escape: backslash + ASCII punct -> literal. */
        if (*p == '\\' && p + 1 < end && md_is_punct_cp((unsigned char)p[1])) {
            md_putn(w, p + 1, 1);
            p += 2;
            continue;
        }
        /* Code span: opaque, dimmed. */
        if (*p == '`') {
            const char *after = md_skip_code_span(p, end);
            if (after != NULL) {
                size_t open_len = 0;
                const char *q = p;
                const char *inner_end;
                while (q < end && *q == '`') {
                    open_len++;
                    q++;
                }
                inner_end = after - open_len;
                md_sgr(w, "\x1b[2m");
                /* Strip one surrounding space pair per CommonMark. */
                if (inner_end - q >= 2 && *q == ' ' && inner_end[-1] == ' ') {
                    q++;
                    inner_end--;
                }
                md_putn(w, q, (size_t)(inner_end - q));
                md_sgr(w, "\x1b[22m");
                p = after;
                continue;
            }
            md_putn(w, p, 1);
            p++;
            continue;
        }
        /* Link. */
        if (*p == '[') {
            const char *after = md_parse_link(p, end, w, depth);
            if (after != NULL) {
                p = after;
                continue;
            }
            md_putn(w, p, 1);
            p++;
            continue;
        }
        /* Emphasis delimiters (longest first via table order). */
        {
            bool matched = false;
            for (i = 0; i < sizeof(MD_DELIMS) / sizeof(MD_DELIMS[0]); i++) {
                const md_delim_t *d = &MD_DELIMS[i];
                if ((size_t)(end - p) >= d->text_len &&
                    memcmp(p, d->text, d->text_len) == 0 &&
                    depth < MD_NEST_MAX) {
                    bool can_open = false;
                    bool can_close = false;
                    /* The run must be able to open here (kills `a * b * c`
                     * false pairs); the closer search validates the rest. */
                    md_flank(base, p, end, d->text_len, d->underscore,
                             &can_open, &can_close);
                    (void)can_close;
                    if (can_open) {
                        const char *closer = md_find_closer(base, p + d->text_len,
                                                            end, d);
                        if (closer != NULL) {
                            md_sgr(w, d->open_sgr);
                            md_parse_inline(p + d->text_len, closer, NULL, 0,
                                            w, depth + 1);
                            md_sgr(w, d->close_sgr);
                            p = closer + d->text_len;
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
        /* Plain codepoint copy. */
        {
            size_t len = md_cp_len(p, end);
            md_putn(w, p, len);
            p += len;
        }
    }
    return p;
}

/* ========================================================================
 * BLOCK LAYER (headings, lists, quotes, hr, fences, tables)
 * ======================================================================== */

/* Skip ASCII spaces/tabs. Returns new position. */
static const char *md_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

/* ATX heading: optional 3-space indent + 1-6 # + space/EOL. Returns level
 * (0 = no match) and sets *content to the text start. */
static int md_match_heading(const char *p, const char *end, const char **content)
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
    *content = md_skip_ws(q, end);
    return level;
}

/* Trim trailing spaces/tabs + closing ATX hashes. Returns new end. */
static const char *md_block_end_trim(const char *q, const char *end, bool hashes)
{
    const char *r = end;
    while (r > q && (r[-1] == ' ' || r[-1] == '\t')) {
        r--;
    }
    if (hashes) {
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

/* List item: indent + (-|+|*|\d+[.)]) + space. Returns marker end (0 = no).
 * Sets *content to text start. */
static const char *md_match_list(const char *p, const char *end, const char **content)
{
    const char *q = p;

    while (q < end && (*q == ' ' || *q == '\t')) {
        q++;
    }
    if (q < end && (*q == '-' || *q == '+' || *q == '*')) {
        if (q + 1 >= end || (q[1] != ' ' && q[1] != '\t')) {
            return NULL;
        }
        *content = md_skip_ws(q + 1, end);
        return q + 1;
    }
    if (q < end && *q >= '0' && *q <= '9') {
        while (q < end && *q >= '0' && *q <= '9') {
            q++;
        }
        if (q >= end || (*q != '.' && *q != ')')) {
            return NULL;
        }
        q++;
        if (q >= end || (*q != ' ' && *q != '\t')) {
            return NULL;
        }
        *content = md_skip_ws(q, end);
        return q;
    }
    return NULL;
}

/* Block quote: optional indent + >. Returns content start (NULL = no). */
static const char *md_match_quote(const char *p, const char *end, const char **content)
{
    const char *q = p;
    int indent = 0;

    while (q < end && *q == ' ' && indent < 4) {
        indent++;
        q++;
    }
    if (indent >= 4 || q >= end || *q != '>') {
        return NULL;
    }
    q++;
    if (q < end && *q == ' ') {
        q++;
    }
    *content = q;
    return q;
}

/* Thematic break: 3+ of (-|*|_) with only spaces between. */
static bool md_match_hr(const char *p, const char *end)
{
    char mark = '\0';
    int count = 0;
    const char *q = p;

    q = md_skip_ws(q, end);
    while (q < end) {
        if (*q == '-' || *q == '*' || *q == '_') {
            if (mark == '\0') {
                mark = *q;
            } else if (*q != mark) {
                return false;
            }
            count++;
            q++;
        } else if (*q == ' ' || *q == '\t') {
            q++;
        } else {
            return false;
        }
    }
    return count >= 3;
}

/* Fence marker: ``` or ~~~ (info string ignored, content verbatim+dim). */
static bool md_match_fence(const char *p, const char *end, char *mark_out)
{
    const char *q = md_skip_ws(p, end);
    char mark;
    int count = 0;

    if (q >= end || (*q != '`' && *q != '~')) {
        return false;
    }
    mark = *q;
    while (q < end && *q == mark) {
        count++;
        q++;
    }
    if (count < 3) {
        return false;
    }
    if (mark_out != NULL) {
        *mark_out = mark;
    }
    return true;
}

/* Split a table row on | honoring code spans. Writes cell bounds into
 * cells[] (start/end pairs), returns cell count (cap pairs). */
#define MD_TABLE_COLS_MAX 16

static int md_split_row(const char *p, const char *end,
                        const char *cells[][2], int cap)
{
    int n = 0;
    const char *q = p;
    const char *cell_start;

    /* Skip leading pipe. */
    {
        const char *t = md_skip_ws(q, end);
        if (t < end && *t == '|') {
            q = t + 1;
        }
    }
    cell_start = q;
    while (q < end && n < cap) {
        if (*q == '`') {
            const char *after = md_skip_code_span(q, end);
            q = (after != NULL) ? after : q + 1;
            continue;
        }
        if (*q == '|') {
            cells[n][0] = cell_start;
            cells[n][1] = q;
            n++;
            cell_start = q + 1;
        }
        q += md_cp_len(q, end);
    }
    /* Trailing cell (ignore a lone trailing pipe's empty tail). */
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

/* Separator row: |? :?-+:? (| :?-+:?)* |? — fills align[] (0 left, 1 center,
 * 2 right). Returns col count or 0. */
static int md_parse_table_sep(const char *p, const char *end, int align[], int cap)
{
    const char *cells[MD_TABLE_COLS_MAX][2];
    int n = md_split_row(p, end, cells, MD_TABLE_COLS_MAX);
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

/* Render one block line (no fences/tables). Returns true if markup changed it. */
static bool md_render_block_line(const char *p, const char *end, md_writer_t *w)
{
    const char *content;
    const char *mark_end;
    bool before = w->changed;

    /* Heading. */
    {
        int level = md_match_heading(p, end, &content);
        if (level > 0) {
            const char *ce = md_block_end_trim(content, end, true);
            md_sgr(w, "\x1b[1m");
            if (level <= 2) {
                md_sgr(w, "\x1b[4m");
            }
            md_parse_inline(content, ce, NULL, 0, w, 0);
            if (level <= 2) {
                md_sgr(w, "\x1b[24m");
            }
            md_sgr(w, "\x1b[22m");
            return true;
        }
    }
    /* Quote. */
    {
        const char *qc = NULL;
        if (md_match_quote(p, end, &qc) != NULL) {
            md_puts(w, "\xe2\x94\x82 ");
            md_parse_inline(qc, end, NULL, 0, w, 0);
            return true;
        }
    }
    /* List item (marker preserved, content inline-rendered). */
    mark_end = md_match_list(p, end, &content);
    if (mark_end != NULL) {
        md_putn(w, p, (size_t)(mark_end - p));
        md_puts(w, " ");
        md_parse_inline(content, end, NULL, 0, w, 0);
        return true;
    }
    /* HR. */
    if (md_match_hr(p, end)) {
        int i;
        for (i = 0; i < 40; i++) {
            if (!md_putn(w, "\xe2\x94\x80", 3)) {
                break;
            }
        }
        return true;
    }
    /* Plain line: inline spans only (flanking-safe). */
    md_parse_inline(p, end, NULL, 0, w, 0);
    return w->changed != before;
}

bool markdown_render_line(const char *line, char *out, size_t out_size)
{
    md_writer_t w;
    const char *end;

    if (line == NULL || out == NULL || out_size == 0) {
        return false;
    }
    end = line + strlen(line);
    while (end > line && (end[-1] == '\n' || end[-1] == '\r')) {
        end--;
    }
    w.out = out;
    w.cap = out_size;
    w.len = 0;
    w.sgr = false;
    w.changed = false;
    {
        /* Structural match (heading/quote/list/hr) counts even without SGR;
         * plain lines report inline-span changes only. */
        bool matched = md_render_block_line(line, end, &w);
        md_puts(&w, "\n");
        md_finish(&w);
        return matched;
    }
}

/* ========================================================================
 * DOCUMENT MODE (fences + tables + block lines)
 * ======================================================================== */

#define MD_TABLE_ROWS_MAX 64

/* Display width ignoring SGR escape sequences. */
static size_t md_ansi_width(const char *s)
{
    size_t width = 0;
    const char *p = s;

    while (*p != '\0') {
        if (*p == '\x1b' && p[1] == '[') {
            p += 2;
            while (*p != '\0' && (*p < '@' || *p > '~')) {
                p++;
            }
            if (*p != '\0') {
                p++;
            }
            continue;
        }
        {
            unsigned long cp = 0;
            size_t len = md_utf8_decode(p, strlen(p), &cp);
            char tmp[8];
            if (len == 0) {
                p++;
                continue;
            }
            memcpy(tmp, p, len);
            tmp[len] = '\0';
            width += markdown_display_width(tmp);
            p += len;
        }
    }
    return width;
}

/* Render one table cell's inline spans into a heap buffer (caller frees). */
static char *md_render_cell(const char *a, const char *b)
{
    size_t raw = (size_t)(b - a);
    char *tmp = malloc(raw * 4 + 64);
    md_writer_t tw;

    while (a < b && (*a == ' ' || *a == '\t')) {
        a++;
    }
    while (b > a && (b[-1] == ' ' || b[-1] == '\t')) {
        b--;
    }
    if (tmp == NULL) {
        return NULL;
    }
    tw.out = tmp;
    tw.cap = raw * 4 + 64;
    tw.len = 0;
    tw.sgr = false;
    tw.changed = false;
    md_parse_inline(a, b, NULL, 0, &tw, 0);
    md_finish(&tw);
    return tmp;
}

/* Render buffered table rows [first, first+count). Returns false when the
 * shape is invalid (caller emits rows verbatim instead). */
static bool md_render_table(const char **starts, const char **ends,
                            int first, int count, md_writer_t *w)
{
    static const char *cells[MD_TABLE_ROWS_MAX][MD_TABLE_COLS_MAX][2];
    int widths[MD_TABLE_COLS_MAX];
    int align[MD_TABLE_COLS_MAX];
    int ncols;
    int r;
    int c;

    if (count < 2 || count > MD_TABLE_ROWS_MAX) {
        return false;
    }
    ncols = md_split_row(starts[first], ends[first], cells[0], MD_TABLE_COLS_MAX);
    if (ncols <= 0 ||
        md_parse_table_sep(starts[first + 1], ends[first + 1], align, ncols) != ncols) {
        return false;
    }
    for (r = first + 2; r < first + count; r++) {
        if (md_split_row(starts[r], ends[r], cells[r - first], MD_TABLE_COLS_MAX) != ncols) {
            return false;
        }
    }
    for (c = 0; c < ncols; c++) {
        widths[c] = 0;
    }
    /* Measure header + body (skip separator). Widths ignore SGR. */
    for (r = 0; r < count; r++) {
        if (r == 1) {
            continue;
        }
        for (c = 0; c < ncols; c++) {
            char *tmp = md_render_cell(cells[r][c][0], cells[r][c][1]);
            size_t wd;
            if (tmp == NULL) {
                return false;
            }
            wd = md_ansi_width(tmp);
            free(tmp);
            if (wd > (size_t)0x7FFFFFFF) {
                wd = 0;
            }
            if ((int)wd > widths[c]) {
                widths[c] = (int)wd;
            }
        }
    }
    /* Emit header, separator, body. */
    for (r = 0; r < count; r++) {
        if (r == 1) {
            md_puts(w, "|");
            for (c = 0; c < ncols; c++) {
                int i;
                md_puts(w, align[c] == 1 ? " :" : " ");
                for (i = 0; i < widths[c]; i++) {
                    md_puts(w, "-");
                }
                md_puts(w, (align[c] == 1 || align[c] == 2) ? ": |" : " |");
            }
            md_puts(w, "\n");
            continue;
        }
        md_puts(w, "|");
        for (c = 0; c < ncols; c++) {
            char *tmp = md_render_cell(cells[r][c][0], cells[r][c][1]);
            size_t wd;
            int pad;
            if (tmp == NULL) {
                return false;
            }
            wd = md_ansi_width(tmp);
            pad = widths[c] - (int)wd;
            if (pad < 0) {
                pad = 0;
            }
            md_puts(w, " ");
            if (align[c] == 2) {
                while (pad-- > 0) {
                    md_puts(w, " ");
                }
                md_puts(w, tmp);
            } else if (align[c] == 1) {
                int left = pad / 2;
                int right = pad - left;
                while (left-- > 0) {
                    md_puts(w, " ");
                }
                md_puts(w, tmp);
                while (right-- > 0) {
                    md_puts(w, " ");
                }
            } else {
                md_puts(w, tmp);
                while (pad-- > 0) {
                    md_puts(w, " ");
                }
            }
            md_puts(w, " |");
            free(tmp);
        }
        md_puts(w, "\n");
    }
    return true;
}

size_t markdown_render_doc(const char *md, char *out, size_t out_size)
{
    /* Line table (pointers into md, no copy). Grows geometrically; a
     * pathological single-line file costs one entry. */
    const char **starts = NULL;
    const char **ends = NULL;
    size_t nlines = 0;
    size_t cap = 0;
    const char *p;
    md_writer_t w;
    bool in_fence = false;
    char fence_mark = '\0';
    /* Pending table row range [table_first, table_first+table_count). */
    int table_first = -1;
    int table_count = 0;
    size_t i;

    if (md == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    w.out = out;
    w.cap = out_size;
    w.len = 0;
    w.sgr = false;
    w.changed = false;

    p = md;
    while (*p != '\0') {
        const char *e = p;
        while (*e != '\0' && *e != '\n') {
            e++;
        }
        {
            const char *le = e;
            while (le > p && (le[-1] == '\r')) {
                le--;
            }
            if (nlines >= cap) {
                size_t ncap = cap == 0 ? 64 : cap * 2;
                const char **ns = realloc((void *)starts, ncap * sizeof(*ns));
                const char **ne = realloc((void *)ends, ncap * sizeof(*ne));
                if (ns == NULL || ne == NULL) {
                    free(ns);
                    free(ne);
                    break;
                }
                starts = ns;
                ends = ne;
                cap = ncap;
            }
            starts[nlines] = p;
            ends[nlines] = le;
            nlines++;
        }
        p = (*e == '\n') ? e + 1 : e;
    }

    /* Flush a pending table (verbatim fallback when malformed). */
    for (i = 0; i < nlines; i++) {
        const char *ls = starts[i];
        const char *le = ends[i];
        char mark = '\0';
        bool is_fence = md_match_fence(ls, le, &mark);

        if (in_fence) {
            if (is_fence && mark == fence_mark) {
                in_fence = false;
            } else {
                md_sgr(&w, "\x1b[2m");
                md_putn(&w, ls, (size_t)(le - ls));
                md_sgr(&w, "\x1b[22m");
                md_puts(&w, "\n");
            }
            continue;
        }
        if (is_fence) {
            in_fence = true;
            fence_mark = mark;
            continue;
        }
        /* Table row candidate: contains a pipe. Accumulate while the shape
         * holds (validated at flush). */
        {
            bool has_pipe = false;
            const char *q = ls;
            while (q < le) {
                if (*q == '`') {
                    const char *after = md_skip_code_span(q, le);
                    q = (after != NULL) ? after : q + 1;
                    continue;
                }
                if (*q == '|') {
                    has_pipe = true;
                    break;
                }
                q += md_cp_len(q, le);
            }
            if (has_pipe) {
                if (table_first < 0) {
                    table_first = (int)i;
                    table_count = 0;
                }
                table_count++;
                if (table_count > MD_TABLE_ROWS_MAX) {
                    /* Overlong table: flush what fits, restart after it. */
                    if (!md_render_table(starts, ends, table_first,
                                         MD_TABLE_ROWS_MAX, &w)) {
                        int r;
                        for (r = table_first;
                             r < table_first + MD_TABLE_ROWS_MAX; r++) {
                            md_render_block_line(starts[r], ends[r], &w);
                            md_puts(&w, "\n");
                        }
                    }
                    table_first = -1;
                    table_count = 0;
                }
                continue;
            }
        }
        /* Non-table line: flush any pending table first. */
        if (table_first >= 0) {
            if (table_count >= 2) {
                if (!md_render_table(starts, ends, table_first,
                                     table_count, &w)) {
                    int r;
                    for (r = table_first; r < table_first + table_count; r++) {
                        md_render_block_line(starts[r], ends[r], &w);
                        md_puts(&w, "\n");
                    }
                }
            } else {
                int r;
                for (r = table_first; r < table_first + table_count; r++) {
                    md_render_block_line(starts[r], ends[r], &w);
                    md_puts(&w, "\n");
                }
            }
            table_first = -1;
            table_count = 0;
        }
        md_render_block_line(ls, le, &w);
        md_puts(&w, "\n");
    }
    /* Trailing table at EOF. */
    if (table_first >= 0) {
        if (table_count >= 2) {
            if (!md_render_table(starts, ends, table_first,
                                 table_count, &w)) {
                int r;
                for (r = table_first; r < table_first + table_count; r++) {
                    md_render_block_line(starts[r], ends[r], &w);
                    md_puts(&w, "\n");
                }
            }
        } else {
            int r;
            for (r = table_first; r < table_first + table_count; r++) {
                md_render_block_line(starts[r], ends[r], &w);
                md_puts(&w, "\n");
            }
        }
    }
    /* Unterminated fence: content already emitted dimmed; nothing to close. */
    free((void *)starts);
    free((void *)ends);
    md_finish(&w);
    return w.len;
}

size_t markdown_strip_ansi(const char *src, char *dst, size_t dst_size)
{
    size_t used = 0;

    if (dst == NULL || dst_size == 0) {
        return 0;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return 0;
    }
    while (*src != '\0' && used + 1 < dst_size) {
        if (*src == '\x1b' && src[1] == '[') {
            src += 2;
            while (*src != '\0' && (*src < '@' || *src > '~')) {
                src++;
            }
            if (*src != '\0') {
                src++;
            }
            continue;
        }
        dst[used++] = *src++;
    }
    dst[used] = '\0';
    return used;
}
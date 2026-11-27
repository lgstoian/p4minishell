/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file dom_render.c
 * @brief The single DOM layout renderer (TUI page and ANSI).
 *
 * Block/inline layout over the parsed DOM: headings, paragraphs, lists,
 * tables, pre, blockquotes, rules, links, images, and inline emphasis. Styles
 * come from the computed CSS (fg/bg/bold/italic/underline/align/font-scale);
 * the renderer never parses HTML or CSS itself and never touches SD/network
 * (the caller supplies the image loader), so the markdown leaf stays a leaf.
 */

#include "dom_render.h"

#include "ansi.h" /* ansi_image_glyph / ANSI_IMAGE_RAMP_* */
#include "css_color.h"
#include "p4minishell_config.h"
#include "p4heap.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t fg;
    uint8_t bg;
    uint8_t fg_alpha;   /* colour alpha of the current colour (255 opaque) */
    uint8_t bg_alpha;   /* background alpha (0 = transparent, no rect) */
    uint8_t opacity;    /* cumulative CSS `opacity` of the ancestors + self */
    bool bold;
    bool italic;
    bool underline;
    uint8_t align;
    uint8_t font_scale;
    bool gradient_set;
    css_gradient_t gradient;
    bool shadow_set;
    uint8_t shadow_dos;
    uint8_t shadow_alpha;
    int shadow_x;
    int shadow_y;
    uint8_t border_radius;
} style_t;

/** One coalesced background run: a rectangle of one colour. */
typedef struct {
    int start;
    int len;
    int y0;
    uint8_t color;
    uint8_t alpha;
} bg_run_t;

typedef struct {
    const dom_render_ops_t *ops;
    int row;
    int col;
    int indent;
    int depth;              /* nested block depth (render_block recursion) */
    bool done;
} render_ctx_t;

/** Deepest block nesting the renderer follows before flattening the rest to
 *  text. Real pages nest a few dozen <div>s; a malformed or hostile document
 *  can nest thousands, and a per-level recursion would exhaust the shell
 *  worker's 28 KB stack (the live Wikipedia page did). 32 levels is far more
 *  than any readable layout needs and keeps the stack bounded. */
#define DOM_RENDER_MAX_DEPTH 32

static size_t utf8_seq_len(const char *p)
{
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/** Decode the same 1..4 byte sequence utf8_seq_len() measured. */
/** Display width of one whitespace-delimited word in CELLS, not bytes.
 *  A CP437 box run uses three UTF-8 bytes per glyph; byte-based wrapping
 *  would fold an 80-cell frame after ~26 cells and print codepoint garbage
 *  on the next row. Only the wrap decision changes - emit_text still walks
 *  one codepoint at a time and never duplicates a decoder. */
static int utf8_word_cells(const char *p)
{
    int cells = 0;

    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') {
        p += utf8_seq_len(p);
        cells++;
    }
    return cells;
}

static uint32_t utf8_codepoint(const char *p, size_t n)
{
    uint32_t cp;
    size_t i;

    if (n == 1) return (unsigned char)p[0];
    if (n < 2 || n > 4) return 0;
    cp = (uint32_t)((unsigned char)p[0] & (0x7F >> n));
    for (i = 1; i < n; i++) {
        if (((unsigned char)p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (uint32_t)((unsigned char)p[i] & 0x3F);
    }
    return cp;
}

/**
 * Presentation fallback for the typographic codepoints the shipped 12x20 cell
 * font does not cover. This is applied at the ONE text sink (emit_text ->
 * emit_utf8) that feeds both the TUI cell and ANSI surfaces, so a page shows
 * an ASCII stand-in instead of an empty box on either surface. The DOM and
 * html_entity_decode() still hold the real codepoint: only drawing falls
 * back, the same rule the emoji adapter uses for glyphs outside the font.
 * No second colour/emoji/glyph map and no font change is involved.
 */
static const char *dom_presentation(uint32_t cp)
{
    static const struct {
        uint32_t cp;
        const char *text;
    } STAND_INS[] = {
        { 0x00A2, "c" },        /* ¢  cent */
        { 0x00A3, "GBP" },      /* £  pound */
        { 0x00A5, "JPY" },      /* ¥  yen */
        { 0x00A7, "S" },        /* §  section */
        { 0x00A9, "(c)" },      /* ©  copyright */
        { 0x00AB, "<<" },       /* «  left guillemet */
        { 0x00AC, "!" },        /* ¬  not */
        { 0x00AD, "" },         /* ­  soft hyphen: no cell */
        { 0x00AE, "(r)" },      /* ®  registered */
        { 0x00B0, "deg" },      /* °  degree */
        { 0x00B1, "+/-" },      /* ±  plus/minus */
        { 0x00B5, "u" },        /* µ  micro */
        { 0x00B6, "P" },        /* ¶  pilcrow */
        { 0x00B7, "." },        /* ·  middle dot */
        { 0x00BB, ">>" },       /* »  right guillemet */
        { 0x00BC, "1/4" },      /* ¼  one quarter */
        { 0x00BD, "1/2" },      /* ½  one half */
        { 0x00BE, "3/4" },      /* ¾  three quarters */
        { 0x00D7, "x" },        /* ×  multiplication */
        { 0x00F7, "/" },        /* ÷  division */
        { 0x2013, "-" },        /* –  en dash */
        { 0x2014, "--" },       /* —  em dash */
        { 0x2018, "'" },        /* ‘  left single quote */
        { 0x2019, "'" },        /* ’  right single quote */
        { 0x201A, "'" },        /* ‚  single low quote (C1) */
        { 0x201C, "\"" },       /* “  left double quote */
        { 0x201D, "\"" },       /* ”  right double quote */
        { 0x201E, "\"" },       /* „  double low quote (C1) */
        { 0x2020, "+" },        /* †  dagger (C1) */
        { 0x2021, "+" },        /* ‡  double dagger (C1) */
        { 0x2022, "*" },        /* •  bullet */
        { 0x2026, "..." },      /* …  ellipsis */
        { 0x2030, "%" },        /* ‰  per mille (C1) */
        { 0x2039, "<" },        /* ‹  single left angle (C1) */
        { 0x203A, ">" },        /* ›  single right angle (C1) */
        { 0x20AC, "EUR" },      /* €  euro */
        { 0x2122, "(tm)" },     /* ™  trademark */
        { 0xFFFD, "?" },        /* replacement: clamp, not a tofu box */
    };

    for (size_t i = 0; i < sizeof(STAND_INS) / sizeof(STAND_INS[0]); i++) {
        if (STAND_INS[i].cp == cp) return STAND_INS[i].text;
    }
    /* Anything else outside the shipped font's cmap would reach the cell sink
     * as raw UTF-8 bytes (a tofu box or codepoint garbage on the device). The
     * TUI font carries ASCII 0x20..0x7F, box drawing 0x2500..0x257F and symbols
     * 0x2600..0x26FF (see tools/gen_tui_font.py RANGES); those pass through.
     * CP437 degree (0x00B0) is NOT in those ranges, so it stays "deg" above.
     * This is presentation only - the DOM still holds the real codepoint. */
    if (cp >= 0x80 && !((cp >= 0x2500 && cp <= 0x257F) ||
                        (cp >= 0x2600 && cp <= 0x26FF))) {
        return "?";
    }
    return NULL;
}

bool dom_image_sample_rgb565(const uint16_t *src, int src_w, int src_h,
                             uint8_t *dst, int dst_w, int dst_h)
{
    if (src == NULL || dst == NULL || src_w < 1 || src_h < 1 ||
        dst_w < 1 || dst_h < 1) {
        return false;
    }
    for (int cy = 0; cy < dst_h; cy++) {
        int sy = (int)((int64_t)cy * src_h / dst_h);

        if (sy >= src_h) sy = src_h - 1;
        for (int cx = 0; cx < dst_w; cx++) {
            int sx = (int)((int64_t)cx * src_w / dst_w);
            uint16_t px;
            uint32_t rgb;
            int r, g, b, lum, level;

            if (sx >= src_w) sx = src_w - 1;
            px = src[(size_t)sy * (size_t)src_w + (size_t)sx];
            /* Same RGB565 unpacking, same nearest-neighbour rule and the same
             * ansi_image_glyph / ansi_cga_nearest mapping as the live TUI
             * sampler in tui_frame.h. This is its pure DOM equivalent, not a
             * second ramp or palette. */
            r = ((px >> 11) & 0x1F) << 3;
            g = ((px >> 5) & 0x3F) << 2;
            b = (px & 0x1F) << 3;
            rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            lum = (r * 30 + g * 59 + b * 11) / 100;
            level = lum * ANSI_IMAGE_RAMP_LAST / 255;
            if (level < 0) level = 0;
            if (level > ANSI_IMAGE_RAMP_LAST) level = ANSI_IMAGE_RAMP_LAST;
            dst[(size_t)cy * (size_t)dst_w + (size_t)cx] =
                (uint8_t)((ansi_cga_nearest(rgb) << 4) | level);
        }
    }
    return true;
}

/** Sample one cell of an already-packed image grid. This is the cell-level
 *  half of the same sampler: the RGB565 -> packed step above runs once at
 *  load time, and layout only picks cells from that grid. */
static uint8_t dom_image_cell(const dom_image_t *img, int cx, int cy,
                              int dst_w, int dst_h)
{
    int sx = (int)((int64_t)cx * img->width / dst_w);
    int sy = (int)((int64_t)cy * img->height / dst_h);

    if (sx >= img->width) sx = img->width - 1;
    if (sy >= img->height) sy = img->height - 1;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    return img->pixels[(size_t)sy * (size_t)img->width + (size_t)sx];
}

/** CSS background-image repeats from the origin (the browser's floor(x/size)
 *  modulo rule for a positive origin), so a backdrop tile wraps instead of
 *  stretching. Same packed cell grid, same one sampler. */
static uint8_t dom_image_cell_tiled(const dom_image_t *img, int cx, int cy)
{
    int sx = cx % img->width;
    int sy = cy % img->height;

    if (sx < 0) sx += img->width;
    if (sy < 0) sy += img->height;
    return img->pixels[(size_t)sy * (size_t)img->width + (size_t)sx];
}

/** Report one image outcome through the optional sink, with a safe default
 *  for a missing native size (a cache hit stores only the packed cells). */
static void dom_report_image(const render_ctx_t *c, const char *kind,
                             const char *src, dom_image_state_t state,
                             const dom_image_t *img, int fit_w, int fit_h,
                             const char *alt)
{
    dom_image_report_t report;

    if (c->ops->report_image == NULL) return;
    memset(&report, 0, sizeof(report));
    report.kind = kind;
    report.src = (src != NULL && src[0] != '\0') ? src : "-";
    report.state = state;
    /* The caller's sink knows the pre-fit native size (it decoded the image);
     * here the best available answer is the packed grid it handed us. */
    report.native_w = img != NULL ? img->width : 0;
    report.native_h = img != NULL ? img->height : 0;
    report.fit_w = fit_w;
    report.fit_h = fit_h;
    report.alt = (alt != NULL && alt[0] != '\0') ? alt : "-";
    c->ops->report_image(c->ops->ctx, &report);
}

static bool is_skip_tag(const char *tag)
{
    return strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0 ||
           strcmp(tag, "head") == 0 || strcmp(tag, "title") == 0 ||
           strcmp(tag, "meta") == 0 || strcmp(tag, "link") == 0 ||
           strcmp(tag, "noscript") == 0;
}

static bool is_block_tag(const char *tag)
{
    static const char *tags[] = {
        "address", "article", "aside", "blockquote", "body", "canvas", "div", "dl",
        "dt", "dd", "fieldset", "figcaption", "figure", "footer", "form",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "li", "main",
        "nav", "ol", "p", "pre", "section", "table", "tbody", "td",
        "tfoot", "th", "thead", "tr", "ul", "br", NULL
    };
    for (int i = 0; tags[i] != NULL; i++) {
        if (strcmp(tag, tags[i]) == 0) return true;
    }
    return false;
}

static style_t style_merge(const dom_node_t *node, const style_t *parent)
{
    style_t s = *parent;

    if (node == NULL || node->type != DOM_NODE_ELEMENT) return s;
    if (node->fg != 16 || node->fg_alpha == 0) {
        s.fg = node->fg;
        s.fg_alpha = node->fg_alpha;
    }
    if (node->bg_set) {
        s.bg = node->bg;
        s.bg_alpha = node->bg_alpha;
    }
    /* `opacity` multiplies the whole subtree (CSS Compositing and Blending);
     * it applies even to a node that replaces the inherited colour. */
    s.opacity = (uint8_t)(((uint32_t)s.opacity * node->opacity) / 255u);
    if (node->bold) s.bold = true;
    if (node->italic) s.italic = true;
    if (node->underline) s.underline = true;
    if (node->align != 0) s.align = node->align;
    if (node->font_scale > s.font_scale) s.font_scale = node->font_scale;
    /* Visual paint is per-box (not inherited), like CSS background/box-shadow. */
    if (node->gradient_set) {
        s.gradient_set = true;
        s.gradient = node->gradient;
    }
    if (node->shadow_set) {
        s.shadow_set = true;
        s.shadow_dos = node->shadow_dos;
        s.shadow_alpha = node->shadow_alpha;
        s.shadow_x = node->shadow_x;
        s.shadow_y = node->shadow_y;
    }
    s.border_radius = node->border_radius;
    return s;
}

static void ansi_sgr(char *buf, size_t n, const style_t *s)
{
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, n - pos, "\x1b[0m");
    if (s->bold) pos += (size_t)snprintf(buf + pos, n - pos, "\x1b[1m");
    if (s->italic) pos += (size_t)snprintf(buf + pos, n - pos, "\x1b[3m");
    if (s->underline) pos += (size_t)snprintf(buf + pos, n - pos, "\x1b[4m");
    if (s->fg < 8) pos += (size_t)snprintf(buf + pos, n - pos, "\x1b[3%dm", s->fg);
    else if (s->fg < 16) pos += (size_t)snprintf(buf + pos, n - pos, "\x1b[9%dm", s->fg - 8);
    if (s->bg < 8) pos += (size_t)snprintf(buf + pos, n - pos, "\x1b[4%dm", s->bg);
    else if (s->bg < 16) pos += (size_t)snprintf(buf + pos, n - pos, "\x1b[10%dm", s->bg - 8);
}

static void newline(render_ctx_t *c)
{
    if (c->done) return;
    if (c->ops->tui) {
        c->row++;
        c->col = c->indent + 1;
        if (c->row > c->ops->rows) c->done = true;
    } else {
        if (c->ops->put_ansi) c->ops->put_ansi(c->ops->ctx, "\n");
        c->row++;
        c->col = c->indent + 1;
    }
}

static void emit_utf8(render_ctx_t *c, const char *seq, size_t len,
                      const style_t *st)
{
    char tmp[8];
    uint8_t attr;
    uint8_t fg;
    uint8_t alpha;

    if (c->done) return;
    if (len == 0 || len >= sizeof(tmp)) return;
    memcpy(tmp, seq, len);
    tmp[len] = '\0';
    /* A cell holds one opaque DOS colour, so translucent text is composited
     * over the effective background here, once, for every surface. The ANSI
     * sink gets the same nearest colour; TUI cells and ANSI output therefore
     * agree on what `rgba()` text looks like. */
    fg = st->fg;
    alpha = (uint8_t)(((uint32_t)st->fg_alpha * st->opacity) / 255u);
    if (alpha < 255) {
        uint8_t over = (st->bg < 16) ? st->bg : c->ops->base_bg;
        css_rgba_t blended = css_color_blend(
            css_color_from_dos(fg < 16 ? fg : c->ops->base_bg),
            css_color_from_dos(over & 15), alpha);

        fg = ansi_cga_nearest(((uint32_t)blended.r << 16) |
                              ((uint32_t)blended.g << 8) | blended.b);
    }
    attr = (uint8_t)((st->bold ? 1u : 0u) | (st->italic ? 4u : 0u) |
                     (st->underline ? 8u : 0u));
    if (c->ops->tui && c->ops->put_cell != NULL) {
        c->ops->put_cell(c->ops->ctx, c->col, c->row, tmp,
                         fg, st->bg, attr);
        c->col++;
    } else if (c->ops->put_ansi != NULL) {
        char buf[64];
        size_t n = 0;
        style_t painted = *st;

        painted.fg = fg;
        ansi_sgr(buf, sizeof(buf), &painted);
        n = strlen(buf);
        if (n + len < sizeof(buf)) {
            memcpy(buf + n, tmp, len);
            buf[n + len] = '\0';
            c->ops->put_ansi(c->ops->ctx, buf);
        }
        c->col++;
    }
}

static void emit_text(render_ctx_t *c, const char *text, const style_t *st)
{
    const char *p = text;

    if (text == NULL) return;
    while (*p != '\0' && !c->done) {
        if (*p == '\n') {
            newline(c);
            p++;
            continue;
        }
        if (*p == ' ' || *p == '\t') {
            if (c->col <= c->ops->cols) emit_utf8(c, " ", 1, st);
            p++;
            continue;
        }
        {
            const char *start = p;
            size_t bytes;
            if (c->col > 1 &&
                c->col + utf8_word_cells(p) - 1 > c->ops->cols) {
                newline(c);
            }
            while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') p++;
            bytes = (size_t)(p - start);
            while (start < p && !c->done) {
                size_t n = utf8_seq_len(start);
                if (start + n > p) n = (size_t)(p - start);
                /* Emoji: one opts-in callback decides, so the markdown leaf
                 * never owns an emoji table (the caller's core answers). */
                if (n >= 3 && (c->ops->put_emoji != NULL ||
                               c->ops->emoji_fallback != NULL)) {
                    char emoji_buf[5] = { 0 };
                    int emoji_cells = 0;

                    if (c->ops->put_emoji != NULL &&
                        c->ops->put_emoji(c->ops->ctx, c->col, c->row, start,
                                          n, &emoji_cells)) {
                        c->col += emoji_cells;
                        start += n;
                        continue;
                    }
                    if (c->ops->emoji_fallback != NULL) {
                        const char *replacement =
                            c->ops->emoji_fallback(c->ops->ctx, start, n,
                                                   emoji_buf);

                        if (replacement != NULL) {
                            emit_utf8(c, replacement, strlen(replacement), st);
                            start += n;
                            continue;
                        }
                    }
                }
                {
                    const char *stand_in =
                        dom_presentation(utf8_codepoint(start, n));

                    if (stand_in != NULL) {
                        emit_utf8(c, stand_in, strlen(stand_in), st);
                        start += n;
                        continue;
                    }
                }
                emit_utf8(c, start, n, st);
                start += n;
            }
            (void)bytes;
        }
    }
}

/* Concatenate a node's text descendants into @p out (spaces between runs). */
static void node_plain_text_append(const dom_node_t *node, char *out, size_t out_size,
                                   size_t *pos, bool *wrote, int depth)
{
    if (node == NULL || depth > 64) {
        return;
    }
    for (const dom_node_t *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->type == DOM_NODE_TEXT) {
            size_t len = strlen(ch->text);

            if (len == 0) {
                continue;
            }
            if (*wrote && *pos + 1 < out_size && out[*pos - 1] != ' ') {
                out[(*pos)++] = ' ';
            }
            if (*pos + len + 1 >= out_size) {
                len = out_size - *pos - 1;
            }
            memcpy(out + *pos, ch->text, len);
            *pos += len;
            out[*pos] = '\0';
            *wrote = true;
        } else if (!is_skip_tag(ch->tag)) {
            /* Same buffer, no per-level 1 KB temporary: a deeply nested page
             * pushed this helper over the shell worker's stack. */
            node_plain_text_append(ch, out, out_size, pos, wrote, depth + 1);
        }
    }
}

static void node_plain_text(const dom_node_t *node, char *out, size_t out_size)
{
    size_t pos = 0;
    bool wrote = false;

    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    node_plain_text_append(node, out, out_size, &pos, &wrote, 0);
}

typedef struct {
    size_t len;
    char last;
    bool wrote;
} text_len_t;

static void node_plain_text_measure(const dom_node_t *node, text_len_t *acc, int depth)
{
    if (node == NULL || depth > 64) {
        return;
    }
    for (const dom_node_t *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->type == DOM_NODE_TEXT) {
            size_t len = strlen(ch->text);

            if (len == 0) {
                continue;
            }
            if (acc->wrote && acc->last != ' ') {
                acc->len++;
            }
            acc->len += len;
            acc->last = ch->text[len - 1];
            acc->wrote = true;
        } else if (!is_skip_tag(ch->tag)) {
            node_plain_text_measure(ch, acc, depth + 1);
        }
    }
}

/** Length of node_plain_text()'s result without materializing it: the centering
 *  branches only need the width, and a 1 KB stack buffer per nested block was
 *  what overflowed the shell worker on a real Wikipedia page. */
static size_t node_plain_text_len(const dom_node_t *node)
{
    text_len_t acc = { 0, '\0', false };

    node_plain_text_measure(node, &acc, 0);
    return acc.len;
}

/** Deepest inline nesting the renderer recurses through.
 *
 * `render_inline_node` frames are ~670 bytes, and real markup nests spans,
 * fonts and anchors deeply (a news discussion page overflowed the command
 * worker's stack here). Past the cap the subtree's text is emitted flat.
 */
#define DOM_RENDER_INLINE_MAX_DEPTH 24

static void render_inline_node(render_ctx_t *c, dom_node_t *node, style_t st,
                               int depth);
static void render_flat_text(render_ctx_t *c, dom_node_t *node, style_t st,
                              int budget);
static void render_children(render_ctx_t *c, dom_node_t *node, style_t st, int indent);

/** Fit a packed image to the live page budget. TUI cells are ~2:1, so the
 *  cell height is img_h/(img_w*2); a tall image is then narrowed to keep that
 *  ratio. Shared by the drawing path and the outcome report. */
static void dom_image_fit(const dom_image_t *img, int avail_w, int avail_h,
                          int max_w, int *out_w, int *out_h)
{
    if (max_w > 0 && avail_w > max_w) avail_w = max_w;
    if (avail_w < 4) avail_w = 4;
    if (avail_h < 2) avail_h = 2;
    *out_w = avail_w;
    *out_h = (avail_w * img->height) / (img->width * 2);
    if (*out_h < 2) *out_h = 2;
    if (*out_h > avail_h) {
        *out_h = avail_h;
        *out_w = (avail_h * img->width * 2) / img->height;
        if (*out_w > avail_w) *out_w = avail_w;
        if (*out_w < 4) *out_w = 4;
    }
}

static bool render_packed_image(render_ctx_t *c, const style_t *st,
                               const dom_image_t *img, int max_cell_w)
{
    int avail_w = c->ops->cols - c->col - 2;
    int avail_h = c->ops->rows - c->row - 2;
    int cell_w;
    int cell_h;
    int x, y;
    int saved_indent = c->indent;

    /* Fit to the LIVE page budget rather than dropping an oversized image:
     * a large photo is scaled down, never silently replaced by alt text. */
    if (avail_h < 2) return false;   /* no rows left at all: honest fallback */
    dom_image_fit(img, avail_w, avail_h, max_cell_w, &cell_w, &cell_h);
    /* A centered image shifts the whole box (border included) so it sits in
     * the page's content column instead of hugging the left edge. The indent
     * makes newline() restart each row at the box's left cell. */
    if (c->ops->tui && st->align == 1) {
        c->indent = (c->ops->cols - (cell_w + 2)) / 2;
        if (c->indent < 0) c->indent = 0;
        c->col = c->indent + 1;
    }
    /* ASCII border/cells: the TUI font's box/block glyphs are full-width,
     * which breaks the 80-column grid; ASCII is monospace. */
    emit_text(c, "+", st);
    for (x = 0; x < cell_w && !c->done; x++) emit_text(c, "-", st);
    emit_text(c, "+", st);
    newline(c);
    for (y = 0; y < cell_h && !c->done; y++) {
        emit_text(c, "|", st);
        for (x = 0; x < cell_w; x++) {
            uint8_t packed = dom_image_cell(img, x, y, cell_w, cell_h);
            style_t cell = *st;
            uint8_t fg = (uint8_t)(packed >> 4);
            /* CSS opacity on an image box composites its sampled DOS colour
             * over the page background via the ONE colour blend helper. */
            if (st->opacity < 255) {
                css_rgba_t over = css_color_from_dos((uint8_t)(c->ops->base_bg & 15));
                css_rgba_t blended = css_color_blend(css_color_from_dos(fg),
                                                     over, st->opacity);

                fg = css_color_to_dos(&blended);
            }
            /* The TUI label renders only the foreground colour and LVGL
             * treats a literal '#' as the recolor delimiter, so the ramp
             * MUST NOT contain '#' (see ansi_image_glyph). */
            {
                char glyph[2];

                glyph[0] = ansi_image_glyph(packed & 0x0F);
                glyph[1] = '\0';
                cell.fg = fg;
                cell.bg = 16;
                emit_text(c, glyph, &cell);
            }
        }
        emit_text(c, "|", st);
        newline(c);
    }
    emit_text(c, "+", st);
    for (x = 0; x < cell_w && !c->done; x++) emit_text(c, "-", st);
    emit_text(c, "+", st);
    newline(c);
    c->indent = saved_indent;
    return true;
}

/** Deepest block nesting at which an image may be fetched INLINE, when the
 * caller has no flat prefetch pass. The browser supplies prefetch_images and
 * warms its decoded cache before layout, so its deep infobox/figure images do
 * not pay a TLS fetch on the worker's 28 KB stack during recursion. */
#define DOM_RENDER_IMAGE_FETCH_DEPTH 8

static void render_image(render_ctx_t *c, dom_node_t *node, const style_t *st)
{
    dom_image_t img;

    memset(&img, 0, sizeof(img));
    if ((c->ops->prefetch_images != NULL ||
         c->depth <= DOM_RENDER_IMAGE_FETCH_DEPTH) &&
        c->ops->load_image != NULL && node->src[0] != '\0' &&
        c->ops->load_image(c->ops->ctx, node->src, &img) &&
        img.pixels != NULL && img.width > 0 && img.height > 0) {
        int fit_w;
        int fit_h;
        bool drawn;

        dom_image_fit(&img, c->ops->cols - c->col - 2,
                      c->ops->rows - c->row - 2, 32, &fit_w, &fit_h);
        drawn = render_packed_image(c, st, &img, 32);
        free(img.pixels);
        if (drawn) {
            dom_report_image(c, "img", node->src,
                             fit_w < img.width || fit_h < img.height
                                 ? DOM_IMAGE_RESIZED : DOM_IMAGE_OK,
                             &img, fit_w, fit_h, node->alt);
            return;
        }
    }
    {
        char alt[300];

        /* A decode failure is visible, not a silent gap: the alt text is the
         * documented fallback and is reported before it is drawn. */
        dom_report_image(c, "img", node->src, DOM_IMAGE_MISSING, &img,
                         0, 0, node->alt);
        snprintf(alt, sizeof(alt), "[%s]", node->alt[0] ? node->alt : "image");
        emit_text(c, alt, st);
    }
}

/** <canvas>: the browser supplies the bitmap (load_canvas); a no-JS surface
 *  (editor preview, print-to-HTML, EPUB) leaves the callback unset and gets
 *  the element's fallback content, exactly as the HTML spec requires. */
static void render_canvas(render_ctx_t *c, dom_node_t *node, const style_t *st)
{
    dom_image_t img;

    memset(&img, 0, sizeof(img));
    if (c->ops->load_canvas != NULL &&
        c->ops->load_canvas(c->ops->ctx, node, &img) &&
        img.pixels != NULL && img.width > 0 && img.height > 0) {
        int fit_w;
        int fit_h;
        bool drawn;

        dom_image_fit(&img, c->ops->cols - c->col - 2,
                      c->ops->rows - c->row - 2, 60, &fit_w, &fit_h);
        drawn = render_packed_image(c, st, &img, 60);
        free(img.pixels);
        if (drawn) {
            dom_report_image(c, "canvas", node->id,
                             fit_w < img.width || fit_h < img.height
                                 ? DOM_IMAGE_RESIZED : DOM_IMAGE_OK,
                             &img, fit_w, fit_h, NULL);
            return;
        }
    }
    dom_report_image(c, "canvas", node->id, DOM_IMAGE_FALLBACK, &img,
                     0, 0, NULL);
    render_children(c, node, *st, c->indent);
}

static void render_inline_node(render_ctx_t *c, dom_node_t *node, style_t st,
                               int depth)
{
    if (depth > DOM_RENDER_INLINE_MAX_DEPTH) {
        /* Too deep to lay out: keep the words, drop the styling. */
        render_flat_text(c, node, st, 64);
        return;
    }
    if (node == NULL || c->done) return;
    if (node->type == DOM_NODE_TEXT) {
        emit_text(c, node->text, &st);
        return;
    }
    if (node->display == DOM_DISPLAY_NONE || is_skip_tag(node->tag)) return;
    st = style_merge(node, &st);
    if (strcmp(node->tag, "br") == 0) {
        newline(c);
        return;
    }
    if (strcmp(node->tag, "img") == 0) {
        render_image(c, node, &st);
        return;
    }
    if (strcmp(node->tag, "input") == 0) {
        char label[300];
        bool toggler = strcmp(node->input_type, "checkbox") == 0 ||
                       strcmp(node->input_type, "radio") == 0;

        if (toggler) {
            snprintf(label, sizeof(label), "[%c]", node->checked ? 'x' : ' ');
        } else if (node->value[0] != '\0') {
            snprintf(label, sizeof(label), "[%s]", node->value);
        } else if (node->placeholder[0] != '\0') {
            snprintf(label, sizeof(label), "[%s]", node->placeholder);
        } else {
            snprintf(label, sizeof(label), "[%s]", node->input_type[0] ? node->input_type : "text");
        }
        if (node->disabled) st.fg = 8;
        emit_text(c, label, &st);
        if (c->ops->put_link != NULL && node->id[0] != '\0') {
            char cmd[600];

            snprintf(cmd, sizeof(cmd), "browser js %s", node->id);
            c->ops->put_link(c->ops->ctx, c->col - (int)strlen(label), c->row,
                             (int)strlen(label), 1, cmd);
        }
        return;
    }
    if (strcmp(node->tag, "select") == 0) {
        dom_node_t *chosen = NULL;
        char label[300];

        for (dom_node_t *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
            if (ch->type == DOM_NODE_ELEMENT && strcmp(ch->tag, "option") == 0) {
                if (ch->checked) { chosen = ch; break; }
                if (chosen == NULL) chosen = ch;
            }
        }
        node_plain_text(chosen != NULL ? chosen : node, label, sizeof(label));
        emit_text(c, "[", &st);
        emit_text(c, label, &st);
        emit_text(c, "]", &st);
        return;
    }
    if (strcmp(node->tag, "progress") == 0 || strcmp(node->tag, "meter") == 0) {
        int percent = atoi(node->value);
        int filled;

        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        filled = (percent * 10) / 100;
        emit_text(c, "[", &st);
        for (int i = 0; i < 10; i++) emit_text(c, i < filled ? "#" : "-", &st);
        emit_text(c, "]", &st);
        return;
    }
    if (strcmp(node->tag, "button") == 0) {
        int start_col = c->col;
        int start_row = c->row;
        emit_text(c, "[ ", &st);
        for (dom_node_t *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
            render_inline_node(c, ch, st, depth + 1);
        }
        emit_text(c, " ]", &st);
        if (c->ops->put_link != NULL && node->id[0] != '\0' &&
            c->row == start_row && c->col > start_col) {
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "browser js %s", node->id);
            c->ops->put_link(c->ops->ctx, start_col, start_row,
                             c->col - start_col, 1, cmd);
        }
        return;
    }
    if (strcmp(node->tag, "a") == 0 && node->href[0] != '\0') {
        int start_col = c->col;
        int start_row = c->row;
        st.fg = 11;
        st.underline = true;
        for (dom_node_t *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
            render_inline_node(c, ch, st, depth + 1);
        }
        if (c->ops->put_link != NULL && c->row == start_row && c->col > start_col) {
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "browser %s", node->href);
            c->ops->put_link(c->ops->ctx, start_col, start_row,
                             c->col - start_col, 1, cmd);
        }
        if (c->ops->ansi_links) {
            char suffix[540];
            snprintf(suffix, sizeof(suffix), " (%s)", node->href);
            emit_text(c, suffix, &st);
        }
        return;
    }
    if (strcmp(node->tag, "strong") == 0 || strcmp(node->tag, "b") == 0) {
        st.bold = true;
    } else if (strcmp(node->tag, "em") == 0 || strcmp(node->tag, "i") == 0) {
        st.italic = true;
    } else if (strcmp(node->tag, "u") == 0) {
        st.underline = true;
    } else if (strcmp(node->tag, "code") == 0) {
        st.fg = 14;
        st.bg = 8;
    }
    for (dom_node_t *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        render_inline_node(c, ch, st, depth + 1);
    }
}

static void render_block(render_ctx_t *c, dom_node_t *node, style_t st, int indent);
static void render_list(render_ctx_t *c, dom_node_t *node, style_t st,
                        int indent, bool ordered);

static void render_flat_text(render_ctx_t *c, dom_node_t *node, style_t st, int budget)
{
    if (node == NULL || c->done || budget <= 0) {
        return;
    }
    if (node->type == DOM_NODE_TEXT) {
        emit_text(c, node->text, &st);
        return;
    }
    if (node->display == DOM_DISPLAY_NONE || is_skip_tag(node->tag)) {
        return;
    }
    for (dom_node_t *ch = node->first_child; ch != NULL && !c->done;
         ch = ch->next_sibling) {
        render_flat_text(c, ch, st, budget - 1);
    }
}

static void render_children(render_ctx_t *c, dom_node_t *node, style_t st, int indent)
{
    for (dom_node_t *ch = node->first_child; ch != NULL && !c->done; ch = ch->next_sibling) {
        if (ch->type == DOM_NODE_TEXT) {
            emit_text(c, ch->text, &st);
            continue;
        }
        if (ch->display == DOM_DISPLAY_NONE || is_skip_tag(ch->tag)) continue;
        if (is_block_tag(ch->tag)) {
            if (c->depth >= DOM_RENDER_MAX_DEPTH) {
                /* Documented degradation: beyond the cap the subtree's text is
                 * emitted inline instead of laid out as nested blocks. No
                 * crash, no unbounded stack, no content silently dropped. */
                render_flat_text(c, ch, st, 64);
            } else {
                c->depth++;
                render_block(c, ch, st, indent);
                c->depth--;
            }
        } else {
            render_inline_node(c, ch, st, 0);   /* inline child of a block */
        }
    }
}

static void render_table(render_ctx_t *c, dom_node_t *node, style_t st, int indent)
{
    enum { TABLE_MAX_ROWS = 8, TABLE_MAX_COLS = 5 };
    char cells[TABLE_MAX_ROWS][TABLE_MAX_COLS][64];
    style_t cell_styles[TABLE_MAX_ROWS][TABLE_MAX_COLS];
    bool header[TABLE_MAX_ROWS];
    bool covered[TABLE_MAX_ROWS][TABLE_MAX_COLS];
    int spans[TABLE_MAX_ROWS][TABLE_MAX_COLS];
    uint8_t aligns[TABLE_MAX_ROWS][TABLE_MAX_COLS];
    int widths[TABLE_MAX_COLS];
    int rows = 0;
    int cols = 0;
    dom_node_t *row;

    memset(cells, 0, sizeof(cells));
    memset(cell_styles, 0, sizeof(cell_styles));
    memset(header, 0, sizeof(header));
    memset(covered, 0, sizeof(covered));
    memset(spans, 0, sizeof(spans));
    memset(aligns, 0, sizeof(aligns));
    memset(widths, 0, sizeof(widths));

    for (row = node->first_child; row != NULL && rows < TABLE_MAX_ROWS; row = row->next_sibling) {
        dom_node_t *tr = row;
        int ci = 0;
        if (tr->type != DOM_NODE_ELEMENT) continue;
        if (strcmp(tr->tag, "thead") == 0 || strcmp(tr->tag, "tbody") == 0 ||
            strcmp(tr->tag, "tfoot") == 0) {
            tr = tr->first_child;
        }
        if (tr == NULL || strcmp(tr->tag, "tr") != 0) continue;
        header[rows] = false;
        for (dom_node_t *td = tr->first_child; td != NULL && ci < TABLE_MAX_COLS; td = td->next_sibling) {
            if (td->type != DOM_NODE_ELEMENT) continue;
            if (strcmp(td->tag, "td") != 0 && strcmp(td->tag, "th") != 0) continue;
            int span = td->col_span > 1 ? td->col_span : 1;

            if (span > TABLE_MAX_COLS - ci) span = TABLE_MAX_COLS - ci;
            node_plain_text(td, cells[rows][ci], sizeof(cells[rows][ci]));
            cell_styles[rows][ci] = style_merge(td, &st);
            aligns[rows][ci] = cell_styles[rows][ci].align;
            spans[rows][ci] = span;
            for (int s = 1; s < span && ci + s < TABLE_MAX_COLS; s++) {
                covered[rows][ci + s] = true;
            }
            if (strcmp(td->tag, "th") == 0) header[rows] = true;
            if ((int)strlen(cells[rows][ci]) > widths[ci]) {
                widths[ci] = (int)strlen(cells[rows][ci]);
            }
            ci += span;
        }
        if (ci > cols) cols = ci;
        rows++;
    }
    if (rows == 0 || cols == 0) return;

    /* Total grid width of the table body (cells + 2-space gutters). */
    int total = 0;
    for (int x = 0; x < cols; x++) {
        total += widths[x];
        if (x + 1 < cols) total += 2;
    }
    /* Responsive mapping: a wide table is squeezed into the live page width,
     * never truncated mid-cell. This is the same one renderer on every
     * surface, so narrow TUI panels and 80-column dumps agree. */
    {
        int budget = c->ops->cols - 2;
        int inner;
        long sum = 0;

        if (budget < cols * 4) budget = cols * 4;
        inner = budget - 2 * (cols - 1);
        if (inner < cols * 4) inner = cols * 4;
        for (int x = 0; x < cols; x++) sum += widths[x];
        if (sum > 0 && total > budget) {
            for (int x = 0; x < cols; x++) {
                int scaled = (int)((long)widths[x] * inner / sum);

                widths[x] = scaled < 4 ? 4 : scaled;
            }
            total = 0;
            for (int x = 0; x < cols; x++) {
                total += widths[x];
                if (x + 1 < cols) total += 2;
            }
        }
    }
    /* Center the table when the element's CSS text-align is center. Every row
     * (and the header rule) must restart at this column: newline() resets c->col
     * to the indent, so setting it once would center only the first row. */
    int start = 1;
    if (st.align == 1) {
        start = 1 + (c->ops->cols - total) / 2;
        if (start < 1) start = 1;
    }

    for (int r = 0; r < rows && !c->done; r++) {
        c->col = start;
        for (int x = 0; x < cols; x++) {
            char padded[80];
            int len;
            int pad;
            int left;
            int right;

            if (covered[r][x]) continue;   /* consumed by a preceding colspan */
            len = (int)strlen(cells[r][x]);
            if (len > (int)sizeof(padded) - 1) len = (int)sizeof(padded) - 1;
            {
                int span = spans[r][x] > 0 ? spans[r][x] : 1;
                int width = widths[x];

                for (int s = 1; s < span && x + s < cols; s++) {
                    width += 2 + widths[x + s];
                }
                pad = width - len;
            }
            if (pad < 0) pad = 0;
            if (pad > (int)sizeof(padded) - 1 - len) {
                pad = (int)sizeof(padded) - 1 - len;
            }
            left = aligns[r][x] == 2 ? pad : (aligns[r][x] == 1 ? pad / 2 : 0);
            right = pad - left;
            memset(padded, ' ', (size_t)left);
            memcpy(padded + left, cells[r][x], (size_t)len);
            memset(padded + left + len, ' ', (size_t)right);
            padded[left + len + right] = '\0';
            emit_text(c, padded, &cell_styles[r][x]);
            if (x + 1 < cols && !covered[r][x + 1]) {
                emit_text(c, "  ", &st);
            }
        }
        newline(c);
        if (header[r]) {
            c->col = start;
            for (int k = 0; k < total && !c->done; k++) {
                emit_text(c, "-", &st);
            }
            newline(c);
        }
    }
}

static void render_pre(render_ctx_t *c, dom_node_t *node, style_t st, int indent)
{
    style_t code = st;
    const char *p;
    int saved = c->indent;

    code.fg = 15;
    code.bg = 8;
    c->indent = indent + 2;
    c->col = c->indent + 1;
    for (p = node->first_child && node->first_child->type == DOM_NODE_TEXT
                 ? node->first_child->text : ""; *p != '\0' && !c->done; ) {
        if (*p == '\n') {
            newline(c);
            p++;
            continue;
        }
        {
            size_t n = utf8_seq_len(p);
            if (c->col <= c->ops->cols) emit_utf8(c, p, n, &code);
            p += n;
        }
    }
    c->indent = saved;
    newline(c);
}

static void render_list(render_ctx_t *c, dom_node_t *node, style_t st,
                        int indent, bool ordered)
{
    int index = 1;
    int saved = c->indent;

    for (dom_node_t *li = node->first_child; li != NULL && !c->done; li = li->next_sibling) {
        char marker[16];
        style_t ls;
        if (li->type != DOM_NODE_ELEMENT || strcmp(li->tag, "li") != 0) continue;
        newline(c);
        c->indent = indent;
        c->col = indent + 1;
        if (ordered) snprintf(marker, sizeof(marker), "%d. ", index++);
        else snprintf(marker, sizeof(marker), "- ");
        emit_text(c, marker, &st);
        ls = style_merge(li, &st);
        render_children(c, li, ls, indent + 2);
    }
    c->indent = saved;
}

/** Sample the shared linear-gradient at a cell position. The angle is mapped
 *  onto the cell grid (horizontal for 90/270, vertical for 0/180); any other
 *  angle uses the horizontal axis, which is the closest a cell grid can show. */
static css_rgba_t box_gradient_at(const css_gradient_t *gradient, int col,
                                  int row, int cols, int rows)
{
    uint8_t t;

    if (gradient->angle == 270 || gradient->angle == 0) {
        int total = (gradient->angle == 270) ? cols : rows;
        int axis = (gradient->angle == 270) ? col : row;
        int pos = (gradient->angle == 270) ? (cols - axis) : (rows - axis);
        t = (uint8_t)((uint32_t)pos * 255u / (uint32_t)(total > 1 ? total - 1 : 1));
    } else if (gradient->angle == 180) {
        t = (uint8_t)((uint32_t)row * 255u / (uint32_t)(rows > 1 ? rows - 1 : 1));
    } else {
        t = (uint8_t)((uint32_t)col * 255u / (uint32_t)(cols > 1 ? cols - 1 : 1));
    }
    return css_gradient_sample(gradient, t);
}

/** Paint one CSS box through the ONE fill_bg sink: optional box-shadow (a
 *  composited offset rectangle), a gradient sampled into <=16 strips, a solid
 *  background, and four cut corners for the border-radius subset. */
static void render_box_paint(render_ctx_t *c, const dom_node_t *node,
                             const style_t *st, int row0, int row1)
{
    int height = row1 - row0 + 1;

    if (c->ops->fill_bg == NULL || height < 1 || st->opacity == 0) return;
    if (st->shadow_set && st->shadow_alpha > 0 && st->shadow_dos < 16) {
        uint8_t alpha = (uint8_t)(((uint32_t)st->shadow_alpha * st->opacity) / 255u);

        if (alpha > 0) {
            css_rgba_t composited = css_color_composite(
                css_color_from_dos(st->shadow_dos),
                css_color_from_dos((uint8_t)(c->ops->base_bg & 15)));

            c->ops->fill_bg(c->ops->ctx, 1 + st->shadow_x, row0 + st->shadow_y,
                            c->ops->cols, height,
                            css_color_to_dos(&composited), alpha);
        }
    }
    if (st->gradient_set) {
        int strips = c->ops->cols < 16 ? c->ops->cols : 16;
        int step = strips > 0 ? (c->ops->cols + strips - 1) / strips : 1;

        if (step < 1) step = 1;
        for (int col = 1; col <= c->ops->cols; col += step) {
            css_rgba_t sample = box_gradient_at(&st->gradient, col - 1, 0,
                                                 c->ops->cols, height);
            uint8_t alpha = (uint8_t)(((uint32_t)sample.a * st->opacity) / 255u);
            uint8_t dos = css_color_to_dos(&sample);

            if (alpha > 0 && dos < 16) {
                int w = col + step - 1 <= c->ops->cols ? step : c->ops->cols - col + 1;

                c->ops->fill_bg(c->ops->ctx, col, row0, w, height, dos, alpha);
            }
        }
    } else if (node != NULL && node->bg_set && st->bg != 16 && st->bg_alpha > 0) {
        uint8_t alpha = (uint8_t)(((uint32_t)st->bg_alpha * st->opacity) / 255u);

        if (alpha > 0) {
            c->ops->fill_bg(c->ops->ctx, 1, row0, c->ops->cols, height,
                            st->bg, alpha);
        }
    }
    if (st->border_radius > 0 && c->ops->cols > 2) {
        uint8_t cut = (uint8_t)(c->ops->base_bg & 15);
        int r = st->border_radius;

        c->ops->fill_bg(c->ops->ctx, 1, row0, r, 1, cut, 255);
        c->ops->fill_bg(c->ops->ctx, c->ops->cols - r + 1, row0, r, 1, cut, 255);
        c->ops->fill_bg(c->ops->ctx, 1, row1, r, 1, cut, 255);
        c->ops->fill_bg(c->ops->ctx, c->ops->cols - r + 1, row1, r, 1, cut, 255);
    }
}

/** CSS grid/flex subset: lay out element children into at most
 *  @p columns tracks. The track count is responsive - a narrow page shows
 *  fewer columns - and each cell keeps its own computed style. */
static void render_grid(render_ctx_t *c, dom_node_t *node, style_t st,
                        int indent, int columns, bool flex)
{
    int used = 0;
    int index = 0;
    int width;

    if (columns < 1) columns = 1;
    if (flex) columns = 1;
    width = c->ops->cols / columns;
    if (width < 8) width = 8;
    newline(c);
    for (dom_node_t *ch = node->first_child; ch != NULL && !c->done;
         ch = ch->next_sibling) {
        if (ch->type != DOM_NODE_ELEMENT || ch->display == DOM_DISPLAY_NONE ||
            is_skip_tag(ch->tag)) {
            continue;
        }
        if (!flex) {
            if (index > 0 && index % columns == 0) newline(c);
            if (index % columns > 0) {
                c->col = 1 + (index % columns) * width;
            }
        } else if (index > 0) {
            emit_text(c, " ", &st);
        }
        {
            style_t child = style_merge(ch, &st);

            render_children(c, ch, child, indent);
        }
        used++;
        index++;
    }
    (void)used;
    newline(c);
}

static void render_block(render_ctx_t *c, dom_node_t *node, style_t st, int indent)
{
    if (node == NULL || c->done) return;
    if (node->display == DOM_DISPLAY_NONE || is_skip_tag(node->tag)) return;
    st = style_merge(node, &st);

    if (node->tag[0] == 'h' && node->tag[1] >= '1' && node->tag[1] <= '6' &&
        node->tag[2] == '\0') {
        static const uint8_t head_color[6] = {15, 14, 11, 13, 12, 10};
        int level = node->tag[1] - '1';
        int len;
        int start = 1;

        st.bold = true;
        if (st.fg == 16) st.fg = head_color[level];
        if (st.font_scale < 2) st.font_scale = 2;
        len = (int)node_plain_text_len(node);
        newline(c);
        if (st.align == 1) {
            start = 1 + (c->ops->cols - len) / 2;
            if (start < 1) start = 1;
            c->col = start;
        }
        render_children(c, node, st, indent);
        newline(c);
        if (level == 0 && len > 0) {
            /* An ASCII rule under the title gives the h1 a visual hierarchy
             * the fixed-size TUI font cannot express by point size. */
            c->col = start;
            for (int i = 0; i < len && !c->done; i++) {
                emit_text(c, "=", &st);
            }
            newline(c);
        }
        return;
    }
    if (strcmp(node->tag, "p") == 0 || strcmp(node->tag, "div") == 0 ||
        strcmp(node->tag, "section") == 0 || strcmp(node->tag, "article") == 0 ||
        strcmp(node->tag, "header") == 0 || strcmp(node->tag, "footer") == 0 ||
        strcmp(node->tag, "main") == 0 || strcmp(node->tag, "nav") == 0 ||
        strcmp(node->tag, "aside") == 0 || strcmp(node->tag, "form") == 0 ||
        strcmp(node->tag, "details") == 0 || strcmp(node->tag, "summary") == 0 ||
        strcmp(node->tag, "dialog") == 0 || strcmp(node->tag, "caption") == 0 ||
        strcmp(node->tag, "legend") == 0 || strcmp(node->tag, "figure") == 0 ||
        strcmp(node->tag, "figcaption") == 0) {
        /* Responsive grid/flex mapping: the computed track count is clamped
         * to the live page width so a 2-column layout still fits 80 cells. */
        if (node->display == DOM_DISPLAY_GRID || node->display == DOM_DISPLAY_FLEX) {
            int columns = node->grid_columns > 0 ? node->grid_columns : 2;
            int max_columns = c->ops->cols / 8;

            if (max_columns < 1) max_columns = 1;
            if (columns > max_columns) columns = max_columns;
            render_grid(c, node, st, indent, columns,
                        node->display == DOM_DISPLAY_FLEX);
            return;
        }
        newline(c);
        for (int pad = 0; pad < node->padding_top; pad++) {
            newline(c);
        }
        {
            int bg_row = c->row;

            if (st.align == 1) {
                int len = (int)node_plain_text_len(node);
                int start = 1 + (c->ops->cols - len) / 2;

                if (start < 1) start = 1;
                c->col = start;
            }
            render_children(c, node, st, indent);
            for (int pad = 0; pad < node->padding_bottom; pad++) {
                newline(c);
            }
            /* One box-paint path for solid colours, gradients, box-shadow
             * opacity composition and the border-radius corner subset. */
            render_box_paint(c, node, &st, bg_row, c->row);
        }
        newline(c);
        return;
    }
    if (strcmp(node->tag, "ul") == 0 || strcmp(node->tag, "ol") == 0) {
        newline(c);
        render_list(c, node, st, indent + 2, strcmp(node->tag, "ol") == 0);
        newline(c);
        return;
    }
    if (strcmp(node->tag, "table") == 0) {
        newline(c);
        render_table(c, node, st, indent);
        newline(c);
        return;
    }
    if (strcmp(node->tag, "pre") == 0) {
        newline(c);
        render_pre(c, node, st, indent);
        return;
    }
    if (strcmp(node->tag, "blockquote") == 0) {
        style_t quote = st;
        int saved = c->indent;
        newline(c);
        c->indent = indent + 2;
        c->col = c->indent + 1;
        quote.fg = 8;
        emit_text(c, "| ", &quote);
        render_children(c, node, quote, indent + 2);
        newline(c);
        c->indent = saved;
        return;
    }
    if (strcmp(node->tag, "hr") == 0) {
        newline(c);
        for (int x = 1; x < c->ops->cols && !c->done; x++) {
            emit_text(c, "-", &st);
        }
        newline(c);
        return;
    }
    if (strcmp(node->tag, "br") == 0) {
        newline(c);
        return;
    }
    if (strcmp(node->tag, "canvas") == 0) {
        render_canvas(c, node, &st);
        newline(c);
        return;
    }
    if (strcmp(node->tag, "img") == 0) {
        render_image(c, node, &st);
        newline(c);
        return;
    }
    if (strcmp(node->tag, "li") == 0 || strcmp(node->tag, "tr") == 0 ||
        strcmp(node->tag, "td") == 0 || strcmp(node->tag, "th") == 0) {
        render_children(c, node, st, indent);
        return;
    }
    /* Unknown block: render its children as a block. */
    newline(c);
    render_children(c, node, st, indent);
    newline(c);
}

/** Find the first <main> element (reader focus). Mobile Wikipedia wraps the
 *  article in <main id="content"> and puts its navigation chrome before it;
 *  on an 80x25 page that chrome otherwise fills the whole viewport and the
 *  article is never reached. A page without <main> renders its body. */
static dom_node_t *dom_find_main(dom_node_t *node, int depth)
{
    if (node == NULL || depth > 64) {
        return NULL;
    }
    if (node->type == DOM_NODE_ELEMENT && strcmp(node->tag, "main") == 0) {
        return node;
    }
    for (dom_node_t *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        dom_node_t *found = dom_find_main(ch, depth + 1);

        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

void dom_render_tree(dom_tree_t *tree, const dom_render_ops_t *ops)
{
    render_ctx_t ctx;
    style_t st;
    dom_node_t *root;

    if (tree == NULL || ops == NULL || tree->body == NULL) return;
    memset(&ctx, 0, sizeof(ctx));
    memset(&st, 0, sizeof(st));
    st.fg = 16;
    st.bg = 16;
    st.fg_alpha = 255;
    st.bg_alpha = 255;
    st.opacity = 255;
    st.font_scale = 1;
    ctx.ops = ops;
    ctx.row = 1;
    ctx.col = 1;
    ctx.indent = 0;
    ctx.depth = 0;
    ctx.done = false;
    root = tree->body;
    {
        dom_node_t *main_node = dom_find_main(tree->body, 0);

        if (main_node != NULL && main_node->first_child != NULL) {
            root = main_node;
        }
    }
    /* Warm image caches FLAT, before the recursive layout walk. Wikipedia's
     * infobox and figure images sit 10+ levels deep in tables; the old renderer
     * either skipped them (depth cap) or ran a TLS fetch on the command worker's
     * 28 KB stack right there. The browser's pre-pass resolves and decodes them
     * in a bounded loop, so render_image below only takes cache hits. */
    if (ops->prefetch_images != NULL) {
        ops->prefetch_images(ops->ctx, tree);
    }
    /* Paint the page background from the body's CSS background-color. The
     * cells the layout writes later keep their own colors. */
    if (ops->tui && ops->put_cell != NULL && tree->body->bg != 16) {
        for (int r = 1; r <= ops->rows; r++) {
            for (int c = 1; c <= ops->cols; c++) {
                ops->put_cell(ops->ctx, c, r, " ", 16, tree->body->bg, 0);
            }
        }
    }
    /* CSS background-image on the page (body/main): tile the image's colours
     * behind the content. Text cells keep their own background, so the image
     * reads as the page backdrop around the text rather than fighting it. */
    if (ops->fill_bg != NULL && ops->load_background != NULL) {
        const char *bg_src = tree->body->bg_src[0] != '\0'
                                 ? tree->body->bg_src
                                 : root->bg_src;

        if (bg_src[0] != '\0') {
            dom_image_t bg;

            memset(&bg, 0, sizeof(bg));
            if (ops->load_background(ops->ctx, bg_src, &bg) &&
                bg.pixels != NULL && bg.width > 0 && bg.height > 0) {
                /* Tile the image as coalesced rectangles. One rectangle per
                 * cell exhausted the backdrop pool after a few rows (and was
                 * needlessly slow), so equal runs are merged horizontally AND
                 * vertically: a solid CSS background becomes ONE rectangle
                 * covering the whole page, a striped one a rectangle per band.
                 * Detailed photographic backgrounds stop at the documented
                 * pool cap instead of covering the page with thousands of
                 * objects. */
                uint8_t page_alpha = (uint8_t)(((uint32_t)tree->body->opacity *
                    (tree->body->bg_alpha > 0 ? tree->body->bg_alpha : 255)) / 255u);
                size_t slots = (size_t)ops->cols + 1u;
                bg_run_t *runs =
                    p4heap_alloc_psram(slots * sizeof(bg_run_t) * 2u);
                bg_run_t *cur = runs;
                bg_run_t *prev = (runs != NULL) ? runs + slots : NULL;
                int prev_n = 0;

                /* PSRAM, not the stack: a render frame lives on the caller's
                 * task and two 80-run tables overflowed the unit-test task. */
                for (int r = 0; runs != NULL && r < ops->rows; r++) {
                    int cur_n = 0;

                    for (int c = 0; c < ops->cols; ) {
                        uint8_t packed = dom_image_cell_tiled(&bg, c, r);
                        /* A backdrop rectangle must keep the decoded DOS hue.
                         * The low nibble is the inline glyph ramp's luminance;
                         * re-quantising a ramp-attenuated blend (as RC2 did)
                         * turned a solid #ff5555 tile into DOS grey 8, because
                         * nearest-CGA prefers grey to a dark red. The ramp
                         * shades glyphs, not the page backdrop: only explicit
                         * CSS opacity attenuates the fill. */
                        uint8_t color = (uint8_t)(packed >> 4);
                        int start = c;

                        while (c < ops->cols &&
                               (uint8_t)(dom_image_cell_tiled(&bg, c, r) >> 4) == color) {
                            c++;
                        }
                        cur[cur_n].start = start;
                        cur[cur_n].len = c - start;
                        cur[cur_n].y0 = r;
                        cur[cur_n].color = color;
                        cur[cur_n].alpha = color != 16 ? page_alpha : 0;
                        cur_n++;
                    }
                    /* Close the runs that did not continue into this row. */
                    for (int i = 0; i < prev_n; i++) {
                        bool same = i < cur_n &&
                                    cur[i].start == prev[i].start &&
                                    cur[i].len == prev[i].len &&
                                    cur[i].color == prev[i].color &&
                                    cur[i].alpha == prev[i].alpha;

                        if (same) {
                            cur[i].y0 = prev[i].y0;
                            continue;
                        }
                        if (prev[i].alpha > 0) {
                            ops->fill_bg(ops->ctx, prev[i].start + 1,
                                         prev[i].y0 + 1, prev[i].len,
                                         r - prev[i].y0, prev[i].color,
                                         prev[i].alpha);
                        }
                    }
                    memcpy(prev, cur, (size_t)cur_n * sizeof(cur[0]));
                    prev_n = cur_n;
                }
                for (int i = 0; i < prev_n; i++) {
                    if (prev[i].alpha > 0) {
                        ops->fill_bg(ops->ctx, prev[i].start + 1,
                                     prev[i].y0 + 1, prev[i].len,
                                     ops->rows - prev[i].y0, prev[i].color,
                                     prev[i].alpha);
                    }
                }
                dom_report_image(&ctx, "background", bg_src,
                                 runs == NULL ? DOM_IMAGE_MISSING
                                 : DOM_IMAGE_OK,
                                 &bg, bg.width, bg.height, NULL);
                p4heap_free(runs);
                free(bg.pixels);
            } else {
                dom_report_image(&ctx, "background", bg_src, DOM_IMAGE_MISSING,
                                 &bg, 0, 0, NULL);
            }
        }
    }
    render_children(&ctx, root, st, 0);
    /* Paint the body background after the content so blank cells (including
     * the gaps inside paragraphs) carry the colour, not only the margins.
     * fill_bg never overwrites a glyph, so the text stays intact. */
    if (ops->fill_bg != NULL && tree->body->bg != 16 &&
        tree->body->bg_alpha > 0 && tree->body->opacity > 0) {
        uint8_t alpha = tree->body->bg_alpha;

        ops->fill_bg(ops->ctx, 1, 1, ops->cols, ops->rows, tree->body->bg,
                     (uint8_t)(((uint32_t)alpha * tree->body->opacity) / 255u));
    }
}

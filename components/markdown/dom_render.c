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
} style_t;

/** One coalesced background run: a rectangle of one colour. */
typedef struct {
    int start;
    int len;
    int y0;
    uint8_t color;
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
            if (c->col > 1 && c->col + (int)strcspn(p, " \t\n") > c->ops->cols) {
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

static bool render_packed_image(render_ctx_t *c, const style_t *st,
                               const dom_image_t *img, int max_cell_w)
{
    int cell_w = c->ops->cols - c->col - 2;
    int cell_h;
    int x, y;
    int saved_indent = c->indent;

    if (max_cell_w > 0 && cell_w > max_cell_w) {
        cell_w = max_cell_w;
    }
    if (cell_w < 4) cell_w = 4;
    cell_h = (cell_w * img->height) / (img->width * 2);
    if (cell_h < 2) cell_h = 2;
    if (cell_h > 20) cell_h = 20;
    if (c->row + cell_h + 2 > c->ops->rows) {
        return false;
    }
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
            int sx = (x * img->width) / cell_w;
            int sy = (y * img->height) / cell_h;
            uint8_t packed = img->pixels[sy * img->width + sx];
            style_t cell = *st;
            /* The TUI label renders only the foreground colour and LVGL
             * treats a literal '#' as the recolor delimiter, so the ramp
             * MUST NOT contain '#' (see ansi_image_glyph). */
            char glyph[2];

            glyph[0] = ansi_image_glyph(packed & 0x0F);
            glyph[1] = '\0';
            cell.fg = (uint8_t)(packed >> 4);
            cell.bg = 16;
            emit_text(c, glyph, &cell);
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

/** Deepest block nesting at which an image is still fetched.
 *
 * The fetch runs the network stack (TLS, several KB) inside the render
 * recursion; a deeply nested table page (a news discussion) used to overflow
 * the command worker's verified 28 KB stack right here. Spacer images live
 * deep inside tables, so beyond this depth the alt text is shown instead -
 * article figures sit near the top and still load. */
#define DOM_RENDER_IMAGE_FETCH_DEPTH 8

static void render_image(render_ctx_t *c, dom_node_t *node, const style_t *st)
{
    dom_image_t img;

    memset(&img, 0, sizeof(img));
    if (c->depth <= DOM_RENDER_IMAGE_FETCH_DEPTH &&
        c->ops->load_image != NULL && node->src[0] != '\0' &&
        c->ops->load_image(c->ops->ctx, node->src, &img) &&
        img.pixels != NULL && img.width > 0 && img.height > 0) {
        bool drawn = render_packed_image(c, st, &img, 32);

        free(img.pixels);
        if (drawn) {
            return;
        }
    }
    {
        char alt[300];

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
        bool drawn = render_packed_image(c, st, &img, 60);

        free(img.pixels);
        if (drawn) {
            return;
        }
    }
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
    int widths[TABLE_MAX_COLS];
    int rows = 0;
    int cols = 0;
    dom_node_t *row;

    memset(cells, 0, sizeof(cells));
    memset(cell_styles, 0, sizeof(cell_styles));
    memset(header, 0, sizeof(header));
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
            node_plain_text(td, cells[rows][ci], sizeof(cells[rows][ci]));
            cell_styles[rows][ci] = style_merge(td, &st);
            if (strcmp(td->tag, "th") == 0) header[rows] = true;
            if ((int)strlen(cells[rows][ci]) > widths[ci]) {
                widths[ci] = (int)strlen(cells[rows][ci]);
            }
            ci++;
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
            int len = (int)strlen(cells[r][x]);
            int pad = widths[x] - len;
            if (pad < 0) pad = 0;
            if (pad > (int)sizeof(padded) - len - 1) pad = (int)sizeof(padded) - len - 1;
            memcpy(padded, cells[r][x], (size_t)len);
            memset(padded + len, ' ', (size_t)pad);
            padded[len + pad] = '\0';
            emit_text(c, padded, &cell_styles[r][x]);
            if (x + 1 < cols) {
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
        strcmp(node->tag, "aside") == 0 || strcmp(node->tag, "form") == 0) {
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
            if (node->bg_set && st.bg != 16 && st.bg_alpha > 0 &&
                c->ops->fill_bg != NULL) {
                /* CSS block background: the whole box width behind the text,
                 * not just the glyph cells. fill_bg changes only the colour,
                 * so text already drawn keeps its glyph and foreground.
                 * `transparent` (alpha 0) draws no rectangle at all, so the
                 * page/surface behind shows through. */
                c->ops->fill_bg(c->ops->ctx, 1, bg_row, c->ops->cols,
                                c->row - bg_row + 1, st.bg,
                                (uint8_t)(((uint32_t)st.bg_alpha * st.opacity) / 255u));
            }
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
                        uint8_t packed = bg.pixels[(r % bg.height) * bg.width +
                                                   (c % bg.width)];
                        uint8_t color = (uint8_t)(packed >> 4);
                        int start = c;

                        while (c < ops->cols) {
                            packed = bg.pixels[(r % bg.height) * bg.width +
                                               (c % bg.width)];
                            if ((uint8_t)(packed >> 4) != color) {
                                break;
                            }
                            c++;
                        }
                        cur[cur_n].start = start;
                        cur[cur_n].len = c - start;
                        cur[cur_n].y0 = r;
                        cur[cur_n].color = color;
                        cur_n++;
                    }
                    /* Close the runs that did not continue into this row. */
                    for (int i = 0; i < prev_n; i++) {
                        bool same = i < cur_n &&
                                    cur[i].start == prev[i].start &&
                                    cur[i].len == prev[i].len &&
                                    cur[i].color == prev[i].color;

                        if (same) {
                            cur[i].y0 = prev[i].y0;
                            continue;
                        }
                        if (prev[i].color != 16) {
                            ops->fill_bg(ops->ctx, prev[i].start + 1,
                                         prev[i].y0 + 1, prev[i].len,
                                         r - prev[i].y0, prev[i].color, 255);
                        }
                    }
                    memcpy(prev, cur, (size_t)cur_n * sizeof(cur[0]));
                    prev_n = cur_n;
                }
                for (int i = 0; i < prev_n; i++) {
                    if (prev[i].color != 16) {
                        ops->fill_bg(ops->ctx, prev[i].start + 1,
                                     prev[i].y0 + 1, prev[i].len,
                                     ops->rows - prev[i].y0, prev[i].color, 255);
                    }
                }
                p4heap_free(runs);
                free(bg.pixels);
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

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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t fg;
    uint8_t bg;
    bool bold;
    bool italic;
    bool underline;
    uint8_t align;
    uint8_t font_scale;
} style_t;

typedef struct {
    const dom_render_ops_t *ops;
    int row;
    int col;
    int indent;
    bool done;
} render_ctx_t;

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
        "address", "article", "aside", "blockquote", "body", "div", "dl",
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
    if (node->fg != 16) s.fg = node->fg;
    if (node->bg != 16) s.bg = node->bg;
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

    if (c->done) return;
    if (len == 0 || len >= sizeof(tmp)) return;
    memcpy(tmp, seq, len);
    tmp[len] = '\0';
    attr = (uint8_t)((st->bold ? 1u : 0u) | (st->italic ? 4u : 0u) |
                     (st->underline ? 8u : 0u));
    if (c->ops->tui && c->ops->put_cell != NULL) {
        c->ops->put_cell(c->ops->ctx, c->col, c->row, tmp,
                         st->fg, st->bg, attr);
        c->col++;
    } else if (c->ops->put_ansi != NULL) {
        char buf[64];
        size_t n = 0;
        ansi_sgr(buf, sizeof(buf), st);
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
                emit_utf8(c, start, n, st);
                start += n;
            }
            (void)bytes;
        }
    }
}

/* Concatenate a node's text descendants into @p out (spaces between runs). */
static void node_plain_text(const dom_node_t *node, char *out, size_t out_size)
{
    size_t pos = 0;

    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (node == NULL) return;
    for (const dom_node_t *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->type == DOM_NODE_TEXT) {
            size_t len = strlen(ch->text);
            if (pos + len + 1 >= out_size) len = out_size - pos - 1;
            memcpy(out + pos, ch->text, len);
            pos += len;
            out[pos] = '\0';
        } else if (!is_skip_tag(ch->tag)) {
            char sub[1024];
            node_plain_text(ch, sub, sizeof(sub));
            if (sub[0] != '\0') {
                if (pos > 0 && out[pos - 1] != ' ' && pos + 1 < out_size) {
                    out[pos++] = ' ';
                }
                {
                    size_t len = strlen(sub);
                    if (pos + len + 1 >= out_size) len = out_size - pos - 1;
                    memcpy(out + pos, sub, len);
                    pos += len;
                    out[pos] = '\0';
                }
            }
        }
    }
}

static void render_inline_node(render_ctx_t *c, dom_node_t *node, style_t st);

static void render_image(render_ctx_t *c, dom_node_t *node, const style_t *st)
{
    dom_image_t img;

    memset(&img, 0, sizeof(img));
    if (c->ops->load_image != NULL && node->src[0] != '\0' &&
        c->ops->load_image(c->ops->ctx, node->src, &img) &&
        img.pixels != NULL && img.width > 0 && img.height > 0) {
        int cell_w = c->ops->cols - c->col - 2;
        int cell_h;
        int x, y;

        if (cell_w > 32) cell_w = 32;
        if (cell_w < 4) cell_w = 4;
        cell_h = (cell_w * img.height) / (img.width * 2);
        if (cell_h < 2) cell_h = 2;
        if (cell_h > 4) cell_h = 4;
        if (c->row + cell_h + 2 > c->ops->rows) {
            free(img.pixels);
            goto alt_text;
        }
        /* Top border. */
        for (x = 0; x < cell_w + 2 && !c->done; x++) {
            emit_utf8(c, x == 0 ? "\xe2\x94\x8c" :
                         (x == cell_w + 1 ? "\xe2\x94\x90" : "\xe2\x94\x80"), 3, st);
        }
        newline(c);
        for (y = 0; y < cell_h && !c->done; y++) {
            emit_utf8(c, "\xe2\x94\x82", 3, st);
            for (x = 0; x < cell_w; x++) {
                int sx = (x * img.width) / cell_w;
                int sy = (y * img.height) / cell_h;
                uint8_t color = img.pixels[sy * img.width + sx];
                style_t cell = *st;
                /* The TUI label renders foreground only (no per-cell bg), so
                 * a full block glyph carries the image pixel colour. */
                cell.fg = color;
                cell.bg = 16;
                emit_utf8(c, "\xe2\x96\x88", 3, &cell);
            }
            emit_utf8(c, "\xe2\x94\x82", 3, st);
            newline(c);
        }
        for (x = 0; x < cell_w + 2 && !c->done; x++) {
            emit_utf8(c, x == 0 ? "\xe2\x94\x94" :
                         (x == cell_w + 1 ? "\xe2\x94\x98" : "\xe2\x94\x80"), 3, st);
        }
        newline(c);
        free(img.pixels);
        return;
    }
alt_text:
    {
        char alt[300];
        snprintf(alt, sizeof(alt), "[%s]", node->alt[0] ? node->alt : "image");
        emit_text(c, alt, st);
    }
}

static void render_inline_node(render_ctx_t *c, dom_node_t *node, style_t st)
{
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
            render_inline_node(c, ch, st);
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
            render_inline_node(c, ch, st);
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
        render_inline_node(c, ch, st);
    }
}

static void render_block(render_ctx_t *c, dom_node_t *node, style_t st, int indent);
static void render_list(render_ctx_t *c, dom_node_t *node, style_t st,
                        int indent, bool ordered);

static void render_children(render_ctx_t *c, dom_node_t *node, style_t st, int indent)
{
    for (dom_node_t *ch = node->first_child; ch != NULL && !c->done; ch = ch->next_sibling) {
        if (ch->type == DOM_NODE_TEXT) {
            emit_text(c, ch->text, &st);
            continue;
        }
        if (ch->display == DOM_DISPLAY_NONE || is_skip_tag(ch->tag)) continue;
        if (is_block_tag(ch->tag)) {
            render_block(c, ch, st, indent);
        } else {
            render_inline_node(c, ch, st);
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

    for (int r = 0; r < rows && !c->done; r++) {
        emit_text(c, "|", &st);
        for (int x = 0; x < cols; x++) {
            char padded[80];
            int len = (int)strlen(cells[r][x]);
            int pad = widths[x] - len;
            if (pad < 0) pad = 0;
            if (pad > (int)sizeof(padded) - len - 1) pad = (int)sizeof(padded) - len - 1;
            memcpy(padded, cells[r][x], (size_t)len);
            memset(padded + len, ' ', (size_t)pad);
            padded[len + pad] = '\0';
            emit_text(c, " ", &st);
            emit_text(c, padded, &cell_styles[r][x]);
            emit_text(c, " |", &st);
        }
        newline(c);
        if (header[r]) {
            emit_text(c, "+", &st);
            for (int x = 0; x < cols; x++) {
                for (int k = 0; k < widths[x] + 2; k++) emit_text(c, "-", &st);
                emit_text(c, "+", &st);
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
        st.bold = true;
        if (st.fg == 16) st.fg = head_color[level];
        if (st.font_scale < 2) st.font_scale = 2;
        newline(c);
        if (st.align == 1) {
            char text[512];
            node_plain_text(node, text, sizeof(text));
            {
                int len = (int)strlen(text);
                int start = 1 + (c->ops->cols - len) / 2;
                if (start < 1) start = 1;
                c->col = start;
            }
        }
        render_children(c, node, st, indent);
        newline(c);
        return;
    }
    if (strcmp(node->tag, "p") == 0 || strcmp(node->tag, "div") == 0 ||
        strcmp(node->tag, "section") == 0 || strcmp(node->tag, "article") == 0 ||
        strcmp(node->tag, "header") == 0 || strcmp(node->tag, "footer") == 0 ||
        strcmp(node->tag, "main") == 0 || strcmp(node->tag, "nav") == 0 ||
        strcmp(node->tag, "aside") == 0 || strcmp(node->tag, "form") == 0) {
        newline(c);
        if (st.align == 1) {
            char text[1024];
            node_plain_text(node, text, sizeof(text));
            {
                int len = (int)strlen(text);
                int start = 1 + (c->ops->cols - len) / 2;
                if (start < 1) start = 1;
                c->col = start;
            }
        }
        render_children(c, node, st, indent);
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
        emit_text(c, "\xe2\x94\x82 ", &quote);
        render_children(c, node, quote, indent + 2);
        newline(c);
        c->indent = saved;
        return;
    }
    if (strcmp(node->tag, "hr") == 0) {
        newline(c);
        for (int x = 1; x < c->ops->cols && !c->done; x++) {
            emit_utf8(c, "\xe2\x94\x80", 3, &st);
        }
        newline(c);
        return;
    }
    if (strcmp(node->tag, "br") == 0) {
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

void dom_render_tree(dom_tree_t *tree, const dom_render_ops_t *ops)
{
    render_ctx_t ctx;
    style_t st;

    if (tree == NULL || ops == NULL || tree->body == NULL) return;
    memset(&ctx, 0, sizeof(ctx));
    memset(&st, 0, sizeof(st));
    st.fg = 16;
    st.bg = 16;
    st.font_scale = 1;
    ctx.ops = ops;
    ctx.row = 1;
    ctx.col = 1;
    ctx.indent = 0;
    ctx.done = false;
    /* Paint the page background from the body's CSS background-color. The
     * cells the layout writes later keep their own colors. */
    if (ops->tui && ops->put_cell != NULL && tree->body->bg != 16) {
        for (int r = 1; r <= ops->rows; r++) {
            for (int c = 1; c <= ops->cols; c++) {
                ops->put_cell(ops->ctx, c, r, " ", 16, tree->body->bg, 0);
            }
        }
    }
    render_children(&ctx, tree->body, st, 0);
}

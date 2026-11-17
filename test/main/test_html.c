/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_html.c
 * @brief Unit tests for the HTML reader (components/markdown/html_read.c).
 *
 * Covers the block/inline/entity mapping to ANSI by checking the ANSI-stripped
 * reading text plus a few SGR-presence assertions. Pure: no SD, no LVGL.
 */

#include "unity.h"
#include "html.h"
#include "markdown.h"
#include "dom.h"
#include "dom_render.h"
#include "css.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Render + strip SGR into a caller buffer for readable assertions. */
static void render_plain(const char *html, char *dst, size_t dst_size)
{
    char *ansi = malloc(4096);

    TEST_ASSERT_NOT_NULL(ansi);
    html_render_ansi(html, ansi, 4096);
    markdown_strip_ansi(ansi, dst, dst_size);
    free(ansi);
}

void test_html_render_basic(void)
{
    char ansi[512];
    char plain[256];

    (void)html_render_ansi("<h1>Title</h1><p>Hello <b>bold</b> and <code>code</code>.</p>",
                           ansi, sizeof(ansi));
    TEST_ASSERT_NOT_NULL(strstr(ansi, "\x1b[1m"));   /* bold heading/strong */
    render_plain("<h1>Title</h1><p>Hello <b>bold</b> and <code>code</code>.</p>",
                 plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "Title"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "Hello"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "bold"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "code"));
}

void test_html_render_entities(void)
{
    char plain[256];

    render_plain("A&amp;B &lt;x&gt; &quot;q&quot; &#39; &mdash; &#x41; end", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "A&B"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "<x>"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "\"q\""));
    TEST_ASSERT_NOT_NULL(strstr(plain, "'"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "A end")); /* &#x41; -> A */
    /* No stray ampersands left from decoded entities. */
    TEST_ASSERT_NULL(strstr(plain, "&amp;"));
    TEST_ASSERT_NULL(strstr(plain, "&#"));
}

void test_html_render_lists(void)
{
    char plain[256];

    render_plain("<ul><li>one</li><li>two</li></ul>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "- one"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "- two"));

    render_plain("<ol><li>alpha</li><li>beta</li></ol>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "1. alpha"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "2. beta"));
}

void test_html_render_nested_list(void)
{
    char plain[256];

    render_plain("<ul><li>a<ul><li>b</li></ul></li></ul>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "- a"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "  - b")); /* nested indent */
}

void test_html_render_table(void)
{
    char plain[256];

    render_plain("<table><tr><th>A</th><th>B</th></tr>"
                 "<tr><td>1</td><td>2</td></tr></table>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "| A"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "| 1"));
}

void test_html_render_links_images(void)
{
    char plain[256];

    render_plain("<a href=\"http://example/x\">text</a>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "text (http://example/x)"));

    render_plain("<img alt=\"a picture\">", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "[a picture]"));

    render_plain("<img>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "[image]"));
}

void test_html_render_skips_head_script_style(void)
{
    char plain[256];

    render_plain("<head><title>Hidden</title></head>"
                 "<p>body</p><script>var x=1;</script><style>.a{}</style>",
                 plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "body"));
    TEST_ASSERT_NULL(strstr(plain, "Hidden"));
    TEST_ASSERT_NULL(strstr(plain, "var x"));
    TEST_ASSERT_NULL(strstr(plain, ".a{}"));
}

void test_html_render_pre_preserves(void)
{
    char plain[256];

    render_plain("<pre>a\n  b</pre>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "a\n  b"));
}

void test_html_render_comment_and_doctype(void)
{
    char plain[256];

    render_plain("<!DOCTYPE html><!-- hidden --><p>x</p>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "x"));
    TEST_ASSERT_NULL(strstr(plain, "DOCTYPE"));
    TEST_ASSERT_NULL(strstr(plain, "hidden"));
}

void test_html_render_blockquote(void)
{
    char plain[256];

    render_plain("<blockquote><p>quoted</p></blockquote>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "quoted"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "\xE2\x94\x82")); /* U+2502 bar */
}

void test_html_render_measure_and_truncate(void)
{
    char small[8];
    size_t need;

    /* NULL source -> 0. */
    TEST_ASSERT_EQUAL_size_t(0, html_render_ansi(NULL, small, sizeof(small)));

    /* Measure-only returns the required length. */
    need = html_render_ansi("<p>hello</p>", NULL, 0);
    TEST_ASSERT_GREATER_THAN_size_t(0, need);

    /* A too-small destination truncates but stays NUL-terminated. */
    need = html_render_ansi("<p>hello world, this is long</p>", small, sizeof(small));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)sizeof(small), (uint32_t)need);
    TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
}

void test_html_render_malformed_is_tolerated(void)
{
    char plain[256];

    /* Unclosed tag, stray '<', attributes with no value: must not crash and
     * should still surface the text. */
    render_plain("<p>ok <notclosed and < 3 >end", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "ok"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "end"));
}

typedef struct {
    char *buf;
    size_t size;
    int bg_calls;
    int bg_last;
    int bg_alpha;
} ansi_sink_t;

/** Records the foreground of every TUI cell the renderer emits, in order. */
typedef struct {
    uint8_t *fg;
    int count;
    int cap;
} cell_probe_t;

static void probe_put_cell(void *ctx, int col, int row, const char *utf8,
                           uint8_t fg, uint8_t bg, uint8_t attr)
{
    cell_probe_t *probe = (cell_probe_t *)ctx;

    (void)col; (void)row; (void)utf8; (void)bg; (void)attr;
    if (probe->count < probe->cap) {
        probe->fg[probe->count++] = fg;
    }
}

static void sink_append_ansi(void *ctx, const char *text)
{
    ansi_sink_t *sink = (ansi_sink_t *)ctx;
    size_t used = strlen(sink->buf);

    if (used + 1 >= sink->size) {
        return;
    }
    snprintf(sink->buf + used, sink->size - used, "%s", text);
}

/** A page that exposes <main> renders that subtree: mobile Wikipedia puts its
 *  navigation chrome before <main>, and on an 80x25 page the chrome otherwise
 *  filled the whole viewport so the article was never reached. */
void test_dom_render_main_focus(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><body><nav>NAV-CHROME</nav>"
        "<main><h1>Article Title</h1><p>Article body.</p></main>"
        "</body></html>");
    char ansi[2048] = { 0 };
    char plain[2048];
    ansi_sink_t sink = { ansi, sizeof(ansi), 0, 0, 0 };
    dom_render_ops_t ops = {
        .put_ansi = sink_append_ansi,
        .ctx = &sink,
        .cols = 80,
        .rows = 25,
        .tui = false,
    };

    TEST_ASSERT_NOT_NULL(tree);
    dom_render_tree(tree, &ops);
    dom_free(tree);
    markdown_strip_ansi(ansi, plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "Article Title"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "Article body."));
    TEST_ASSERT_NULL(strstr(plain, "NAV-CHROME"));
}

/** The single-pass ANSI HTML reader (editor preview, print, EPUB) has no JS:
 *  a <canvas> therefore shows its fallback content, as the spec requires. */
void test_html_canvas_fallback_ansi(void)
{
    char plain[256];

    render_plain("<p>before</p><canvas width=\"32\" height=\"16\">"
                 "<p>Canvas fallback text</p></canvas><p>after</p>",
                 plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "before"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "Canvas fallback text"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "after"));
}

/** Beyond the nesting cap the subtree is flattened to text instead of
 *  recursing per level (the real Wikipedia page exhausted the 28 KB shell
 *  worker stack). Content is preserved, layout is not. */
void test_dom_render_depth_cap_keeps_text(void)
{
    char html[4096];
    size_t used = 0;
    char ansi[2048] = { 0 };
    char plain[2048];
    ansi_sink_t sink = { ansi, sizeof(ansi), 0, 0, 0 };
    dom_render_ops_t ops = {
        .put_ansi = sink_append_ansi,
        .ctx = &sink,
        .cols = 80,
        .rows = 25,
        .tui = false,
    };
    dom_tree_t *tree;

    used += (size_t)snprintf(html + used, sizeof(html) - used, "<html><body>");
    for (int i = 0; i < 40; i++) {
        used += (size_t)snprintf(html + used, sizeof(html) - used, "<div>");
    }
    used += (size_t)snprintf(html + used, sizeof(html) - used, "DEEP-CONTENT");
    for (int i = 0; i < 40; i++) {
        used += (size_t)snprintf(html + used, sizeof(html) - used, "</div>");
    }
    used += (size_t)snprintf(html + used, sizeof(html) - used, "</body></html>");
    TEST_ASSERT_TRUE(used < sizeof(html));

    tree = dom_parse(html);
    TEST_ASSERT_NOT_NULL(tree);
    dom_render_tree(tree, &ops);
    dom_free(tree);
    markdown_strip_ansi(ansi, plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "DEEP-CONTENT"));
}

/** <canvas width height> attributes size the bitmap (HTML defaults are
 *  300x150). The parser must keep them for getContext and the renderer. */
void test_dom_canvas_attributes(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><body><canvas id=\"c\" width=\"96\" height=\"48\">"
        "<p>fallback</p></canvas></body></html>");
    dom_node_t *node;

    TEST_ASSERT_NOT_NULL(tree);
    node = dom_find_by_id(tree, "c");
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_EQUAL_STRING("canvas", node->tag);
    TEST_ASSERT_EQUAL_INT(96, node->canvas_w);
    TEST_ASSERT_EQUAL_INT(48, node->canvas_h);
    dom_free(tree);
}

static void sink_record_fill(void *ctx, int col, int row, int w, int h,
                             uint8_t bg, uint8_t alpha)
{
    ansi_sink_t *sink = (ansi_sink_t *)ctx;

    (void)col; (void)row; (void)w; (void)h;
    sink->bg_calls++;
    sink->bg_last = bg;
    sink->bg_alpha = alpha;
}

static bool sink_load_canvas(void *ctx, dom_node_t *node, dom_image_t *out)
{
    (void)ctx; (void)node;
    out->width = 2;
    out->height = 2;
    out->pixels = malloc(4);
    if (out->pixels == NULL) return false;
    memset(out->pixels, (12 << 4) | 9, 4);
    return true;
}

/** <canvas> fallback content is what a no-JS surface (editor preview,
 *  print-to-HTML, EPUB) shows, per the HTML spec. */
void test_dom_render_canvas_fallback(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><body><canvas width=\"32\" height=\"16\">"
        "<p>fallback text</p></canvas></body></html>");
    char ansi[1024] = { 0 };
    char plain[1024];
    ansi_sink_t sink = { ansi, sizeof(ansi), 0, 0, 0 };
    dom_render_ops_t ops = {
        .put_ansi = sink_append_ansi,
        .ctx = &sink,
        .cols = 80,
        .rows = 25,
        .tui = false,
    };

    TEST_ASSERT_NOT_NULL(tree);
    dom_render_tree(tree, &ops);
    dom_free(tree);
    markdown_strip_ansi(ansi, plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "fallback text"));
}

/** With a canvas bitmap the renderer draws the bitmap instead of the
 *  fallback (the browser path). */
void test_dom_render_canvas_bitmap(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><body><canvas width=\"32\" height=\"16\">"
        "<p>fallback text</p></canvas></body></html>");
    char ansi[1024] = { 0 };
    char plain[1024];
    ansi_sink_t sink = { ansi, sizeof(ansi), 0, 0, 0 };
    dom_render_ops_t ops = {
        .put_ansi = sink_append_ansi,
        .load_canvas = sink_load_canvas,
        .ctx = &sink,
        .cols = 80,
        .rows = 25,
        .tui = false,
    };

    TEST_ASSERT_NOT_NULL(tree);
    dom_render_tree(tree, &ops);
    dom_free(tree);
    markdown_strip_ansi(ansi, plain, sizeof(plain));
    TEST_ASSERT_NULL(strstr(plain, "fallback text"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "+"));   /* the image box */
}

/** CSS Backgrounds 3 subset: colour and background-image URL. */
void test_dom_css_background_properties(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><body><div id=\"d\">x</div></body></html>");
    dom_node_t *node;

    TEST_ASSERT_NOT_NULL(tree);
    node = dom_find_by_id(tree, "d");
    TEST_ASSERT_NOT_NULL(node);
    dom_set_style(node, "background-color", "#aa0000");
    TEST_ASSERT_TRUE(node->bg_set);
    TEST_ASSERT_EQUAL_INT(4, node->bg);   /* DOS red */
    dom_set_style(node, "background-image", "url(\"BG.BMP\")");
    TEST_ASSERT_EQUAL_STRING("BG.BMP", node->bg_src);
    dom_free(tree);
}

/** A block with an explicit background asks the surface to fill it. */
void test_dom_render_fill_bg_callback(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><body><div id=\"d\">x</div></body></html>");
    dom_node_t *node;
    char ansi[1024] = { 0 };
    char plain[1024];
    ansi_sink_t sink = { ansi, sizeof(ansi), 0, 0, 0 };
    dom_render_ops_t ops = {
        .put_ansi = sink_append_ansi,
        .fill_bg = sink_record_fill,
        .ctx = &sink,
        .cols = 80,
        .rows = 25,
        .tui = false,
    };

    TEST_ASSERT_NOT_NULL(tree);
    node = dom_find_by_id(tree, "d");
    TEST_ASSERT_NOT_NULL(node);
    dom_set_style(node, "background-color", "#aa0000");
    dom_render_tree(tree, &ops);
    dom_free(tree);
    markdown_strip_ansi(ansi, plain, sizeof(plain));
    TEST_ASSERT_GREATER_THAN_INT(0, sink.bg_calls);
    TEST_ASSERT_EQUAL_INT(4, sink.bg_last);
}

/** CSS Color alpha reaches the DOM: `rgba()` keeps its alpha and
 *  `transparent` is an explicit, fully transparent background. */
void test_dom_background_alpha(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><body><div id=\"d\">x</div></body></html>");
    dom_node_t *node;

    TEST_ASSERT_NOT_NULL(tree);
    node = dom_find_by_id(tree, "d");
    TEST_ASSERT_NOT_NULL(node);

    dom_set_style(node, "background-color", "rgba(0, 0, 0, 0.5)");
    TEST_ASSERT_TRUE(node->bg_set);
    TEST_ASSERT_EQUAL_UINT8(0, node->bg);
    TEST_ASSERT_UINT8_WITHIN(2, 128, node->bg_alpha);

    dom_set_style(node, "background-color", "transparent");
    TEST_ASSERT_TRUE(node->bg_set);
    TEST_ASSERT_EQUAL_UINT8(16, node->bg);   /* no DOS colour */
    TEST_ASSERT_EQUAL_UINT8(0, node->bg_alpha);

    dom_set_style(node, "color", "rgba(255, 255, 255, 0.25)");
    TEST_ASSERT_EQUAL_UINT8(15, node->fg);
    TEST_ASSERT_UINT8_WITHIN(2, 64, node->fg_alpha);

    dom_free(tree);
}

/** `transparent` draws no rectangle at all, so the page behind shows. */
void test_dom_transparent_skips_fill(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><body><div id=\"d\">x</div></body></html>");
    dom_node_t *node;
    char ansi[1024] = { 0 };
    ansi_sink_t sink = { ansi, sizeof(ansi), 0, 0, 0 };
    dom_render_ops_t ops = {
        .put_ansi = sink_append_ansi,
        .fill_bg = sink_record_fill,
        .ctx = &sink,
        .cols = 80,
        .rows = 25,
        .tui = false,
    };

    TEST_ASSERT_NOT_NULL(tree);
    node = dom_find_by_id(tree, "d");
    TEST_ASSERT_NOT_NULL(node);
    dom_set_style(node, "background-color", "transparent");
    dom_render_tree(tree, &ops);
    dom_free(tree);
    TEST_ASSERT_EQUAL_INT(0, sink.bg_calls);
}

/** `opacity` multiplies the background alpha of the subtree. */
void test_dom_opacity_multiplies_fill(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><body><div id=\"d\">x</div></body></html>");
    dom_node_t *node;
    char ansi[1024] = { 0 };
    ansi_sink_t sink = { ansi, sizeof(ansi), 0, 0, 0 };
    dom_render_ops_t ops = {
        .put_ansi = sink_append_ansi,
        .fill_bg = sink_record_fill,
        .ctx = &sink,
        .cols = 80,
        .rows = 25,
        .tui = false,
    };

    TEST_ASSERT_NOT_NULL(tree);
    node = dom_find_by_id(tree, "d");
    TEST_ASSERT_NOT_NULL(node);
    dom_set_style(node, "background-color", "#aa0000");
    dom_set_style(node, "opacity", "0.5");
    TEST_ASSERT_UINT8_WITHIN(2, 128, node->opacity);
    dom_render_tree(tree, &ops);
    dom_free(tree);
    TEST_ASSERT_GREATER_THAN_INT(0, sink.bg_calls);
    TEST_ASSERT_EQUAL_INT(4, sink.bg_last);
    TEST_ASSERT_UINT8_WITHIN(2, 128, (uint8_t)sink.bg_alpha);
}

/** Translucent text is composited to one opaque DOS cell colour: opaque text
 *  keeps its exact colour, translucent text does not. */
void test_dom_render_text_alpha_composited(void)
{
    static uint8_t cells_fg[256];
    cell_probe_t probe = { cells_fg, 0, 256 };
    dom_tree_t *tree = dom_parse(
        "<html><body><p><span style=\"color:#ffffff\">A</span>"
        "<span style=\"color:rgba(255,255,255,0.4)\">B</span></p>"
        "</body></html>");
    dom_render_ops_t ops = {
        .put_cell = probe_put_cell,
        .ctx = &probe,
        .cols = 80,
        .rows = 25,
        .tui = true,
        .base_bg = 0,
    };

    TEST_ASSERT_NOT_NULL(tree);
    {
        /* An empty ruleset still runs the inline-style pass (that is how the
         * browser computes styles; inline declarations are applied there). */
        css_ruleset_t no_rules = { 0 };

        css_compute_styles(tree, &no_rules);
    }
    dom_render_tree(tree, &ops);
    dom_free(tree);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, probe.count);
    /* Find the opaque-white glyph cell, then the cell after it: the 40% white
     * must land on a dimmer DOS colour, never the opaque white and never the
     * invisible/black cell. */
    {
        int white = -1;

        for (int i = 0; i < probe.count; i++) {
            if (cells_fg[i] == 15) {
                white = i;
                break;
            }
        }
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, white);
        TEST_ASSERT_LESS_THAN_INT(probe.count, white + 1);
        TEST_ASSERT_NOT_EQUAL_UINT8(15, cells_fg[white + 1]);
        TEST_ASSERT_NOT_EQUAL_UINT8(0, cells_fg[white + 1]);
    }
}

/** A tag attribute larger than the parser's store must not leak.
 *
 * Parsoid (Wikipedia) puts kilobytes of JSON in `data-mw='...'`; the old
 * scanner stopped at its 512-byte buffer edge and re-parsed the remainder as
 * text, so `{{cite web ...}}` appeared in the rendered article. The value is
 * now consumed to its closing quote.
 */
void test_html_long_attribute_does_not_leak(void)
{
    char html[2048];
    size_t pos = 0;
    dom_tree_t *tree;
    dom_node_t *p;

    pos += (size_t)snprintf(html + pos, sizeof(html) - pos,
                            "<html><body><p data-mw='");
    while (pos < 1200) {
        html[pos++] = 'x';
    }
    pos += (size_t)snprintf(html + pos, sizeof(html) - pos,
                            "{{cite web|title=Espressif}}'>Hello</p></body></html>");
    TEST_ASSERT_TRUE(pos < sizeof(html));

    tree = dom_parse(html);
    TEST_ASSERT_NOT_NULL(tree);
    p = dom_find_first_by_tag(tree, "p");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("Hello", dom_get_text(p));
    TEST_ASSERT_NULL(strstr(dom_get_text(p), "cite"));
    dom_free(tree);
}

/** <script>/<style> are raw text: '<' inside JS/CSS is not a tag. */
void test_dom_raw_text_script_style(void)
{
    dom_tree_t *tree = dom_parse(
        "<html><head><style>/* a<b */ body{color:red}</style></head>"
        "<body><script>if (a < b && c > d) { s = '<img>'; }</script>"
        "</body></html>");
    dom_node_t *script;

    TEST_ASSERT_NOT_NULL(tree);
    script = dom_find_first_by_tag(tree, "script");
    TEST_ASSERT_NOT_NULL(script);
    TEST_ASSERT_NOT_NULL(script->first_child);
    TEST_ASSERT_NOT_NULL(strstr(script->first_child->text, "a < b"));
    TEST_ASSERT_NOT_NULL(strstr(script->first_child->text, "<img>"));
    dom_free(tree);
}

/** A CSS comment before a rule must not become part of its selector. */
void test_css_comments_are_skipped(void)
{
    css_ruleset_t rules;

    memset(&rules, 0, sizeof(rules));
    css_parse_block("/* page colours */ body { background-color: #00aaaa; }"
                    "/* banner */ .banner { background-color: #aa0000; }",
                    &rules);
    TEST_ASSERT_EQUAL_INT(2, rules.count);
    TEST_ASSERT_EQUAL_STRING("body", rules.rules[0].selector);
    TEST_ASSERT_EQUAL_STRING("background-color", rules.rules[0].property);
    TEST_ASSERT_EQUAL_STRING(".banner", rules.rules[1].selector);
}

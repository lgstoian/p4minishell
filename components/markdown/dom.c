#include "dom.h"
#include "css.h"
#include "css_color.h"
#include "html.h"
#include "strutil.h"
#include "p4heap.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/** DOM mutation epoch. It starts at 1 so a zero-initialised tree never looks
 *  "already computed". Only mutations that can change a computed style bump
 *  it; the cascade itself writes styles through dom_set_style() and must not. */
static uint32_t s_style_epoch = 1;

uint32_t dom_style_epoch(void)
{
    return s_style_epoch;
}

void dom_mark_style_dirty(void)
{
    s_style_epoch++;
    if (s_style_epoch == 0) s_style_epoch = 1;
}

/** Alpha/opacity defaults: 255 means "fully opaque, nothing set". A node's
 *  fg_alpha is honoured only when the node also sets a colour, bg_alpha only
 *  when it sets a background, so an inherited value behaves like CSS. */
static void dom_node_defaults(dom_node_t *node, dom_node_type_t type)
{
    node->type = type;
    node->display = DOM_DISPLAY_INLINE;
    node->fg = 16;
    node->bg = 16;
    node->fg_alpha = 255;
    node->bg_alpha = 255;
    node->opacity = 255;
}

dom_node_t *dom_node_new(dom_node_type_t type) {
    dom_node_t *node = calloc(1, sizeof(dom_node_t));
    if (!node) return NULL;
    dom_node_defaults(node, type);
    return node;
}

dom_node_t *dom_node_new_in(dom_arena_t *arena, dom_node_type_t type) {
    dom_node_t *node;

    if (arena != NULL) {
        node = (dom_node_t *)dom_arena_alloc(arena, sizeof(dom_node_t));
        if (node != NULL) {
            memset(node, 0, sizeof(*node));
            dom_node_defaults(node, type);
            node->arena = arena;
            return node;
        }
        /* Arena exhausted: fall back to an individually-owned node so the page
         * still renders instead of truncating at the arena boundary. */
    }
    return dom_node_new(type);
}

/** Unlink @p node from its parent and release its children: arena-owned
 *  children are only unlinked (their memory returns with the arena), while
 *  individually-owned children are freed. */
static void dom_node_release(dom_node_t *node) {
    dom_node_t *child;

    if (node == NULL) return;

    if (node->parent != NULL) {
        dom_node_remove_child(node->parent, node);
    }

    child = node->first_child;
    node->first_child = NULL;
    node->last_child = NULL;
    while (child != NULL) {
        dom_node_t *next = child->next_sibling;

        child->parent = NULL;
        child->next_sibling = NULL;
        child->prev_sibling = NULL;
        if (child->arena != NULL) {
            dom_node_release(child);
        } else {
            dom_node_free(child);
        }
        child = next;
    }
}

void dom_node_free(dom_node_t *node) {
    if (!node) return;
    if (node->arena != NULL) {
        /* Arena-owned: unlinking is the whole job, and the arena's destroy
         * reclaims the memory. Freeing here would be a double free. */
        dom_node_release(node);
        return;
    }

    /* Individually owned: release the children, then the node itself. */
    dom_node_t *child = node->first_child;
    while (child) {
        dom_node_t *next = child->next_sibling;
        dom_node_free(child);
        child = next;
    }
    node->first_child = NULL;
    node->last_child = NULL;
    free(node);
}

void dom_node_append_child(dom_node_t *parent, dom_node_t *child) {
    if (!parent || !child) return;
    dom_mark_style_dirty();     /* a new subtree needs the cascade/inherit pass */
    child->parent = parent;
    child->next_sibling = NULL;
    if (parent->last_child) {
        parent->last_child->next_sibling = child;
        child->prev_sibling = parent->last_child;
        parent->last_child = child;
    } else {
        parent->first_child = child;
        parent->last_child = child;
        child->prev_sibling = NULL;
    }
}

void dom_node_remove_child(dom_node_t *parent, dom_node_t *child) {
    if (!parent || !child) return;
    dom_mark_style_dirty();
    if (child->prev_sibling) {
        child->prev_sibling->next_sibling = child->next_sibling;
    } else {
        parent->first_child = child->next_sibling;
    }
    if (child->next_sibling) {
        child->next_sibling->prev_sibling = child->prev_sibling;
    } else {
        parent->last_child = child->prev_sibling;
    }
    child->parent = NULL;
    child->next_sibling = NULL;
    child->prev_sibling = NULL;
}

/* Maximum element nesting accepted by the parser. Parsing is recursive, so a
 * malformed document with thousands of unclosed tags used to consume the whole
 * command-worker stack (the 64 B parse_element frame times the nesting depth).
 * Real pages stay well under this; deeper content is kept as a leaf element so
 * rendering still shows the text but the stack stays bounded. */
#define DOM_MAX_DEPTH 64

static void skip_whitespace(const char **p) {
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

/** Decode HTML entities in a text run (shared html_entity_decode table). */
static void dom_decode_entities(const char *src, size_t len, char *dst, size_t dst_size) {
    const char *p = src;
    const char *end = src + len;
    size_t out = 0;

    if (dst == NULL || dst_size == 0) return;
    while (p < end && out + 1 < dst_size) {
        if (*p == '&') {
            unsigned long cp = 0;
            size_t consumed = 0;
            if (html_entity_decode(p, end, &cp, &consumed)) {
                char utf8[4];
                size_t n = html_utf8_encode(cp, utf8);
                size_t i;
                for (i = 0; i < n && out + 1 < dst_size; i++) {
                    dst[out++] = utf8[i];
                }
                p += consumed;
                continue;
            }
        }
        dst[out++] = *p++;
    }
    dst[out] = '\0';
}

static void parse_tag_name(const char **p, char *out, size_t out_size) {
    size_t i = 0;
    while (**p && !isspace((unsigned char)**p) && **p != '>' && **p != '/' && i < out_size - 1) {
        out[i++] = tolower((unsigned char)**p);
        (*p)++;
    }
    out[i] = '\0';
}

static void parse_attributes(const char **p, dom_node_t *node) {
    /* HTML keeps the FIRST occurrence of a duplicate attribute. The scanner
     * still consumes every value, so a repeated name cannot leak its text. */
    char seen[12][32];
    int nseen = 0;

    skip_whitespace(p);
    while (**p && **p != '>' && **p != '/') {
        char attr_name[64] = {0};
        char attr_value[512] = {0};
        bool duplicate = false;
        size_t i = 0;
        while (**p && !isspace((unsigned char)**p) && **p != '=' && **p != '>' && **p != '/' && i < sizeof(attr_name) - 1) {
            attr_name[i++] = tolower((unsigned char)**p);
            (*p)++;
        }
        attr_name[i] = '\0';
        if (attr_name[0] != '\0') {
            for (int k = 0; k < nseen; k++) {
                if (strcmp(seen[k], attr_name) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && nseen < (int)(sizeof(seen) / sizeof(seen[0]))) {
                strncpy(seen[nseen], attr_name, sizeof(seen[0]) - 1);
                seen[nseen][sizeof(seen[0]) - 1] = '\0';
                nseen++;
            }
        }
        while (**p && !isspace((unsigned char)**p) && **p != '=' && **p != '>' &&
               **p != '/') {
            (*p)++;   /* over-long name: consume the rest, keep the prefix */
        }
        skip_whitespace(p);
        if (**p == '=') {
            (*p)++;
            skip_whitespace(p);
            char quote = 0;
            if (**p == '"' || **p == '\'') {
                quote = **p;
                (*p)++;
            }
            i = 0;
            /* An unquoted value runs to whitespace or '>'. Stopping at '/' too
             * (as this scanner used to) truncated `src=/s.js` to `src=`, and
             * the rest leaked into the document as text. `/` is only the
             * self-closing marker before '>'. */
            while (**p && (quote ? **p != quote : (!isspace((unsigned char)**p) && **p != '>')) && i < sizeof(attr_value) - 1) {
                attr_value[i++] = **p;
                (*p)++;
            }
            attr_value[i] = '\0';
            /* Consume the remainder of an over-long value. Real pages carry
             * attributes far larger than the 512-byte store (Parsoid's
             * `data-mw` JSON is kilobytes); stopping at the buffer edge left
             * the scanner inside the value, and the rest leaked into the
             * document as text (`{{cite web ...}}` on a live Wikipedia page). */
            if (quote) {
                while (**p && **p != quote) (*p)++;
                if (**p == quote) (*p)++;
            } else {
                while (**p && !isspace((unsigned char)**p) && **p != '>') {
                    (*p)++;
                }
            }
        }
        if (!duplicate) {
        if (strcmp(attr_name, "id") == 0) {
            strncpy(node->id, attr_value, sizeof(node->id) - 1);
        } else if (strcmp(attr_name, "class") == 0) {
            strncpy(node->class, attr_value, sizeof(node->class) - 1);
        } else if (strcmp(attr_name, "href") == 0) {
            /* URLs in markup carry `&amp;`; the stored href must be the real
             * URL or the fetch asks for `...&amp;goto=...`. */
            dom_decode_entities(attr_value, strlen(attr_value), node->href,
                                sizeof(node->href));
        } else if (strcmp(attr_name, "src") == 0) {
            dom_decode_entities(attr_value, strlen(attr_value), node->src,
                                sizeof(node->src));
        } else if (strcmp(attr_name, "alt") == 0) {
            strncpy(node->alt, attr_value, sizeof(node->alt) - 1);
        } else if (strcmp(attr_name, "style") == 0) {
            strncpy(node->style, attr_value, sizeof(node->style) - 1);
        } else if (strcmp(attr_name, "onclick") == 0) {
            strncpy(node->onclick, attr_value, sizeof(node->onclick) - 1);
        } else if (strcmp(attr_name, "onload") == 0) {
            strncpy(node->onload, attr_value, sizeof(node->onload) - 1);
        } else if (strcmp(attr_name, "width") == 0 &&
                   strcmp(node->tag, "canvas") == 0) {
            node->canvas_w = atoi(attr_value);
        } else if (strcmp(attr_name, "height") == 0 &&
                   strcmp(node->tag, "canvas") == 0) {
            node->canvas_h = atoi(attr_value);
        } else if (strcmp(attr_name, "type") == 0) {
            strncpy(node->input_type, attr_value, sizeof(node->input_type) - 1);
        } else if (strcmp(attr_name, "value") == 0) {
            strncpy(node->value, attr_value, sizeof(node->value) - 1);
        } else if (strcmp(attr_name, "placeholder") == 0) {
            strncpy(node->placeholder, attr_value, sizeof(node->placeholder) - 1);
        } else if (strcmp(attr_name, "checked") == 0 ||
                   strcmp(attr_name, "selected") == 0) {
            node->checked = true;
        } else if (strcmp(attr_name, "disabled") == 0) {
            node->disabled = true;
        } else if (strcmp(attr_name, "required") == 0) {
            node->required = true;
        } else if (strcmp(attr_name, "colspan") == 0) {
            node->col_span = atoi(attr_value);
        } else if (strcmp(attr_name, "rowspan") == 0) {
            node->row_span = atoi(attr_value);
        }
        }
        skip_whitespace(p);
    }
}

static bool is_void_element(const char *tag) {
    static const char *void_tags[] = {"area","base","br","col","embed","hr","img","input","link","meta","param","source","track","wbr",NULL};
    for (int i = 0; void_tags[i]; i++) {
        if (strcmp(tag, void_tags[i]) == 0) return true;
    }
    return false;
}

static bool is_block_element(const char *tag) {
    static const char *block_tags[] = {"address","article","aside","blockquote","body","br","caption","dd","details","dialog","div","dl","dt","fieldset","figcaption","figure","footer","form","canvas","h1","h2","h3","h4","h5","h6","header","hr","html","legend","li","main","nav","ol","p","pre","section","summary","table","tbody","td","tfoot","th","thead","tr","ul",NULL};
    for (int i = 0; block_tags[i]; i++) {
        if (strcmp(tag, block_tags[i]) == 0) return true;
    }
    return false;
}

/** Implied end tags the parser recovers without a DTD: a new <p>/<li>/<td>/
 *  <tr>/<option> closes the open one instead of nesting inside it. Kept small
 *  and explicit - a full HTML5 tree builder would be a second parser. */
static bool dom_auto_close(const char *open, const char *incoming)
{
    if (strcmp(open, "p") == 0) {
        static const char *p_closers[] = {
            "p", "div", "section", "article", "header", "footer", "main",
            "nav", "aside", "ul", "ol", "table", "blockquote", "pre", "hr",
            "h1", "h2", "h3", "h4", "h5", "h6", "form", "details", "figure",
            NULL
        };
        for (int i = 0; p_closers[i]; i++) {
            if (strcmp(incoming, p_closers[i]) == 0) return true;
        }
        return false;
    }
    if (strcmp(open, "li") == 0) return strcmp(incoming, "li") == 0;
    if (strcmp(open, "td") == 0 || strcmp(open, "th") == 0) {
        return strcmp(incoming, "td") == 0 || strcmp(incoming, "th") == 0 ||
               strcmp(incoming, "tr") == 0;
    }
    if (strcmp(open, "tr") == 0) return strcmp(incoming, "tr") == 0;
    if (strcmp(open, "option") == 0) return strcmp(incoming, "option") == 0;
    return false;
}

void dom_node_reset_style(dom_node_t *node)
{
    if (node == NULL) return;
    /* Only computed-style fields: attributes, text, event handlers and the
     * raw style attribute survive, so re-resolving the cascade is idempotent. */
    node->display = is_block_element(node->tag) ? DOM_DISPLAY_BLOCK : DOM_DISPLAY_INLINE;
    node->fg = 16;
    node->bg = 16;
    node->fg_alpha = 255;
    node->bg_alpha = 255;
    node->opacity = 255;
    node->bg_set = false;
    node->align = 0;
    node->font_scale = 1;
    node->bold = false;
    node->italic = false;
    node->underline = false;
    node->border = false;
    node->margin_top = node->margin_bottom = node->margin_left = node->margin_right = 0;
    node->padding_top = node->padding_bottom = node->padding_left = node->padding_right = 0;
    node->border_radius = 0;
    node->shadow_set = false;
    node->shadow_dos = 0;
    node->shadow_x = node->shadow_y = 0;
    node->shadow_alpha = 0;
    node->gradient_set = false;
    memset(&node->gradient, 0, sizeof(node->gradient));
    node->grid_columns = 0;
    node->bg_src[0] = '\0';
}

/* Consume the remainder of an element whose nesting limit was reached. Tracks
 * same-name nesting so the correct close tag is honoured, and tolerates the
 * strings, comments and void tags a malformed document can contain. */
static void skip_element_content(const char **p, const char *tag) {
    int depth = 1;

    while (**p) {
        if (**p != '<') {
            (*p)++;
            continue;
        }
        if (*(*p + 1) == '/' || *(*p + 1) == '!' || *(*p + 1) == '?') {
            const char *q = *p + 1 + (*(*p + 1) == '/' ? 1 : 0);
            char name[32] = {0};
            size_t i = 0;
            bool is_close = (*(*p + 1) == '/');
            while (*q && !isspace((unsigned char)*q) && *q != '>' && *q != '/' &&
                   i < sizeof(name) - 1) {
                name[i++] = tolower((unsigned char)*q);
                q++;
            }
            while (**p && **p != '>') (*p)++;
            if (**p == '>') (*p)++;
            if (is_close && strcmp(name, tag) == 0 && --depth == 0) return;
        } else {
            const char *q = *p + 1;
            char name[32] = {0};
            size_t i = 0;
            while (*q && !isspace((unsigned char)*q) && *q != '>' && *q != '/' &&
                   i < sizeof(name) - 1) {
                name[i++] = tolower((unsigned char)*q);
                q++;
            }
            while (**p && **p != '>') (*p)++;
            if (**p == '>') (*p)++;
            if (strcmp(name, tag) == 0 && !is_void_element(name)) depth++;
        }
    }
}

static dom_node_t *parse_element(const char **p, dom_node_t *parent, int depth,
                                 dom_arena_t *arena, size_t *node_count) {
    if (**p != '<') return NULL;
    (*p)++;
    if (**p == '!' || **p == '?') {
        while (**p && **p != '>') (*p)++;
        if (**p == '>') (*p)++;
        return NULL;
    }
    if (**p == '/') {
        while (**p && **p != '>') (*p)++;
        if (**p == '>') (*p)++;
        return (dom_node_t *)-1;
    }
    if (*node_count >= P4_CONFIG_DOM_MAX_NODES) return NULL;
    dom_node_t *node = dom_node_new_in(arena, DOM_NODE_ELEMENT);
    if (!node) return NULL;
    (*node_count)++;
    parse_tag_name(p, node->tag, sizeof(node->tag));
    parse_attributes(p, node);
    if (is_block_element(node->tag)) {
        node->display = DOM_DISPLAY_BLOCK;
    }
    if (**p == '/') {
        (*p)++;
        if (**p == '>') (*p)++;
        return node;
    }
    if (**p == '>') (*p)++;
    if (is_void_element(node->tag)) {
        return node;
    }
    if (depth >= DOM_MAX_DEPTH) {
        skip_element_content(p, node->tag);
        return node;
    }
    /* <script> and <style> are raw-text elements (HTML "script data" and
     * "RAWTEXT" states): their content is text until the matching close tag,
     * even if it contains '<', '>' or something that looks like a tag. A JS
     * comparison or an `</...>` inside a string comment must not create DOM
     * nodes (a "<img>" in a script comment used to truncate the script). */
    if (strcmp(node->tag, "script") == 0 || strcmp(node->tag, "style") == 0) {
        const char *content = *p;
        const char *close = content;

        while (*close != '\0') {
            if (close[0] == '<' && close[1] == '/') {
                size_t i = 0;

                while (node->tag[i] != '\0' &&
                       tolower((unsigned char)close[2 + i]) == node->tag[i]) {
                    i++;
                }
                if (node->tag[i] == '\0' &&
                    (close[2 + i] == '>' || close[2 + i] == ' ' ||
                     close[2 + i] == '\t' || close[2 + i] == '\r' ||
                     close[2 + i] == '\n')) {
                    break;
                }
            }
            close++;
        }
        {
            size_t len = (size_t)(close - content);

            if (len > 0) {
                dom_node_t *text_node = dom_node_new_in(arena, DOM_NODE_TEXT);
                if (text_node != NULL) {
                    if (len >= sizeof(text_node->text)) {
                        len = sizeof(text_node->text) - 1;
                    }
                    memcpy(text_node->text, content, len);
                    text_node->text[len] = '\0';
                    (*node_count)++;
                    dom_node_append_child(node, text_node);
                }
            }
        }
        *p = close;
        while (**p != '\0' && **p != '>') {
            (*p)++;
        }
        if (**p == '>') {
            (*p)++;
        }
        return node;
    }
    while (**p) {
        skip_whitespace(p);
        if (**p == '<' && *(*p + 1) == '/') {
            const char *close = *p + 2;
            char close_tag[32] = {0};
            size_t i = 0;
            while (*close && !isspace((unsigned char)*close) && *close != '>' && i < sizeof(close_tag) - 1) {
                close_tag[i++] = tolower((unsigned char)*close);
                close++;
            }
            close_tag[i] = '\0';
            if (strcmp(close_tag, node->tag) == 0) {
                while (**p && **p != '>') (*p)++;
                if (**p == '>') (*p)++;
                break;
            }
            /* An ancestor's close tag closes THIS element implicitly. Leave
             * the tag for the parent: consuming it here made every following
             * sibling (e.g. a <div> after </ul>) a child of this element. */
            break;
        }
        /* Implied end tags: `<p>one<p>two` must produce two siblings, not a
         * nested p. The parent loop re-parses the same tag as a sibling. */
        if (**p == '<' && *(*p + 1) != '/' && *(*p + 1) != '!' &&
            *(*p + 1) != '?') {
            const char *q = *p + 1;
            char incoming[32] = {0};
            size_t il = 0;

            while (*q && isalnum((unsigned char)*q) && il < sizeof(incoming) - 1) {
                incoming[il++] = tolower((unsigned char)*q);
                q++;
            }
            incoming[il] = '\0';
            if (incoming[0] != '\0' && dom_auto_close(node->tag, incoming)) {
                break;
            }
        }
        if (**p == '<') {
            dom_node_t *child = parse_element(p, node, depth + 1, arena, node_count);
            if (child == (dom_node_t *)-1) break;
            if (child) dom_node_append_child(node, child);
        } else {
            /* Text run: write straight into the new node so no 1 KB scratch is
             * carried on every recursion level (dom_parse runs on the worker
             * stack; a nested document would otherwise overflow it). */
            const char *start = *p;
            const char *end = start;
            while (*end != '\0' && *end != '<') {
                end++;
            }
            while (start < end && isspace((unsigned char)*start)) {
                start++;
            }
            while (end > start && isspace((unsigned char)end[-1])) {
                end--;
            }
            if (end > start) {
                dom_node_t *text_node = dom_node_new_in(arena, DOM_NODE_TEXT);
                if (text_node) {
                    (*node_count)++;
                    dom_decode_entities(start, (size_t)(end - start),
                                        text_node->text, sizeof(text_node->text));
                    dom_node_append_child(node, text_node);
                }
            }
            *p = end;
        }
    }
    return node;
}

/* ---- bounded chunk-fed recovery for truncated markup --------------------
 * The linear scanner needs a NUL-terminated, tag-balanced source. When a page
 * is cut inside a tag or an attribute value, one bounded PSRAM buffer is filled
 * in fixed chunks, the dangling construct is closed and a short set of implied
 * end tags is appended. The public signature is unchanged and the ONE arena
 * cap still allocates every node; there is no second parser. */
#define DOM_PARSE_CHUNK_BYTES 512u
#define DOM_PARSE_RECOVERY_MAX_BYTES (256u * 1024u)

static bool dom_tail_truncated(const char *html, size_t len)
{
    const char *lt = NULL;
    char quote = 0;

    for (size_t i = 0; i < len; i++) {
        char c = html[i];

        if (quote != 0) {
            if (c == quote) quote = 0;
        } else if (c == '<') {
            lt = html + i;
        } else if (c == '>') {
            lt = NULL;
        } else if (lt != NULL && (c == '"' || c == '\'')) {
            quote = c;
        }
    }
    return lt != NULL || quote != 0;
}

static dom_tree_t *dom_parse_impl(const char *html) {
    size_t node_count = 0;

    if (!html) return NULL;
    dom_tree_t *tree = calloc(1, sizeof(dom_tree_t));
    if (!tree) return NULL;
    tree->arena = dom_arena_create();
    tree->root = dom_node_new_in(tree->arena, DOM_NODE_ELEMENT);
    if (!tree->root) {
        dom_arena_destroy(tree->arena);
        free(tree);
        return NULL;
    }
    node_count++;
    strcpy(tree->root->tag, "#document");
    const char *p = html;
    while (*p) {
        skip_whitespace(&p);
        if (*p == '<') {
            dom_node_t *child = parse_element(&p, tree->root, 0, tree->arena, &node_count);
            if (child == (dom_node_t *)-1) break;
            if (child) {
                dom_node_append_child(tree->root, child);
                if (strcmp(child->tag, "head") == 0) tree->head = child;
                if (strcmp(child->tag, "body") == 0) tree->body = child;
            }
        } else {
            p++;
        }
    }
    /* A normal document nests head/body inside <html>, so the direct-child
     * scan above misses them and the renderer used to walk the whole tree
     * (showing <title> and raw <style> CSS as page text). Resolve them
     * anywhere in the tree before falling back to the root. */
    if (!tree->head) {
        tree->head = dom_find_first_by_tag(tree, "head");
    }
    if (!tree->body) {
        tree->body = dom_find_first_by_tag(tree, "body");
    }
    if (!tree->body) {
        tree->body = tree->root;
    }
    if (tree->head != NULL) {
        dom_node_t *child = tree->head->first_child;
        while (child) {
            if (strcmp(child->tag, "title") == 0) {
                dom_node_t *text = child->first_child;
                if (text && text->type == DOM_NODE_TEXT) {
                    strncpy(tree->title, text->text, sizeof(tree->title) - 1);
                }
                break;
            }
            child = child->next_sibling;
        }
    }
    return tree;
}

dom_tree_t *dom_parse(const char *html)
{
    static const char TAIL[] =
        "</td></tr></li></p></ul></ol></table></body></html>";
    size_t len;
    char *buf;
    dom_tree_t *tree;

    if (html == NULL) return NULL;
    len = strnlen(html, DOM_PARSE_RECOVERY_MAX_BYTES);
    if (len == 0 || !dom_tail_truncated(html, len)) {
        return dom_parse_impl(html);
    }

    buf = p4heap_alloc_psram(len + 2u + sizeof(TAIL));
    if (buf == NULL) {
        buf = malloc(len + 2u + sizeof(TAIL));
    }
    if (buf == NULL) {
        return dom_parse_impl(html);    /* OOM: parse what we have */
    }
    for (size_t off = 0; off < len; ) {
        size_t n = len - off;

        if (n > DOM_PARSE_CHUNK_BYTES) n = DOM_PARSE_CHUNK_BYTES;
        memcpy(buf + off, html + off, n);
        off += n;
    }
    {
        size_t extra = 0;
        char quote = 0;

        for (size_t i = 0; i < len; i++) {
            if (quote != 0) {
                if (html[i] == quote) quote = 0;
            } else if (html[i] == '"' || html[i] == '\'') {
                quote = html[i];
            }
        }
        if (quote != 0) {
            buf[len + extra++] = quote; /* close the dangling attribute value */
        }
        buf[len + extra++] = '>';       /* close the dangling tag */
        memcpy(buf + len + extra, TAIL, sizeof(TAIL) - 1);
        extra += sizeof(TAIL) - 1;
        buf[len + extra] = '\0';
    }
    tree = dom_parse_impl(buf);
    p4heap_free(buf);
    return tree;
}

void dom_free(dom_tree_t *tree) {
    if (!tree) return;
    css_forget_tree(tree);      /* never leave the cache pointing at freed nodes */
    /* Release every individually-owned node first (JS-created text nodes), then
     * hand the arena's blocks back. dom_node_free() unlinks arena-owned nodes
     * instead of freeing them, so a single recursive walk covers both. */
    dom_node_free(tree->root);
    if (tree->arena != NULL) {
        dom_arena_destroy(tree->arena);
        tree->arena = NULL;
    }
    free(tree);
}

dom_node_t *dom_find_by_id(dom_tree_t *tree, const char *id) {
    if (!tree || !tree->root || !id) return NULL;
    dom_node_t *stack[256];
    int sp = 0;
    stack[sp++] = tree->root;
    while (sp > 0) {
        dom_node_t *node = stack[--sp];
        if (node->type == DOM_NODE_ELEMENT && strcmp(node->id, id) == 0) {
            return node;
        }
        dom_node_t *child = node->first_child;
        while (child) {
            if (sp < 256) stack[sp++] = child;
            child = child->next_sibling;
        }
    }
    return NULL;
}

dom_node_t *dom_find_by_tag(dom_tree_t *tree, const char *tag) {
    return dom_find_first_by_tag(tree, tag);
}

dom_node_t *dom_find_first_by_tag(dom_tree_t *tree, const char *tag) {
    if (!tree || !tree->root || !tag) return NULL;
    dom_node_t *stack[256];
    int sp = 0;
    stack[sp++] = tree->root;
    while (sp > 0) {
        dom_node_t *node = stack[--sp];
        if (node->type == DOM_NODE_ELEMENT && strcmp(node->tag, tag) == 0) {
            return node;
        }
        dom_node_t *child = node->first_child;
        while (child) {
            if (sp < 256) stack[sp++] = child;
            child = child->next_sibling;
        }
    }
    return NULL;
}

const char *dom_get_attribute(const dom_node_t *node, const char *name) {
    if (!node || !name) return NULL;
    if (strcmp(name, "id") == 0) return node->id;
    if (strcmp(name, "class") == 0) return node->class;
    if (strcmp(name, "href") == 0) return node->href;
    if (strcmp(name, "src") == 0) return node->src;
    if (strcmp(name, "alt") == 0) return node->alt;
    if (strcmp(name, "style") == 0) return node->style;
    if (strcmp(name, "onclick") == 0) return node->onclick;
    if (strcmp(name, "onload") == 0) return node->onload;
    if (strcmp(name, "type") == 0) return node->input_type;
    if (strcmp(name, "value") == 0) return node->value;
    if (strcmp(name, "placeholder") == 0) return node->placeholder;
    if (strcmp(name, "checked") == 0) return node->checked ? "checked" : "";
    if (strcmp(name, "disabled") == 0) return node->disabled ? "disabled" : "";
    return NULL;
}

void dom_set_attribute(dom_node_t *node, const char *name, const char *value) {
    bool style_selector = false;

    if (!node || !name || !value) return;
    if (strcmp(name, "id") == 0) {
        strncpy(node->id, value, sizeof(node->id) - 1);
        style_selector = true;
    } else if (strcmp(name, "class") == 0) {
        strncpy(node->class, value, sizeof(node->class) - 1);
        style_selector = true;
    } else if (strcmp(name, "href") == 0) {
        strncpy(node->href, value, sizeof(node->href) - 1);
    } else if (strcmp(name, "src") == 0) {
        strncpy(node->src, value, sizeof(node->src) - 1);
    } else if (strcmp(name, "alt") == 0) {
        strncpy(node->alt, value, sizeof(node->alt) - 1);
    } else if (strcmp(name, "style") == 0) {
        snprintf(node->style, sizeof(node->style), "%s", value);
        css_apply_inline_text(node, value);
    } else if (strcmp(name, "onclick") == 0) {
        strncpy(node->onclick, value, sizeof(node->onclick) - 1);
    } else if (strcmp(name, "onload") == 0) {
        strncpy(node->onload, value, sizeof(node->onload) - 1);
    } else if (strcmp(name, "type") == 0) {
        strncpy(node->input_type, value, sizeof(node->input_type) - 1);
    } else if (strcmp(name, "value") == 0) {
        strncpy(node->value, value, sizeof(node->value) - 1);
    } else if (strcmp(name, "placeholder") == 0) {
        strncpy(node->placeholder, value, sizeof(node->placeholder) - 1);
    } else if (strcmp(name, "checked") == 0) {
        node->checked = (value[0] != '\0' && strcmp(value, "false") != 0);
    } else if (strcmp(name, "disabled") == 0) {
        node->disabled = (value[0] != '\0' && strcmp(value, "false") != 0);
    }
    if (style_selector) {
        /* A class/id change can promote or demote selector matches. Re-resolve
         * this subtree from the last stylesheet immediately so the next render
         * sees the new computed style without a full-tree recompute. */
        dom_mark_style_dirty();
        css_propagate_mutation(node);
    }
}

/* Append @p node's descendant text into @p buf in document order. Iterative in
 * effect (each child is consumed as scanned), so a nested element's text is never
 * copied onto itself (the old version memcpy'd the shared buffer onto itself). */
static void dom_collect_text(const dom_node_t *node, char *buf, size_t cap, size_t *pos)
{
    const dom_node_t *child;

    for (child = node->first_child; child != NULL; child = child->next_sibling) {
        if (child->type == DOM_NODE_TEXT) {
            size_t len = strlen(child->text);
            if (*pos + len < cap - 1) {
                memcpy(buf + *pos, child->text, len);
                *pos += len;
            }
        } else {
            dom_collect_text(child, buf, cap, pos);
        }
    }
}

/** Scratch arena for dom_get_text(). It is bulk data (4 KiB) that was a
 * static .bss table on every board; allocate it lazily from PSRAM instead so
 * the scarce internal DRAM stays free. p4heap's any-pool prefers PSRAM and
 * falls back to malloc, so a failed PSRAM request still renders. The small
 * static fallback keeps the "valid until the next call" contract even under
 * extreme OOM (text is truncated, never NULL). */
#define DOM_TEXT_SCRATCH_BYTES 4096
static char *s_dom_text_scratch;
static char s_dom_text_fallback[128];

const char *dom_get_text(const dom_node_t *node) {
    char *buf;
    size_t pos = 0;
    size_t cap;

    if (!node) return "";
    if (node->type == DOM_NODE_TEXT) return node->text;

    if (s_dom_text_scratch == NULL) {
        s_dom_text_scratch = p4heap_alloc_any(DOM_TEXT_SCRATCH_BYTES);
    }
    buf = (s_dom_text_scratch != NULL) ? s_dom_text_scratch : s_dom_text_fallback;
    cap = (buf == s_dom_text_scratch) ? (size_t)DOM_TEXT_SCRATCH_BYTES
                                      : sizeof(s_dom_text_fallback);

    buf[0] = '\0';
    dom_collect_text(node, buf, cap, &pos);
    buf[pos] = '\0';
    return buf;
}

void dom_set_text(dom_node_t *node, const char *text) {
    if (!node || !text) return;
    if (node->type == DOM_NODE_TEXT) {
        strncpy(node->text, text, sizeof(node->text) - 1);
        return;
    }
    dom_node_t *child = node->first_child;
    while (child) {
        dom_node_t *next = child->next_sibling;
        dom_node_remove_child(node, child);
        dom_node_free(child);
        child = next;
    }
    dom_node_t *text_node = dom_node_new_in(node->arena, DOM_NODE_TEXT);
    if (text_node) {
        strncpy(text_node->text, text, sizeof(text_node->text) - 1);
        dom_node_append_child(node, text_node);
    }
}

/** Apply one CSS colour to a node's background, keeping its alpha so
 *  `rgba()`/`transparent` reach the renderer instead of being flattened. */
static void dom_css_background_colour(dom_node_t *node, const char *value)
{
    css_rgba_t color;

    if (!css_color_parse(value, &color)) {
        return;
    }
    node->bg = css_color_to_dos(&color);
    node->bg_alpha = color.a;
    node->bg_set = true;
}

static void dom_css_background(dom_node_t *node, const char *value)
{
    const char *url;

    if (node == NULL || value == NULL) {
        return;
    }
    url = strstr(value, "url(");
    if (url == NULL) {
        dom_css_background_colour(node, value);
        return;
    }
    url += 4;
    while (*url == ' ' || *url == '\'' || *url == '"') {
        url++;
    }
    {
        size_t i = 0;

        while (url[i] != '\0' && url[i] != ')' && url[i] != '\'' &&
               url[i] != '"' && i + 1 < sizeof(node->bg_src)) {
            node->bg_src[i] = url[i];
            i++;
        }
        node->bg_src[i] = '\0';
    }
    {
        const char *close = strchr(url, ')');

        if (close != NULL) {
            dom_css_background_colour(node, close + 1);
        }
    }
}

void dom_set_style(dom_node_t *node, const char *property, const char *value) {
    if (!node || !property || !value) return;
    if (strcmp(property, "display") == 0) {
        if (strcmp(value, "none") == 0) node->display = DOM_DISPLAY_NONE;
        else if (strcmp(value, "grid") == 0) node->display = DOM_DISPLAY_GRID;
        else if (strcmp(value, "flex") == 0) node->display = DOM_DISPLAY_FLEX;
        else if (strcmp(value, "block") == 0) node->display = DOM_DISPLAY_BLOCK;
        else if (strcmp(value, "inline") == 0) node->display = DOM_DISPLAY_INLINE;
    } else if (strcmp(property, "grid-template-columns") == 0 ||
               strcmp(property, "grid-auto-flow") == 0) {
        /* One responsive mapping: count the declared tracks, clamp to the
         * smallest grid that still fits an 80-column page (8 cells/track). */
        int tracks = 1;
        const char *p = value;

        while (*p) {
            if (*p == 'r' && strncmp(p, "repeat", 6) == 0) {
                const char *n = strchr(p, '(');
                if (n != NULL) {
                    int count = atoi(n + 1);

                    tracks = count > 0 ? count : tracks;
                }
                break;
            }
            if (*p == ')' || *p == '(') { p++; continue; }
            if (*p >= '0' && *p <= '9' && (p == value || !isdigit((unsigned char)p[-1]))) {
                int n = atoi(p);
                if (n > tracks) tracks = n;
            }
            p++;
        }
        if (tracks > 10) tracks = 10;
        if (tracks > 0) node->grid_columns = (uint8_t)tracks;
    } else if (strcmp(property, "border-radius") == 0) {
        int radius = css_length_px(value, 4);

        if (radius < 0) radius = 0;
        if (radius > 8) radius = 8;         /* cell grid: beyond 8 is a pill */
        node->border_radius = (uint8_t)radius;
    } else if (strcmp(property, "box-shadow") == 0) {
        css_rgba_t shadow;
        const char *q;
        const char *colour = NULL;

        q = value;
        while (*q) {
            if (*q == '#' || strncmp(q, "rgb", 3) == 0 ||
                strncmp(q, "red", 3) == 0 || strncmp(q, "black", 5) == 0 ||
                strncmp(q, "white", 5) == 0 || strncmp(q, "transparent", 11) == 0) {
                const char *end = q;

                while (*end && *end != ' ' && *end != ',') end++;
                if (colour == NULL || *q == '#') colour = q;
                {
                    char token[64] = { 0 };
                    size_t n = (size_t)(end - q);

                    if (n >= sizeof(token)) n = sizeof(token) - 1;
                    memcpy(token, q, n);
                    if (css_color_parse(token, &shadow)) {
                        node->shadow_dos = css_color_to_dos(&shadow);
                        node->shadow_alpha = shadow.a;
                        node->shadow_set = true;
                    }
                }
                q = end;
                continue;
            }
            q++;
        }
        (void)colour;
        /* Offsets are the first one or two numeric tokens (px/em/unitless). */
        {
            int nums[2] = { 1, 1 };
            int n = 0;
            const char *p = value;

            while (*p && n < 2) {
                if (*p == '-' || (*p >= '0' && *p <= '9')) {
                    nums[n++] = css_length_px(p, 1);
                    while (*p && *p != ' ' && *p != ',') p++;
                } else {
                    p++;
                }
            }
            node->shadow_x = nums[0];
            node->shadow_y = nums[1];
        }
        if (!node->shadow_set) node->shadow_x = node->shadow_y = 0;
    } else if (strcmp(property, "color") == 0) {
        css_rgba_t color;

        if (css_color_parse(value, &color)) {
            node->fg = css_color_to_dos(&color);
            node->fg_alpha = color.a;
        }
    } else if (strcmp(property, "opacity") == 0) {
        /* CSS opacity multiplies the subtree, it does not replace it. */
        double amount = strtod(value, NULL);

        if (amount < 0.0) amount = 0.0;
        if (amount > 1.0) amount = 1.0;
        node->opacity = (uint8_t)(amount * 255.0 + 0.5);
    } else if (strcmp(property, "background-color") == 0) {
        dom_css_background_colour(node, value);
    } else if (strcmp(property, "background-image") == 0 ||
               strcmp(property, "background") == 0) {
        css_gradient_t gradient;

        if (css_gradient_parse(value, &gradient)) {
            node->gradient = gradient;
            node->gradient_set = true;
        } else {
            dom_css_background(node, value);
        }
    } else if (strcmp(property, "text-align") == 0) {
        if (strcmp(value, "center") == 0) node->align = 1;
        else if (strcmp(value, "right") == 0) node->align = 2;
        else node->align = 0;
    } else if (strcmp(property, "font-size") == 0) {
        if (strstr(value, "3") != NULL || strstr(value, "x-large") != NULL) node->font_scale = 3;
        else if (strstr(value, "2") != NULL || strstr(value, "large") != NULL) node->font_scale = 2;
        else node->font_scale = 1;
        if (strchr(value, 'p') != NULL || strchr(value, 'e') != NULL) {
            int px = css_length_px(value, 8);

            node->font_scale = px >= 16 ? 3 : (px >= 11 ? 2 : 1);
        }
    } else if (strcmp(property, "border") == 0) {
        node->border = (strcmp(value, "none") != 0 && strcmp(value, "0") != 0);
    } else if (strcmp(property, "border-width") == 0) {
        node->border = (atoi(value) > 0);
    } else if (strcmp(property, "font-weight") == 0) {
        node->bold = (strcmp(value, "bold") == 0 || strcmp(value, "bolder") == 0);
    } else if (strcmp(property, "font-style") == 0) {
        node->italic = (strcmp(value, "italic") == 0 || strcmp(value, "oblique") == 0);
    } else if (strcmp(property, "text-decoration") == 0) {
        node->underline = (strcmp(value, "underline") == 0);
    } else if (strcmp(property, "margin") == 0) {
        node->margin_top = node->margin_bottom = node->margin_left =
            node->margin_right = css_length_px(value, 8);
    } else if (strcmp(property, "margin-top") == 0) {
        node->margin_top = css_length_px(value, 8);
    } else if (strcmp(property, "margin-bottom") == 0) {
        node->margin_bottom = css_length_px(value, 8);
    } else if (strcmp(property, "margin-left") == 0) {
        node->margin_left = css_length_px(value, 8);
    } else if (strcmp(property, "margin-right") == 0) {
        node->margin_right = css_length_px(value, 8);
    } else if (strcmp(property, "padding") == 0) {
        node->padding_top = node->padding_bottom = node->padding_left =
            node->padding_right = css_length_px(value, 8);
    } else if (strcmp(property, "padding-top") == 0) {
        node->padding_top = css_length_px(value, 8);
    } else if (strcmp(property, "padding-bottom") == 0) {
        node->padding_bottom = css_length_px(value, 8);
    } else if (strcmp(property, "padding-left") == 0) {
        node->padding_left = css_length_px(value, 8);
    } else if (strcmp(property, "padding-right") == 0) {
        node->padding_right = css_length_px(value, 8);
    }
}

const char *dom_get_style(const dom_node_t *node, const char *property) {
    if (!node || !property) return "";
    static char buf[64];
    if (strcmp(property, "display") == 0) {
        if (node->display == DOM_DISPLAY_NONE) return "none";
        if (node->display == DOM_DISPLAY_GRID) return "grid";
        if (node->display == DOM_DISPLAY_FLEX) return "flex";
        if (node->display == DOM_DISPLAY_BLOCK) return "block";
        return "inline";
    } else if (strcmp(property, "color") == 0) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)node->fg);
        return buf;
    } else if (strcmp(property, "background-color") == 0) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)node->bg);
        return buf;
    } else if (strcmp(property, "text-align") == 0) {
        return node->align == 1 ? "center" : (node->align == 2 ? "right" : "left");
    } else if (strcmp(property, "font-size") == 0) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)node->font_scale);
        return buf;
    } else if (strcmp(property, "border") == 0) {
        return node->border ? "1" : "0";
    } else if (strcmp(property, "font-weight") == 0) {
        return node->bold ? "bold" : "normal";
    } else if (strcmp(property, "font-style") == 0) {
        return node->italic ? "italic" : "normal";
    } else if (strcmp(property, "text-decoration") == 0) {
        return node->underline ? "underline" : "none";
    } else if (strcmp(property, "margin") == 0) {
        snprintf(buf, sizeof(buf), "%d", node->margin_top);
        return buf;
    } else if (strcmp(property, "padding") == 0) {
        snprintf(buf, sizeof(buf), "%d", node->padding_top);
        return buf;
    } else if (strcmp(property, "opacity") == 0) {
        snprintf(buf, sizeof(buf), "%.3f", (double)node->opacity / 255.0);
        return buf;
    } else if (strcmp(property, "border-radius") == 0) {
        snprintf(buf, sizeof(buf), "%upx", (unsigned)node->border_radius);
        return buf;
    } else if (strcmp(property, "grid-template-columns") == 0) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)node->grid_columns);
        return buf;
    }
    return "";
}

bool dom_node_has_class(const dom_node_t *node, const char *name) {
    const char *p;
    size_t want;

    if (!node || !name || name[0] == '\0') return false;
    want = strlen(name);
    p = node->class;
    while (*p) {
        const char *start;
        size_t len;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        len = (size_t)(p - start);
        if (len == want && strncmp(start, name, len) == 0) return true;
    }
    return false;
}

void dom_add_event_listener(dom_node_t *node, const char *event, const char *handler) {
    if (!node || !event || !handler) return;
    if (strcmp(event, "click") == 0) {
        snprintf(node->onclick, sizeof(node->onclick), "%s", handler);
    } else if (strcmp(event, "load") == 0) {
        snprintf(node->onload, sizeof(node->onload), "%s", handler);
    }
}

/* Event dispatch is inverted through one registration hook: the DOM leaf owns
 * no JS runtime, but a registered engine can evaluate the handler source. */
static dom_event_hook_t s_event_hook = NULL;
static void *s_event_hook_ctx = NULL;

void dom_set_event_hook(dom_event_hook_t hook, void *ctx)
{
    s_event_hook = hook;
    s_event_hook_ctx = ctx;
}

void dom_dispatch_event(dom_node_t *node, const char *event)
{
    const char *handler = NULL;

    if (node == NULL || event == NULL) return;
    if (strcmp(event, "click") == 0 || strcmp(event, "input") == 0 ||
        strcmp(event, "change") == 0 || strcmp(event, "submit") == 0) {
        handler = node->onclick;
    } else if (strcmp(event, "load") == 0) {
        handler = node->onload;
    }
    if (s_event_hook != NULL && handler != NULL && handler[0] != '\0') {
        s_event_hook(s_event_hook_ctx, node, event, handler);
    }
    /* Document-level listeners see bubbling events from any node. */
    if (s_event_hook != NULL) {
        s_event_hook(s_event_hook_ctx, node, event, NULL);
    }
}

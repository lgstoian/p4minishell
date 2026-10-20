#include "dom.h"
#include "strutil.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

dom_node_t *dom_node_new(dom_node_type_t type) {
    dom_node_t *node = calloc(1, sizeof(dom_node_t));
    if (!node) return NULL;
    node->type = type;
    node->display = DOM_DISPLAY_INLINE;
    node->fg = 16;
    node->bg = 16;
    return node;
}

void dom_node_free(dom_node_t *node) {
    if (!node) return;
    dom_node_t *child = node->first_child;
    while (child) {
        dom_node_t *next = child->next_sibling;
        dom_node_free(child);
        child = next;
    }
    free(node);
}

void dom_node_append_child(dom_node_t *parent, dom_node_t *child) {
    if (!parent || !child) return;
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

static void parse_tag_name(const char **p, char *out, size_t out_size) {
    size_t i = 0;
    while (**p && !isspace((unsigned char)**p) && **p != '>' && **p != '/' && i < out_size - 1) {
        out[i++] = tolower((unsigned char)**p);
        (*p)++;
    }
    out[i] = '\0';
}

static void parse_attributes(const char **p, dom_node_t *node) {
    skip_whitespace(p);
    while (**p && **p != '>' && **p != '/') {
        char attr_name[64] = {0};
        char attr_value[512] = {0};
        size_t i = 0;
        while (**p && !isspace((unsigned char)**p) && **p != '=' && **p != '>' && **p != '/' && i < sizeof(attr_name) - 1) {
            attr_name[i++] = tolower((unsigned char)**p);
            (*p)++;
        }
        attr_name[i] = '\0';
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
            while (**p && (quote ? **p != quote : (!isspace((unsigned char)**p) && **p != '>' && **p != '/')) && i < sizeof(attr_value) - 1) {
                attr_value[i++] = **p;
                (*p)++;
            }
            attr_value[i] = '\0';
            if (quote && **p == quote) (*p)++;
        }
        if (strcmp(attr_name, "id") == 0) {
            strncpy(node->id, attr_value, sizeof(node->id) - 1);
        } else if (strcmp(attr_name, "class") == 0) {
            strncpy(node->class, attr_value, sizeof(node->class) - 1);
        } else if (strcmp(attr_name, "href") == 0) {
            strncpy(node->href, attr_value, sizeof(node->href) - 1);
        } else if (strcmp(attr_name, "src") == 0) {
            strncpy(node->src, attr_value, sizeof(node->src) - 1);
        } else if (strcmp(attr_name, "alt") == 0) {
            strncpy(node->alt, attr_value, sizeof(node->alt) - 1);
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
    static const char *block_tags[] = {"address","article","aside","blockquote","body","br","dd","div","dl","dt","fieldset","figcaption","figure","footer","form","h1","h2","h3","h4","h5","h6","header","hr","html","li","main","nav","ol","p","pre","section","table","tbody","td","tfoot","th","thead","tr","ul",NULL};
    for (int i = 0; block_tags[i]; i++) {
        if (strcmp(tag, block_tags[i]) == 0) return true;
    }
    return false;
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

static dom_node_t *parse_element(const char **p, dom_node_t *parent, int depth) {
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
    dom_node_t *node = dom_node_new(DOM_NODE_ELEMENT);
    if (!node) return NULL;
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
        }
        if (**p == '<') {
            dom_node_t *child = parse_element(p, node, depth + 1);
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
                dom_node_t *text_node = dom_node_new(DOM_NODE_TEXT);
                if (text_node) {
                    size_t len = (size_t)(end - start);
                    if (len >= sizeof(text_node->text)) {
                        len = sizeof(text_node->text) - 1;
                    }
                    memcpy(text_node->text, start, len);
                    text_node->text[len] = '\0';
                    dom_node_append_child(node, text_node);
                }
            }
            *p = end;
        }
    }
    return node;
}

dom_tree_t *dom_parse(const char *html) {
    if (!html) return NULL;
    dom_tree_t *tree = calloc(1, sizeof(dom_tree_t));
    if (!tree) return NULL;
    tree->root = dom_node_new(DOM_NODE_ELEMENT);
    if (!tree->root) {
        free(tree);
        return NULL;
    }
    strcpy(tree->root->tag, "#document");
    const char *p = html;
    while (*p) {
        skip_whitespace(&p);
        if (*p == '<') {
            dom_node_t *child = parse_element(&p, tree->root, 0);
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

void dom_free(dom_tree_t *tree) {
    if (!tree) return;
    dom_node_free(tree->root);
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
    return NULL;
}

void dom_set_attribute(dom_node_t *node, const char *name, const char *value) {
    if (!node || !name || !value) return;
    if (strcmp(name, "id") == 0) {
        strncpy(node->id, value, sizeof(node->id) - 1);
    } else if (strcmp(name, "class") == 0) {
        strncpy(node->class, value, sizeof(node->class) - 1);
    } else if (strcmp(name, "href") == 0) {
        strncpy(node->href, value, sizeof(node->href) - 1);
    } else if (strcmp(name, "src") == 0) {
        strncpy(node->src, value, sizeof(node->src) - 1);
    } else if (strcmp(name, "alt") == 0) {
        strncpy(node->alt, value, sizeof(node->alt) - 1);
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

const char *dom_get_text(const dom_node_t *node) {
    if (!node) return "";
    if (node->type == DOM_NODE_TEXT) return node->text;
    static char buf[4096];
    size_t pos = 0;
    buf[0] = '\0';
    dom_collect_text(node, buf, sizeof(buf), &pos);
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
    dom_node_t *text_node = dom_node_new(DOM_NODE_TEXT);
    if (text_node) {
        strncpy(text_node->text, text, sizeof(text_node->text) - 1);
        dom_node_append_child(node, text_node);
    }
}

void dom_set_style(dom_node_t *node, const char *property, const char *value) {
    if (!node || !property || !value) return;
    if (strcmp(property, "display") == 0) {
        if (strcmp(value, "none") == 0) node->display = DOM_DISPLAY_NONE;
        else if (strcmp(value, "block") == 0) node->display = DOM_DISPLAY_BLOCK;
        else if (strcmp(value, "inline") == 0) node->display = DOM_DISPLAY_INLINE;
    } else if (strcmp(property, "font-weight") == 0) {
        node->bold = (strcmp(value, "bold") == 0 || strcmp(value, "bolder") == 0);
    } else if (strcmp(property, "font-style") == 0) {
        node->italic = (strcmp(value, "italic") == 0 || strcmp(value, "oblique") == 0);
    } else if (strcmp(property, "text-decoration") == 0) {
        node->underline = (strcmp(value, "underline") == 0);
    } else if (strcmp(property, "margin") == 0) {
        node->margin_top = node->margin_bottom = node->margin_left = node->margin_right = atoi(value);
    } else if (strcmp(property, "margin-top") == 0) {
        node->margin_top = atoi(value);
    } else if (strcmp(property, "margin-bottom") == 0) {
        node->margin_bottom = atoi(value);
    } else if (strcmp(property, "margin-left") == 0) {
        node->margin_left = atoi(value);
    } else if (strcmp(property, "margin-right") == 0) {
        node->margin_right = atoi(value);
    } else if (strcmp(property, "padding") == 0) {
        node->padding_top = node->padding_bottom = node->padding_left = node->padding_right = atoi(value);
    } else if (strcmp(property, "padding-top") == 0) {
        node->padding_top = atoi(value);
    } else if (strcmp(property, "padding-bottom") == 0) {
        node->padding_bottom = atoi(value);
    } else if (strcmp(property, "padding-left") == 0) {
        node->padding_left = atoi(value);
    } else if (strcmp(property, "padding-right") == 0) {
        node->padding_right = atoi(value);
    }
}

const char *dom_get_style(const dom_node_t *node, const char *property) {
    if (!node || !property) return "";
    static char buf[64];
    if (strcmp(property, "display") == 0) {
        if (node->display == DOM_DISPLAY_NONE) return "none";
        if (node->display == DOM_DISPLAY_BLOCK) return "block";
        return "inline";
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
    }
    return "";
}

void dom_add_event_listener(dom_node_t *node, const char *event, const char *handler) {
    (void)node;
    (void)event;
    (void)handler;
}

void dom_dispatch_event(dom_node_t *node, const char *event) {
    (void)node;
    (void)event;
}

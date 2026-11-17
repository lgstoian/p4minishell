#ifndef P4MINISHELL_DOM_H
#define P4MINISHELL_DOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dom_arena.h"

typedef enum {
    DOM_NODE_ELEMENT,
    DOM_NODE_TEXT,
} dom_node_type_t;

typedef enum {
    DOM_DISPLAY_BLOCK,
    DOM_DISPLAY_INLINE,
    DOM_DISPLAY_NONE,
} dom_display_t;

typedef struct dom_node {
    dom_node_type_t type;
    char tag[32];
    char id[64];
    char class[128];
    char href[512];
    char src[512];
    char alt[256];
    char text[1024];
    char style[256];      /* raw style="..." attribute (inline CSS, applied last) */
    char bg_src[512];     /* CSS background-image URL ("" = none) */
    int canvas_w;         /* <canvas width> attribute (0 = spec default) */
    int canvas_h;         /* <canvas height> attribute (0 = spec default) */
    char onclick[512];    /* inline click handler (JS) */
    char onload[512];     /* inline load handler (JS) */
    dom_display_t display;
    uint8_t fg;
    uint8_t bg;
    uint8_t fg_alpha;     /* 0-255; 255 opaque. Honoured when the node sets fg. */
    uint8_t bg_alpha;     /* 0-255; 0 = `transparent`. Honoured when bg_set. */
    uint8_t opacity;      /* CSS `opacity`, multiplies the whole subtree. */
    bool bg_set;         /* an explicit CSS background colour (not inherited) */
    uint8_t align;        /* 0 left, 1 center, 2 right */
    uint8_t font_scale;   /* 1 normal, 2 heading/large */
    bool bold;
    bool italic;
    bool underline;
    bool border;          /* draw a box border around the block */
    int margin_top;
    int margin_bottom;
    int margin_left;
    int margin_right;
    int padding_top;
    int padding_bottom;
    int padding_left;
    int padding_right;
    struct dom_node *parent;
    struct dom_node *first_child;
    struct dom_node *last_child;
    struct dom_node *next_sibling;
    struct dom_node *prev_sibling;
    dom_arena_t *arena;      /**< non-NULL when the node came from an arena; such a
                                  node is released by unlinking, not by free(). */
} dom_node_t;

typedef struct dom_tree {
    dom_node_t *root;
    dom_node_t *head;
    dom_node_t *body;
    char title[256];
    dom_arena_t *arena;      /**< the arena the parsed nodes came from, or NULL */
} dom_tree_t;

dom_node_t *dom_node_new(dom_node_type_t type);

/** Same as dom_node_new(), but the node comes from @p arena when it has one and
 *  falls back to a plain calloc() when the arena is exhausted. Arena-owned
 *  nodes are released by dom_arena_destroy(), not by dom_node_free(). */
dom_node_t *dom_node_new_in(dom_arena_t *arena, dom_node_type_t type);
void dom_node_free(dom_node_t *node);
void dom_node_append_child(dom_node_t *parent, dom_node_t *child);
void dom_node_remove_child(dom_node_t *parent, dom_node_t *child);

dom_tree_t *dom_parse(const char *html);
void dom_free(dom_tree_t *tree);

dom_node_t *dom_find_by_id(dom_tree_t *tree, const char *id);
dom_node_t *dom_find_by_tag(dom_tree_t *tree, const char *tag);
dom_node_t *dom_find_first_by_tag(dom_tree_t *tree, const char *tag);

const char *dom_get_attribute(const dom_node_t *node, const char *name);
void dom_set_attribute(dom_node_t *node, const char *name, const char *value);

const char *dom_get_text(const dom_node_t *node);
void dom_set_text(dom_node_t *node, const char *text);

void dom_set_style(dom_node_t *node, const char *property, const char *value);
const char *dom_get_style(const dom_node_t *node, const char *property);

void dom_add_event_listener(dom_node_t *node, const char *event, const char *handler);
void dom_dispatch_event(dom_node_t *node, const char *event);

/** True when @p node's class list contains @p name (space/tab separated). */
bool dom_node_has_class(const dom_node_t *node, const char *name);

#endif

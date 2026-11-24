#ifndef P4MINISHELL_DOM_H
#define P4MINISHELL_DOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dom_arena.h"
#include "css_color.h"

typedef enum {
    DOM_NODE_ELEMENT,
    DOM_NODE_TEXT,
} dom_node_type_t;

typedef enum {
    DOM_DISPLAY_BLOCK,
    DOM_DISPLAY_INLINE,
    DOM_DISPLAY_NONE,
    DOM_DISPLAY_GRID,    /* CSS grid subset, mapped to the 80-column cell grid */
    DOM_DISPLAY_FLEX,    /* inline flow subset */
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
    /* Additive HTML5 form/table/visual state. All values are bounded and the
     * visual ones still quantise through css_color_to_dos() (one palette). */
    char input_type[24];  /* <input type>, <button type> (`type` is the node enum) */
    char value[256];      /* form control value */
    char placeholder[128];
    bool checked;
    bool disabled;
    bool required;
    int col_span;
    int row_span;
    uint8_t border_radius;
    bool shadow_set;
    uint8_t shadow_dos;
    int shadow_x;
    int shadow_y;
    uint8_t shadow_alpha;
    bool gradient_set;
    css_gradient_t gradient;
    uint8_t grid_columns; /* computed responsive column count (>=1) */
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
    /* Computed-style cache bookkeeping. No second style store: the resolved
     * values live in dom_node_t. These four fields only say which stylesheet
     * version / DOM mutation epoch those values came from. */
    const void *style_rules; /**< css_ruleset_t* of the last css_compute_styles */
    uint32_t style_rules_version;
    uint32_t style_epoch;
    bool style_computed;
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

/** Monotonic DOM mutation epoch. css_compute_styles() snapshots it and skips
 *  redundant recomputation when neither the stylesheet nor the DOM changed. */
uint32_t dom_style_epoch(void);
/** Mark the computed styles stale (used by the JS style setter and mutations
 *  that add/remove nodes). */
void dom_mark_style_dirty(void);
/** Reset only the computed-style fields of @p node to their element defaults;
 *  attributes and text survive. Used before re-resolving the cascade. */
void dom_node_reset_style(dom_node_t *node);

const char *dom_get_text(const dom_node_t *node);
void dom_set_text(dom_node_t *node, const char *text);

void dom_set_style(dom_node_t *node, const char *property, const char *value);
const char *dom_get_style(const dom_node_t *node, const char *property);

void dom_add_event_listener(dom_node_t *node, const char *event, const char *handler);
void dom_dispatch_event(dom_node_t *node, const char *event);

/** True when @p node's class list contains @p name (space/tab separated). */
bool dom_node_has_class(const dom_node_t *node, const char *name);

/** Inverted dependency: the JS engine registers one event hook so the DOM can
 *  dispatch click/load/input/change handlers without depending on QuickJS. */
typedef void (*dom_event_hook_t)(void *ctx, dom_node_t *node, const char *event,
                                 const char *handler);
void dom_set_event_hook(dom_event_hook_t hook, void *ctx);

#endif

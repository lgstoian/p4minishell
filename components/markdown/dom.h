#ifndef P4MINISHELL_DOM_H
#define P4MINISHELL_DOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    dom_display_t display;
    uint8_t fg;
    uint8_t bg;
    bool bold;
    bool italic;
    bool underline;
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
} dom_node_t;

typedef struct {
    dom_node_t *root;
    dom_node_t *head;
    dom_node_t *body;
    char title[256];
} dom_tree_t;

dom_node_t *dom_node_new(dom_node_type_t type);
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

#endif

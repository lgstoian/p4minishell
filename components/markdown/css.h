#ifndef P4MINISHELL_CSS_H
#define P4MINISHELL_CSS_H

#include "dom.h"
#include <stdbool.h>
#include <stddef.h>

#define CSS_MAX_RULES 64
#define CSS_MAX_SELECTOR_LEN 128
#define CSS_MAX_PROPERTY_LEN 64
#define CSS_MAX_VALUE_LEN 256

typedef struct {
    char selector[CSS_MAX_SELECTOR_LEN];
    char property[CSS_MAX_PROPERTY_LEN];
    char value[CSS_MAX_VALUE_LEN];
    int specificity;   /* id=100, class/attr=10, tag=1; higher wins */
} css_rule_t;

typedef struct {
    css_rule_t rules[CSS_MAX_RULES];
    int count;
} css_ruleset_t;

void css_parse_inline(const char *style_attr, css_ruleset_t *rules);
void css_parse_block(const char *css_text, css_ruleset_t *rules);
/** Apply a `prop: value; ...` declaration string directly to @p node (no
 *  ruleset, so no 28 KB stack local). Shared with the JS `style` setter. */
void css_apply_inline_text(dom_node_t *node, const char *style);
void css_apply(dom_node_t *node, const css_ruleset_t *rules);
void css_compute_styles(dom_tree_t *tree, const css_ruleset_t *rules);

#endif

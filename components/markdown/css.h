#ifndef P4MINISHELL_CSS_H
#define P4MINISHELL_CSS_H

#include "dom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CSS_MAX_RULES 64
#define CSS_MAX_SELECTOR_LEN 128
#define CSS_MAX_PROPERTY_LEN 64
#define CSS_MAX_VALUE_LEN 256

/**
 * Packed specificity: (id << 16) | (class_or_attr << 8) | type, each component
 * clamped to 255. A triple beats the old 100/10/1 scalar: eleven classes can
 * no longer outrank one id. `important` and `order` are the other two cascade
 * inputs; css_rule_compare() is their ONE comparator (shared by css_apply and
 * the `browser css` inspection command).
 */
typedef struct {
    char selector[CSS_MAX_SELECTOR_LEN];
    char property[CSS_MAX_PROPERTY_LEN];
    char value[CSS_MAX_VALUE_LEN];
    int specificity;            /* packed id/class/type triple, see above */
    bool important;             /* a trailing `!important` was stripped */
    int order;                  /* declaration order in the stylesheet */
} css_rule_t;

typedef struct {
    css_rule_t rules[CSS_MAX_RULES];
    int count;
    uint32_t version;           /* bumped on every parse, for the style cache */
} css_ruleset_t;

/** Unpack the specificity triple. Bounded helpers, no second matcher. */
int css_specificity_id(const css_rule_t *rule);
int css_specificity_class(const css_rule_t *rule);
int css_specificity_type(const css_rule_t *rule);

/** The ONLY cascade comparator: negative when @p a loses to @p b, positive
 *  when @p a wins, zero when they are the same declaration. Order of
 *  precedence: !important, then id/class/type specificity, then source order. */
int css_rule_compare(const css_rule_t *a, const css_rule_t *b);

/** The one selector matcher, exposed so `browser css` can report which rule
 *  the comparator picks for a node without a second matching implementation. */
bool css_selector_matches(const char *selector, const dom_node_t *node);

/** Convert a CSS length (px / em / rem / % / unitless) to whole cells.
 *  @p base is the value of 1em and 100% (8 by default for margins/padding).
 *  One unit parser for the shared CSS subset. */
int css_length_px(const char *value, int base);

void css_parse_inline(const char *style_attr, css_ruleset_t *rules);
void css_parse_block(const char *css_text, css_ruleset_t *rules);
/** Apply a `prop: value; ...` declaration string directly to @p node (no
 *  ruleset, so no 28 KB stack local). Shared with the JS `style` setter. */
void css_apply_inline_text(dom_node_t *node, const char *style);
void css_apply(dom_node_t *node, const css_ruleset_t *rules);
void css_compute_styles(dom_tree_t *tree, const css_ruleset_t *rules);

/** Computed-style cache control. The computed values stay in the DOM node;
 *  only the "this tree was resolved with this ruleset at this mutation epoch"
 *  fact is remembered, so a redundant css_compute_styles() is skipped. */
void css_style_cache_clear(void);
void css_forget_tree(const dom_tree_t *tree);
/** Re-resolve a node (and its descendants) after a class/id/style mutation. */
void css_propagate_mutation(dom_node_t *node);

#endif

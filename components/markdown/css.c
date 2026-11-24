#include "css.h"
#include "strutil.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void skip_ws(const char **p) {
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

int css_length_px(const char *value, int base)
{
    double number;
    char *end = NULL;

    if (value == NULL || base <= 0) return 0;
    number = strtod(value, &end);
    while (*end == ' ') end++;
    if (strcasecmp(end, "em") == 0 || strcasecmp(end, "rem") == 0) {
        number *= (double)base;
    } else if (*end == '%') {
        number = number * (double)base / 100.0;
    } else if (strcasecmp(end, "vw") == 0) {
        number = number * 80.0 / 100.0;   /* one logical page = 80 columns */
    }
    if (number > 1000.0) number = 1000.0;
    if (number < -1000.0) number = -1000.0;
    return (int)(number < 0.0 ? number - 0.5 : number + 0.5);
}

static void add_rule(css_ruleset_t *rules, const char *selector,
                     const char *property, const char *value, bool important) {
    css_rule_t *rule;
    int ids = 0;
    int classes = 0;
    int types = 0;
    int depth = 0;
    char quote = 0;
    const char *p;

    if (!rules || rules->count >= CSS_MAX_RULES) return;
    rule = &rules->rules[rules->count];
    memset(rule, 0, sizeof(*rule));
    strncpy(rule->selector, selector, CSS_MAX_SELECTOR_LEN - 1);
    strncpy(rule->property, property, CSS_MAX_PROPERTY_LEN - 1);
    strncpy(rule->value, value, CSS_MAX_VALUE_LEN - 1);

    /* One specificity walk: (id, class/attr/pseudo, type), each clamped to
     * 255 so the packed triple cannot bleed into its neighbour. Comma lists
     * take the highest part; attribute values never count as tag tokens. */
    {
        bool at_start = true;

        for (p = selector; *p; p++) {
            char c = *p;

            if (quote != 0) {
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                at_start = false;
                continue;
            }
            if (c == '[') {
                depth++;
                if (classes < 255) classes++;
                at_start = false;
                continue;
            }
            if (c == ']') {
                if (depth > 0) depth--;
                continue;
            }
            if (depth > 0) continue;
            if (c == '#') {
                if (ids < 255) ids++;
                at_start = false;
            } else if (c == '.' || c == ':') {
                if (classes < 255) classes++;
                at_start = false;
            } else if (c == '>' || c == '+' || c == '~' || c == ',' ||
                       isspace((unsigned char)c)) {
                at_start = true;     /* next identifier starts a new compound */
            } else if (isalpha((unsigned char)c)) {
                if (at_start && types < 255) types++;
                at_start = false;
            } else if (!isalnum((unsigned char)c) && c != '-' && c != '_') {
                at_start = false;
            }
        }
    }
    rule->specificity = (ids << 16) | (classes << 8) | types;
    rule->important = important;
    rule->order = rules->count;
    rules->count++;
    rules->version++;
}

int css_specificity_id(const css_rule_t *rule)
{
    return rule != NULL ? (rule->specificity >> 16) & 0xFF : 0;
}

int css_specificity_class(const css_rule_t *rule)
{
    return rule != NULL ? (rule->specificity >> 8) & 0xFF : 0;
}

int css_specificity_type(const css_rule_t *rule)
{
    return rule != NULL ? rule->specificity & 0xFF : 0;
}

int css_rule_compare(const css_rule_t *a, const css_rule_t *b)
{
    if (a == NULL || b == NULL) return 0;
    if (a->important != b->important) return a->important ? 1 : -1;
    if (a->specificity != b->specificity) {
        return a->specificity > b->specificity ? 1 : -1;
    }
    if (a->order != b->order) return a->order > b->order ? 1 : -1;
    return 0;
}

static void parse_declaration(css_ruleset_t *rules, const char *selector,
                              const char *decl) {
    const char *colon = strchr(decl, ':');
    if (!colon) return;
    char property[CSS_MAX_PROPERTY_LEN] = {0};
    size_t plen = colon - decl;
    if (plen >= sizeof(property)) plen = sizeof(property) - 1;
    memcpy(property, decl, plen);
    property[plen] = '\0';
    char *p = property + plen - 1;
    while (p >= property && isspace((unsigned char)*p)) *p-- = '\0';
    p = property;
    while (*p && isspace((unsigned char)*p)) p++;
    char value[CSS_MAX_VALUE_LEN] = {0};
    strncpy(value, colon + 1, sizeof(value) - 1);
    char *v = value;
    bool important = false;
    while (*v && isspace((unsigned char)*v)) v++;
    {
        size_t vlen = strlen(v);

        if (vlen >= 10 && strcasecmp(v + vlen - 10, "!important") == 0) {
            important = true;
            v[vlen - 10] = '\0';
            while (vlen > 0 && isspace((unsigned char)v[vlen - 1])) {
                v[--vlen] = '\0';
            }
        }
    }
    char *vend = v + strlen(v) - 1;
    while (vend >= v && isspace((unsigned char)*vend)) *vend-- = '\0';
    if (strlen(p) > 0 && strlen(v) > 0) {
        add_rule(rules, selector, p, v, important);
    }
}

void css_parse_inline(const char *style_attr, css_ruleset_t *rules) {
    if (!style_attr || !rules) return;
    const char *p = style_attr;
    while (*p) {
        skip_ws(&p);
        const char *semicolon = strchr(p, ';');
        size_t len = semicolon ? (size_t)(semicolon - p) : strlen(p);
        char decl[512] = {0};
        if (len >= sizeof(decl)) len = sizeof(decl) - 1;
        memcpy(decl, p, len);
        decl[len] = '\0';
        parse_declaration(rules, "", decl);
        if (!semicolon) break;
        p = semicolon + 1;
    }
}

void css_parse_block(const char *css_text, css_ruleset_t *rules) {
    if (!css_text || !rules) return;
    const char *p = css_text;
    while (*p) {
        skip_ws(&p);
        /* CSS comments are not rules: skip them (a comment before the first
         * selector otherwise becomes part of that selector and the rule is
         * silently lost). */
        if (p[0] == '/' && p[1] == '*') {
            const char *end = strstr(p + 2, "*/");
            p = (end != NULL) ? end + 2 : p + strlen(p);
            continue;
        }
        if (!*p) break;
        const char *brace = strchr(p, '{');
        if (!brace) break;
        char selector[CSS_MAX_SELECTOR_LEN] = {0};
        size_t slen = brace - p;
        if (slen >= sizeof(selector)) slen = sizeof(selector) - 1;
        memcpy(selector, p, slen);
        selector[slen] = '\0';
        char *s = selector + slen - 1;
        while (s >= selector && isspace((unsigned char)*s)) *s-- = '\0';
        s = selector;
        while (*s && isspace((unsigned char)*s)) s++;
        const char *close = strchr(brace + 1, '}');
        if (!close) break;
        char body[2048] = {0};
        size_t blen = close - (brace + 1);
        if (blen >= sizeof(body)) blen = sizeof(body) - 1;
        memcpy(body, brace + 1, blen);
        body[blen] = '\0';
        const char *decl = body;
        while (*decl) {
            skip_ws(&decl);
            const char *semicolon = strchr(decl, ';');
            size_t len = semicolon ? (size_t)(semicolon - decl) : strlen(decl);
            char d[512] = {0};
            if (len >= sizeof(d)) len = sizeof(d) - 1;
            memcpy(d, decl, len);
            d[len] = '\0';
            parse_declaration(rules, s, d);
            if (!semicolon) break;
            decl = semicolon + 1;
        }
        p = close + 1;
    }
}

/* ---- selector matching -------------------------------------------------
 * Minimal CSS2 selector engine: comma lists, descendant (`a b`) and child
 * (`a > b`) combinators, compound tag/id/class/attribute selectors. Pseudo
 * classes are ignored (stripped) rather than rejected.
 */

static bool simple_matches(const char *sel, const char *end, const dom_node_t *node)
{
    const char *p;
    char tag[32] = {0};
    size_t tl = 0;

    if (!node || node->type != DOM_NODE_ELEMENT) return false;
    while (sel < end && isspace((unsigned char)*sel)) sel++;
    while (end > sel && isspace((unsigned char)end[-1])) end--;
    if (sel >= end) return false;

    p = sel;
    while (p < end && *p != '.' && *p != '#' && *p != '[' && *p != ':') {
        if (tl + 1 < sizeof(tag)) tag[tl++] = (char)tolower((unsigned char)*p);
        p++;
    }
    tag[tl] = '\0';
    if (tl > 0 && strcmp(tag, "*") != 0 && strcasecmp(tag, node->tag) != 0) {
        return false;
    }

    while (p < end) {
        if (*p == '.') {
            char cls[64];
            size_t cl = 0;
            p++;
            while (p < end && *p != '.' && *p != '#' && *p != '[' && *p != ':') {
                if (cl + 1 < sizeof(cls)) cls[cl++] = *p;
                p++;
            }
            cls[cl] = '\0';
            if (!dom_node_has_class(node, cls)) return false;
        } else if (*p == '#') {
            char id[64];
            size_t il = 0;
            p++;
            while (p < end && *p != '.' && *p != '#' && *p != '[' && *p != ':') {
                if (il + 1 < sizeof(id)) id[il++] = *p;
                p++;
            }
            id[il] = '\0';
            if (strcmp(id, node->id) != 0) return false;
        } else if (*p == '[') {
            char attr[64];
            size_t al = 0;
            p++;
            while (p < end && *p != ']' && *p != '=' && !isspace((unsigned char)*p)) {
                if (al + 1 < sizeof(attr)) attr[al++] = (char)tolower((unsigned char)*p);
                p++;
            }
            attr[al] = '\0';
            /* [name=value] / [name="value"] compares the same attribute store
             * the DOM exposes to JS, including the HTML5 form fields. */
            {
                char want[64] = { 0 };
                size_t wl = 0;

                if (p < end && *p == '=') {
                    p++;
                    if (p < end && (*p == '"' || *p == '\'')) {
                        char quote = *p++;

                        while (p < end && *p != quote && wl + 1 < sizeof(want)) {
                            want[wl++] = *p++;
                        }
                        if (p < end) p++;
                    } else {
                        while (p < end && *p != ']' && wl + 1 < sizeof(want)) {
                            want[wl++] = *p++;
                        }
                    }
                }
                want[wl] = '\0';
                while (p < end && *p != ']') p++;
                if (p < end) p++;
                if (wl > 0) {
                    const char *got = dom_get_attribute(node, attr);

                    if (strcmp(attr, "type") == 0) got = node->input_type;
                    else if (strcmp(attr, "value") == 0) got = node->value;
                    else if (strcmp(attr, "placeholder") == 0) got = node->placeholder;
                    else if (strcmp(attr, "checked") == 0) got = node->checked ? "checked" : "";
                    else if (strcmp(attr, "disabled") == 0) got = node->disabled ? "disabled" : "";
                    if (got == NULL || strcmp(got, want) != 0) return false;
                } else if (strcmp(attr, "href") == 0) {
                    if (node->href[0] == '\0') return false;
                } else if (strcmp(attr, "src") == 0) {
                    if (node->src[0] == '\0') return false;
                } else if (strcmp(attr, "id") == 0) {
                    if (node->id[0] == '\0') return false;
                } else if (strcmp(attr, "class") == 0) {
                    if (node->class[0] == '\0') return false;
                } else if (strcmp(attr, "alt") == 0) {
                    if (node->alt[0] == '\0') return false;
                } else if (strcmp(attr, "onclick") == 0) {
                    if (node->onclick[0] == '\0') return false;
                } else if (strcmp(attr, "type") == 0) {
                    if (node->input_type[0] == '\0') return false;
                } else if (strcmp(attr, "value") == 0) {
                    if (node->value[0] == '\0') return false;
                } else if (strcmp(attr, "checked") == 0) {
                    if (!node->checked) return false;
                } else if (strcmp(attr, "disabled") == 0) {
                    if (!node->disabled) return false;
                }
            }
        } else if (*p == ':') {
            char pseudo[32] = { 0 };
            size_t pl = 0;
            bool known = false;

            p++;
            while (p < end && (isalnum((unsigned char)*p) || *p == '-')) {
                if (pl + 1 < sizeof(pseudo)) pseudo[pl++] = *p;
                p++;
            }
            pseudo[pl] = '\0';
            if (strcmp(pseudo, "first-child") == 0) {
                known = true;
                if (node->parent == NULL || node->parent->first_child != node) return false;
            } else if (strcmp(pseudo, "last-child") == 0) {
                known = true;
                if (node->parent == NULL || node->parent->last_child != node) return false;
            } else if (strncmp(pseudo, "nth-child", 9) == 0) {
                known = true;
                int nth = 0;
                int index = 1;
                const dom_node_t *sib;

                if (p < end && *p == '(') {
                    p++;
                    nth = atoi(p);
                    while (p < end && *p != ')') p++;
                    if (p < end) p++;
                }
                for (sib = node->parent != NULL ? node->parent->first_child : NULL;
                     sib != NULL && sib != node; sib = sib->next_sibling) {
                    index++;
                }
                if (nth < 1 || index != nth) return false;
            } else if (strcmp(pseudo, "checked") == 0) {
                known = true;
                if (!node->checked) return false;
            } else if (strcmp(pseudo, "disabled") == 0) {
                known = true;
                if (!node->disabled) return false;
            }
            /* :hover/:focus and unknown pseudo classes have no state on a
             * deterministic cell surface, so they simply do not match. */
            if (!known) return false;
            if (p < end && *p == '(') {
                int depth2 = 1;
                p++;
                while (p < end && depth2 > 0) {
                    if (*p == '(') depth2++;
                    else if (*p == ')') depth2--;
                    p++;
                }
            }
        } else {
            p++;
        }
    }
    return true;
}

/* Find the last top-level combinator ('>', '+', '~' or space) in [sel,end). */
static const char *last_combinator(const char *sel, const char *end, char *comb_out)
{
    const char *last = NULL;
    char comb = ' ';
    int depth = 0;
    const char *p;

    for (p = sel; p < end; p++) {
        if (*p == '[' || *p == '(') {
            depth++;
        } else if (*p == ']' || *p == ')') {
            if (depth > 0) depth--;
        } else if (depth == 0 && (*p == '>' || *p == '+' || *p == '~' ||
                                  isspace((unsigned char)*p))) {
            if (*p == '>' || *p == '+' || *p == '~') {
                last = p;
                comb = *p;
            } else if (last == NULL || (comb != '>' && comb != '+' && comb != '~')) {
                const char *q = p;
                while (q < end && isspace((unsigned char)*q)) q++;
                if (q < end && *q != '>') {
                    last = p;
                    comb = ' ';
                }
            }
        }
    }
    if (comb_out) *comb_out = comb;
    return last;
}

static bool complex_matches(const char *sel, const char *end, const dom_node_t *node)
{
    char comb = ' ';
    const char *split;
    const char *right;
    const char *right_end;

    while (sel < end && isspace((unsigned char)*sel)) sel++;
    while (end > sel && isspace((unsigned char)end[-1])) end--;
    if (sel >= end) return false;

    split = last_combinator(sel, end, &comb);
    if (split == NULL) {
        return simple_matches(sel, end, node);
    }
    right = split + 1;
    while (right < end && isspace((unsigned char)*right)) right++;
    right_end = end;
    while (right_end > right && isspace((unsigned char)right_end[-1])) right_end--;
    if (!simple_matches(right, right_end, node)) return false;

    if (comb == '>') {
        return node->parent != NULL && complex_matches(sel, split, node->parent);
    }
    if (comb == '+' || comb == '~') {
        const dom_node_t *sib = node->prev_sibling;

        while (sib != NULL) {
            if (sib->type == DOM_NODE_ELEMENT && complex_matches(sel, split, sib)) {
                return true;
            }
            if (comb == '+') break;
            sib = sib->prev_sibling;
        }
        return false;
    }
    {
        const dom_node_t *anc;
        for (anc = node->parent; anc != NULL; anc = anc->parent) {
            if (anc->type != DOM_NODE_ELEMENT) continue;
            if (complex_matches(sel, split, anc)) return true;
        }
    }
    return false;
}

static bool selector_matches_impl(const char *selector, const dom_node_t *node)
{
    const char *p = selector;
    const char *start = selector;

    if (!selector || !node || node->type != DOM_NODE_ELEMENT) return false;
    while (*p) {
        if (*p == ',') {
            if (complex_matches(start, p, node)) return true;
            start = p + 1;
        }
        p++;
    }
    return complex_matches(start, p, node);
}

bool css_selector_matches(const char *selector, const dom_node_t *node)
{
    return selector_matches_impl(selector, node);
}

/* Apply inline `style="..."` declarations directly (no ruleset: the 28 KB
 * ruleset must never be a per-node stack local). */
void css_apply_inline_text(dom_node_t *node, const char *style)
{
    const char *p = style;

    while (*p) {
        char decl[256];
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len >= sizeof(decl)) len = sizeof(decl) - 1;
        memcpy(decl, p, len);
        decl[len] = '\0';
        {
            char *colon = strchr(decl, ':');
            if (colon != NULL) {
                char *name = decl;
                char *value = colon + 1;
                char *e;
                *colon = '\0';
                while (*name && isspace((unsigned char)*name)) name++;
                e = name + strlen(name);
                while (e > name && isspace((unsigned char)e[-1])) *--e = '\0';
                while (*value && isspace((unsigned char)*value)) value++;
                e = value + strlen(value);
                while (e > value && isspace((unsigned char)e[-1])) *--e = '\0';
                /* Inline declarations accept `!important` too, but strip the
                 * marker before the one property parser sees the value. Inline
                 * wins over ordinary author rules; author !important rules are
                 * resolved by css_rule_compare() in css_apply(). */
                {
                    size_t vlen = strlen(value);

                    if (vlen >= 10 && strcasecmp(value + vlen - 10,
                                                 "!important") == 0) {
                        value[vlen - 10] = '\0';
                        while (vlen > 0 && isspace((unsigned char)value[vlen - 1])) {
                            value[--vlen] = '\0';
                        }
                    }
                }
                if (name[0] && value[0]) dom_set_style(node, name, value);
            }
        }
        if (!semi) break;
        p = semi + 1;
    }
}

void css_apply(dom_node_t *node, const css_ruleset_t *rules) {
    int order[CSS_MAX_RULES];
    int n = 0;
    int i;

    if (!node || !rules) return;
    for (i = 0; i < rules->count; i++) {
        if (selector_matches_impl(rules->rules[i].selector, node)) {
            order[n++] = i;
        }
    }
    /* One stable insertion sort through the ONE cascade comparator: ascending
     * (loser first), so the winner is applied last. !important, the packed
     * specificity triple and source order are all decided by that comparator;
     * `browser css` calls the same function when it reports a decision. */
    for (i = 1; i < n; i++) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && css_rule_compare(&rules->rules[order[j]],
                                          &rules->rules[key]) > 0) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
    for (i = 0; i < n; i++) {
        const css_rule_t *rule = &rules->rules[order[i]];
        dom_set_style(node, rule->property, rule->value);
    }
    if (node->style[0] != '\0') {
        css_apply_inline_text(node, node->style);
    }
}

static void apply_to_tree(dom_node_t *node, const css_ruleset_t *rules) {
    if (!node) return;
    css_apply(node, rules);
    dom_node_t *child = node->first_child;
    while (child) {
        apply_to_tree(child, rules);
        child = child->next_sibling;
    }
}

/* Inherit the properties CSS inherits by default: color, background, and the
 * text emphasis flags (a <b> inside a <p> stays bold). */
static void inherit_one(dom_node_t *node, const dom_node_t *parent) {
    if (node == NULL || parent == NULL) return;
    if (node->fg == 16) {
        node->fg = parent->fg;
        node->fg_alpha = parent->fg_alpha;
    }
    if (node->bg == 16) {
        node->bg = parent->bg;
        node->bg_alpha = parent->bg_alpha;
    }
    if (!node->bold) node->bold = parent->bold;
    if (!node->italic) node->italic = parent->italic;
    if (!node->underline) node->underline = parent->underline;
    if (node->align == 0) node->align = parent->align;
}

static void inherit_styles(dom_node_t *node, const dom_node_t *parent) {
    dom_node_t *child;

    if (!node) return;
    inherit_one(node, parent);
    child = node->first_child;
    while (child) {
        inherit_styles(child, node);
        child = child->next_sibling;
    }
}

/* The computed style lives in the existing dom_node_t fields - there is no
 * second style object. What is cached is the *work*: a tree keeps the ruleset
 * pointer, ruleset version and DOM mutation epoch it last resolved. A second
 * css_compute_styles() with no stylesheet/DOM change is a no-op, so a re-render
 * (or a JS mutation that only touched text) never re-walks the cascade. */
static const css_ruleset_t *s_cached_rules = NULL;
static const dom_tree_t *s_cached_tree = NULL;

void css_style_cache_clear(void)
{
    s_cached_rules = NULL;
    s_cached_tree = NULL;
}

void css_forget_tree(const dom_tree_t *tree)
{
    if (tree != NULL && s_cached_tree == tree) {
        css_style_cache_clear();
    }
}

void css_compute_styles(dom_tree_t *tree, const css_ruleset_t *rules) {
    uint32_t epoch;

    if (!tree || !tree->root || !rules) return;
    epoch = dom_style_epoch();
    if (tree->style_computed && tree->style_rules == (const void *)rules &&
        tree->style_rules_version == rules->version &&
        tree->style_epoch == epoch) {
        return;                 /* nothing changed: reuse the computed fields */
    }
    apply_to_tree(tree->root, rules);
    inherit_styles(tree->root, NULL);
    tree->style_rules = (const void *)rules;
    tree->style_rules_version = rules->version;
    tree->style_epoch = epoch;
    tree->style_computed = true;
    s_cached_rules = rules;
    s_cached_tree = tree;
}

/** Re-resolve one mutated subtree from the cached stylesheet. Called by the
 *  DOM when a class/id changes, and by the browser after in-page JS runs, so
 *  the new computed style is visible in the same render. */
void css_propagate_mutation(dom_node_t *node)
{
    const css_ruleset_t *rules = s_cached_rules;
    dom_node_t *child;

    if (node == NULL || rules == NULL) return;
    dom_node_reset_style(node);
    css_apply(node, rules);
    inherit_one(node, node->parent);
    child = node->first_child;
    while (child) {
        css_propagate_mutation(child);
        child = child->next_sibling;
    }
}

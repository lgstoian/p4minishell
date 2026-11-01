#include "css.h"
#include "strutil.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void skip_ws(const char **p) {
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

static void add_rule(css_ruleset_t *rules, const char *selector,
                     const char *property, const char *value) {
    if (!rules || rules->count >= CSS_MAX_RULES) return;
    css_rule_t *rule = &rules->rules[rules->count];
    strncpy(rule->selector, selector, CSS_MAX_SELECTOR_LEN - 1);
    strncpy(rule->property, property, CSS_MAX_PROPERTY_LEN - 1);
    strncpy(rule->value, value, CSS_MAX_VALUE_LEN - 1);
    {
        /* Specificity: count #id, .class/[attr], and tag tokens. Comma lists
         * take the highest part (each part is matched independently). */
        int score = 0;
        const char *p = selector;
        while (*p) {
            if (*p == '#') score += 100;
            else if (*p == '.' || *p == '[') score += 10;
            else if (isalpha((unsigned char)*p) &&
                     (p == selector || !isalnum((unsigned char)p[-1]))) score += 1;
            p++;
        }
        rule->specificity = score;
    }
    rules->count++;
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
    while (*v && isspace((unsigned char)*v)) v++;
    char *vend = v + strlen(v) - 1;
    while (vend >= v && isspace((unsigned char)*vend)) *vend-- = '\0';
    if (strlen(p) > 0 && strlen(v) > 0) {
        add_rule(rules, selector, p, v);
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
            while (p < end && *p != ']') p++;
            if (p < end) p++;
            if (strcmp(attr, "href") == 0) {
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
            }
        } else if (*p == ':') {
            p++;
            while (p < end && (isalnum((unsigned char)*p) || *p == '-')) p++;
        } else {
            p++;
        }
    }
    return true;
}

/* Find the last top-level combinator (space or '>') in [sel,end). */
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
        } else if (depth == 0 && (*p == '>' || isspace((unsigned char)*p))) {
            if (*p == '>') {
                last = p;
                comb = '>';
            } else if (last == NULL || comb != '>') {
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
    {
        const dom_node_t *anc;
        for (anc = node->parent; anc != NULL; anc = anc->parent) {
            if (anc->type != DOM_NODE_ELEMENT) continue;
            if (complex_matches(sel, split, anc)) return true;
        }
    }
    return false;
}

static bool selector_matches(const char *selector, const dom_node_t *node)
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
        if (selector_matches(rules->rules[i].selector, node)) {
            order[n++] = i;
        }
    }
    /* Stable insertion sort by specificity ascending: the most specific rule
     * is applied last, so it wins regardless of source order. */
    for (i = 1; i < n; i++) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && rules->rules[order[j]].specificity >
                          rules->rules[key].specificity) {
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
static void inherit_styles(dom_node_t *node, const dom_node_t *parent) {
    if (!node) return;
    if (parent != NULL) {
        if (node->fg == 16) node->fg = parent->fg;
        if (node->bg == 16) node->bg = parent->bg;
        if (!node->bold) node->bold = parent->bold;
        if (!node->italic) node->italic = parent->italic;
        if (!node->underline) node->underline = parent->underline;
        if (node->align == 0) node->align = parent->align;
    }
    dom_node_t *child = node->first_child;
    while (child) {
        inherit_styles(child, node);
        child = child->next_sibling;
    }
}

void css_compute_styles(dom_tree_t *tree, const css_ruleset_t *rules) {
    if (!tree || !tree->root || !rules) return;
    apply_to_tree(tree->root, rules);
    inherit_styles(tree->root, NULL);
}

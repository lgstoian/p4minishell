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

static bool selector_matches(const char *selector, const dom_node_t *node) {
    if (!selector || !node || node->type != DOM_NODE_ELEMENT) return false;
    if (strcmp(selector, "*") == 0) return true;
    if (selector[0] == '#') {
        return strcmp(selector + 1, node->id) == 0;
    }
    if (selector[0] == '.') {
        char *tok = strtok((char *)node->class, " ");
        while (tok) {
            if (strcmp(tok, selector + 1) == 0) return true;
            tok = strtok(NULL, " ");
        }
        return false;
    }
    return strcasecmp(selector, node->tag) == 0;
}

void css_apply(dom_node_t *node, const css_ruleset_t *rules) {
    if (!node || !rules) return;
    for (int i = 0; i < rules->count; i++) {
        const css_rule_t *rule = &rules->rules[i];
        if (selector_matches(rule->selector, node)) {
            dom_set_style(node, rule->property, rule->value);
        }
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

void css_compute_styles(dom_tree_t *tree, const css_ruleset_t *rules) {
    if (!tree || !tree->root || !rules) return;
    apply_to_tree(tree->root, rules);
}

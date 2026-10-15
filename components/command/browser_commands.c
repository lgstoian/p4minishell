#include "p4minishell_config.h"
#include "shell.h"
#include "jsengine.h"
#include "dom.h"
#include "css.h"
#include "html.h"
#include "networking.h"
#include "tui.h"
#include "storage.h"
#include <string.h>
#include <stdlib.h>

#define BROWSER_HISTORY_MAX 16

static char s_history[BROWSER_HISTORY_MAX][512];
static int s_history_count = 0;
static int s_history_pos = 0;

static void browser_push_history(const char *url) {
    if (s_history_count < BROWSER_HISTORY_MAX) {
        strncpy(s_history[s_history_count], url, 511);
        s_history[s_history_count][511] = '\0';
        s_history_count++;
        s_history_pos = s_history_count - 1;
    } else {
        for (int i = 0; i < BROWSER_HISTORY_MAX - 1; i++) {
            strcpy(s_history[i], s_history[i + 1]);
        }
        strncpy(s_history[BROWSER_HISTORY_MAX - 1], url, 511);
        s_history[BROWSER_HISTORY_MAX - 1][511] = '\0';
        s_history_pos = BROWSER_HISTORY_MAX - 1;
    }
}

static const char *browser_back(void) {
    if (s_history_pos > 0) s_history_pos--;
    return s_history[s_history_pos];
}

static const char *browser_forward(void) {
    if (s_history_pos < s_history_count - 1) s_history_pos++;
    return s_history[s_history_pos];
}

static void browser_render_dom(dom_tree_t *tree, int start_row, int end_row) {
    if (!tree || !tree->body) return;
    int row = start_row;
    dom_node_t *stack[64];
    int sp = 0;
    stack[sp++] = tree->body;
    while (sp > 0 && row <= end_row) {
        dom_node_t *node = stack[--sp];
        if (node->type == DOM_NODE_TEXT) {
            const char *text = node->text;
            int col = 1;
            while (*text && col <= P4_CONFIG_TUI_COLS) {
                char ch[2] = {*text, '\0'};
                tui_page_set_cell(col, row, ch, 16, 16, 0);
                col++;
                text++;
            }
            row++;
        } else if (node->type == DOM_NODE_ELEMENT) {
            if (node->display == DOM_DISPLAY_NONE) continue;
            if (strcmp(node->tag, "a") == 0 && node->href[0]) {
                const char *text = dom_get_text(node);
                int col = 1;
                while (*text && col <= P4_CONFIG_TUI_COLS) {
                    char ch[2] = {*text, '\0'};
                    tui_page_set_cell(col, row, ch, 9, 16, 8);
                    col++;
                    text++;
                }
                tui_link_register(1, row, col - 1, 1, node->href);
                row++;
            } else if (strcmp(node->tag, "img") == 0) {
                const char *alt = node->alt[0] ? node->alt : "[image]";
                int col = 1;
                while (*alt && col <= P4_CONFIG_TUI_COLS) {
                    char ch[2] = {*alt, '\0'};
                    tui_page_set_cell(col, row, ch, 16, 16, 0);
                    col++;
                    alt++;
                }
                row++;
            } else if (node->display == DOM_DISPLAY_BLOCK) {
                dom_node_t *child = node->first_child;
                while (child) {
                    if (sp < 64) stack[sp++] = child;
                    child = child->next_sibling;
                }
                row++;
            } else {
                dom_node_t *child = node->first_child;
                while (child) {
                    if (sp < 64) stack[sp++] = child;
                    child = child->next_sibling;
                }
            }
        }
    }
}

static int browser_load_url(const char *url) {
    networking_http_result_t result;
    memset(&result, 0, sizeof(result));
    esp_err_t err = networking_http_get(url, &result);
    if (err != ESP_OK) {
        shell_print_error("Failed to fetch URL");
        return 1;
    }
    if (result.body_size == 0) {
        shell_print_error("Empty response");
        free(result.body);
        return 1;
    }
    dom_tree_t *tree = dom_parse((const char *)result.body);
    free(result.body);
    if (!tree) {
        shell_print_error("Failed to parse HTML");
        return 1;
    }
    css_ruleset_t rules;
    memset(&rules, 0, sizeof(rules));
    dom_node_t *child = tree->head ? tree->head->first_child : NULL;
    while (child) {
        if (strcmp(child->tag, "style") == 0) {
            const char *css_text = dom_get_text(child);
            css_parse_block(css_text, &rules);
        }
        child = child->next_sibling;
    }
    css_compute_styles(tree, &rules);
    tui_viewport_init(P4_CONFIG_TUI_PAGE_ROWS);
    tui_page_clear();
    tui_link_clear();
    tui_address_bar_set(url);
    browser_render_dom(tree, 1, P4_CONFIG_TUI_PAGE_ROWS);
    tui_page_render();
    tui_address_bar_render();
    char status[128];
    snprintf(status, sizeof(status), " links ");
    tui_status_bar_set(status);
    tui_status_bar_render();
    tui_flush();
    dom_free(tree);
    browser_push_history(url);
    return 0;
}

int shell_command_browser(int argc, char **argv) {
    if (argc < 2) {
        shell_print_usage("browser <url>");
        return 2;
    }
    if (strcmp(argv[1], "back") == 0) {
        const char *url = browser_back();
        if (url && url[0]) {
            return browser_load_url(url);
        }
        shell_print_error("No history");
        return 1;
    }
    if (strcmp(argv[1], "forward") == 0) {
        const char *url = browser_forward();
        if (url && url[0]) {
            return browser_load_url(url);
        }
        shell_print_error("No history");
        return 1;
    }
    return browser_load_url(argv[1]);
}

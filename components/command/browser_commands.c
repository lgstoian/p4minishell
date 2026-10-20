#include "p4minishell_config.h"
#include "shell.h"
#include "jsengine.h"
#include "dom.h"
#include "css.h"
#include "html.h"
#include "networking.h"
#include "tui.h"
#include "storage.h"
#include "p4heap.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Persisted under the SD root. ALL SD I/O goes through components/storage (a
 * guarded session + atomic temp+rename); `sd:/` is the storage path alias, not
 * a VFS mount, so a raw fopen() on it would silently fail. */
#define BROWSER_HIST_FILE  "sd:/BROWSER.HIST"
#define BROWSER_BOOK_FILE  "sd:/BROWSER.BOOK"
#define BROWSER_URL_LEN    512
#define BROWSER_NAME_LEN   64

/* Bulk tables live in PSRAM, allocated on first use, NOT in .bss: internal
 * SRAM is the scarce boot resource (see ai-context.md). */
static char (*s_history)[BROWSER_URL_LEN] = NULL;
static int s_history_count = 0;
static int s_history_pos = 0;
static bool s_history_loaded = false;

typedef struct {
    char name[BROWSER_NAME_LEN];
    char url[BROWSER_URL_LEN];
} browser_bookmark_t;

static browser_bookmark_t *s_bookmarks = NULL;
static int s_bookmark_count = 0;

/* Lazily allocate the history and bookmark tables (PSRAM-first, internal
 * fallback). Return false on OOM so callers degrade instead of faulting. */
static bool browser_history_ensure(void) {
    if (s_history == NULL) {
        s_history = p4heap_calloc_any(P4_CONFIG_BROWSER_HISTORY_MAX, BROWSER_URL_LEN);
    }
    return s_history != NULL;
}

static bool browser_bookmarks_ensure(void) {
    if (s_bookmarks == NULL) {
        s_bookmarks = p4heap_calloc_any(P4_CONFIG_BROWSER_BOOKMARK_MAX, sizeof(*s_bookmarks));
    }
    return s_bookmarks != NULL;
}

/* ---- History persistence ---- */

/* Read a whole SD text file through the guarded session into a heap buffer.
 * Returns a NUL-terminated buffer (caller frees) or NULL when unavailable. */
static char *browser_read_text_file(const char *path)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *f;
    char *buf;
    size_t cap = 512;
    size_t len = 0;
    int c;

    if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        return NULL;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return NULL;
    }
    f = fopen(resolved, "r");
    if (f == NULL) {
        shell_sd_end(&session, "browser");
        return NULL;
    }
    buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        shell_sd_end(&session, "browser");
        return NULL;
    }
    while ((c = fgetc(f)) != EOF) {
        if (len + 1 >= cap) {
            size_t ncap = cap * 2;
            char *grown = realloc(buf, ncap);
            if (grown == NULL) {
                break;
            }
            buf = grown;
            cap = ncap;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    fclose(f);
    shell_sd_end(&session, "browser");
    return buf;
}

static void browser_history_load(void) {
    if (!browser_history_ensure()) return;
    if (s_history_loaded) return;
    s_history_loaded = true;

    char *text = browser_read_text_file(BROWSER_HIST_FILE);
    if (text == NULL) return;

    char *line = text;
    while (s_history_count < P4_CONFIG_BROWSER_HISTORY_MAX) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        size_t len = strlen(line);
        while (len > 0 && line[len - 1] == '\r') {
            line[--len] = '\0';
        }
        if (len > 0) {
            strncpy(s_history[s_history_count], line, BROWSER_URL_LEN - 1);
            s_history[s_history_count][BROWSER_URL_LEN - 1] = '\0';
            s_history_count++;
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    free(text);
    s_history_pos = s_history_count > 0 ? s_history_count - 1 : 0;
}

static void browser_history_save(void) {
    if (s_history == NULL) return;
    size_t cap = (size_t)P4_CONFIG_BROWSER_HISTORY_MAX * (BROWSER_URL_LEN + 1) + 1;
    char *text = malloc(cap);
    if (text == NULL) return;
    size_t pos = 0;
    for (int i = 0; i < s_history_count && pos < cap; i++) {
        pos += (size_t)snprintf(text + pos, cap - pos, "%s\n", s_history[i]);
    }
    (void)storage_write_text_file(BROWSER_HIST_FILE, text);
    free(text);
}

static void browser_push_history(const char *url) {
    if (!browser_history_ensure()) return;
    browser_history_load();
    /* Don't add duplicate of current entry. */
    if (s_history_count > 0 && strcmp(s_history[s_history_pos], url) == 0) {
        return;
    }
    if (s_history_count < P4_CONFIG_BROWSER_HISTORY_MAX) {
        strncpy(s_history[s_history_count], url, BROWSER_URL_LEN - 1);
        s_history[s_history_count][BROWSER_URL_LEN - 1] = '\0';
        s_history_count++;
        s_history_pos = s_history_count - 1;
    } else {
        memmove(s_history[0], s_history[1], (P4_CONFIG_BROWSER_HISTORY_MAX - 1) * BROWSER_URL_LEN);
        strncpy(s_history[P4_CONFIG_BROWSER_HISTORY_MAX - 1], url, BROWSER_URL_LEN - 1);
        s_history[P4_CONFIG_BROWSER_HISTORY_MAX - 1][BROWSER_URL_LEN - 1] = '\0';
        s_history_pos = P4_CONFIG_BROWSER_HISTORY_MAX - 1;
    }
    browser_history_save();
}

static const char *browser_back(void) {
    if (!browser_history_ensure()) return NULL;
    browser_history_load();
    if (s_history_pos > 0) s_history_pos--;
    return s_history[s_history_pos];
}

static const char *browser_forward(void) {
    if (!browser_history_ensure()) return NULL;
    browser_history_load();
    if (s_history_pos < s_history_count - 1) s_history_pos++;
    return s_history[s_history_pos];
}

/* ---- Bookmark persistence ---- */

static void browser_bookmarks_load(void) {
    if (!browser_bookmarks_ensure()) return;

    char *text = browser_read_text_file(BROWSER_BOOK_FILE);
    if (text == NULL) return;

    char *line = text;
    while (s_bookmark_count < P4_CONFIG_BROWSER_BOOKMARK_MAX) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        char *sep = strchr(line, '|');
        if (sep != NULL) {
            *sep = '\0';
            char *name = line;
            char *url = sep + 1;
            size_t nl_name = strlen(name);
            while (nl_name > 0 && name[nl_name - 1] == '\r') {
                name[--nl_name] = '\0';
            }
            size_t nl_url = strlen(url);
            while (nl_url > 0 && url[nl_url - 1] == '\r') {
                url[--nl_url] = '\0';
            }
            if (nl_name > 0 && nl_url > 0) {
                strncpy(s_bookmarks[s_bookmark_count].name, name, BROWSER_NAME_LEN - 1);
                s_bookmarks[s_bookmark_count].name[BROWSER_NAME_LEN - 1] = '\0';
                strncpy(s_bookmarks[s_bookmark_count].url, url, BROWSER_URL_LEN - 1);
                s_bookmarks[s_bookmark_count].url[BROWSER_URL_LEN - 1] = '\0';
                s_bookmark_count++;
            }
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    free(text);
}

static void browser_bookmarks_save(void) {
    if (s_bookmarks == NULL) return;
    size_t per = (size_t)BROWSER_NAME_LEN + BROWSER_URL_LEN + 2;
    size_t cap = (size_t)P4_CONFIG_BROWSER_BOOKMARK_MAX * per + 1;
    char *text = malloc(cap);
    if (text == NULL) return;
    size_t pos = 0;
    for (int i = 0; i < s_bookmark_count && pos < cap; i++) {
        pos += (size_t)snprintf(text + pos, cap - pos, "%s|%s\n",
                                s_bookmarks[i].name, s_bookmarks[i].url);
    }
    (void)storage_write_text_file(BROWSER_BOOK_FILE, text);
    free(text);
}

static int browser_bookmark_add(const char *name, const char *url) {
    if (!browser_bookmarks_ensure()) return -1;
    for (int i = 0; i < s_bookmark_count; i++) {
        if (strcmp(s_bookmarks[i].name, name) == 0) {
            strncpy(s_bookmarks[i].url, url, BROWSER_URL_LEN - 1);
            s_bookmarks[i].url[BROWSER_URL_LEN - 1] = '\0';
            browser_bookmarks_save();
            return 0;
        }
    }
    if (s_bookmark_count >= P4_CONFIG_BROWSER_BOOKMARK_MAX) return -1;
    strncpy(s_bookmarks[s_bookmark_count].name, name, BROWSER_NAME_LEN - 1);
    s_bookmarks[s_bookmark_count].name[BROWSER_NAME_LEN - 1] = '\0';
    strncpy(s_bookmarks[s_bookmark_count].url, url, BROWSER_URL_LEN - 1);
    s_bookmarks[s_bookmark_count].url[BROWSER_URL_LEN - 1] = '\0';
    s_bookmark_count++;
    browser_bookmarks_save();
    return 0;
}

static int browser_bookmark_del(const char *name) {
    if (!browser_bookmarks_ensure()) return -1;
    for (int i = 0; i < s_bookmark_count; i++) {
        if (strcmp(s_bookmarks[i].name, name) == 0) {
            memmove(&s_bookmarks[i], &s_bookmarks[i + 1],
                    (s_bookmark_count - i - 1) * sizeof(browser_bookmark_t));
            s_bookmark_count--;
            browser_bookmarks_save();
            return 0;
        }
    }
    return -1;
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
                /* Push last-to-first so the LIFO stack pops siblings in
                 * document order; first-to-last drew every page upside down
                 * (the text-presence tests could not see the reversal). */
                dom_node_t *child = node->last_child;
                while (child) {
                    if (sp < 64) stack[sp++] = child;
                    child = child->prev_sibling;
                }
                row++;
            } else {
                dom_node_t *child = node->last_child;
                while (child) {
                    if (sp < 64) stack[sp++] = child;
                    child = child->prev_sibling;
                }
            }
        }
    }
}

int browser_render_html(const char *html, const char *base_url) {
    if (html == NULL) {
        return 1;
    }
    dom_tree_t *tree = dom_parse(html);
    if (tree == NULL) {
        return 1;
    }
    /* The ruleset is ~28 KB (64 rules x 448 B). It used to be a stack local,
     * which alone forced the command worker to reserve 32 KB of internal RAM
     * and starved C6 hosted bring-up. PSRAM-first keeps the stack small; if
     * every pool refuses, render without styles rather than fail the page. */
    css_ruleset_t *rules = p4heap_calloc_psram(1, sizeof(*rules));
    if (rules == NULL) {
        rules = p4heap_calloc_any(1, sizeof(*rules));
    }
    if (rules != NULL) {
        dom_node_t *child = tree->head ? tree->head->first_child : NULL;
        while (child) {
            if (strcmp(child->tag, "style") == 0) {
                const char *css_text = dom_get_text(child);
                css_parse_block(css_text, rules);
            }
            child = child->next_sibling;
        }
        css_compute_styles(tree, rules);
    }
    /* The browser is a visible surface: enter TUI mode if the caller was at the
     * transcript. Without this the page buffer was filled but never shown
     * (tui_page_render only copies into the active cell grid), so `browser`
     * looked like a no-op on screen even though `tui dump` saw the page. */
    if (!tui_is_active() && tui_ready()) {
        (void)tui_init();
    }
    tui_viewport_init(P4_CONFIG_TUI_PAGE_ROWS);
    tui_page_clear();
    tui_link_clear();
    tui_address_bar_set(base_url != NULL ? base_url : "");
    browser_render_dom(tree, 1, P4_CONFIG_TUI_PAGE_ROWS);
    tui_page_render();
    tui_address_bar_render();
    tui_status_bar_set(" links ");
    tui_status_bar_render();
    tui_flush();
    dom_free(tree);
    p4heap_free(rules);
    return 0;
}

/* Split optional Basic-auth userinfo out of a URL. `http://user:pass@host/p`
 * yields clean="http://host/p" + user/pass; a URL without userinfo is copied
 * verbatim. Standard URL form, so no extra switch is needed and the existing
 * networking_http_get_ex() auth path is reused. */
static void browser_split_userinfo(const char *url, char *clean, size_t clean_size,
                                   char *user, size_t user_size,
                                   char *pass, size_t pass_size)
{
    const char *scheme;
    const char *authority;
    const char *authority_end;
    const char *at = NULL;
    const char *colon = NULL;
    const char *p;

    user[0] = '\0';
    pass[0] = '\0';
    if (url == NULL) {
        clean[0] = '\0';
        return;
    }
    scheme = strstr(url, "://");
    authority = scheme ? scheme + 3 : url;
    authority_end = authority;
    while (*authority_end != '\0' && *authority_end != '/') {
        authority_end++;
    }
    for (p = authority; p < authority_end; p++) {
        if (*p == '@') {
            at = p;
            break;
        }
    }
    if (at == NULL) {
        snprintf(clean, clean_size, "%s", url);
        return;
    }
    for (p = authority; p < at; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }
    {
        size_t ulen = (size_t)((colon != NULL ? colon : at) - authority);
        if (ulen >= user_size) ulen = user_size - 1;
        memcpy(user, authority, ulen);
        user[ulen] = '\0';
    }
    if (colon != NULL) {
        size_t plen = (size_t)(at - (colon + 1));
        if (plen >= pass_size) plen = pass_size - 1;
        memcpy(pass, colon + 1, plen);
        pass[plen] = '\0';
    }
    if (scheme != NULL) {
        size_t pre = (size_t)(authority - url);
        snprintf(clean, clean_size, "%.*s%s", (int)pre, url, at + 1);
    } else {
        snprintf(clean, clean_size, "%s", at + 1);
    }
}

/* Offline page: render an SD HTML file directly. Useful for local docs and for
 * the deterministic website showcase (no AP or httpd needed). */
static int browser_load_file(const char *path) {
    char *html = browser_read_text_file(path);
    if (html == NULL) {
        shell_print_error("Failed to read %s", path);
        return 1;
    }
    if (html[0] == '\0') {
        free(html);
        shell_print_error("Empty file");
        return 1;
    }
    int render_rc = browser_render_html(html, path);
    free(html);
    if (render_rc != 0) {
        shell_print_error("Failed to parse HTML");
        return 1;
    }
    browser_push_history(path);
    return 0;
}

static int browser_load_url(const char *url) {
    char clean[512];
    char user[64];
    char pass[128];
    const char *auth_user = NULL;
    const char *auth_pass = NULL;
    networking_http_result_t result;

    /* Anything that is not http(s) is an SD path: render it locally. */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return browser_load_file(url);
    }

    memset(&result, 0, sizeof(result));
    browser_split_userinfo(url, clean, sizeof(clean),
                           user, sizeof(user), pass, sizeof(pass));
    if (user[0] != '\0') {
        auth_user = user;
        auth_pass = pass;
    }

    esp_err_t err = networking_http_get_ex(clean, NULL, 0, auth_user, auth_pass,
                                           NULL, &result);
    if (err != ESP_OK) {
        shell_print_error("Failed to fetch URL");
        return 1;
    }
    if (result.body_size == 0) {
        shell_print_error("Empty response");
        free(result.body);
        return 1;
    }
    int render_rc = browser_render_html((const char *)result.body, clean);
    free(result.body);
    if (render_rc != 0) {
        shell_print_error("Failed to parse HTML");
        return 1;
    }
    /* Keep the userinfo in history so back/forward can re-authenticate the
     * same target (the SD card is already the credential trust boundary:
     * WIFI.KNOWN / NET.INI live there too). */
    browser_push_history(url);
    return 0;
}

int shell_command_browser(int argc, char **argv) {
    if (argc < 2) {
        shell_print_usage("browser <url|file> | browser back | browser forward | browser bookmark [add|del|list]");
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
    if (strcmp(argv[1], "bookmark") == 0) {
        browser_bookmarks_load();
        if (argc < 3) {
            /* List bookmarks. */
            if (s_bookmark_count == 0) {
                shell_transcript_appendf("No bookmarks\n");
            } else {
                for (int i = 0; i < s_bookmark_count; i++) {
                    shell_transcript_appendf("  %-16s %s\n", s_bookmarks[i].name, s_bookmarks[i].url);
                }
            }
            return 0;
        }
        if (strcmp(argv[2], "list") == 0) {
            if (s_bookmark_count == 0) {
                shell_transcript_appendf("No bookmarks\n");
            } else {
                for (int i = 0; i < s_bookmark_count; i++) {
                    shell_transcript_appendf("  %-16s %s\n", s_bookmarks[i].name, s_bookmarks[i].url);
                }
            }
            return 0;
        }
        if (strcmp(argv[2], "add") == 0) {
            if (argc < 5) {
                shell_print_usage("browser bookmark add <name> <url>");
                return 2;
            }
            if (browser_bookmark_add(argv[3], argv[4]) == 0) {
                shell_print_ok("bookmark: added %s", argv[3]);
                return 0;
            }
            shell_print_error("bookmark: table is full");
            return 1;
        }
        if (strcmp(argv[2], "del") == 0) {
            if (argc < 4) {
                shell_print_usage("browser bookmark del <name>");
                return 2;
            }
            if (browser_bookmark_del(argv[3]) == 0) {
                shell_print_ok("bookmark: removed %s", argv[3]);
                return 0;
            }
            shell_print_error("bookmark: not found");
            return 1;
        }
        shell_print_usage("browser bookmark [add|del|list]");
        return 2;
    }
    return browser_load_url(argv[1]);
}

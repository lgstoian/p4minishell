#include "p4minishell_config.h"
#include "shell.h"
#include "jsengine.h"
#include "dom.h"
#include "dom_render.h"
#include "css.h"
#include "html.h"
#include "networking.h"
#include "tui.h"
#include "storage.h"
#include "p4heap.h"
#include "gfx.h"
#include "command.h"
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

/* ---- Page state (kept alive for in-page JS event dispatch) ---- */
static dom_tree_t *s_page_tree = NULL;
static css_ruleset_t *s_page_rules = NULL;
static char s_page_base[BROWSER_URL_LEN];
static size_t s_js_budget = 0;

static void browser_page_release(void)
{
    if (s_page_tree != NULL) {
        dom_free(s_page_tree);
        s_page_tree = NULL;
    }
    if (s_page_rules != NULL) {
        p4heap_free(s_page_rules);
        s_page_rules = NULL;
    }
    s_page_base[0] = '\0';
}

static void browser_put_cell(void *ctx, int col, int row, const char *utf8,
                             uint8_t fg, uint8_t bg, uint8_t attr)
{
    (void)ctx;
    tui_page_set_cell(col, row, utf8, fg, bg, attr);
}

static void browser_put_link(void *ctx, int x, int y, int w, int h, const char *url)
{
    (void)ctx;
    tui_link_register(x, y, w, h, url);
}

static uint32_t browser_rgb565_to_rgb888(uint16_t c)
{
    uint32_t r = (c >> 11) & 0x1F;
    uint32_t g = (c >> 5) & 0x3F;
    uint32_t b = c & 0x1F;
    return (((r * 255) / 31) << 16) | (((g * 255) / 63) << 8) | ((b * 255) / 31);
}

/* Image loader for the renderer: resolve the src against the page URL, fetch
 * (HTTP) or read (SD), decode with the one gfx BMP decoder, and map to DOS
 * cells. Returns false so the renderer falls back to alt text. */
static bool browser_load_image(void *ctx, const char *src, dom_image_t *out)
{
    const char *base = (const char *)ctx;
    char target[1024];
    uint8_t *data = NULL;
    size_t len = 0;
    gfx_bmp_info_t info;
    gfx_surface_t surf;
    uint8_t *pixels;
    int w;
    int h;
    int x;
    int y;

    if (src == NULL || out == NULL || !P4_CONFIG_BROWSER_IMAGES) return false;
    memset(out, 0, sizeof(*out));
    if (strncmp(src, "http://", 7) == 0 || strncmp(src, "https://", 8) == 0 ||
        strncmp(src, "sd:", 3) == 0 || src[0] == '/') {
        snprintf(target, sizeof(target), "%s", src);
    } else {
        const char *slash = base != NULL ? strrchr(base, '/') : NULL;
        if (slash != NULL) {
            snprintf(target, sizeof(target), "%.*s/%s", (int)(slash - base), base, src);
        } else {
            snprintf(target, sizeof(target), "%s/%s", base != NULL ? base : "sd:/", src);
        }
    }
    if (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0) {
        networking_http_result_t result;
        memset(&result, 0, sizeof(result));
        if (networking_http_get(target, &result) != ESP_OK) return false;
        data = result.body;
        len = result.body_size;
    } else if (command_load_file_psram(target, "browser image",
                                       P4_CONFIG_BROWSER_IMAGE_MAX_BYTES,
                                       &data, &len) != 0) {
        return false;
    }
    if (data == NULL || len == 0) {
        free(data);
        return false;
    }
    memset(&info, 0, sizeof(info));
    if (!gfx_bmp_parse_header_ex(data, len, &info, GFX_IMAGE_MAX_W, GFX_IMAGE_MAX_H)) {
        free(data);
        return false;
    }
    w = info.w;
    h = info.h;
    if (w > P4_CONFIG_BROWSER_IMAGE_MAX_WIDTH) {
        h = h * P4_CONFIG_BROWSER_IMAGE_MAX_WIDTH / w;
        w = P4_CONFIG_BROWSER_IMAGE_MAX_WIDTH;
    }
    if (h > P4_CONFIG_BROWSER_IMAGE_MAX_HEIGHT) {
        w = w * P4_CONFIG_BROWSER_IMAGE_MAX_HEIGHT / h;
        h = P4_CONFIG_BROWSER_IMAGE_MAX_HEIGHT;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    memset(&surf, 0, sizeof(surf));
    if (!gfx_bmp_decode_scaled_565(data, len, &info, w, h, &surf)) {
        free(data);
        return false;
    }
    pixels = malloc((size_t)w * (size_t)h);
    if (pixels == NULL) {
        gfx_surface_free(&surf);
        free(data);
        return false;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint16_t c = gfx_surface_get(&surf, x, y);
            uint32_t rgb = browser_rgb565_to_rgb888(c);
            uint8_t dos = tui_rgb_to_dos(rgb);
            /* Pack the DOS foreground and the luminance-ramp level (0..9) so
             * the renderer can pick a shade glyph without a local palette. */
            int lum = (int)((((rgb >> 16) & 0xFF) * 30 +
                             ((rgb >> 8) & 0xFF) * 59 +
                             (rgb & 0xFF) * 11) / 100);
            int level = lum * 9 / 255;
            if (level < 0) level = 0;
            if (level > 9) level = 9;
            pixels[y * w + x] = (uint8_t)((dos << 4) | level);
        }
    }
    gfx_surface_free(&surf);
    free(data);
    out->width = w;
    out->height = h;
    out->pixels = pixels;
    return true;
}

static void browser_run_scripts(dom_node_t *node)
{
    for (dom_node_t *ch = node != NULL ? node->first_child : NULL;
         ch != NULL; ch = ch->next_sibling) {
        if (ch->type != DOM_NODE_ELEMENT) continue;
        if (strcmp(ch->tag, "script") == 0) {
            const char *code = (ch->first_child != NULL &&
                                ch->first_child->type == DOM_NODE_TEXT)
                                   ? ch->first_child->text : NULL;
            if (code != NULL && code[0] != '\0') {
                size_t len = strlen(code);
                if (len <= s_js_budget) {
                    char result[128];
                    s_js_budget -= len;
                    (void)jsengine_eval(code, result, sizeof(result));
                }
            }
        } else {
            browser_run_scripts(ch);
        }
    }
}

static void browser_run_onload(dom_node_t *node)
{
    for (dom_node_t *ch = node != NULL ? node->first_child : NULL;
         ch != NULL; ch = ch->next_sibling) {
        if (ch->type != DOM_NODE_ELEMENT) continue;
        if (ch->onload[0] != '\0') {
            char result[128];
            (void)jsengine_eval(ch->onload, result, sizeof(result));
        }
        browser_run_onload(ch);
    }
}

static void browser_page_render(void)
{
    dom_render_ops_t ops;

    if (s_page_tree == NULL) return;
    if (!tui_is_active() && tui_ready()) {
        (void)tui_init();
    }
    tui_viewport_init(P4_CONFIG_TUI_PAGE_ROWS);
    tui_page_clear();
    tui_link_clear();
    tui_address_bar_set(s_page_base);
    memset(&ops, 0, sizeof(ops));
    ops.put_cell = browser_put_cell;
    ops.put_link = browser_put_link;
    ops.load_image = browser_load_image;
    ops.ctx = s_page_base;
    ops.cols = P4_CONFIG_TUI_COLS;
    ops.rows = P4_CONFIG_TUI_PAGE_ROWS;
    ops.tui = true;
    ops.base_url = s_page_base;
    dom_render_tree(s_page_tree, &ops);
    tui_page_render();
    tui_address_bar_render();
    tui_status_bar_set(" links ");
    tui_status_bar_render();
    tui_flush();
}

int browser_render_html(const char *html, const char *base_url) {
    dom_tree_t *tree;
    css_ruleset_t *rules;

    if (html == NULL) {
        return 1;
    }
    tree = dom_parse(html);
    if (tree == NULL) {
        return 1;
    }
    /* The ruleset is ~28 KB (64 rules x 448 B). It used to be a stack local,
     * which alone forced the command worker to reserve 32 KB of internal RAM
     * and starved C6 hosted bring-up. PSRAM-first keeps the stack small; if
     * every pool refuses, render without styles rather than fail the page. */
    rules = p4heap_calloc_psram(1, sizeof(*rules));
    if (rules == NULL) {
        rules = p4heap_calloc_any(1, sizeof(*rules));
    }
    if (rules != NULL) {
        dom_node_t *child = tree->head ? tree->head->first_child : NULL;
        while (child != NULL) {
            if (strcmp(child->tag, "style") == 0) {
                const char *css_text = dom_get_text(child);
                css_parse_block(css_text, rules);
            }
            child = child->next_sibling;
        }
        css_compute_styles(tree, rules);
    }
    browser_page_release();
    s_page_tree = tree;
    s_page_rules = rules;
    snprintf(s_page_base, sizeof(s_page_base), "%s", base_url != NULL ? base_url : "");
    if (P4_CONFIG_BROWSER_JS) {
        char result[128];

        jsengine_set_dom_tree(tree);
        s_js_budget = P4_CONFIG_BROWSER_JS_MAX_BYTES;
        browser_run_scripts(tree->root);
        browser_run_onload(tree->root);
        (void)jsengine_eval("if (typeof onload === 'function') onload();",
                            result, sizeof(result));
        (void)jsengine_eval("if (typeof window !== 'undefined' && typeof window.onload === 'function') window.onload();",
                            result, sizeof(result));
        jsengine_set_dom_tree(NULL);
    }
    browser_page_render();
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
        shell_print_usage("browser <url|file> | browser back | browser forward | browser js <id> | browser css | browser bookmark [add|del|list]");
        return 2;
    }
    if (strcmp(argv[1], "css") == 0) {
        shell_transcript_appendf("css: rules=%d tree=%p\n",
                                 s_page_rules != NULL ? s_page_rules->count : -1,
                                 (void *)s_page_tree);
        if (s_page_rules != NULL) {
            for (int i = 0; i < s_page_rules->count && i < 24; i++) {
                shell_transcript_appendf("  %s { %s: %s } spec=%d\n",
                                         s_page_rules->rules[i].selector,
                                         s_page_rules->rules[i].property,
                                         s_page_rules->rules[i].value,
                                         s_page_rules->rules[i].specificity);
            }
        }
        if (s_page_tree != NULL && s_page_tree->body != NULL) {
            dom_node_t *h1 = dom_find_first_by_tag(s_page_tree, "h1");
            dom_node_t *th = dom_find_first_by_tag(s_page_tree, "th");
            shell_transcript_appendf("body fg=%u bg=%u\n",
                                     (unsigned)s_page_tree->body->fg,
                                     (unsigned)s_page_tree->body->bg);
            if (h1 != NULL) {
                shell_transcript_appendf("h1 fg=%u bold=%d\n",
                                         (unsigned)h1->fg, h1->bold);
            }
            if (th != NULL) {
                shell_transcript_appendf("th fg=%u bg=%u\n",
                                         (unsigned)th->fg, (unsigned)th->bg);
            }
        }
        return 0;
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
    if (strcmp(argv[1], "js") == 0) {
        dom_node_t *node;
        char result[128];

        if (argc < 3) {
            shell_print_usage("browser js <element-id>");
            return 2;
        }
        if (s_page_tree == NULL) {
            shell_print_error("No page loaded");
            return 1;
        }
        node = dom_find_by_id(s_page_tree, argv[2]);
        if (node == NULL) {
            shell_print_error("No element '%s'", argv[2]);
            return 1;
        }
        if (node->onclick[0] == '\0') {
            shell_print_error("Element '%s' has no click handler", argv[2]);
            return 1;
        }
        jsengine_set_dom_tree(s_page_tree);
        (void)jsengine_eval(node->onclick, result, sizeof(result));
        (void)jsengine_eval("if (typeof onload === 'function') onload();",
                            result, sizeof(result));
        jsengine_set_dom_tree(NULL);
        browser_page_render();
        return 0;
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

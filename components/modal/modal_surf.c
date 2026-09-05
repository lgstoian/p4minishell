/**
 * @file modal_surf.c
 * @brief Native modal surfaces for batch apps: dialog, list, ask, filebrowser, viewer, hexview.
 *
 * All surfaces run on the shared modal runtime (modal.c) and take over the
 * transcript region via windows_enter_editor_mode(). The pixel rect follows
 * the live transcript region (rotation / keyboard) so every surface fills
 * exactly the same space as the shell.
 */

#include "modal_surf.h"
#include "modal.h"
#include "windows.h"
#include "shell.h"
#include "keyboard.h"
#include "p4minishell_config.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>

#define SURF_TAG "modal_surf"

/* ---- Shared helpers ---- */

bool modal_parse_timeout_arg(const char *arg, uint32_t *timeout_ms_out)
{
    int secs;

    if (arg == NULL || timeout_ms_out == NULL) {
        return false;
    }
    if (strncasecmp(arg, "/t:", 3) != 0 || strlen(arg) <= 3) {
        return false;
    }
    secs = atoi(arg + 3);
    *timeout_ms_out = (secs > 0) ? (uint32_t)secs * 1000u : 0u;
    return true;
}

bool modal_parse_var_arg(const char *arg, const char **name_out)
{
    if (arg == NULL || name_out == NULL) {
        return false;
    }
    if (strncasecmp(arg, "/v:", 3) != 0 || strlen(arg) <= 3) {
        return false;
    }
    *name_out = arg + 3;
    return true;
}

static void surf_request_close(EventGroupHandle_t eg)
{
    if (eg) xEventGroupSetBits(eg, MODAL_EVENT_CLOSE_REQUEST);
}

static void surf_timeout_cb(TimerHandle_t timer)
{
    EventGroupHandle_t eg = (EventGroupHandle_t)pvTimerGetTimerID(timer);
    surf_request_close(eg);
}

static lv_obj_t *surf_create_container(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x050806), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x335577), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(c, 6, 0);
    return c;
}

static lv_obj_t *surf_create_title(lv_obj_t *parent, const char *title)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title ? title : "");
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x8DFF96), 0);
    lv_obj_set_style_text_font(lbl, windows_get_terminal_font(), 0);
    return lbl;
}

static lv_obj_t *surf_create_button(lv_obj_t *parent, const char *label)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label ? label : "");
    lv_obj_center(lbl);
    return btn;
}

/* ========================================================================
 * DIALOG
 * ======================================================================== */

typedef struct {
    EventGroupHandle_t eg;
    int result; /* 0 btn1, 1 btn2, -1 cancel */
    uint32_t timeout_ms;
    TimerHandle_t timer;
    lv_obj_t *panel;
    const char *title;
    const char *message;
    const char *button1;
    const char *button2;
} dialog_ctx_t;

static dialog_ctx_t *s_dialog_active = NULL;

static void dialog_btn1_cb(lv_event_t *e) { (void)e; if (s_dialog_active) { s_dialog_active->result = 0; surf_request_close(s_dialog_active->eg); } }
static void dialog_btn2_cb(lv_event_t *e) { (void)e; if (s_dialog_active) { s_dialog_active->result = 1; surf_request_close(s_dialog_active->eg); } }
static void dialog_cancel_cb(lv_event_t *e) { (void)e; if (s_dialog_active) { s_dialog_active->result = -1; surf_request_close(s_dialog_active->eg); } }

static bool dialog_surface_open(void *ctx_ptr, EventGroupHandle_t eg)
{
    dialog_ctx_t *ctx = (dialog_ctx_t *)ctx_ptr;
    ctx->eg = eg;
    ctx->result = -1;
    s_dialog_active = ctx;
    if (ctx->timeout_ms > 0) {
        ctx->timer = xTimerCreate("dlg_tmr", pdMS_TO_TICKS(ctx->timeout_ms), pdFALSE, (void *)eg, surf_timeout_cb);
        if (ctx->timer) xTimerStart(ctx->timer, 0);
    }
    lvgl_port_lock(0);
    lv_obj_t *surf = windows_enter_editor_mode();
    if (!surf) { lvgl_port_unlock(); s_dialog_active = NULL; return false; }
    ctx->panel = surf_create_container(surf);
    surf_create_title(ctx->panel, ctx->title ? ctx->title : "Dialog");
    lv_obj_t *msg = lv_label_create(ctx->panel);
    lv_label_set_text(msg, ctx->message ? ctx->message : "");
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xC7FFD0), 0);
    lv_obj_t *row = lv_obj_create(ctx->panel);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *b1 = surf_create_button(row, ctx->button1 ? ctx->button1 : "OK");
    lv_obj_add_event_cb(b1, dialog_btn1_cb, LV_EVENT_CLICKED, NULL);
    if (ctx->button2) {
        lv_obj_t *b2 = surf_create_button(row, ctx->button2);
        lv_obj_add_event_cb(b2, dialog_btn2_cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_t *b2 = surf_create_button(row, "Cancel");
        lv_obj_add_event_cb(b2, dialog_cancel_cb, LV_EVENT_CLICKED, NULL);
    }
    lvgl_port_unlock();
    windows_refresh_editor_surface();
    return true;
}

static void dialog_surface_close(void *ctx_ptr)
{
    dialog_ctx_t *ctx = (dialog_ctx_t *)ctx_ptr;
    if (ctx->timer) { xTimerStop(ctx->timer, 0); xTimerDelete(ctx->timer, 0); ctx->timer = NULL; }
    lvgl_port_lock(0);
    if (ctx->panel) { lv_obj_del(ctx->panel); ctx->panel = NULL; }
    windows_exit_editor_mode();
    lvgl_port_unlock();
    s_dialog_active = NULL;
    if (ctx->eg) xEventGroupSetBits(ctx->eg, MODAL_EVENT_CLOSED);
}

static bool dialog_handle_usb_key(void *ctx_ptr, uint8_t key_code, uint8_t modifiers, char ascii)
{
    (void)modifiers; (void)ascii;
    dialog_ctx_t *ctx = (dialog_ctx_t *)ctx_ptr;
    if (key_code == 0x29) { ctx->result = -1; surf_request_close(ctx->eg); return true; } /* ESC */
    if (ascii == 'y' || ascii == 'Y' || key_code == 0x28) { ctx->result = 0; surf_request_close(ctx->eg); return true; } /* Enter/Y */
    if (ascii == 'n' || ascii == 'N') { ctx->result = ctx->button2 ? 1 : -1; surf_request_close(ctx->eg); return true; }
    if (key_code == 0x2B) { /* Tab -> toggle? */ return false; }
    return false;
}

static bool dialog_handle_serial_line(void *ctx_ptr, const char *line)
{
    dialog_ctx_t *ctx = (dialog_ctx_t *)ctx_ptr;
    if (!line) return false;
    ESP_LOGW("modal", "dialog serial line: '%s'", line);
    if (strcasecmp(line, "y") == 0 || strcasecmp(line, "yes") == 0 || strcasecmp(line, "1") == 0 || strcasecmp(line, "ok") == 0) { ctx->result = 0; surf_request_close(ctx->eg); return true; }
    if (strcasecmp(line, "n") == 0 || strcasecmp(line, "no") == 0 || strcasecmp(line, "2") == 0) { ctx->result = ctx->button2 ? 1 : -1; surf_request_close(ctx->eg); return true; }
    if (strcasecmp(line, "q") == 0 || strcasecmp(line, "quit") == 0 || strcasecmp(line, "") == 0) { ctx->result = -1; surf_request_close(ctx->eg); return true; }
    return false;
}

static const modal_surface_t dialog_surface = {
    .name = "dialog",
    .open = dialog_surface_open,
    .close = dialog_surface_close,
    .handle_usb_key = dialog_handle_usb_key,
    .handle_serial_line = dialog_handle_serial_line,
};

int modal_dialog_run(const char *title, const char *message,
                     const char *button1, const char *button2,
                     uint32_t timeout_ms)
{
    dialog_ctx_t ctx = {0};
    int errorlevel = 0;
    ctx.title = title;
    ctx.message = message;
    ctx.button1 = button1;
    ctx.button2 = button2;
    ctx.timeout_ms = timeout_ms ? timeout_ms : P4_CONFIG_TUI_TIMEOUT_DEFAULT_MS;
    if (modal_surface_run(&dialog_surface, &ctx, &errorlevel) != ESP_OK) return -1;
    return ctx.result;
}

/* ========================================================================
 * LIST
 * ======================================================================== */

typedef struct {
    EventGroupHandle_t eg;
    int result; /* index or -1 */
    uint32_t timeout_ms;
    TimerHandle_t timer;
    lv_obj_t *panel;
    lv_obj_t *list;
    const char *title;
    const char **items;
    int count;
    int selected;
} list_ctx_t;

static list_ctx_t *s_list_active = NULL;

static void list_select_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_list_active && idx >= 0 && idx < s_list_active->count) {
        s_list_active->result = idx;
        s_list_active->selected = idx;
        surf_request_close(s_list_active->eg);
    }
}
static void list_cancel_cb(lv_event_t *e) { (void)e; if (s_list_active) { s_list_active->result = -1; surf_request_close(s_list_active->eg); } }

static bool list_surface_open(void *ctx_ptr, EventGroupHandle_t eg)
{
    list_ctx_t *ctx = (list_ctx_t *)ctx_ptr;
    ctx->eg = eg;
    ctx->result = -1;
    ctx->selected = 0;
    s_list_active = ctx;
    if (ctx->timeout_ms > 0) {
        ctx->timer = xTimerCreate("lst_tmr", pdMS_TO_TICKS(ctx->timeout_ms), pdFALSE, (void *)eg, surf_timeout_cb);
        if (ctx->timer) xTimerStart(ctx->timer, 0);
    }
    if (ctx->count <= 0 || ctx->items == NULL) return false;
    if (ctx->count > P4_CONFIG_TUI_LIST_MAX_ITEMS) ctx->count = P4_CONFIG_TUI_LIST_MAX_ITEMS;
    lvgl_port_lock(0);
    lv_obj_t *surf = windows_enter_editor_mode();
    if (!surf) { lvgl_port_unlock(); s_list_active = NULL; return false; }
    ctx->panel = surf_create_container(surf);
    surf_create_title(ctx->panel, ctx->title ? ctx->title : "Select");
    ctx->list = lv_obj_create(ctx->panel);
    lv_obj_set_size(ctx->list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(ctx->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ctx->list, 2, 0);
    for (int i = 0; i < ctx->count; i++) {
        const char *label = ctx->items[i] ? ctx->items[i] : "";
        char buf[160];
        snprintf(buf, sizeof(buf), "%d. %s", i + 1, label);
        lv_obj_t *btn = lv_btn_create(ctx->list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, buf);
        lv_obj_add_event_cb(btn, list_select_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *row = lv_obj_create(ctx->panel);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *b = surf_create_button(row, "Cancel");
    lv_obj_add_event_cb(b, list_cancel_cb, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();
    windows_refresh_editor_surface();
    return true;
}

static void list_surface_close(void *ctx_ptr)
{
    list_ctx_t *ctx = (list_ctx_t *)ctx_ptr;
    if (ctx->timer) { xTimerStop(ctx->timer, 0); xTimerDelete(ctx->timer, 0); ctx->timer = NULL; }
    lvgl_port_lock(0);
    if (ctx->panel) { lv_obj_del(ctx->panel); ctx->panel = NULL; }
    windows_exit_editor_mode();
    lvgl_port_unlock();
    s_list_active = NULL;
    if (ctx->eg) xEventGroupSetBits(ctx->eg, MODAL_EVENT_CLOSED);
}

static bool list_handle_usb_key(void *ctx_ptr, uint8_t key_code, uint8_t modifiers, char ascii)
{
    (void)modifiers;
    list_ctx_t *ctx = (list_ctx_t *)ctx_ptr;
    if (key_code == 0x29) { ctx->result = -1; surf_request_close(ctx->eg); return true; }
    if (key_code == 0x28) { if (ctx->selected >= 0 && ctx->selected < ctx->count) { ctx->result = ctx->selected; } surf_request_close(ctx->eg); return true; }
    if (key_code == 0x52) { if (ctx->selected > 0) ctx->selected--; return true; } /* Up */
    if (key_code == 0x51) { if (ctx->selected < ctx->count - 1) ctx->selected++; return true; } /* Down */
    if (ascii >= '1' && ascii <= '9') { int idx = ascii - '1'; if (idx < ctx->count) { ctx->result = idx; surf_request_close(ctx->eg); return true; } }
    return false;
}

static bool list_handle_serial_line(void *ctx_ptr, const char *line)
{
    list_ctx_t *ctx = (list_ctx_t *)ctx_ptr;
    if (!line) return false;
    if (strcasecmp(line, "q") == 0 || strcasecmp(line, "quit") == 0 || strcasecmp(line, "cancel") == 0) { ctx->result = -1; surf_request_close(ctx->eg); return true; }
    char *end = NULL;
    long v = strtol(line, &end, 10);
    if (end != line && v >= 1 && v <= ctx->count) { ctx->result = (int)(v - 1); surf_request_close(ctx->eg); return true; }
    /* Try label match */
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->items[i] && strcasecmp(line, ctx->items[i]) == 0) { ctx->result = i; surf_request_close(ctx->eg); return true; }
    }
    return false;
}

static const modal_surface_t list_surface = {
    .name = "list",
    .open = list_surface_open,
    .close = list_surface_close,
    .handle_usb_key = list_handle_usb_key,
    .handle_serial_line = list_handle_serial_line,
};

int modal_list_run(const char *title, const char **items, int count, uint32_t timeout_ms)
{
    list_ctx_t ctx = {0};
    int errorlevel = 0;
    ctx.title = title;
    ctx.items = items;
    ctx.count = count;
    ctx.timeout_ms = timeout_ms ? timeout_ms : P4_CONFIG_TUI_TIMEOUT_DEFAULT_MS;
    if (count <= 0 || items == NULL) return -1;
    if (modal_surface_run(&list_surface, &ctx, &errorlevel) != ESP_OK) return -1;
    return ctx.result;
}

/* ========================================================================
 * ASK
 * ======================================================================== */

typedef struct {
    EventGroupHandle_t eg;
    int result; /* 0 ok, -1 cancel */
    uint32_t timeout_ms;
    TimerHandle_t timer;
    lv_obj_t *panel;
    lv_obj_t *ta;
    const char *prompt;
    const char *def;
    bool password;
    char *out;
    size_t out_size;
} ask_ctx_t;

static ask_ctx_t *s_ask_active = NULL;

static void ask_ok_cb(lv_event_t *e) { (void)e; if (s_ask_active) { s_ask_active->result = 0; surf_request_close(s_ask_active->eg); } }
static void ask_cancel_cb(lv_event_t *e) { (void)e; if (s_ask_active) { s_ask_active->result = -1; surf_request_close(s_ask_active->eg); } }

static bool ask_surface_open(void *ctx_ptr, EventGroupHandle_t eg)
{
    ask_ctx_t *ctx = (ask_ctx_t *)ctx_ptr;
    ctx->eg = eg;
    ctx->result = -1;
    s_ask_active = ctx;
    if (ctx->timeout_ms > 0) {
        ctx->timer = xTimerCreate("ask_tmr", pdMS_TO_TICKS(ctx->timeout_ms), pdFALSE, (void *)eg, surf_timeout_cb);
        if (ctx->timer) xTimerStart(ctx->timer, 0);
    }
    lvgl_port_lock(0);
    lv_obj_t *surf = windows_enter_editor_mode();
    if (!surf) { lvgl_port_unlock(); s_ask_active = NULL; return false; }
    ctx->panel = surf_create_container(surf);
    surf_create_title(ctx->panel, "Input");
    lv_obj_t *prompt = lv_label_create(ctx->panel);
    lv_label_set_text(prompt, ctx->prompt ? ctx->prompt : "");
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
    ctx->ta = lv_textarea_create(ctx->panel);
    lv_obj_set_width(ctx->ta, LV_PCT(100));
    lv_textarea_set_one_line(ctx->ta, true);
    lv_textarea_set_password_mode(ctx->ta, ctx->password);
    if (ctx->prompt) lv_textarea_set_placeholder_text(ctx->ta, ctx->prompt);
    if (ctx->def) lv_textarea_set_text(ctx->ta, ctx->def);
    keyboard_bind_textarea(ctx->ta);
    lv_obj_t *row = lv_obj_create(ctx->panel);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *b1 = surf_create_button(row, "OK");
    lv_obj_add_event_cb(b1, ask_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b2 = surf_create_button(row, "Cancel");
    lv_obj_add_event_cb(b2, ask_cancel_cb, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();
    windows_refresh_editor_surface();
    return true;
}

static void ask_surface_close(void *ctx_ptr)
{
    ask_ctx_t *ctx = (ask_ctx_t *)ctx_ptr;
    if (ctx->timer) { xTimerStop(ctx->timer, 0); xTimerDelete(ctx->timer, 0); ctx->timer = NULL; }
    /* Unbind keyboard before deleting textarea to avoid event to deleted object */
    lvgl_port_lock(0);
    keyboard_bind_textarea(NULL);
    lvgl_port_unlock();
    /* Capture textarea text before destroying */
    if (ctx->ta && ctx->out && ctx->out_size) {
        lvgl_port_lock(0);
        const char *txt = lv_textarea_get_text(ctx->ta);
        if (txt) {
            strncpy(ctx->out, txt, ctx->out_size - 1);
            ctx->out[ctx->out_size - 1] = '\0';
        } else {
            ctx->out[0] = '\0';
        }
        lvgl_port_unlock();
    }
    lvgl_port_lock(0);
    if (ctx->panel) { lv_obj_del(ctx->panel); ctx->panel = NULL; ctx->ta = NULL; }
    windows_exit_editor_mode();
    lvgl_port_unlock();
    s_ask_active = NULL;
    if (ctx->eg) xEventGroupSetBits(ctx->eg, MODAL_EVENT_CLOSED);
}

static bool ask_handle_usb_key(void *ctx_ptr, uint8_t key_code, uint8_t modifiers, char ascii)
{
    (void)ctx_ptr; (void)key_code; (void)modifiers; (void)ascii;
    ask_ctx_t *ctx = (ask_ctx_t *)ctx_ptr;
    if (key_code == 0x29) { ctx->result = -1; surf_request_close(ctx->eg); return true; }
    if (key_code == 0x28) { ctx->result = 0; surf_request_close(ctx->eg); return true; }
    return false;
}

static bool ask_handle_serial_line(void *ctx_ptr, const char *line)
{
    ask_ctx_t *ctx = (ask_ctx_t *)ctx_ptr;
    ESP_LOGW("modal", "ask serial line: '%s'", line ? line : "(null)");
    if (line == NULL) return false;
    /* Serial line is the answer itself; empty line is cancel if no default, else OK with empty */
    if (ctx->out && ctx->out_size) {
        strncpy(ctx->out, line, ctx->out_size - 1);
        ctx->out[ctx->out_size - 1] = '\0';
    }
    ctx->result = 0;
    surf_request_close(ctx->eg);
    return true;
}

static const modal_surface_t ask_surface = {
    .name = "ask",
    .open = ask_surface_open,
    .close = ask_surface_close,
    .handle_usb_key = ask_handle_usb_key,
    .handle_serial_line = ask_handle_serial_line,
};

int modal_ask_run(const char *prompt, const char *default_text, bool password,
                  uint32_t timeout_ms, char *result, size_t result_size)
{
    ask_ctx_t ctx = {0};
    int errorlevel = 0;
    if (!result || result_size == 0) return -1;
    result[0] = '\0';
    ctx.prompt = prompt;
    ctx.def = default_text;
    ctx.password = password;
    ctx.out = result;
    ctx.out_size = result_size;
    ctx.timeout_ms = timeout_ms ? timeout_ms : P4_CONFIG_TUI_TIMEOUT_DEFAULT_MS;
    if (modal_surface_run(&ask_surface, &ctx, &errorlevel) != ESP_OK) return -1;
    if (ctx.result != 0) { result[0] = '\0'; return -1; }
    return 0;
}

/* ========================================================================
 * FILE BROWSER
 * ======================================================================== */

#define FB_MAX_ENTRIES  128
#define FB_MAX_PATH     320

typedef struct {
    EventGroupHandle_t eg;
    int result;
    uint32_t timeout_ms;
    TimerHandle_t timer;
    lv_obj_t *panel;
    lv_obj_t *path_label;
    lv_obj_t *list;
    const char *title;
    char current_path[FB_MAX_PATH];
    char selected_path[FB_MAX_PATH];
    char *entries[FB_MAX_ENTRIES];
    bool is_dir[FB_MAX_ENTRIES];
    int entry_count;
    int selected;
} fb_ctx_t;

static fb_ctx_t *s_fb_active = NULL;

static void fb_free_entries(fb_ctx_t *ctx)
{
    for (int i = 0; i < ctx->entry_count; i++) {
        if (ctx->entries[i]) { free(ctx->entries[i]); ctx->entries[i] = NULL; }
    }
    ctx->entry_count = 0;
}

static int fb_entry_cmp(const void *a, const void *b) __attribute__((unused));
static int fb_entry_cmp(const void *a, const void *b)
{
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcasecmp(sa, sb);
}

static void fb_refresh_list(fb_ctx_t *ctx);

static void fb_entry_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_fb_active || idx < 0 || idx >= s_fb_active->entry_count) return;
    fb_ctx_t *ctx = s_fb_active;
    ctx->selected = idx;
    const char *path = ctx->entries[idx];
    strncpy(ctx->selected_path, path, FB_MAX_PATH - 1);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        strncpy(ctx->current_path, path, FB_MAX_PATH - 1);
        fb_refresh_list(ctx);
    } else {
        ctx->result = 0;
        surf_request_close(ctx->eg);
    }
}
static void fb_up_cb(lv_event_t *e) { (void)e; if (!s_fb_active) return; fb_ctx_t *ctx = s_fb_active; char *slash = strrchr(ctx->current_path, '/'); if (slash && slash != ctx->current_path) { *slash = '\0'; if (ctx->current_path[0] == '\0') strcpy(ctx->current_path, "/"); } else if (strcmp(ctx->current_path, "/sdcard") == 0) strcpy(ctx->current_path, "/"); else if (strcmp(ctx->current_path, "/") != 0) { char *s2 = strrchr(ctx->current_path, '/'); if (s2) { if (s2 == ctx->current_path) *(s2 + 1) = '\0'; else *s2 = '\0'; } } fb_refresh_list(ctx); }
static void fb_cancel_cb(lv_event_t *e) { (void)e; if (s_fb_active) { s_fb_active->result = -1; surf_request_close(s_fb_active->eg); } }
static void fb_select_cb(lv_event_t *e) { (void)e; if (!s_fb_active) return; fb_ctx_t *ctx = s_fb_active; if (ctx->selected >= 0 && ctx->selected < ctx->entry_count) { strncpy(ctx->selected_path, ctx->entries[ctx->selected], FB_MAX_PATH - 1); ctx->result = 0; surf_request_close(ctx->eg); } }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void fb_refresh_list(fb_ctx_t *ctx)
{
    fb_free_entries(ctx);
    lvgl_port_lock(0);
    if (ctx->list) lv_obj_clean(ctx->list);
    if (ctx->path_label) lv_label_set_text(ctx->path_label, ctx->current_path);
    lvgl_port_unlock();

    DIR *dir = opendir(ctx->current_path);
    if (!dir) return;
    struct dirent *ent;
    char *names[FB_MAX_ENTRIES] = {0};
    char *paths[FB_MAX_ENTRIES] = {0};
    bool isdir[FB_MAX_ENTRIES] = {0};
    int n = 0;
    while ((ent = readdir(dir)) != NULL && n < FB_MAX_ENTRIES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (ent->d_name[0] == '.' ) continue; /* hidden */
        if (strlen(ctx->current_path) + 1 + strlen(ent->d_name) >= FB_MAX_PATH) continue;
        char full[FB_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", ctx->current_path, ent->d_name);
        /* trim double slash */
        char *p = strstr(full, "//");
        if (p) memmove(p, p + 1, strlen(p));
        struct stat st;
        if (stat(full, &st) != 0) continue;
        names[n] = strdup(ent->d_name);
        paths[n] = strdup(full);
        isdir[n] = S_ISDIR(st.st_mode);
        if (!names[n] || !paths[n]) { free(names[n]); free(paths[n]); continue; }
        n++;
    }
    closedir(dir);

    /* Sort: dirs first, then alpha */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            bool swap = false;
            if (isdir[j] && !isdir[i]) swap = true;
            else if (isdir[i] == isdir[j] && strcasecmp(names[i], names[j]) > 0) swap = true;
            if (swap) {
                char *tn = names[i]; names[i] = names[j]; names[j] = tn;
                char *tp = paths[i]; paths[i] = paths[j]; paths[j] = tp;
                bool td = isdir[i]; isdir[i] = isdir[j]; isdir[j] = td;
            }
        }
    }

    lvgl_port_lock(0);
    for (int i = 0; i < n && ctx->entry_count < FB_MAX_ENTRIES; i++) {
        char label[260];
        if (isdir[i]) snprintf(label, sizeof(label), "[D] %s", names[i]);
        else snprintf(label, sizeof(label), "    %s", names[i]);
        lv_obj_t *btn = lv_btn_create(ctx->list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_add_event_cb(btn, fb_entry_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ctx->entry_count);
        ctx->entries[ctx->entry_count] = paths[i];
        ctx->is_dir[ctx->entry_count] = isdir[i];
        ctx->entry_count++;
        free(names[i]);
        /* paths[i] ownership transferred to ctx->entries */
    }
    /* free remaining names where not transferred (should be none) */
    for (int i = ctx->entry_count; i < n; i++) { free(names[i]); free(paths[i]); }
    ctx->selected = 0;
    lvgl_port_unlock();
}
#pragma GCC diagnostic pop

static bool fb_surface_open(void *ctx_ptr, EventGroupHandle_t eg)
{
    fb_ctx_t *ctx = (fb_ctx_t *)ctx_ptr;
    ctx->eg = eg;
    ctx->result = -1;
    s_fb_active = ctx;
    if (ctx->current_path[0] == '\0') {
        strcpy(ctx->current_path, "/sdcard");
        struct stat st;
        if (stat(ctx->current_path, &st) != 0 || !S_ISDIR(st.st_mode)) strcpy(ctx->current_path, "/");
    }
    if (ctx->timeout_ms > 0) {
        ctx->timer = xTimerCreate("fb_tmr", pdMS_TO_TICKS(ctx->timeout_ms), pdFALSE, (void *)eg, surf_timeout_cb);
        if (ctx->timer) xTimerStart(ctx->timer, 0);
    }
    lvgl_port_lock(0);
    lv_obj_t *surf = windows_enter_editor_mode();
    if (!surf) { lvgl_port_unlock(); s_fb_active = NULL; return false; }
    ctx->panel = surf_create_container(surf);
    surf_create_title(ctx->panel, ctx->title ? ctx->title : "Browse");
    ctx->path_label = lv_label_create(ctx->panel);
    lv_obj_set_width(ctx->path_label, LV_PCT(100));
    lv_obj_set_style_text_color(ctx->path_label, lv_color_hex(0x00FFFF), 0);
    lv_label_set_text(ctx->path_label, ctx->current_path);
    ctx->list = lv_obj_create(ctx->panel);
    lv_obj_set_size(ctx->list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(ctx->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ctx->list, 2, 0);
    lv_obj_t *row = lv_obj_create(ctx->panel);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *b1 = surf_create_button(row, "Up");
    lv_obj_add_event_cb(b1, fb_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b2 = surf_create_button(row, "Select");
    lv_obj_add_event_cb(b2, fb_select_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b3 = surf_create_button(row, "Cancel");
    lv_obj_add_event_cb(b3, fb_cancel_cb, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();
    fb_refresh_list(ctx);
    windows_refresh_editor_surface();
    return true;
}

static void fb_surface_close(void *ctx_ptr)
{
    fb_ctx_t *ctx = (fb_ctx_t *)ctx_ptr;
    if (ctx->timer) { xTimerStop(ctx->timer, 0); xTimerDelete(ctx->timer, 0); ctx->timer = NULL; }
    fb_free_entries(ctx);
    lvgl_port_lock(0);
    if (ctx->panel) { lv_obj_del(ctx->panel); ctx->panel = NULL; ctx->list = NULL; ctx->path_label = NULL; }
    windows_exit_editor_mode();
    lvgl_port_unlock();
    s_fb_active = NULL;
    if (ctx->eg) xEventGroupSetBits(ctx->eg, MODAL_EVENT_CLOSED);
}

static bool fb_handle_usb_key(void *ctx_ptr, uint8_t key_code, uint8_t modifiers, char ascii)
{
    (void)modifiers; (void)ascii;
    fb_ctx_t *ctx = (fb_ctx_t *)ctx_ptr;
    if (key_code == 0x29) { ctx->result = -1; surf_request_close(ctx->eg); return true; }
    if (key_code == 0x2A) { /* Backspace -> up */ char *s = strrchr(ctx->current_path, '/'); if (s && s != ctx->current_path) { *s = '\0'; } fb_refresh_list(ctx); return true; }
    if (key_code == 0x52) { if (ctx->selected > 0) ctx->selected--; return true; }
    if (key_code == 0x51) { if (ctx->selected < ctx->entry_count - 1) ctx->selected++; return true; }
    if (key_code == 0x28) { if (ctx->selected >= 0 && ctx->selected < ctx->entry_count) { const char *p = ctx->entries[ctx->selected]; struct stat st; if (stat(p, &st)==0 && S_ISDIR(st.st_mode)) { strncpy(ctx->current_path, p, FB_MAX_PATH-1); fb_refresh_list(ctx);} else { strncpy(ctx->selected_path, p, FB_MAX_PATH-1); ctx->result=0; surf_request_close(ctx->eg);} } return true; }
    return false;
}

static bool fb_handle_serial_line(void *ctx_ptr, const char *line)
{
    fb_ctx_t *ctx = (fb_ctx_t *)ctx_ptr;
    if (!line) return false;
    if (strcasecmp(line, "q")==0 || strcasecmp(line, "quit")==0 || strcasecmp(line, "cancel")==0) { ctx->result=-1; surf_request_close(ctx->eg); return true; }
    if (strcasecmp(line, "up")==0 || strcmp(line, "..")==0) { char *s=strrchr(ctx->current_path,'/'); if(s && s!=ctx->current_path) *s='\0'; fb_refresh_list(ctx); return true; }
    /* Try to match entry name */
    for (int i=0;i<ctx->entry_count;i++) {
        const char *base = strrchr(ctx->entries[i], '/');
        base = base ? base+1 : ctx->entries[i];
        if (strcasecmp(line, base)==0 || strcasecmp(line, ctx->entries[i])==0) {
            struct stat st; if (stat(ctx->entries[i],&st)==0 && S_ISDIR(st.st_mode)) { strncpy(ctx->current_path, ctx->entries[i], FB_MAX_PATH-1); fb_refresh_list(ctx); } else { strncpy(ctx->selected_path, ctx->entries[i], FB_MAX_PATH-1); ctx->result=0; surf_request_close(ctx->eg); } return true;
        }
    }
    return false;
}

static const modal_surface_t fb_surface = {
    .name = "filebrowser",
    .open = fb_surface_open,
    .close = fb_surface_close,
    .handle_usb_key = fb_handle_usb_key,
    .handle_serial_line = fb_handle_serial_line,
};

int modal_filebrowser_run(const char *title, const char *start_path,
                          char *selected_path, size_t path_size,
                          uint32_t timeout_ms)
{
    fb_ctx_t ctx = {0};
    int el = 0;
    if (!selected_path || path_size==0) return -1;
    selected_path[0] = '\0';
    ctx.title = title;
    ctx.timeout_ms = timeout_ms ? timeout_ms : P4_CONFIG_TUI_TIMEOUT_DEFAULT_MS;
    if (start_path && start_path[0]) strncpy(ctx.current_path, start_path, FB_MAX_PATH-1);
    if (modal_surface_run(&fb_surface, &ctx, &el) != ESP_OK) return -1;
    if (ctx.result != 0 || ctx.selected_path[0]=='\0') return -1;
    strncpy(selected_path, ctx.selected_path, path_size-1);
    selected_path[path_size-1]='\0';
    return 0;
}

/* ========================================================================
 * VIEWER
 * ======================================================================== */

typedef struct {
    EventGroupHandle_t eg;
    int result;
    uint32_t timeout_ms;
    TimerHandle_t timer;
    lv_obj_t *panel;
    lv_obj_t *ta;
    const char *title;
    const char *path;
    char *content;
    size_t content_size;
} viewer_ctx_t;

static viewer_ctx_t *s_viewer_active = NULL;
static void viewer_close_btn_cb(lv_event_t *e) { (void)e; if (s_viewer_active) { s_viewer_active->result = 0; surf_request_close(s_viewer_active->eg); } }

static bool viewer_surface_open(void *ctx_ptr, EventGroupHandle_t eg)
{
    viewer_ctx_t *ctx = (viewer_ctx_t *)ctx_ptr;
    ctx->eg = eg;
    ctx->result = 0;
    s_viewer_active = ctx;
    if (ctx->timeout_ms > 0) {
        ctx->timer = xTimerCreate("vw_tmr", pdMS_TO_TICKS(ctx->timeout_ms), pdFALSE, (void *)eg, surf_timeout_cb);
        if (ctx->timer) xTimerStart(ctx->timer, 0);
    }
    /* Load file */
    if (ctx->path && ctx->path[0]) {
        FILE *f = fopen(ctx->path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz < 0) sz = 0;
            if ((size_t)sz > P4_CONFIG_TUI_VIEW_MAX_BYTES) sz = P4_CONFIG_TUI_VIEW_MAX_BYTES;
            ctx->content = malloc((size_t)sz + 1);
            if (ctx->content) {
                size_t r = fread(ctx->content, 1, (size_t)sz, f);
                ctx->content[r] = '\0';
                ctx->content_size = r;
                /* Ensure printable: replace NUL etc */
                for (size_t i=0;i<r;i++) if (ctx->content[i]=='\0') ctx->content[i]=' ';
            }
            fclose(f);
        }
        if (!ctx->content) {
            ctx->content = strdup("(cannot open file)");
            ctx->content_size = ctx->content ? strlen(ctx->content) : 0;
        }
    } else {
        ctx->content = strdup("(no file)");
        ctx->content_size = ctx->content ? strlen(ctx->content) : 0;
    }

    lvgl_port_lock(0);
    lv_obj_t *surf = windows_enter_editor_mode();
    if (!surf) { lvgl_port_unlock(); s_viewer_active=NULL; return false; }
    ctx->panel = surf_create_container(surf);
    surf_create_title(ctx->panel, ctx->title ? ctx->title : (ctx->path ? ctx->path : "Viewer"));
    ctx->ta = lv_textarea_create(ctx->panel);
    lv_obj_set_size(ctx->ta, LV_PCT(100), LV_PCT(100));
    lv_textarea_set_text(ctx->ta, ctx->content ? ctx->content : "");
    lv_obj_set_style_text_font(ctx->ta, windows_get_terminal_font(), 0);
    lv_textarea_set_cursor_click_pos(ctx->ta, false);
    lv_obj_t *row = lv_obj_create(ctx->panel);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *btn = surf_create_button(row, "Close");
    lv_obj_add_event_cb(btn, viewer_close_btn_cb, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();
    windows_refresh_editor_surface();
    return true;
}

static void viewer_surface_close(void *ctx_ptr)
{
    viewer_ctx_t *ctx = (viewer_ctx_t *)ctx_ptr;
    if (ctx->timer) { xTimerStop(ctx->timer,0); xTimerDelete(ctx->timer,0); ctx->timer=NULL; }
    if (ctx->content) { free(ctx->content); ctx->content=NULL; }
    lvgl_port_lock(0);
    if (ctx->panel) { lv_obj_del(ctx->panel); ctx->panel=NULL; ctx->ta=NULL; }
    windows_exit_editor_mode();
    lvgl_port_unlock();
    s_viewer_active=NULL;
    if (ctx->eg) xEventGroupSetBits(ctx->eg, MODAL_EVENT_CLOSED);
}

static bool viewer_handle_usb_key(void *ctx_ptr, uint8_t key_code, uint8_t modifiers, char ascii)
{
    (void)ctx_ptr; (void)modifiers; (void)ascii;
    viewer_ctx_t *ctx = (viewer_ctx_t *)ctx_ptr;
    if (key_code==0x29 || key_code==0x28 || ascii=='q' || ascii=='Q') { ctx->result=0; surf_request_close(ctx->eg); return true; }
    return false;
}
static bool viewer_handle_serial_line(void *ctx_ptr, const char *line)
{
    viewer_ctx_t *ctx = (viewer_ctx_t *)ctx_ptr;
    if (!line) return false;
    if (strcasecmp(line,"q")==0 || strcasecmp(line,"quit")==0 || strcasecmp(line,"close")==0 || strcmp(line,"")==0) { ctx->result=0; surf_request_close(ctx->eg); return true; }
    return false;
}

static const modal_surface_t viewer_surface = {
    .name = "viewer",
    .open = viewer_surface_open,
    .close = viewer_surface_close,
    .handle_usb_key = viewer_handle_usb_key,
    .handle_serial_line = viewer_handle_serial_line,
};

int modal_viewer_run(const char *title, const char *file_path, uint32_t timeout_ms)
{
    viewer_ctx_t ctx = {0};
    int el=0;
    ctx.title = title;
    ctx.path = file_path;
    ctx.timeout_ms = timeout_ms ? timeout_ms : P4_CONFIG_TUI_TIMEOUT_DEFAULT_MS;
    if (modal_surface_run(&viewer_surface, &ctx, &el) != ESP_OK) return -1;
    return 0;
}

/* ========================================================================
 * HEXVIEW
 * ======================================================================== */

typedef struct {
    EventGroupHandle_t eg;
    int result;
    uint32_t timeout_ms;
    TimerHandle_t timer;
    lv_obj_t *panel;
    lv_obj_t *ta;
    const char *title;
    const char *path;
    char *content;
} hex_ctx_t;

static hex_ctx_t *s_hex_active = NULL;
static void hex_close_btn_cb(lv_event_t *e) { (void)e; if (s_hex_active) { s_hex_active->result = 0; surf_request_close(s_hex_active->eg); } }

static bool hex_surface_open(void *ctx_ptr, EventGroupHandle_t eg)
{
    hex_ctx_t *ctx = (hex_ctx_t *)ctx_ptr;
    ctx->eg = eg;
    ctx->result = 0;
    s_hex_active = ctx;
    if (ctx->timeout_ms > 0) {
        ctx->timer = xTimerCreate("hx_tmr", pdMS_TO_TICKS(ctx->timeout_ms), pdFALSE, (void *)eg, surf_timeout_cb);
        if (ctx->timer) xTimerStart(ctx->timer, 0);
    }
    /* Load binary and format hex dump */
    FILE *f = ctx->path ? fopen(ctx->path, "rb") : NULL;
    size_t max_bytes = P4_CONFIG_TUI_VIEW_MAX_BYTES;
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0) sz = 0;
        if ((size_t)sz > max_bytes) sz = (long)max_bytes;
        uint8_t *buf = malloc((size_t)sz);
        size_t r = buf ? fread(buf, 1, (size_t)sz, f) : 0;
        fclose(f);
        if (buf && r>0) {
            /* Each line: 8 hex offset + 16 bytes hex + ascii -> ~80 chars */
            size_t lines = (r + 15) / 16;
            size_t cap = lines * 80 + 32;
            ctx->content = malloc(cap);
            if (ctx->content) {
                size_t pos = 0;
                for (size_t off=0; off<r; off+=16) {
                    int n = snprintf(ctx->content+pos, cap-pos, "%08X  ", (unsigned)off);
                    if (n<0) break;
                    if ((size_t)n >= cap-pos) { pos = cap-1; break; } // truncated: stop, keep NUL room
                    pos += (size_t)n;
                    for (int i=0;i<16;i++) {
                        if (off+i < r) n = snprintf(ctx->content+pos, cap-pos, "%02X ", buf[off+i]);
                        else n = snprintf(ctx->content+pos, cap-pos, "   ");
                        if (n<0) break;
                        if ((size_t)n >= cap-pos) { pos = cap-1; break; }
                        pos += (size_t)n;
                    }
                    if (pos >= cap-1) break;
                    n = snprintf(ctx->content+pos, cap-pos, " |");
                    if (n<0) break;
                    if ((size_t)n >= cap-pos) { pos = cap-1; break; }
                    pos += (size_t)n;
                    for (int i=0;i<16 && off+i<r;i++) {
                        char c = (char)buf[off+i];
                        if (c < 32 || c > 126) c='.';
                        if (pos+1 < cap) ctx->content[pos++]=c;
                    }
                    if (pos+2 < cap) { ctx->content[pos++]='|'; ctx->content[pos++]='\n'; ctx->content[pos]='\0'; }
                }
            }
        }
        free(buf);
        if (!ctx->content) ctx->content = strdup("(cannot read file)");
    } else {
        ctx->content = strdup("(cannot open file)");
    }

    lvgl_port_lock(0);
    lv_obj_t *surf = windows_enter_editor_mode();
    if (!surf) { lvgl_port_unlock(); s_hex_active=NULL; return false; }
    ctx->panel = surf_create_container(surf);
    surf_create_title(ctx->panel, ctx->title ? ctx->title : (ctx->path ? ctx->path : "Hexview"));
    ctx->ta = lv_textarea_create(ctx->panel);
    lv_obj_set_size(ctx->ta, LV_PCT(100), LV_PCT(100));
    lv_textarea_set_text(ctx->ta, ctx->content ? ctx->content : "");
    lv_obj_set_style_text_font(ctx->ta, windows_get_terminal_font(), 0);
    lv_obj_t *row = lv_obj_create(ctx->panel);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *btn = surf_create_button(row, "Close");
    lv_obj_add_event_cb(btn, hex_close_btn_cb, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();
    windows_refresh_editor_surface();
    return true;
}

static void hex_surface_close(void *ctx_ptr)
{
    hex_ctx_t *ctx = (hex_ctx_t *)ctx_ptr;
    if (ctx->timer) { xTimerStop(ctx->timer,0); xTimerDelete(ctx->timer,0); ctx->timer=NULL; }
    if (ctx->content) { free(ctx->content); ctx->content=NULL; }
    lvgl_port_lock(0);
    if (ctx->panel) { lv_obj_del(ctx->panel); ctx->panel=NULL; ctx->ta=NULL; }
    windows_exit_editor_mode();
    lvgl_port_unlock();
    s_hex_active=NULL;
    if (ctx->eg) xEventGroupSetBits(ctx->eg, MODAL_EVENT_CLOSED);
}

static bool hex_handle_usb_key(void *ctx_ptr, uint8_t key_code, uint8_t modifiers, char ascii)
{
    (void)ctx_ptr; (void)modifiers; (void)ascii;
    hex_ctx_t *ctx = (hex_ctx_t *)ctx_ptr;
    if (key_code==0x29 || key_code==0x28 || ascii=='q' || ascii=='Q') { ctx->result=0; surf_request_close(ctx->eg); return true; }
    return false;
}
static bool hex_handle_serial_line(void *ctx_ptr, const char *line)
{
    hex_ctx_t *ctx = (hex_ctx_t *)ctx_ptr;
    if (!line) return false;
    if (strcasecmp(line,"q")==0 || strcasecmp(line,"quit")==0 || strcasecmp(line,"close")==0 || strcmp(line,"")==0) { ctx->result=0; surf_request_close(ctx->eg); return true; }
    return false;
}

static const modal_surface_t hex_surface = {
    .name = "hexview",
    .open = hex_surface_open,
    .close = hex_surface_close,
    .handle_usb_key = hex_handle_usb_key,
    .handle_serial_line = hex_handle_serial_line,
};

int modal_hexview_run(const char *title, const char *file_path, uint32_t timeout_ms)
{
    hex_ctx_t ctx = {0};
    int el=0;
    ctx.title = title;
    ctx.path = file_path;
    ctx.timeout_ms = timeout_ms ? timeout_ms : P4_CONFIG_TUI_TIMEOUT_DEFAULT_MS;
    if (modal_surface_run(&hex_surface, &ctx, &el) != ESP_OK) return -1;
    return 0;
}

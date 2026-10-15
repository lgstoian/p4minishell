/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tui_pane.c
 * @brief TUI workspace implementation: panes, widgets, HUD, events, taps, keys.
 */

#include "tui_pane.h"
#include "tui.h"
#include "shell.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========================================================================
 * PANE STACK
 * ======================================================================== */

typedef struct {
    char name[P4_CONFIG_WS_PANE_NAME_BYTES];
    char title[P4_CONFIG_WS_PANE_TITLE_BYTES];
    int x, y, w, h;
    bool used;
    bool hidden;
    char *scrollback[P4_CONFIG_WS_PANE_SCROLLBACK_LINES];
    int sb_count;
    int sb_head;
} ws_pane_t;

static ws_pane_t s_panes[P4_CONFIG_WS_PANE_MAX];
static int s_pane_count = 0;
static int s_focus_index = -1;

static ws_pane_t *pane_find(const char *name)
{
    int i;
    if (name == NULL) return NULL;
    for (i = 0; i < P4_CONFIG_WS_PANE_MAX; i++) {
        if (s_panes[i].used && strcmp(s_panes[i].name, name) == 0) {
            return &s_panes[i];
        }
    }
    return NULL;
}

static ws_pane_t *pane_free_slot(void)
{
    int i;
    for (i = 0; i < P4_CONFIG_WS_PANE_MAX; i++) {
        if (!s_panes[i].used) return &s_panes[i];
    }
    return NULL;
}

static void pane_free_scrollback(ws_pane_t *p)
{
    int i;
    for (i = 0; i < p->sb_count; i++) {
        if (p->scrollback[i]) {
            free(p->scrollback[i]);
            p->scrollback[i] = NULL;
        }
    }
    p->sb_count = 0;
    p->sb_head = 0;
}

bool tui_pane_add(const char *name, int x, int y, int w, int h, const char *title)
{
    ws_pane_t *p;

    if (name == NULL || name[0] == '\0') return false;
    if (strlen(name) >= P4_CONFIG_WS_PANE_NAME_BYTES) return false;
    if (w < 3 || h < 3) return false;
    if (x < 1 || y < 1 || x + w - 1 > P4_CONFIG_TUI_COLS || y + h - 1 > P4_CONFIG_TUI_ROWS) return false;
    if (pane_find(name) != NULL) return false;

    p = pane_free_slot();
    if (p == NULL) return false;

    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    if (title != NULL) {
        snprintf(p->title, sizeof(p->title), "%s", title);
    }
    p->x = x;
    p->y = y;
    p->w = w;
    p->h = h;
    p->used = true;
    p->hidden = false;
    s_pane_count++;
    if (s_focus_index < 0) {
        int i;
        for (i = 0; i < P4_CONFIG_WS_PANE_MAX; i++) {
            if (&s_panes[i] == p) {
                s_focus_index = i;
                break;
            }
        }
    }
    return true;
}

bool tui_pane_close(const char *name)
{
    ws_pane_t *p = pane_find(name);
    int i;
    if (p == NULL) return false;
    pane_free_scrollback(p);
    i = (int)(p - s_panes);
    memset(p, 0, sizeof(*p));
    s_pane_count--;
    if (s_focus_index == i) {
        s_focus_index = -1;
        for (i = 0; i < P4_CONFIG_WS_PANE_MAX; i++) {
            if (s_panes[i].used && !s_panes[i].hidden) {
                s_focus_index = i;
                break;
            }
        }
    }
    return true;
}

bool tui_pane_focus(const char *name)
{
    ws_pane_t *p = pane_find(name);
    int i;
    if (p == NULL || p->hidden) return false;
    for (i = 0; i < P4_CONFIG_WS_PANE_MAX; i++) {
        if (&s_panes[i] == p) {
            s_focus_index = i;
            return true;
        }
    }
    return false;
}

bool tui_pane_cycle(void)
{
    int i;
    if (s_pane_count == 0) return false;
    for (i = 1; i <= P4_CONFIG_WS_PANE_MAX; i++) {
        int idx = (s_focus_index + i) % P4_CONFIG_WS_PANE_MAX;
        if (idx < 0) idx += P4_CONFIG_WS_PANE_MAX;
        if (s_panes[idx].used && !s_panes[idx].hidden) {
            s_focus_index = idx;
            return true;
        }
    }
    return false;
}

bool tui_pane_hide(const char *name, bool hide)
{
    ws_pane_t *p = pane_find(name);
    if (p == NULL) return false;
    p->hidden = hide;
    if (hide && s_focus_index >= 0) {
        if (&s_panes[s_focus_index] == p) {
            s_focus_index = -1;
            int i;
            for (i = 0; i < P4_CONFIG_WS_PANE_MAX; i++) {
                if (s_panes[i].used && !s_panes[i].hidden) {
                    s_focus_index = i;
                    break;
                }
            }
        }
    }
    return true;
}

bool tui_pane_split(const char *name, bool horizontal)
{
    ws_pane_t *p = pane_find(name);
    ws_pane_t *np;
    int nx, ny, nw, nh;

    if (p == NULL) return false;
    if (horizontal) {
        if (p->h < 6) return false;
        nh = p->h / 2;
        p->h = p->h - nh;
        nx = p->x;
        ny = p->y + p->h;
        nw = p->w;
    } else {
        if (p->w < 6) return false;
        nw = p->w / 2;
        p->w = p->w - nw;
        nx = p->x + p->w;
        ny = p->y;
        nh = p->h;
    }
    if (nx + nw - 1 > P4_CONFIG_TUI_COLS || ny + nh - 1 > P4_CONFIG_TUI_ROWS) return false;
    np = pane_free_slot();
    if (np == NULL) return false;

    memset(np, 0, sizeof(*np));
    snprintf(np->name, sizeof(np->name), "%s_%d", name, s_pane_count);
    np->x = nx;
    np->y = ny;
    np->w = nw;
    np->h = nh;
    np->used = true;
    s_pane_count++;
    return true;
}

int tui_pane_count(void) { return s_pane_count; }

bool tui_pane_get_name(int index, char *buf, size_t buf_size)
{
    int i, n = 0;
    if (buf == NULL || buf_size == 0) return false;
    for (i = 0; i < P4_CONFIG_WS_PANE_MAX; i++) {
        if (s_panes[i].used) {
            if (n == index) {
                snprintf(buf, buf_size, "%s", s_panes[i].name);
                return true;
            }
            n++;
        }
    }
    return false;
}

int tui_pane_get_focus(void)
{
    if (s_focus_index < 0) return -1;
    if (!s_panes[s_focus_index].used) return -1;
    return s_focus_index;
}

bool tui_pane_append(const char *name, const char *text)
{
    ws_pane_t *p = pane_find(name);
    char *copy;
    int idx;

    if (p == NULL || text == NULL) return false;
    copy = (char *)malloc(strlen(text) + 1);
    if (copy == NULL) return false;
    snprintf(copy, strlen(text) + 1, "%s", text);

    if (p->sb_count < P4_CONFIG_WS_PANE_SCROLLBACK_LINES) {
        idx = p->sb_count++;
    } else {
        idx = p->sb_head;
        if (p->scrollback[idx]) free(p->scrollback[idx]);
        p->scrollback[idx] = NULL;
        p->sb_head = (p->sb_head + 1) % P4_CONFIG_WS_PANE_SCROLLBACK_LINES;
    }
    p->scrollback[idx] = copy;
    return true;
}

bool tui_pane_clear(const char *name)
{
    ws_pane_t *p = pane_find(name);
    if (p == NULL) return false;
    pane_free_scrollback(p);
    return true;
}

void tui_pane_render(void)
{
    int i, j;
    for (i = 0; i < P4_CONFIG_WS_PANE_MAX; i++) {
        ws_pane_t *p = &s_panes[i];
        if (!p->used || p->hidden) continue;

        uint8_t fg = (p->title[0] != '\0') ? 14 : 7;
        uint8_t bg = 16;
        tui_draw_box(p->x, p->y, p->w, p->h, "single", fg, bg,
                     (p->title[0] != '\0') ? p->title : NULL);

        if (s_focus_index == i) {
            for (j = p->x + 1; j <= p->x + p->w - 2; j++) {
                tui_print_at(j, p->y, " ", 15, 4);
            }
        }

        int visible_lines = p->h - 2;
        int start = 0;
        if (p->sb_count > visible_lines) {
            start = p->sb_count - visible_lines;
        }
        for (j = 0; j < visible_lines; j++) {
            int sb_idx;
            int line_idx = start + j;
            if (line_idx >= p->sb_count) break;
            sb_idx = (p->sb_head + line_idx) % P4_CONFIG_WS_PANE_SCROLLBACK_LINES;
            if (p->scrollback[sb_idx] == NULL) continue;
            tui_print_at(p->x + 1, p->y + 1 + j, p->scrollback[sb_idx], 7, 16);
        }
    }
}

/* ========================================================================
 * WIDGET LAYER
 * ======================================================================== */

typedef struct {
    char name[P4_CONFIG_WS_WIDGET_NAME_BYTES];
    tui_widget_type_t type;
    char label[P4_CONFIG_WS_WIDGET_LABEL_BYTES];
    char text[P4_CONFIG_WS_EVENT_PAYLOAD_BYTES];
    int x, y, w, h;
    bool used;
    char bind_label[P4_CONFIG_WS_PANE_TITLE_BYTES];
    int value;
    int max_val;
} ws_widget_t;

static ws_widget_t s_widgets[P4_CONFIG_WS_WIDGET_MAX];

static ws_widget_t *widget_find(const char *name)
{
    int i;
    if (name == NULL) return NULL;
    for (i = 0; i < P4_CONFIG_WS_WIDGET_MAX; i++) {
        if (s_widgets[i].used && strcmp(s_widgets[i].name, name) == 0) {
            return &s_widgets[i];
        }
    }
    return NULL;
}

static tui_widget_type_t widget_type_from_str(const char *type_str)
{
    if (shell_text_equals_ignore_case(type_str, "text")) return TUI_WIDGET_TEXT;
    if (shell_text_equals_ignore_case(type_str, "gauge")) return TUI_WIDGET_GAUGE;
    if (shell_text_equals_ignore_case(type_str, "button")) return TUI_WIDGET_BUTTON;
    if (shell_text_equals_ignore_case(type_str, "list")) return TUI_WIDGET_LIST;
    if (shell_text_equals_ignore_case(type_str, "panel")) return TUI_WIDGET_PANEL;
    return TUI_WIDGET_TEXT;
}

bool tui_widget_add(const char *name, const char *type_str, int x, int y,
                    int w, int h, const char *label)
{
    ws_widget_t *wd;
    int i;

    if (name == NULL || name[0] == '\0' || type_str == NULL) return false;
    if (strlen(name) >= P4_CONFIG_WS_WIDGET_NAME_BYTES) return false;

    wd = widget_find(name);
    if (wd == NULL) {
        for (i = 0; i < P4_CONFIG_WS_WIDGET_MAX; i++) {
            if (!s_widgets[i].used) {
                wd = &s_widgets[i];
                break;
            }
        }
    }
    if (wd == NULL) return false;

    memset(wd, 0, sizeof(*wd));
    snprintf(wd->name, sizeof(wd->name), "%s", name);
    wd->type = widget_type_from_str(type_str);
    wd->x = x;
    wd->y = y;
    wd->w = w;
    wd->h = h;
    if (label != NULL) {
        snprintf(wd->label, sizeof(wd->label), "%s", label);
    }
    wd->used = true;
    wd->value = 0;
    wd->max_val = 100;
    return true;
}

bool tui_widget_set_text(const char *name, const char *text)
{
    ws_widget_t *wd = widget_find(name);
    if (wd == NULL || text == NULL) return false;
    snprintf(wd->text, sizeof(wd->text), "%s", text);
    return true;
}

bool tui_widget_set_value(const char *name, int value)
{
    ws_widget_t *wd = widget_find(name);
    if (wd == NULL) return false;
    wd->value = value;
    return true;
}

bool tui_widget_set_max(const char *name, int max_val)
{
    ws_widget_t *wd = widget_find(name);
    if (wd == NULL || max_val <= 0) return false;
    wd->max_val = max_val;
    return true;
}

bool tui_widget_bind_label(const char *name, const char *label)
{
    ws_widget_t *wd = widget_find(name);
    if (wd == NULL || label == NULL) return false;
    snprintf(wd->bind_label, sizeof(wd->bind_label), "%s", label);
    return true;
}

bool tui_widget_remove(const char *name)
{
    ws_widget_t *wd = widget_find(name);
    if (wd == NULL) return false;
    memset(wd, 0, sizeof(*wd));
    return true;
}

bool tui_widget_exists(const char *name)
{
    return widget_find(name) != NULL;
}

void tui_widget_render(void)
{
    int i;
    for (i = 0; i < P4_CONFIG_WS_WIDGET_MAX; i++) {
        ws_widget_t *wd = &s_widgets[i];
        if (!wd->used) continue;

        switch (wd->type) {
        case TUI_WIDGET_TEXT: {
            char buf[P4_CONFIG_WS_EVENT_PAYLOAD_BYTES + P4_CONFIG_WS_WIDGET_LABEL_BYTES];
            snprintf(buf, sizeof(buf), "%s%s",
                     (wd->label[0] != '\0') ? wd->label : "",
                     wd->text);
            tui_print_at(wd->x, wd->y, buf, 15, 16);
            break;
        }
        case TUI_WIDGET_GAUGE: {
            int pct = 0;
            if (wd->max_val > 0) {
                pct = (wd->value * 100) / wd->max_val;
                if (pct > 100) pct = 100;
                if (pct < 0) pct = 0;
            }
            tui_draw_bar(wd->x, wd->y, wd->w, pct, 0xDB, 0xB0, 10, 16);
            break;
        }
        case TUI_WIDGET_BUTTON: {
            char buf[P4_CONFIG_WS_WIDGET_LABEL_BYTES + 4];
            snprintf(buf, sizeof(buf), "[ %s ]",
                     (wd->label[0] != '\0') ? wd->label : "Btn");
            tui_print_at(wd->x, wd->y, buf, 14, 16);
            break;
        }
        case TUI_WIDGET_LIST: {
            tui_print_at(wd->x, wd->y, (wd->label[0] != '\0') ? wd->label : "", 7, 16);
            tui_print_at(wd->x, wd->y + 1, (wd->value >= 0) ? ">" : " ", 10, 16);
            break;
        }
        case TUI_WIDGET_PANEL:
        default:
            tui_draw_box(wd->x, wd->y, wd->w, wd->h, "single", 7, 16,
                         (wd->label[0] != '\0') ? wd->label : NULL);
            break;
        }
    }
}

/* ========================================================================
 * HUD REGION
 * ======================================================================== */

typedef struct {
    char name[P4_CONFIG_WS_HUD_NAME_BYTES];
    char text[P4_CONFIG_WS_HUD_FORMAT_BYTES];
    char last_rendered[P4_CONFIG_WS_HUD_FORMAT_BYTES];
    int position;
    bool used;
    int flash_left;
} ws_hud_cell_t;

static ws_hud_cell_t s_hud_cells[P4_CONFIG_WS_HUD_MAX];
static uint32_t s_hud_refresh_ms = P4_CONFIG_WS_HUD_REFRESH_MS;
static uint32_t s_hud_last_refresh = 0;
static uint8_t s_hud_fg = 7;
static uint8_t s_hud_bg = 16;

bool tui_hud_add(const char *name, int position)
{
    int i;
    if (name == NULL || name[0] == '\0') return false;
    for (i = 0; i < P4_CONFIG_WS_HUD_MAX; i++) {
        if (!s_hud_cells[i].used) {
            snprintf(s_hud_cells[i].name, sizeof(s_hud_cells[i].name), "%s", name);
            s_hud_cells[i].position = position;
            s_hud_cells[i].used = true;
            s_hud_cells[i].flash_left = 0;
            s_hud_cells[i].text[0] = '\0';
            s_hud_cells[i].last_rendered[0] = '\0';
            return true;
        }
    }
    return false;
}

bool tui_hud_set_text(const char *name, const char *text)
{
    int i;
    if (name == NULL) return false;
    for (i = 0; i < P4_CONFIG_WS_HUD_MAX; i++) {
        if (s_hud_cells[i].used && strcmp(s_hud_cells[i].name, name) == 0) {
            if (text != NULL) {
                snprintf(s_hud_cells[i].text, sizeof(s_hud_cells[i].text), "%s", text);
            } else {
                s_hud_cells[i].text[0] = '\0';
            }
            return true;
        }
    }
    return false;
}

bool tui_hud_remove(const char *name)
{
    int i;
    if (name == NULL) return false;
    for (i = 0; i < P4_CONFIG_WS_HUD_MAX; i++) {
        if (s_hud_cells[i].used && strcmp(s_hud_cells[i].name, name) == 0) {
            memset(&s_hud_cells[i], 0, sizeof(s_hud_cells[i]));
            return true;
        }
    }
    return false;
}

void tui_hud_set_refresh(uint32_t ms) { s_hud_refresh_ms = ms; }
uint32_t tui_hud_get_refresh(void) { return s_hud_refresh_ms; }

void tui_hud_set_colors(uint8_t fg, uint8_t bg)
{
    s_hud_fg = fg;
    s_hud_bg = bg;
}

bool tui_hud_flash(const char *name, int duration)
{
    int i;
    if (name == NULL) return false;
    for (i = 0; i < P4_CONFIG_WS_HUD_MAX; i++) {
        if (s_hud_cells[i].used && strcmp(s_hud_cells[i].name, name) == 0) {
            s_hud_cells[i].flash_left = duration;
            return true;
        }
    }
    return false;
}

void tui_hud_refresh(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_hud_refresh_ms > 0 && (now - s_hud_last_refresh) < s_hud_refresh_ms) return;
    s_hud_last_refresh = now;
    tui_hud_render();
}

void tui_hud_render(void)
{
    int i;
    int left_col = 1;
    int right_col = P4_CONFIG_TUI_COLS;

    for (i = 0; i < P4_CONFIG_WS_HUD_MAX; i++) {
        ws_hud_cell_t *cell = &s_hud_cells[i];
        if (!cell->used) continue;
        uint8_t fg = (cell->flash_left > 0) ? 15 : s_hud_fg;
        uint8_t bg = (cell->flash_left > 0) ? 4 : s_hud_bg;
        if (cell->position == 0) {
            if (strcmp(cell->last_rendered, cell->text) != 0) {
                tui_print_at(left_col, 1, cell->text, fg, bg);
                size_t len = strlen(cell->text);
                if (len > sizeof(cell->last_rendered) - 1) {
                    len = sizeof(cell->last_rendered) - 1;
                }
                memmove(cell->last_rendered, cell->text, len);
                cell->last_rendered[len] = '\0';
            }
            left_col += (int)strlen(cell->text) + 1;
        } else {
            int len = (int)strlen(cell->text);
            right_col -= len;
            if (right_col < left_col) right_col = left_col;
            if (strcmp(cell->last_rendered, cell->text) != 0) {
                tui_print_at(right_col, 1, cell->text, fg, bg);
                size_t cp_len = strlen(cell->text);
                if (cp_len > sizeof(cell->last_rendered) - 1) {
                    cp_len = sizeof(cell->last_rendered) - 1;
                }
                memmove(cell->last_rendered, cell->text, cp_len);
                cell->last_rendered[cp_len] = '\0';
            }
        }
        if (cell->flash_left > 0) cell->flash_left--;
    }
}

/* ========================================================================
 * EVENT MULTIPLEXER
 * ======================================================================== */

typedef struct {
    tui_event_type_t type;
    char payload[P4_CONFIG_WS_EVENT_PAYLOAD_BYTES];
    uint32_t timestamp;
} ws_event_t;

static ws_event_t s_events[P4_CONFIG_WS_EVENT_MAX];
static int s_event_head = 0;
static int s_event_count = 0;

static SemaphoreHandle_t s_event_sem = NULL;

static void event_ensure_sem(void)
{
    if (s_event_sem == NULL) {
        s_event_sem = xSemaphoreCreateBinary();
    }
}

bool tui_event_post(tui_event_type_t type, const char *payload)
{
    if (type < 0 || type >= TUI_EVENT_TYPE_COUNT) return false;
    event_ensure_sem();

    ws_event_t *ev = &s_events[s_event_head];
    ev->type = type;
    ev->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
    if (payload != NULL) {
        snprintf(ev->payload, sizeof(ev->payload), "%s", payload);
    } else {
        ev->payload[0] = '\0';
    }
    s_event_head = (s_event_head + 1) % P4_CONFIG_WS_EVENT_MAX;
    if (s_event_count < P4_CONFIG_WS_EVENT_MAX) s_event_count++;
    if (s_event_sem != NULL) xSemaphoreGive(s_event_sem);
    return true;
}

int tui_event_count(void) { return s_event_count; }

bool tui_event_get(int index, tui_event_type_t *type, char *payload,
                   size_t payload_size)
{
    if (index < 0 || index >= s_event_count) return false;
    int idx = (s_event_head - s_event_count + index + P4_CONFIG_WS_EVENT_MAX)
              % P4_CONFIG_WS_EVENT_MAX;
    ws_event_t *ev = &s_events[idx];
    if (type != NULL) *type = ev->type;
    if (payload != NULL && payload_size > 0) {
        snprintf(payload, payload_size, "%s", ev->payload);
    }
    return true;
}

int tui_event_wait(int timeout_ms)
{
    if (s_event_count > 0) return 0;
    if (timeout_ms <= 0) return -1;
    event_ensure_sem();
    if (s_event_sem != NULL) {
        if (xSemaphoreTake(s_event_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            if (s_event_count > 0) return 0;
        }
    }
    return -1;
}

void tui_event_clear(void)
{
    s_event_head = 0;
    s_event_count = 0;
    if (s_event_sem != NULL) {
        while (xSemaphoreTake(s_event_sem, 0) == pdTRUE) {}
    }
}

/* ========================================================================
 * KEY BINDINGS
 * ======================================================================== */

typedef struct {
    char key[P4_CONFIG_WS_KEY_NAME_BYTES];
    char label[P4_CONFIG_WS_PANE_TITLE_BYTES];
    bool used;
} ws_keybind_t;

static ws_keybind_t s_keybinds[P4_CONFIG_WS_KEY_BIND_MAX];
static int s_key_mode = 0;

bool tui_key_bind(const char *key, const char *label)
{
    int i;
    if (key == NULL || label == NULL) return false;
    for (i = 0; i < P4_CONFIG_WS_KEY_BIND_MAX; i++) {
        if (s_keybinds[i].used && strcmp(s_keybinds[i].key, key) == 0) {
            snprintf(s_keybinds[i].label, sizeof(s_keybinds[i].label), "%s", label);
            return true;
        }
    }
    for (i = 0; i < P4_CONFIG_WS_KEY_BIND_MAX; i++) {
        if (!s_keybinds[i].used) {
            snprintf(s_keybinds[i].key, sizeof(s_keybinds[i].key), "%s", key);
            snprintf(s_keybinds[i].label, sizeof(s_keybinds[i].label), "%s", label);
            s_keybinds[i].used = true;
            return true;
        }
    }
    return false;
}

bool tui_key_unbind(const char *key)
{
    int i;
    if (key == NULL) return false;
    for (i = 0; i < P4_CONFIG_WS_KEY_BIND_MAX; i++) {
        if (s_keybinds[i].used && strcmp(s_keybinds[i].key, key) == 0) {
            memset(&s_keybinds[i], 0, sizeof(s_keybinds[i]));
            return true;
        }
    }
    return false;
}

bool tui_key_inject(const char *text)
{
    if (text == NULL) return false;
    shell_input_line_set_text(text);
    return true;
}

int tui_key_count(void)
{
    int i, n = 0;
    for (i = 0; i < P4_CONFIG_WS_KEY_BIND_MAX; i++) {
        if (s_keybinds[i].used) n++;
    }
    return n;
}

bool tui_key_get_name(int index, char *buf, size_t buf_size)
{
    int i, n = 0;
    if (buf == NULL || buf_size == 0) return false;
    for (i = 0; i < P4_CONFIG_WS_KEY_BIND_MAX; i++) {
        if (s_keybinds[i].used) {
            if (n == index) {
                snprintf(buf, buf_size, "%s", s_keybinds[i].key);
                return true;
            }
            n++;
        }
    }
    return false;
}

const char *tui_key_lookup(const char *key)
{
    int i;
    if (key == NULL) return NULL;
    for (i = 0; i < P4_CONFIG_WS_KEY_BIND_MAX; i++) {
        if (s_keybinds[i].used && strcmp(s_keybinds[i].key, key) == 0) {
            return s_keybinds[i].label;
        }
    }
    return NULL;
}

void tui_key_set_mode(int mode) { s_key_mode = mode; }
int tui_key_get_mode(void) { return s_key_mode; }

/* ========================================================================
 * TOUCH HOTSPOTS
 * ======================================================================== */

typedef struct {
    int x, y, w, h;
    char label[P4_CONFIG_WS_PANE_TITLE_BYTES];
    bool used;
} ws_tap_t;

static ws_tap_t s_taps[P4_CONFIG_WS_TAP_MAX];

bool tui_tap_add(int x, int y, int w, int h, const char *label)
{
    int i;
    if (label == NULL) return false;
    for (i = 0; i < P4_CONFIG_WS_TAP_MAX; i++) {
        if (!s_taps[i].used) {
            s_taps[i].x = x;
            s_taps[i].y = y;
            s_taps[i].w = w;
            s_taps[i].h = h;
            snprintf(s_taps[i].label, sizeof(s_taps[i].label), "%s", label);
            s_taps[i].used = true;
            return true;
        }
    }
    return false;
}

int tui_tap_remove(int x, int y)
{
    int i, removed = 0;
    for (i = 0; i < P4_CONFIG_WS_TAP_MAX; i++) {
        if (s_taps[i].used && s_taps[i].x == x && s_taps[i].y == y) {
            memset(&s_taps[i], 0, sizeof(s_taps[i]));
            removed++;
        }
    }
    return removed;
}

void tui_tap_clear(void)
{
    memset(s_taps, 0, sizeof(s_taps));
}

const char *tui_tap_hit(int x, int y)
{
    int i;
    for (i = P4_CONFIG_WS_TAP_MAX - 1; i >= 0; i--) {
        if (!s_taps[i].used) continue;
        if (x >= s_taps[i].x && x < s_taps[i].x + s_taps[i].w &&
            y >= s_taps[i].y && y < s_taps[i].y + s_taps[i].h) {
            return s_taps[i].label;
        }
    }
    return NULL;
}

int tui_tap_count(void)
{
    int i, n = 0;
    for (i = 0; i < P4_CONFIG_WS_TAP_MAX; i++) {
        if (s_taps[i].used) n++;
    }
    return n;
}

/* ========================================================================
 * WORKSPACE RENDER
 * ======================================================================== */

void tui_workspace_render(void)
{
    if (!tui_is_active()) return;
    tui_hud_render();
    tui_pane_render();
    tui_widget_render();
}

void tui_workspace_reset(void)
{
    int i;
    for (i = 0; i < P4_CONFIG_WS_PANE_MAX; i++) {
        pane_free_scrollback(&s_panes[i]);
    }
    memset(s_panes, 0, sizeof(s_panes));
    s_pane_count = 0;
    s_focus_index = -1;
    memset(s_widgets, 0, sizeof(s_widgets));
    memset(s_hud_cells, 0, sizeof(s_hud_cells));
    s_event_head = 0;
    s_event_count = 0;
    memset(s_keybinds, 0, sizeof(s_keybinds));
    s_key_mode = 0;
    memset(s_taps, 0, sizeof(s_taps));
    s_hud_refresh_ms = P4_CONFIG_WS_HUD_REFRESH_MS;
    s_hud_last_refresh = 0;
}

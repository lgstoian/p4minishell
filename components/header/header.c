/**
 * @file header.c
 * @brief Fixed top status bar implementation for P4MiniShell.
 *
 * Owns the LVGL widget tree for the fixed header bar with a unified icon list.
 * All public functions are safe to call from any task context.
 *
 * Architecture:
 *   - s_status_icons[] array maps icon enum to LVGL label widget
 *   - header_render() iterates the array, rendering each icon from cached state
 *   - Each icon always visible, showing state text (ON/OFF/NO/ERR/etc.)
 *   - Config-driven spacing via P4_CONFIG_HEADER_* macros
 *   - Notification area on far right, transient only
 *
 * Visual design:
 *   - Non-scrollable, resolution-scaled height (display_height / 15, clamped 32-56)
 *   - Dark background (#111816) with green accent (#8DFF96) for active state
 *   - Status icons laid out left-to-right in a flex row
 *   - Battery: LVGL symbol icon + small bar + percentage label
 *   - Wi-Fi: symbol + HI/MID/LOW/OFF signal quality label
 *   - Bluetooth: symbol + ON/PAIR/OFF label
 *   - USB: symbol + ON/OFF label
 *   - SD: ASCII "SD:" prefix + NO/INS/ON/ERR (4 states, always visible)
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "header.h"
#include "p4minishell_config.h"

/* ---- Backward-compatibility aliases ---- */
#define HEADER_NOTIFICATION_BYTES   P4_CONFIG_HEADER_NOTIFICATION_BYTES
#define HEADER_TEXT_COLOR           P4_CONFIG_HEADER_TEXT_COLOR
#define HEADER_MUTED_COLOR          P4_CONFIG_HEADER_MUTED_COLOR
#define HEADER_ACCENT_COLOR         P4_CONFIG_HEADER_ACCENT_COLOR
#define HEADER_WARN_COLOR           P4_CONFIG_HEADER_WARN_COLOR
#define HEADER_BG_COLOR             P4_CONFIG_HEADER_BG_COLOR
#define HEADER_PANEL_COLOR          P4_CONFIG_HEADER_PANEL_COLOR

/* ---- Unified icon type enum ---- */
typedef enum {
    HEADER_ICON_WIFI = 0,
    HEADER_ICON_BLUETOOTH,
    HEADER_ICON_USB,
    HEADER_ICON_SD,
    HEADER_ICON_COUNT,   /* number of icons in the status row */
} header_icon_type_t;

/* ---- Cached state structure ---- */
typedef struct {
    bool wifi_connected;
    int wifi_rssi;
    int battery_percent;
    bool bluetooth_enabled;
    bool bluetooth_connected;
    bool usb_connected;
    header_sd_state_t sd_state;
    char notification[HEADER_NOTIFICATION_BYTES];
} header_state_t;

/* ---- Async update payload types ---- */
typedef struct {
    char text[HEADER_NOTIFICATION_BYTES];
    uint32_t timeout_ms;
} header_notification_update_t;

typedef struct {
    bool connected;
    int rssi;
} header_wifi_update_t;

typedef struct {
    bool enabled;
    bool connected;
} header_bluetooth_update_t;

typedef struct {
    bool connected;
} header_toggle_update_t;

typedef struct {
    int percent;
} header_battery_update_t;

typedef struct {
    header_sd_state_t state;
} header_sd_update_t;

/* ---- Static widget handles ---- */
static lv_obj_t *s_header_root;
static lv_obj_t *s_notification_label;
static lv_obj_t *s_status_icons[HEADER_ICON_COUNT];
static lv_obj_t *s_battery_icon_label;
static lv_obj_t *s_battery_bar;
static lv_obj_t *s_battery_value_label;
static lv_timer_t *s_notification_timer;
static lv_coord_t s_header_height = HEADER_HEIGHT;
static header_state_t s_header_state = {
    .battery_percent = 100,
    .wifi_rssi = -127,
    .sd_state = HEADER_SD_NONE,
};

/* ---- Font selection ---- */
static const lv_font_t *header_select_font(void)
{
#if LV_FONT_UNSCII_16
    return &lv_font_unscii_16;
#else
    return LV_FONT_DEFAULT;
#endif
}

/* ---- Dynamic height scaling ---- */
static lv_coord_t header_scale_height(void)
{
    lv_display_t *display = lv_display_get_default();
    int32_t vertical_res = 600;
    lv_coord_t scaled;

    if (display != NULL) {
        vertical_res = lv_display_get_vertical_resolution(display);
    }

    scaled = (lv_coord_t)(vertical_res / 15);
    if (scaled < 32) {
        scaled = 32;
    }
    if (scaled > 56) {
        scaled = 56;
    }

    return scaled;
}

/* ---- Battery symbol helper ---- */
static const char *header_battery_symbol_for_percent(int percent)
{
    if (percent >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (percent >= 65) return LV_SYMBOL_BATTERY_3;
    if (percent >= 40) return LV_SYMBOL_BATTERY_2;
    if (percent >= 15) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* ---- Status color helper ---- */
static lv_color_t header_status_color(bool active)
{
    return lv_color_hex(active ? HEADER_ACCENT_COLOR : HEADER_MUTED_COLOR);
}

/* ---- Notification timeout callback ---- */
static void header_notification_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    s_header_state.notification[0] = '\0';
    if (s_notification_timer != NULL) {
        lv_timer_pause(s_notification_timer);
    }
    if (s_notification_label != NULL) {
        lv_label_set_text(s_notification_label, "");
    }
}

/* ---- Render a single icon by type ---- */
static void header_render_icon(header_icon_type_t type)
{
    char buffer[64];
    lv_obj_t *label;
    lv_color_t color;

    if (type < 0 || type >= HEADER_ICON_COUNT) {
        return;
    }

    label = s_status_icons[type];
    if (label == NULL) {
        return;
    }

    switch (type) {
    case HEADER_ICON_WIFI:
        if (s_header_state.wifi_connected) {
            const char *quality;
            if (s_header_state.wifi_rssi >= -60) quality = "HI";
            else if (s_header_state.wifi_rssi >= -72) quality = "MID";
            else quality = "LOW";
            snprintf(buffer, sizeof(buffer), "%s %s", LV_SYMBOL_WIFI, quality);
        } else {
            snprintf(buffer, sizeof(buffer), "%s OFF", LV_SYMBOL_WIFI);
        }
        color = header_status_color(s_header_state.wifi_connected);
        break;

    case HEADER_ICON_BLUETOOTH:
        if (s_header_state.bluetooth_connected) {
            snprintf(buffer, sizeof(buffer), "%s PAIR", LV_SYMBOL_BLUETOOTH);
        } else if (s_header_state.bluetooth_enabled) {
            snprintf(buffer, sizeof(buffer), "%s ON", LV_SYMBOL_BLUETOOTH);
        } else {
            snprintf(buffer, sizeof(buffer), "%s OFF", LV_SYMBOL_BLUETOOTH);
        }
        color = header_status_color(s_header_state.bluetooth_enabled);
        break;

    case HEADER_ICON_USB:
        snprintf(buffer, sizeof(buffer), "%s %s", LV_SYMBOL_USB,
                 s_header_state.usb_connected ? "ON" : "OFF");
        color = header_status_color(s_header_state.usb_connected);
        break;

    case HEADER_ICON_SD:
        switch (s_header_state.sd_state) {
        case HEADER_SD_NONE:
            snprintf(buffer, sizeof(buffer), "SD:NO");
            color = header_status_color(false);
            break;
        case HEADER_SD_INSERTED:
            snprintf(buffer, sizeof(buffer), "SD:INS");
            color = lv_color_hex(HEADER_WARN_COLOR);
            break;
        case HEADER_SD_MOUNTED:
            snprintf(buffer, sizeof(buffer), "SD:ON");
            color = header_status_color(true);
            break;
        case HEADER_SD_ERROR:
            snprintf(buffer, sizeof(buffer), "SD:ERR");
            color = lv_color_hex(HEADER_WARN_COLOR);
            break;
        default:
            snprintf(buffer, sizeof(buffer), "SD:?");
            color = header_status_color(false);
            break;
        }
        break;

    default:
        return;
    }

    lv_label_set_text(label, buffer);
    lv_obj_set_style_text_color(label, color, 0);
}

/* ---- Main render: updates all icons and battery ---- */
static void header_render(void)
{
    lv_color_t battery_color;
    int i;

    if (s_header_root == NULL) {
        return;
    }

    /* Render notification area */
    if (s_header_state.notification[0] != '\0') {
        lv_label_set_text(s_notification_label, s_header_state.notification);
    } else {
        lv_label_set_text(s_notification_label, "");
    }

    /* Render all status icons via unified loop */
    for (i = 0; i < HEADER_ICON_COUNT; i++) {
        header_render_icon((header_icon_type_t)i);
    }

    /* Render battery (special: icon + bar + label) */
    lv_label_set_text(s_battery_icon_label,
                      header_battery_symbol_for_percent(s_header_state.battery_percent));
    battery_color = s_header_state.battery_percent <= 15
                        ? lv_color_hex(HEADER_WARN_COLOR)
                        : lv_color_hex(HEADER_ACCENT_COLOR);
    lv_obj_set_style_text_color(s_battery_icon_label, battery_color, 0);
    lv_bar_set_value(s_battery_bar, s_header_state.battery_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_battery_bar, battery_color, LV_PART_INDICATOR);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", s_header_state.battery_percent);
    lv_label_set_text(s_battery_value_label, buf);
    lv_obj_set_style_text_color(s_battery_value_label, battery_color, 0);

    /* Force layout update so flex container recalculates after text changes */
    lv_obj_update_layout(s_header_root);
}

/* ---- Async dispatch helper ---- */
static bool header_schedule(lv_async_cb_t cb, void *payload)
{
    if (lv_async_call(cb, payload) != LV_RESULT_OK) {
        free(payload);
        return false;
    }
    return true;
}

/* ---- Async callbacks (state already set by caller, just re-render) ---- */
static void header_async_refresh(void *user_data)     { (void)user_data; header_render(); }
static void header_async_notification(void *user_data) {
    header_notification_update_t *u = (header_notification_update_t *)user_data;
    if (u == NULL) return;
    snprintf(s_header_state.notification, sizeof(s_header_state.notification), "%s", u->text);
    if (s_notification_timer == NULL) {
        s_notification_timer = lv_timer_create(header_notification_timeout_cb, u->timeout_ms, NULL);
        if (s_notification_timer != NULL) lv_timer_pause(s_notification_timer);
    }
    if (s_notification_timer != NULL) {
        if (u->timeout_ms > 0) {
            lv_timer_set_period(s_notification_timer, u->timeout_ms);
            lv_timer_set_repeat_count(s_notification_timer, 1);
            lv_timer_reset(s_notification_timer);
            lv_timer_resume(s_notification_timer);
        } else {
            lv_timer_pause(s_notification_timer);
        }
    }
    header_render();
    free(u);
}
static void header_async_wifi(void *u_data)  { free(u_data); header_render(); }
static void header_async_battery(void *u_data) { free(u_data); header_render(); }
static void header_async_bluetooth(void *u_data) { free(u_data); header_render(); }
static void header_async_usb(void *u_data)   { free(u_data); header_render(); }
static void header_async_sd(void *u_data)    { free(u_data); header_render(); }

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

void header_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *status_row;
    lv_obj_t *battery_wrap;
    const lv_font_t *terminal_font = header_select_font();
    const lv_font_t *status_font = LV_FONT_DEFAULT;
    lv_coord_t vertical_pad;
    lv_coord_t horizontal_pad;
    int i;

    if (screen == NULL || s_header_root != NULL) {
        return;
    }

    s_header_height = header_scale_height();
    vertical_pad = s_header_height >= 44 ? 6 : 4;
    horizontal_pad = s_header_height >= 44 ? 12 : 8;

    /* Create header root: flex row, space-between */
    s_header_root = lv_obj_create(screen);
    lv_obj_set_width(s_header_root, LV_PCT(100));
    lv_obj_set_height(s_header_root, s_header_height);
    lv_obj_set_layout(s_header_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_header_root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_header_root, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(s_header_root, horizontal_pad, 0);
    lv_obj_set_style_pad_right(s_header_root, horizontal_pad, 0);
    lv_obj_set_style_pad_top(s_header_root, vertical_pad, 0);
    lv_obj_set_style_pad_bottom(s_header_root, vertical_pad, 0);
    lv_obj_set_style_pad_column(s_header_root, 8, 0);
    lv_obj_set_style_radius(s_header_root, 0, 0);
    lv_obj_set_style_bg_color(s_header_root, lv_color_hex(HEADER_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_header_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_header_root, 0, 0);
    lv_obj_set_scrollbar_mode(s_header_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_header_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Create status row: flex row, left-aligned, contains all 4 icons + battery */
    status_row = lv_obj_create(s_header_root);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_all(status_row, 0, 0);
    lv_obj_set_style_pad_column(status_row, 8, 0);
    lv_obj_set_layout(status_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(status_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Create unified icon labels: WiFi, BT, USB, SD */
    for (i = 0; i < HEADER_ICON_COUNT; i++) {
        s_status_icons[i] = lv_label_create(status_row);
        lv_obj_set_style_text_font(s_status_icons[i], status_font, 0);
        lv_obj_set_style_text_letter_space(s_status_icons[i], 0, 0);
        lv_obj_clear_flag(s_status_icons[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Create battery widget cluster (inside status_row) */
    battery_wrap = lv_obj_create(status_row);
    lv_obj_set_style_bg_color(battery_wrap, lv_color_hex(HEADER_PANEL_COLOR), 0);
    lv_obj_set_style_bg_opa(battery_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_wrap, 0, 0);
    lv_obj_set_style_radius(battery_wrap, 2, 0);
    lv_obj_set_style_pad_left(battery_wrap, 6, 0);
    lv_obj_set_style_pad_right(battery_wrap, 6, 0);
    lv_obj_set_style_pad_top(battery_wrap, 3, 0);
    lv_obj_set_style_pad_bottom(battery_wrap, 3, 0);
    lv_obj_set_style_pad_column(battery_wrap, 4, 0);
    lv_obj_set_layout(battery_wrap, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(battery_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_wrap, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(battery_wrap, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(battery_wrap, LV_OBJ_FLAG_SCROLLABLE);

    s_battery_icon_label = lv_label_create(battery_wrap);
    lv_obj_set_style_text_font(s_battery_icon_label, status_font, 0);

    s_battery_bar = lv_bar_create(battery_wrap);
    lv_obj_set_size(s_battery_bar,
                    s_header_height >= 44 ? 32 : 24,
                    s_header_height >= 44 ? 8 : 6);
    lv_bar_set_range(s_battery_bar, 0, 100);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(0x253229), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_battery_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(HEADER_ACCENT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_battery_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_battery_bar, 0, LV_PART_MAIN | LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_battery_bar, 1, LV_PART_MAIN);

    s_battery_value_label = lv_label_create(battery_wrap);
    lv_obj_set_style_text_font(s_battery_value_label, terminal_font, 0);

    /* Create notification label (right side of header root) */
    s_notification_label = lv_label_create(s_header_root);
    lv_obj_set_flex_grow(s_notification_label, 1);
    lv_obj_set_width(s_notification_label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_notification_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_notification_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_notification_label, lv_color_hex(HEADER_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(s_notification_label, terminal_font, 0);
    lv_obj_set_scrollbar_mode(s_notification_label, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_notification_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(s_notification_label, "");

    /* Initial render */
    header_render();
}

void header_update_status(void)
{
    (void)lv_async_call(header_async_refresh, NULL);
}

void header_set_notification(const char *text, uint32_t timeout_ms)
{
    header_notification_update_t *update = calloc(1, sizeof(*update));
    if (update == NULL) return;
    snprintf(update->text, sizeof(update->text), "%s", text != NULL ? text : "");
    update->timeout_ms = timeout_ms;
    (void)header_schedule(header_async_notification, update);
}

void header_update_wifi(bool connected, int rssi)
{
    s_header_state.wifi_connected = connected;
    s_header_state.wifi_rssi = rssi;
    header_wifi_update_t *u = calloc(1, sizeof(*u));
    if (u != NULL) {
        u->connected = connected; u->rssi = rssi;
        if (!header_schedule(header_async_wifi, u)) { free(u); header_render(); }
    } else { header_render(); }
}

void header_update_battery(int percent)
{
    int clamped = (percent < 0) ? 0 : (percent > 100) ? 100 : percent;
    s_header_state.battery_percent = clamped;
    header_battery_update_t *u = calloc(1, sizeof(*u));
    if (u != NULL) {
        u->percent = clamped;
        if (!header_schedule(header_async_battery, u)) { free(u); header_render(); }
    } else { header_render(); }
}

void header_update_bluetooth(bool enabled, bool connected)
{
    s_header_state.bluetooth_enabled = enabled;
    s_header_state.bluetooth_connected = connected;
    header_bluetooth_update_t *u = calloc(1, sizeof(*u));
    if (u != NULL) {
        u->enabled = enabled; u->connected = connected;
        if (!header_schedule(header_async_bluetooth, u)) { free(u); header_render(); }
    } else { header_render(); }
}

void header_update_usb(bool connected)
{
    s_header_state.usb_connected = connected;
    header_toggle_update_t *u = calloc(1, sizeof(*u));
    if (u != NULL) {
        u->connected = connected;
        if (!header_schedule(header_async_usb, u)) { free(u); header_render(); }
    } else { header_render(); }
}

void header_update_sd(header_sd_state_t state)
{
    s_header_state.sd_state = state;
    header_sd_update_t *u = calloc(1, sizeof(*u));
    if (u != NULL) {
        u->state = state;
        if (!header_schedule(header_async_sd, u)) { free(u); header_render(); }
    } else { header_render(); }
}

void header_force_render(void)
{
    header_render();
}
/**
 * @file header.c
 * @brief Fixed top status bar implementation for P4MiniShell.
 *
 * Owns the LVGL widget tree for the fixed header bar: status icons for
 * Wi-Fi, battery, Bluetooth, USB, and SD, plus a transient notification area.
 * All public functions use LVGL async dispatch so they are safe to call from
 * any task context (shell worker, module callback, timer, etc.).
 *
 * Visual design:
 *   - Non-scrollable, resolution-scaled height (display_height / 15, clamped 32-56)
 *   - Dark background (#111816) with green accent (#8DFF96) for active state
 *   - Status icons laid out left-to-right, notification area on the far right
 *   - Battery: LVGL symbol icon + small bar + percentage label
 *   - Wi-Fi: symbol + HI/MID/LOW/OFF signal quality label
 *   - Bluetooth: symbol + ON/PAIR/OFF label
 *   - USB: symbol + ON/OFF label
 *   - SD: symbol + "SD" label, hidden when no card mounted
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "header.h"
#include "p4minishell_config.h"

/* Use a consistent SD icon string even if the active font does not include the LVGL symbol glyph.
 * If the LVGL symbol is available, prefer it; otherwise fall back to an ASCII "SD" label.
 */
#if defined(LV_SYMBOL_SD_CARD)
#define HEADER_SD_SYMBOL LV_SYMBOL_SD_CARD
#elif defined(LV_SYMBOL_SAVE)
#define HEADER_SD_SYMBOL LV_SYMBOL_SAVE
#elif defined(LV_SYMBOL_DIRECTORY)
#define HEADER_SD_SYMBOL LV_SYMBOL_DIRECTORY
#else
#define HEADER_SD_SYMBOL "SD"
#endif

/* ---- Backward-compatibility aliases ---- */
#define HEADER_NOTIFICATION_BYTES   P4_CONFIG_HEADER_NOTIFICATION_BYTES
#define HEADER_TEXT_COLOR           P4_CONFIG_HEADER_TEXT_COLOR
#define HEADER_MUTED_COLOR          P4_CONFIG_HEADER_MUTED_COLOR
#define HEADER_ACCENT_COLOR         P4_CONFIG_HEADER_ACCENT_COLOR
#define HEADER_WARN_COLOR           P4_CONFIG_HEADER_WARN_COLOR
#define HEADER_BG_COLOR             P4_CONFIG_HEADER_BG_COLOR
#define HEADER_PANEL_COLOR          P4_CONFIG_HEADER_PANEL_COLOR

typedef struct {
    bool wifi_connected;
    int wifi_rssi;
    int battery_percent;
    bool bluetooth_enabled;
    bool bluetooth_connected;
    bool usb_connected;
    bool sd_mounted;
    char notification[HEADER_NOTIFICATION_BYTES];
} header_state_t;

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

static lv_obj_t *s_header_root;
static lv_obj_t *s_notification_label;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_bluetooth_label;
static lv_obj_t *s_usb_label;
static lv_obj_t *s_sd_label;
static lv_obj_t *s_battery_icon_label;
static lv_obj_t *s_battery_bar;
static lv_obj_t *s_battery_value_label;
static lv_timer_t *s_notification_timer;
static lv_coord_t s_header_height = HEADER_HEIGHT;
static header_state_t s_header_state = {
    .battery_percent = 100,
    .wifi_rssi = -127,
};

static const lv_font_t *header_select_font(void)
{
#if LV_FONT_UNSCII_16
    return &lv_font_unscii_16;
#else
    return LV_FONT_DEFAULT;
#endif
}

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

static const char *header_battery_symbol_for_percent(int percent)
{
    if (percent >= 90) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (percent >= 65) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (percent >= 40) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (percent >= 15) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

static lv_color_t header_status_color(bool active)
{
    return lv_color_hex(active ? HEADER_ACCENT_COLOR : HEADER_MUTED_COLOR);
}

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

static void header_render(void)
{
    char buffer[64];
    const char *wifi_quality;
    lv_color_t battery_color;

    if (s_header_root == NULL) {
        return;
    }

    if (s_header_state.notification[0] != '\0') {
        lv_label_set_text(s_notification_label, s_header_state.notification);
    } else {
        lv_label_set_text(s_notification_label, "");
    }

    if (s_header_state.wifi_connected) {
        if (s_header_state.wifi_rssi >= -60) {
            wifi_quality = "HI";
        } else if (s_header_state.wifi_rssi >= -72) {
            wifi_quality = "MID";
        } else {
            wifi_quality = "LOW";
        }
        snprintf(buffer, sizeof(buffer), "%s %s", LV_SYMBOL_WIFI, wifi_quality);
    } else {
        snprintf(buffer, sizeof(buffer), "%s OFF", LV_SYMBOL_WIFI);
    }
    lv_label_set_text(s_wifi_label, buffer);
    lv_obj_set_style_text_color(s_wifi_label, header_status_color(s_header_state.wifi_connected), 0);

    if (s_header_state.bluetooth_connected) {
        snprintf(buffer, sizeof(buffer), "%s PAIR", LV_SYMBOL_BLUETOOTH);
    } else if (s_header_state.bluetooth_enabled) {
        snprintf(buffer, sizeof(buffer), "%s ON", LV_SYMBOL_BLUETOOTH);
    } else {
        snprintf(buffer, sizeof(buffer), "%s OFF", LV_SYMBOL_BLUETOOTH);
    }
    lv_label_set_text(s_bluetooth_label, buffer);
    lv_obj_set_style_text_color(s_bluetooth_label,
                                header_status_color(s_header_state.bluetooth_enabled),
                                0);

    snprintf(buffer, sizeof(buffer), "%s %s", LV_SYMBOL_USB, s_header_state.usb_connected ? "ON" : "OFF");
    lv_label_set_text(s_usb_label, buffer);
    lv_obj_set_style_text_color(s_usb_label, header_status_color(s_header_state.usb_connected), 0);

    if (s_header_state.sd_mounted) {
        lv_obj_clear_flag(s_sd_label, LV_OBJ_FLAG_HIDDEN);
        snprintf(buffer, sizeof(buffer), "%s SD", HEADER_SD_SYMBOL);
        lv_obj_set_style_text_color(s_sd_label, header_status_color(true), 0);
    } else {
        /* Hide the SD indicator when no card is mounted. */
        lv_obj_add_flag(s_sd_label, LV_OBJ_FLAG_HIDDEN);
        snprintf(buffer, sizeof(buffer), "%s SD", HEADER_SD_SYMBOL);
        lv_obj_set_style_text_color(s_sd_label, header_status_color(false), 0);
    }
    lv_label_set_text(s_sd_label, buffer);

    lv_label_set_text(s_battery_icon_label, header_battery_symbol_for_percent(s_header_state.battery_percent));
    battery_color = s_header_state.battery_percent <= 15 ? lv_color_hex(HEADER_WARN_COLOR) : lv_color_hex(HEADER_ACCENT_COLOR);
    lv_obj_set_style_text_color(s_battery_icon_label, battery_color, 0);
    lv_bar_set_value(s_battery_bar, s_header_state.battery_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_battery_bar, battery_color, LV_PART_INDICATOR);
    snprintf(buffer, sizeof(buffer), "%d%%", s_header_state.battery_percent);
    lv_label_set_text(s_battery_value_label, buffer);
    lv_obj_set_style_text_color(s_battery_value_label, battery_color, 0);
}

static bool header_schedule(lv_async_cb_t cb, void *payload)
{
    if (lv_async_call(cb, payload) != LV_RESULT_OK) {
        free(payload);
        return false;
    }
    return true;
}

static void header_async_refresh(void *user_data)
{
    (void)user_data;
    header_render();
}

static void header_async_notification(void *user_data)
{
    header_notification_update_t *update = (header_notification_update_t *)user_data;

    if (update == NULL) {
        return;
    }

    snprintf(s_header_state.notification, sizeof(s_header_state.notification), "%s", update->text);
    if (s_notification_timer == NULL) {
        s_notification_timer = lv_timer_create(header_notification_timeout_cb, update->timeout_ms, NULL);
        if (s_notification_timer != NULL) {
            lv_timer_pause(s_notification_timer);
        }
    }

    if (s_notification_timer != NULL) {
        if (update->timeout_ms > 0) {
            lv_timer_set_period(s_notification_timer, update->timeout_ms);
            lv_timer_set_repeat_count(s_notification_timer, 1);
            lv_timer_reset(s_notification_timer);
            lv_timer_resume(s_notification_timer);
        } else {
            lv_timer_pause(s_notification_timer);
        }
    }

    header_render();
    free(update);
}

static void header_async_wifi(void *user_data)
{
    header_wifi_update_t *update = (header_wifi_update_t *)user_data;
    if (update == NULL) return;
    /* State already set immediately in header_update_wifi(); just re-render */
    header_render();
    free(update);
}

static void header_async_battery(void *user_data)
{
    header_battery_update_t *update = (header_battery_update_t *)user_data;
    if (update == NULL) return;
    /* State already set immediately in header_update_battery(); just re-render */
    header_render();
    free(update);
}

static void header_async_bluetooth(void *user_data)
{
    header_bluetooth_update_t *update = (header_bluetooth_update_t *)user_data;
    if (update == NULL) return;
    /* State already set immediately in header_update_bluetooth(); just re-render */
    header_render();
    free(update);
}

static void header_async_usb(void *user_data)
{
    header_toggle_update_t *update = (header_toggle_update_t *)user_data;
    if (update == NULL) return;
    /* State already set immediately in header_update_usb(); just re-render */
    header_render();
    free(update);
}

static void header_async_sd(void *user_data)
{
    header_toggle_update_t *update = (header_toggle_update_t *)user_data;

    if (update == NULL) {
        return;
    }

    /* State already set immediately in header_update_sd(); just re-render */
    header_render();
    free(update);
}

void header_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *status_row;
    lv_obj_t *battery_wrap;
    const lv_font_t *terminal_font = header_select_font();
    const lv_font_t *status_font = LV_FONT_DEFAULT;
    lv_coord_t vertical_pad;
    lv_coord_t horizontal_pad;

    if (screen == NULL || s_header_root != NULL) {
        return;
    }

    s_header_height = header_scale_height();
    vertical_pad = s_header_height >= 44 ? 6 : 4;
    horizontal_pad = s_header_height >= 44 ? 12 : 8;

    // Build the fixed header bar: non-scrollable, resolution-scaled, flex-row layout.
    // Owns all LVGL widget creation internally. SD icon uses consistent symbol/text
    // and is shown only when mounted, matching the style of other status indicators.
    s_header_root = lv_obj_create(screen);
    lv_obj_set_width(s_header_root, LV_PCT(100));
    lv_obj_set_height(s_header_root, s_header_height);
    lv_obj_set_layout(s_header_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_header_root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_header_root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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

    status_row = lv_obj_create(s_header_root);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_all(status_row, 0, 0);
    lv_obj_set_style_pad_column(status_row, 8, 0);
    lv_obj_set_layout(status_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(status_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_label = lv_label_create(status_row);
    s_bluetooth_label = lv_label_create(status_row);
    s_usb_label = lv_label_create(status_row);
    s_sd_label = lv_label_create(status_row);

    lv_obj_set_style_text_font(s_wifi_label, status_font, 0);
    lv_obj_set_style_text_font(s_bluetooth_label, status_font, 0);
    lv_obj_set_style_text_font(s_usb_label, status_font, 0);
    lv_obj_set_style_text_font(s_sd_label, status_font, 0);
    lv_obj_set_style_text_letter_space(s_wifi_label, 0, 0);
    lv_obj_set_style_text_letter_space(s_bluetooth_label, 0, 0);
    lv_obj_set_style_text_letter_space(s_usb_label, 0, 0);
    lv_obj_set_style_text_letter_space(s_sd_label, 0, 0);
    lv_obj_clear_flag(s_wifi_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_bluetooth_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_usb_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sd_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sd_label, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_set_flex_align(battery_wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(battery_wrap, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(battery_wrap, LV_OBJ_FLAG_SCROLLABLE);

    s_battery_icon_label = lv_label_create(battery_wrap);
    lv_obj_set_style_text_font(s_battery_icon_label, status_font, 0);

    s_battery_bar = lv_bar_create(battery_wrap);
    lv_obj_set_size(s_battery_bar, s_header_height >= 44 ? 32 : 24, s_header_height >= 44 ? 8 : 6);
    lv_bar_set_range(s_battery_bar, 0, 100);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(0x253229), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_battery_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(HEADER_ACCENT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_battery_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_battery_bar, 0, LV_PART_MAIN | LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_battery_bar, 1, LV_PART_MAIN);

    s_battery_value_label = lv_label_create(battery_wrap);
    lv_obj_set_style_text_font(s_battery_value_label, terminal_font, 0);

    s_notification_label = lv_label_create(s_header_root);
    lv_obj_set_flex_grow(s_notification_label, 1);
    lv_obj_set_width(s_notification_label, LV_PCT(100));
    lv_label_set_long_mode(s_notification_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_notification_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_notification_label, lv_color_hex(HEADER_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(s_notification_label, terminal_font, 0);
    lv_obj_set_scrollbar_mode(s_notification_label, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_notification_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(s_notification_label, "");

    header_render();
}

void header_update_status(void)
{
    (void)lv_async_call(header_async_refresh, NULL);
}

void header_set_notification(const char *text, uint32_t timeout_ms)
{
    header_notification_update_t *update;

    update = calloc(1, sizeof(*update));
    if (update == NULL) {
        return;
    }

    snprintf(update->text, sizeof(update->text), "%s", text != NULL ? text : "");
    update->timeout_ms = timeout_ms;
    (void)header_schedule(header_async_notification, update);
}

void header_update_wifi(bool connected, int rssi)
{
    s_header_state.wifi_connected = connected;
    s_header_state.wifi_rssi = rssi;

    header_wifi_update_t *update = calloc(1, sizeof(*update));
    if (update != NULL) {
        update->connected = connected;
        update->rssi = rssi;
        if (!header_schedule(header_async_wifi, update)) {
            free(update);
            header_render();
        }
    } else {
        header_render();
    }
}

void header_update_battery(int percent)
{
    int clamped = (percent < 0) ? 0 : (percent > 100) ? 100 : percent;
    s_header_state.battery_percent = clamped;

    header_battery_update_t *update = calloc(1, sizeof(*update));
    if (update != NULL) {
        update->percent = clamped;
        if (!header_schedule(header_async_battery, update)) {
            free(update);
            header_render();
        }
    } else {
        header_render();
    }
}

void header_update_bluetooth(bool enabled, bool connected)
{
    s_header_state.bluetooth_enabled = enabled;
    s_header_state.bluetooth_connected = connected;

    header_bluetooth_update_t *update = calloc(1, sizeof(*update));
    if (update != NULL) {
        update->enabled = enabled;
        update->connected = connected;
        if (!header_schedule(header_async_bluetooth, update)) {
            free(update);
            header_render();
        }
    } else {
        header_render();
    }
}

void header_update_usb(bool connected)
{
    s_header_state.usb_connected = connected;

    header_toggle_update_t *update = calloc(1, sizeof(*update));
    if (update != NULL) {
        update->connected = connected;
        if (!header_schedule(header_async_usb, update)) {
            free(update);
            header_render();
        }
    } else {
        header_render();
    }
}

void header_update_sd(bool mounted)
{
    /* Update state immediately so callers see the change even if async render is delayed.
     * The bool write is atomic on this platform and safe from any task context. */
    s_header_state.sd_mounted = mounted;

    /* Schedule a render via LVGL async dispatch to refresh the widget */
    header_toggle_update_t *update = calloc(1, sizeof(*update));
    if (update != NULL) {
        update->connected = mounted;
        if (!header_schedule(header_async_sd, update)) {
            free(update);
            /* If async dispatch failed, try direct render (caller is on LVGL task) */
            header_render();
        }
    } else {
        /* Allocation failed, try direct render */
        header_render();
    }
}
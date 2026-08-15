/**
 * @file header.c
 * @brief Fixed top status bar implementation for P4MiniShell.
 *
 * Completely rewritten header with grouped icon panels, animated notifications,
 * system memory, CPU usage, and battery info — all dynamically linked to
 * FreeRTOS runtime statistics for real-time updates.
 *
 * Visual layout (left to right):
 *   [Status Panel: WiFi | BT | USB | SD]  [Notification Area (center)]  [Sys Panel: MEM | CPU | BAT]
 *
 * Architecture:
 *   - Two panel containers (status + system) with subtle background for grouping
 *   - Notification area in the center with icon prefix and scroll animation
 *   - System panel shows memory (free heap), CPU usage (bar + %), and battery (bar + %)
 *   - Battery always visible — shows "BAT N/C" when ADC is not connected
 *   - All state cached in s_header_state, rendered via header_render()
 *   - Async updates via lv_async_call for thread safety
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"

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
#define HEADER_CPU_GRAPH            P4_CONFIG_HEADER_CPU_GRAPH
#define HEADER_CPU_GRAPH_POINTS     P4_CONFIG_HEADER_CPU_GRAPH_POINTS
#define HEADER_CPU_GRAPH_WIDTH_PX   P4_CONFIG_HEADER_CPU_GRAPH_WIDTH_PX

/* ---- Panel background tint (slightly lighter than main BG for depth) ---- */
#define HEADER_PANEL_BG             0x1A2A22
#define HEADER_SYS_PANEL_BG         0x16241E

/* ---- Header visibility (survives deinit so a hidden bar stays hidden
 * across a UI rebuild, e.g. when `config HEADER=OFF` is applied). ---- */
static bool s_header_visible = true;

/* ---- Icon type enum ---- */
typedef enum {
    HEADER_ICON_WIFI = 0,
    HEADER_ICON_BLUETOOTH,
    HEADER_ICON_USB,
    HEADER_ICON_SD,
    HEADER_ICON_COUNT,
} header_icon_type_t;

/* ---- Cached state structure ---- */
typedef struct {
    bool wifi_connected;
    int wifi_rssi;
    int battery_percent;
    bool battery_adc_ready;       /* true when ADC is connected and reading valid data */
    bool bluetooth_enabled;
    bool bluetooth_connected;
    bool usb_connected;
    header_sd_state_t sd_state;
    char notification[HEADER_NOTIFICATION_BYTES];
    uint32_t free_heap_bytes;     /* real-time from FreeRTOS heap_caps */
    uint32_t total_heap_bytes;    /* real-time total heap */
    int cpu_percent;              /* real-time from FreeRTOS runtime stats */
    uint32_t task_count;          /* real-time FreeRTOS task count */
    uint32_t uptime_seconds;      /* system uptime */
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
    bool adc_ready;
} header_battery_update_t;

typedef struct {
    header_sd_state_t state;
} header_sd_update_t;

typedef struct {
    uint32_t free_heap_bytes;
    uint32_t total_heap_bytes;
} header_mem_update_t;

typedef struct {
    int cpu_percent;
    uint32_t task_count;
} header_cpu_update_t;

typedef struct {
    uint32_t uptime_seconds;
} header_uptime_update_t;

/* ---- Static widget handles ---- */
static lv_obj_t *s_header_root;
static lv_obj_t *s_notification_label;
static lv_obj_t *s_notification_icon;
static lv_obj_t *s_status_icons[HEADER_ICON_COUNT];
static lv_obj_t *s_status_panel;
static lv_obj_t *s_sys_panel;
static lv_obj_t *s_battery_icon_label;
static lv_obj_t *s_battery_bar;
static lv_obj_t *s_battery_value_label;
static lv_obj_t *s_mem_label;
static lv_obj_t *s_cpu_label;
static lv_obj_t *s_cpu_bar;
static lv_obj_t *s_cpu_graph;
static lv_chart_series_t *s_cpu_graph_series;
static lv_obj_t *s_cpu_value_label;
static lv_timer_t *s_notification_timer;
__attribute__((unused)) static lv_coord_t s_header_height = HEADER_HEIGHT;
static header_state_t s_header_state = {
    .battery_percent = 100,
    .battery_adc_ready = false,
    .wifi_rssi = -127,
    .sd_state = HEADER_SD_NONE,
    .free_heap_bytes = 0,
    .total_heap_bytes = 0,
    .cpu_percent = 0,
    .task_count = 0,
    .uptime_seconds = 0,
};

/* ---- Dynamic height scaling - rotation-aware ---- */
__attribute__((unused)) static lv_coord_t header_scale_height(void)
{
    lv_display_t *display = lv_display_get_default();
    int32_t vertical_res = 600;
    lv_coord_t scaled;

    if (display != NULL) {
        lv_display_rotation_t rot = lv_display_get_rotation(display);
        /* When rotated 90 or 270 degrees, width and height swap */
        if (rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270) {
            vertical_res = lv_display_get_horizontal_resolution(display);
        } else {
            vertical_res = lv_display_get_vertical_resolution(display);
        }
    }

    scaled = (lv_coord_t)(vertical_res / 15);
    if (scaled < 36) {
        scaled = 36;
    }
    if (scaled > 64) {
        scaled = 64;
    }

    return scaled;
}

/* ---- Battery text indicator (ASCII, guaranteed to render with any font) ---- */
static const char *header_battery_text_for_percent(int percent)
{
    if (percent >= 90) return "[####]";
    if (percent >= 65) return "[### ]";
    if (percent >= 40) return "[##  ]";
    if (percent >= 15) return "[#   ]";
    return "[    ]";
}

/* ---- Wi-Fi signal quality label ---- */
static const char *header_wifi_quality_label(int rssi)
{
    if (rssi >= -55) return "HI";
    if (rssi >= -68) return "MID";
    if (rssi >= -80) return "LOW";
    return "WEAK";
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

    /* The notification timer is armed as a ONE-SHOT (repeat_count = 1). After
     * this callback returns, lv_timer_exec() auto-deletes it (lv_timer_delete
     * frees the timer node) because auto_delete defaults to true. If we do not
     * clear the pointer here, the next header_async_notification() sees a
     * non-NULL s_notification_timer and calls lv_timer_set_period/reset/resume
     * on the FREED node — a use-after-free that writes {period, last_run,
     * paused} into reclaimed heap memory and corrupts the heap (observed as
     * "CORRUPT HEAP" and LVGL blue-screen failures after WiFi connect). */
    s_notification_timer = NULL;

    if (s_notification_label != NULL) {
        lv_label_set_text(s_notification_label, "");
    }
    if (s_notification_icon != NULL) {
        lv_obj_add_flag(s_notification_icon, LV_OBJ_FLAG_HIDDEN);
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
    case HEADER_ICON_WIFI: {
        const char *quality = s_header_state.wifi_connected
            ? header_wifi_quality_label(s_header_state.wifi_rssi)
            : "OFF";
        snprintf(buffer, sizeof(buffer), "WiFi %s", quality);
        color = header_status_color(s_header_state.wifi_connected);
        break;
    }
    case HEADER_ICON_BLUETOOTH:
        if (s_header_state.bluetooth_connected) {
            snprintf(buffer, sizeof(buffer), "BT ON");
        } else if (s_header_state.bluetooth_enabled) {
            snprintf(buffer, sizeof(buffer), "BT IDLE");
        } else {
            snprintf(buffer, sizeof(buffer), "BT OFF");
        }
        color = header_status_color(s_header_state.bluetooth_enabled);
        break;

    case HEADER_ICON_USB:
        snprintf(buffer, sizeof(buffer), "USB %s",
                 s_header_state.usb_connected ? "ON" : "OFF");
        color = header_status_color(s_header_state.usb_connected);
        break;

    case HEADER_ICON_SD: {
        const char *sd_text;
        switch (s_header_state.sd_state) {
        case HEADER_SD_NONE:      sd_text = "NO";  color = header_status_color(false); break;
        case HEADER_SD_INSERTED:  sd_text = "INS"; color = lv_color_hex(HEADER_WARN_COLOR); break;
        case HEADER_SD_MOUNTED:   sd_text = "ON";  color = header_status_color(true); break;
        case HEADER_SD_ERROR:     sd_text = "ERR"; color = lv_color_hex(HEADER_WARN_COLOR); break;
        default:                  sd_text = "?";   color = header_status_color(false); break;
        }
        snprintf(buffer, sizeof(buffer), "SD %s", sd_text);
        break;
    }
    default:
        return;
    }

    lv_label_set_text(label, buffer);
    lv_obj_set_style_text_color(label, color, 0);
}

/* ---- Format free heap for display ---- */
static void header_format_mem(char *buf, size_t buf_size)
{
    uint32_t free_heap = s_header_state.free_heap_bytes;
    uint32_t total_heap = s_header_state.total_heap_bytes;
    int percent = (total_heap > 0) ? (int)((free_heap * 100) / total_heap) : 0;

    const char *prefix = percent > 30 ? "MEM" : "LOW";
    if (free_heap >= 1048576) {
        snprintf(buf, buf_size, "%s %.1fM", prefix, (double)free_heap / 1048576.0);
    } else if (free_heap >= 1024) {
        snprintf(buf, buf_size, "%s %" PRIu32 "K", prefix, free_heap / 1024);
    } else {
        snprintf(buf, buf_size, "%s %" PRIu32, prefix, free_heap);
    }
}

/* ---- Format CPU for display ---- */
static void header_format_cpu(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "CPU %d%%", s_header_state.cpu_percent);
}

/* ---- Main render: updates all icons, battery, mem, cpu, notification ---- */
static void header_render(void)
{
    if (s_header_root == NULL) {
        return;
    }

    /* Header rendering touches LVGL objects directly. The async callbacks
     * that call this function run on the LVGL task, but the fallback paths
     * (when lv_async_call fails to schedule) and the periodic refresh timer
     * can invoke it from other tasks. Hold the recursive LVGL port lock so
     * every call serialises with the LVGL render cycle regardless of caller. */
    lvgl_port_lock(0);

    lv_color_t battery_color;
    lv_color_t cpu_color;
    char mem_buf[32];
    char cpu_buf[16];
    int i;

    if (s_header_root == NULL) {
        return;
    }

    /* Render notification area */
    if (s_header_state.notification[0] != '\0') {
        lv_label_set_text(s_notification_label, s_header_state.notification);
        lv_obj_clear_flag(s_notification_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_notification_label, "");
        lv_obj_add_flag(s_notification_icon, LV_OBJ_FLAG_HIDDEN);
    }

    /* Render all status icons */
    for (i = 0; i < HEADER_ICON_COUNT; i++) {
        header_render_icon((header_icon_type_t)i);
    }

    /* Render battery (always visible — shows N/C when ADC is not connected) */
    if (s_header_state.battery_adc_ready) {
        lv_label_set_text(s_battery_icon_label,
                          header_battery_text_for_percent(s_header_state.battery_percent));
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
    } else {
        /* Battery not connected — show N/C with muted styling */
        lv_label_set_text(s_battery_icon_label, "[    ]");
        lv_obj_set_style_text_color(s_battery_icon_label, lv_color_hex(HEADER_MUTED_COLOR), 0);
        lv_bar_set_value(s_battery_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(HEADER_MUTED_COLOR), LV_PART_INDICATOR);
        lv_label_set_text(s_battery_value_label, "N/C");
        lv_obj_set_style_text_color(s_battery_value_label, lv_color_hex(HEADER_MUTED_COLOR), 0);
    }

    /* Render memory info */
    header_format_mem(mem_buf, sizeof(mem_buf));
    lv_label_set_text(s_mem_label, mem_buf);
    lv_obj_set_style_text_color(s_mem_label, lv_color_hex(HEADER_TEXT_COLOR), 0);

    /* Render CPU info */
    header_format_cpu(cpu_buf, sizeof(cpu_buf));
    lv_label_set_text(s_cpu_label, cpu_buf);
    cpu_color = s_header_state.cpu_percent >= P4_CONFIG_HEADER_CPU_WARN_PCT
                    ? lv_color_hex(HEADER_WARN_COLOR)
                    : lv_color_hex(HEADER_ACCENT_COLOR);
    lv_obj_set_style_text_color(s_cpu_label, cpu_color, 0);

    if (HEADER_CPU_GRAPH && s_cpu_graph != NULL) {
        /* Push the fresh sample onto the sparkline; SHIFT mode scrolls the
         * history left automatically. */
        lv_chart_set_next_value(s_cpu_graph, s_cpu_graph_series, s_header_state.cpu_percent);
        if (s_cpu_graph_series != NULL) {
            lv_chart_set_series_color(s_cpu_graph, s_cpu_graph_series, cpu_color);
        }
    } else if (s_cpu_bar != NULL) {
        lv_bar_set_value(s_cpu_bar, s_header_state.cpu_percent, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cpu_bar, cpu_color, LV_PART_INDICATOR);
    }

    char cpu_pct[16];
    snprintf(cpu_pct, sizeof(cpu_pct), "%d%%", s_header_state.cpu_percent);
    lv_label_set_text(s_cpu_value_label, cpu_pct);
    lv_obj_set_style_text_color(s_cpu_value_label, cpu_color, 0);

    /* Force layout update */
    lv_obj_update_layout(s_header_root);

    lvgl_port_unlock();
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

/* ---- Batch async update payload ---- */
typedef struct {
    bool wifi_connected;
    int wifi_rssi;
    int battery_percent;
    bool battery_adc_ready;
    bool bluetooth_enabled;
    bool bluetooth_connected;
    bool usb_connected;
    header_sd_state_t sd_state;
    uint32_t free_heap_bytes;
    uint32_t total_heap_bytes;
    int cpu_percent;
    uint32_t task_count;
    uint32_t uptime_seconds;
} header_batch_update_t;

/* ---- Async callbacks ---- */
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
static void header_async_wifi(void *u_data)       { free(u_data); header_render(); }
static void header_async_battery(void *u_data)    { free(u_data); header_render(); }
static void header_async_bluetooth(void *u_data)  { free(u_data); header_render(); }
static void header_async_usb(void *u_data)        { free(u_data); header_render(); }
static void header_async_sd(void *u_data)         { free(u_data); header_render(); }
static void header_async_mem(void *u_data)        { free(u_data); header_render(); }
static void header_async_cpu(void *u_data)        { free(u_data); header_render(); }
static void header_async_uptime(void *u_data)     { free(u_data); header_render(); }
/* ---- Batch async callback: updates all state then renders once ---- */
__attribute__((unused)) static void header_async_batch(void *user_data)
{
    header_batch_update_t *u = (header_batch_update_t *)user_data;
    if (u == NULL) return;
    s_header_state.wifi_connected = u->wifi_connected;
    s_header_state.wifi_rssi = u->wifi_rssi;
    s_header_state.battery_percent = u->battery_percent;
    s_header_state.battery_adc_ready = u->battery_adc_ready;
    s_header_state.bluetooth_enabled = u->bluetooth_enabled;
    s_header_state.bluetooth_connected = u->bluetooth_connected;
    s_header_state.usb_connected = u->usb_connected;
    s_header_state.sd_state = u->sd_state;
    s_header_state.free_heap_bytes = u->free_heap_bytes;
    s_header_state.total_heap_bytes = u->total_heap_bytes;
    s_header_state.cpu_percent = u->cpu_percent;
    s_header_state.task_count = u->task_count;
    s_header_state.uptime_seconds = u->uptime_seconds;
    free(u);
    header_render();
}
/* ========================================================================
 * HEADER TOUCH (long-press shows build identity)
 * ======================================================================== */

/** Recursively make every header widget bubble events to the header root, so a
 *  long-press anywhere in the status bar reaches the root handler (LVGL does
 *  not bubble by default). */
static void header_enable_event_bubble(lv_obj_t *obj)
{
    uint32_t i;
    uint32_t count;

    if (obj == NULL) {
        return;
    }
    count = lv_obj_get_child_count(obj);
    for (i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
        header_enable_event_bubble(child);
    }
}

/** Long-press on the status bar surfaces the build identity banner. */
static void header_touch_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_LONG_PRESSED) {
        return;
    }

    const esp_app_desc_t *d = esp_app_get_description();
    const char *git  = (d != NULL) ? d->version : "n/a";
    const char *date = (d != NULL) ? d->date : "n/a";
    const char *time = (d != NULL) ? d->time : "n/a";
    char identity[P4_CONFIG_HEADER_NOTIFICATION_BYTES];

    snprintf(identity, sizeof(identity), "%s %s | built %s %s | git %s",
             P4_CONFIG_PRODUCT_NAME,
             P4_CONFIG_VERSION_STRING,
             date, time, git);

    header_set_notification(identity, P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS * 2);
}

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

void header_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    const lv_font_t *status_font = LV_FONT_DEFAULT;
    lv_coord_t vert_pad;
    lv_coord_t horz_pad;
    lv_coord_t panel_pad_v;
    int i;
    if (screen == NULL || s_header_root != NULL) {
        return;
    }

    s_header_height = header_scale_height();
    vert_pad  = s_header_height >= 48 ? 6 : 4;
    horz_pad  = s_header_height >= 48 ? 10 : 6;
    panel_pad_v = s_header_height >= 48 ? 4 : 2;

    /* ====================================================================
     * HEADER ROOT: full-width flex row, items centered vertically
     * Tight padding to maximize space for content panels.
     * ==================================================================== */
    s_header_root = lv_obj_create(screen);
    lv_obj_set_width(s_header_root, LV_PCT(100));
    lv_obj_set_height(s_header_root, s_header_height);
    lv_obj_set_layout(s_header_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_header_root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_header_root, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(s_header_root, horz_pad, 0);
    lv_obj_set_style_pad_right(s_header_root, horz_pad, 0);
    lv_obj_set_style_pad_top(s_header_root, vert_pad, 0);
    lv_obj_set_style_pad_bottom(s_header_root, vert_pad, 0);
    lv_obj_set_style_pad_column(s_header_root, 4, 0);
    lv_obj_set_style_radius(s_header_root, 0, 0);
    lv_obj_set_style_bg_color(s_header_root, lv_color_hex(HEADER_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_header_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_header_root, 0, 0);
    lv_obj_set_scrollbar_mode(s_header_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_header_root, LV_OBJ_FLAG_SCROLLABLE);
    if (!s_header_visible) {
        lv_obj_add_flag(s_header_root, LV_OBJ_FLAG_HIDDEN);
    }

    /* Long-press the status bar to show the build identity banner. */
    lv_obj_add_flag(s_header_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_header_root, header_touch_event_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* ====================================================================
     * LEFT: STATUS PANEL — Wi-Fi, BT, USB, SD icons
     * Compact layout: tight gaps, smaller min-width.
     * ==================================================================== */
    s_status_panel = lv_obj_create(s_header_root);
    lv_obj_set_style_bg_color(s_status_panel, lv_color_hex(HEADER_PANEL_BG), 0);
    lv_obj_set_style_bg_opa(s_status_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_panel, 0, 0);
    lv_obj_set_style_radius(s_status_panel, 4, 0);
    lv_obj_set_style_pad_left(s_status_panel, 6, 0);
    lv_obj_set_style_pad_right(s_status_panel, 6, 0);
    lv_obj_set_style_pad_top(s_status_panel, panel_pad_v, 0);
    lv_obj_set_style_pad_bottom(s_status_panel, panel_pad_v, 0);
    lv_obj_set_style_pad_column(s_status_panel, 6, 0);
    lv_obj_set_layout(s_status_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_status_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_status_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_status_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_flex_grow(s_status_panel, 0, 0);
    /* Fixed min-width: each icon ~55px + 6px gap + 6px padding each side = 55*4+18+12 = 250 */
    lv_obj_set_style_min_width(s_status_panel, 250, 0);

    /* Create icon labels inside status panel — compact letter spacing */
    for (i = 0; i < HEADER_ICON_COUNT; i++) {
        s_status_icons[i] = lv_label_create(s_status_panel);
        lv_obj_set_style_text_font(s_status_icons[i], status_font, 0);
        lv_obj_set_style_text_letter_space(s_status_icons[i], -1, 0);
        lv_obj_clear_flag(s_status_icons[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ====================================================================
     * CENTER: NOTIFICATION AREA — icon + scrolling text
     * Wrapped in a container with flex_grow=1 so it fills available space
     * between the two fixed-width panels, but the label inside is capped
     * so it never overlaps the side panels.
     * ==================================================================== */
    /* Notification container — absorbs free space, clips overflow */
    lv_obj_t *notif_container = lv_obj_create(s_header_root);
    lv_obj_set_style_bg_opa(notif_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(notif_container, 0, 0);
    lv_obj_set_style_pad_all(notif_container, 0, 0);
    lv_obj_set_style_flex_grow(notif_container, 1, 0);
    lv_obj_set_flex_grow(notif_container, 1);
    lv_obj_set_layout(notif_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(notif_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(notif_container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(notif_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(notif_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Notification icon (bell text, hidden when no notification) */
    s_notification_icon = lv_label_create(notif_container);
    lv_label_set_text(s_notification_icon, "!");
    lv_obj_set_style_text_color(s_notification_icon, lv_color_hex(HEADER_WARN_COLOR), 0);
    lv_obj_set_style_text_font(s_notification_icon, status_font, 0);
    lv_obj_add_flag(s_notification_icon, LV_OBJ_FLAG_HIDDEN);

    /* Notification text — no flex_grow, just content-sized, scrolls if too long */
    s_notification_label = lv_label_create(notif_container);
    lv_obj_set_width(s_notification_label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_notification_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_notification_label, lv_color_hex(HEADER_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(s_notification_label, status_font, 0);
    lv_obj_set_scrollbar_mode(s_notification_label, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_notification_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(s_notification_label, "");

    /* ====================================================================
     * RIGHT: SYSTEM PANEL — MEM | CPU | BAT (pinned to far right)
     * Compact layout: tight gaps, smaller bars, generous min-width.
     * ==================================================================== */
    s_sys_panel = lv_obj_create(s_header_root);
    lv_obj_set_style_bg_color(s_sys_panel, lv_color_hex(HEADER_SYS_PANEL_BG), 0);
    lv_obj_set_style_bg_opa(s_sys_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sys_panel, 0, 0);
    lv_obj_set_style_radius(s_sys_panel, 4, 0);
    lv_obj_set_style_pad_left(s_sys_panel, 6, 0);
    lv_obj_set_style_pad_right(s_sys_panel, 6, 0);
    lv_obj_set_style_pad_top(s_sys_panel, panel_pad_v, 0);
    lv_obj_set_style_pad_bottom(s_sys_panel, panel_pad_v, 0);
    lv_obj_set_style_pad_column(s_sys_panel, 5, 0);
    lv_obj_set_layout(s_sys_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_sys_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sys_panel, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_sys_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_sys_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_flex_grow(s_sys_panel, 0, 0);
    /* Fixed min-width: MEM(~85px) + CPU label+bar+%(~60px) + BAT icon+bar+%(~75px) + 2 separators + 6px padding each side + gaps */
    lv_obj_set_style_min_width(s_sys_panel, 280, 0);

    /* ---- MEM label ---- */
    s_mem_label = lv_label_create(s_sys_panel);
    lv_obj_set_style_text_font(s_mem_label, status_font, 0);
    lv_obj_set_style_text_letter_space(s_mem_label, -1, 0);
    lv_obj_set_style_text_color(s_mem_label, lv_color_hex(HEADER_TEXT_COLOR), 0);

    /* Separator MEM|CPU */
    lv_obj_t *sep1 = lv_label_create(s_sys_panel);
    lv_label_set_text(sep1, "|");
    lv_obj_set_style_text_color(sep1, lv_color_hex(HEADER_MUTED_COLOR), 0);

    /* ---- CPU label ---- */
    s_cpu_label = lv_label_create(s_sys_panel);
    lv_obj_set_style_text_font(s_cpu_label, status_font, 0);
    lv_obj_set_style_text_letter_space(s_cpu_label, -1, 0);

    /* CPU indicator: a compact sparkline of recent samples when the graph is
     * enabled, otherwise the classic single-value bar. */
    if (HEADER_CPU_GRAPH) {
        lv_coord_t graph_h = s_header_height >= 48 ? 12 : 10;

        s_cpu_graph = lv_chart_create(s_sys_panel);
        lv_obj_set_size(s_cpu_graph, HEADER_CPU_GRAPH_WIDTH_PX, graph_h);
        lv_chart_set_type(s_cpu_graph, LV_CHART_TYPE_BAR);
        lv_chart_set_update_mode(s_cpu_graph, LV_CHART_UPDATE_MODE_SHIFT);
        lv_chart_set_point_count(s_cpu_graph, HEADER_CPU_GRAPH_POINTS);
        lv_chart_set_axis_range(s_cpu_graph, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_obj_set_style_bg_color(s_cpu_graph, lv_color_hex(0x253229), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_cpu_graph, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_cpu_graph, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_cpu_graph, 1, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_cpu_graph, 0, LV_PART_ITEMS);
        lv_obj_set_style_pad_all(s_cpu_graph, 0, LV_PART_ITEMS);
        s_cpu_graph_series = lv_chart_add_series(s_cpu_graph,
                                                 lv_color_hex(HEADER_ACCENT_COLOR),
                                                 LV_CHART_AXIS_PRIMARY_Y);
    } else {
        s_cpu_bar = lv_bar_create(s_sys_panel);
        lv_obj_set_size(s_cpu_bar, 24, s_header_height >= 48 ? 10 : 8);
        lv_bar_set_range(s_cpu_bar, 0, 100);
        lv_obj_set_style_bg_color(s_cpu_bar, lv_color_hex(0x253229), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_cpu_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_cpu_bar, lv_color_hex(HEADER_ACCENT_COLOR), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_cpu_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_cpu_bar, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(s_cpu_bar, 2, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(s_cpu_bar, 1, LV_PART_MAIN);
    }

    /* CPU percentage label */
    s_cpu_value_label = lv_label_create(s_sys_panel);
    lv_obj_set_style_text_font(s_cpu_value_label, status_font, 0);
    lv_obj_set_style_text_letter_space(s_cpu_value_label, -1, 0);

    /* Separator CPU|BAT */
    lv_obj_t *sep2 = lv_label_create(s_sys_panel);
    lv_label_set_text(sep2, "|");
    lv_obj_set_style_text_color(sep2, lv_color_hex(HEADER_MUTED_COLOR), 0);

    /* Battery icon label */
    s_battery_icon_label = lv_label_create(s_sys_panel);
    lv_obj_set_style_text_font(s_battery_icon_label, status_font, 0);
    lv_obj_set_style_text_letter_space(s_battery_icon_label, -1, 0);

    /* Battery bar — compact */
    s_battery_bar = lv_bar_create(s_sys_panel);
    lv_obj_set_size(s_battery_bar, 24, s_header_height >= 48 ? 10 : 8);
    lv_bar_set_range(s_battery_bar, 0, 100);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(0x253229), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_battery_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(HEADER_ACCENT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_battery_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_battery_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_battery_bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_battery_bar, 1, LV_PART_MAIN);

    /* Battery percentage label */
    s_battery_value_label = lv_label_create(s_sys_panel);
    lv_obj_set_style_text_font(s_battery_value_label, status_font, 0);
    lv_obj_set_style_text_letter_space(s_battery_value_label, -1, 0);

    /* Any long-press anywhere on the status bar bubbles to the root handler. */
    header_enable_event_bubble(s_header_root);

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

void header_update_battery(int percent, bool adc_ready)
{
    int clamped = (percent < 0) ? 0 : (percent > 100) ? 100 : percent;
    s_header_state.battery_percent = clamped;
    s_header_state.battery_adc_ready = adc_ready;
    header_battery_update_t *u = calloc(1, sizeof(*u));
    if (u != NULL) {
        u->percent = clamped;
        u->adc_ready = adc_ready;
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

void header_update_mem(uint32_t free_heap_bytes, uint32_t total_heap_bytes)
{
    s_header_state.free_heap_bytes = free_heap_bytes;
    s_header_state.total_heap_bytes = total_heap_bytes;
    header_mem_update_t *u = calloc(1, sizeof(*u));
    if (u != NULL) {
        u->free_heap_bytes = free_heap_bytes;
        u->total_heap_bytes = total_heap_bytes;
        if (!header_schedule(header_async_mem, u)) { free(u); header_render(); }
    } else { header_render(); }
}

void header_update_cpu(int cpu_percent, uint32_t task_count)
{
    int clamped = (cpu_percent < 0) ? 0 : (cpu_percent > 100) ? 100 : cpu_percent;
    s_header_state.cpu_percent = clamped;
    s_header_state.task_count = task_count;
    header_cpu_update_t *u = calloc(1, sizeof(*u));
    if (u != NULL) {
        u->cpu_percent = clamped;
        u->task_count = task_count;
        if (!header_schedule(header_async_cpu, u)) { free(u); header_render(); }
    } else { header_render(); }
}

void header_update_uptime(uint32_t uptime_seconds)
{
    s_header_state.uptime_seconds = uptime_seconds;
    header_uptime_update_t *u = calloc(1, sizeof(*u));
    if (u != NULL) {
        u->uptime_seconds = uptime_seconds;
        if (!header_schedule(header_async_uptime, u)) { free(u); header_render(); }
    } else { header_render(); }
}

void header_update_batch(
    bool wifi_connected, int wifi_rssi,
    int battery_percent, bool battery_adc_ready,
    bool bt_enabled, bool bt_connected,
    bool usb_connected, header_sd_state_t sd_state,
    uint32_t free_heap, uint32_t total_heap,
    int cpu_percent, uint32_t task_count,
    uint32_t uptime_seconds)
{
    /* Set all state directly (atomic on this platform) then schedule
     * a single async render. No dynamic allocation needed — avoids
     * heap corruption from deferred async call payloads. */
    s_header_state.wifi_connected = wifi_connected;
    s_header_state.wifi_rssi = wifi_rssi;
    s_header_state.battery_percent = (battery_percent < 0) ? 0 : (battery_percent > 100) ? 100 : battery_percent;
    s_header_state.battery_adc_ready = battery_adc_ready;
    s_header_state.bluetooth_enabled = bt_enabled;
    s_header_state.bluetooth_connected = bt_connected;
    s_header_state.usb_connected = usb_connected;
    s_header_state.sd_state = sd_state;
    s_header_state.free_heap_bytes = free_heap;
    s_header_state.total_heap_bytes = total_heap;
    s_header_state.cpu_percent = (cpu_percent < 0) ? 0 : (cpu_percent > 100) ? 100 : cpu_percent;
    s_header_state.task_count = task_count;
    s_header_state.uptime_seconds = uptime_seconds;

    /* Schedule a single async render (no payload needed) */
    (void)lv_async_call(header_async_refresh, NULL);
}

void header_set_visible(bool visible)
{
    s_header_visible = visible;
    if (s_header_root != NULL) {
        if (visible) {
            lv_obj_clear_flag(s_header_root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_header_root, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

bool header_get_visible(void)
{
    return s_header_visible;
}

void header_deinit(void)
{
    /* Do NOT call lv_timer_delete here — the timer may have been invalidated
     * by a previous cleanup or may be owned by a different LVGL context.
     * lv_obj_clean() in shell_build_ui will destroy all widgets and the
     * timer will be cleaned up by LVGL's internal timer management.
     * Just NULL out the pointer so header_init() creates a fresh one. */
    s_notification_timer = NULL;

    /* Clear all widget handles — the screen will be cleaned by shell_build_ui
     * via lv_obj_clean(), which deletes all children including our widgets.
     * Reset state to defaults for fresh init. */
    s_header_root = NULL;
    s_notification_label = NULL;
    s_notification_icon = NULL;
    s_status_panel = NULL;
    s_sys_panel = NULL;
    s_battery_icon_label = NULL;
    s_battery_bar = NULL;
    s_battery_value_label = NULL;
    s_mem_label = NULL;
    s_cpu_label = NULL;
    s_cpu_bar = NULL;
    s_cpu_graph = NULL;
    s_cpu_graph_series = NULL;
    s_cpu_value_label = NULL;
    for (int i = 0; i < HEADER_ICON_COUNT; i++) {
        s_status_icons[i] = NULL;
    }

    /* Reset state to defaults for fresh init */
    s_header_state = (header_state_t){
        .battery_percent = 100,
        .battery_adc_ready = false,
        .wifi_rssi = -127,
        .sd_state = HEADER_SD_NONE,
        .free_heap_bytes = 0,
        .total_heap_bytes = 0,
        .cpu_percent = 0,
        .task_count = 0,
        .uptime_seconds = 0,
    };
}
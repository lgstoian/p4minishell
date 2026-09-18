/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
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
#include <strings.h>
#include <inttypes.h>

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"

#include "header.h"
#include "header_layout.h"
#include "header_status.h"
#include "header_notify_queue.h"
#include "theme.h"
#include "p4minishell_config.h"

/* Chained UI font (windows component). Declared extern instead of including
 * windows.h: windows already REQUIRES header, so a CMake edge back would be
 * a dependency cycle (Phase 1 moves fonts into components/font/). */
extern const lv_font_t *windows_get_ui_font(void);

/* Smaller proportional fallback used only when the auto layout cannot fit the
 * primary UI font (dynamic font sizing). Always compiled (LVGL default). */
extern const lv_font_t lv_font_montserrat_14;

/* ---- Backward-compatibility aliases ---- */
#define HEADER_NOTIFICATION_BYTES   P4_CONFIG_HEADER_NOTIFICATION_BYTES
/* Colors follow the active theme (components/font/theme.c); the default
 * table's values equal the original P4_CONFIG_* literals, so the default
 * theme is pixel-identical to the pre-theme build. */
#define HEADER_TEXT_COLOR           (theme_current()->text_body)
#define HEADER_MUTED_COLOR          (theme_current()->text_muted)
#define HEADER_ACCENT_COLOR         (theme_current()->text)
#define HEADER_WARN_COLOR           (theme_current()->warn)
#define HEADER_ERROR_COLOR          (theme_current()->err)
#define HEADER_BG_COLOR             (theme_current()->bg_input_row)
#define HEADER_PANEL_COLOR          (theme_current()->bg_transcript)
#define HEADER_CPU_GRAPH            P4_CONFIG_HEADER_CPU_GRAPH
#define HEADER_CPU_GRAPH_POINTS     P4_CONFIG_HEADER_CPU_GRAPH_POINTS
#define HEADER_CPU_GRAPH_WIDTH_PX   P4_CONFIG_HEADER_CPU_GRAPH_WIDTH_PX

/* ---- Panel background tint (slightly lighter than main BG for depth) ---- */
#define HEADER_PANEL_BG             (theme_current()->header_panel_bg)
#define HEADER_SYS_PANEL_BG         (theme_current()->header_sys_bg)

/* ---- Header visibility (survives deinit so a hidden bar stays hidden
 * across a UI rebuild, e.g. when `config HEADER=OFF` is applied). ---- */
static bool s_header_visible = true;

/* The indicator identity enum (header_status_kind_t) and its panel count now
 * live in header.h so the tap/long-press callback ABI is public. */

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
    char notification[HEADER_NOTIFICATION_BYTES]; /* currently displayed */
    header_notify_level_t notification_level;
    uint32_t notification_timeout_ms;
    bool notification_active;
    header_notify_queue_t notify_queue;           /* waiting notifications */
    char clock_text[16];                          /* idle "HH:MM" / "--:--" */
    bool c6ota_busy;                              /* activity inputs */
    bool bg_jobs_running;
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
    header_notify_level_t level;
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
static lv_obj_t *s_notif_container;
static lv_obj_t *s_status_icons[HEADER_STATUS_PANEL_COUNT];
static lv_obj_t *s_status_panel;
static lv_obj_t *s_sys_panel;
static lv_obj_t *s_sep1;
static lv_obj_t *s_sep2;
static lv_obj_t *s_battery_icon_label;
static lv_obj_t *s_battery_bar;
static lv_obj_t *s_battery_value_label;
static lv_obj_t *s_mem_label;
static lv_obj_t *s_cpu_label;
static lv_obj_t *s_cpu_bar;
static lv_obj_t *s_cpu_graph;
static lv_chart_series_t *s_cpu_graph_series;
static lv_obj_t *s_cpu_value_label;
static lv_obj_t *s_uptime_label;
static lv_timer_t *s_notification_timer;
static lv_coord_t s_header_height = HEADER_HEIGHT;
static header_mode_t s_header_mode = HEADER_MODE_AUTO;
static int s_header_font_step = 0;      /* 0 = UI font, 1 = compact font */
/* Last resolved layout + measured geometry (diagnostics / `header status`). */
static header_layout_t s_last_layout;
static int s_last_screen_w = 0;
static int s_last_actual_left = 0;
static int s_last_actual_center = 0;
static int s_last_actual_right = 0;
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

/* ---- Dynamic height scaling - rotation-aware, single source of truth ----
 * Uses the same percentage/clamp policy as the window manager's reserved
 * header region (P4_CONFIG_WINDOW_HEADER_*), so the bar can never overlap the
 * transcript or leave a gap on any board/rotation. */
static lv_coord_t header_scale_height(void)
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

    scaled = (lv_coord_t)(vertical_res * P4_CONFIG_WINDOW_HEADER_HEIGHT_PCT / 100);
    if (scaled < P4_CONFIG_WINDOW_HEADER_HEIGHT_MIN) {
        scaled = P4_CONFIG_WINDOW_HEADER_HEIGHT_MIN;
    }
    if (scaled > P4_CONFIG_WINDOW_HEADER_HEIGHT_MAX) {
        scaled = P4_CONFIG_WINDOW_HEADER_HEIGHT_MAX;
    }

    return scaled;
}

int header_get_height(void)
{
    return (int)header_scale_height();
}

/* ---- Battery text indicator (ASCII, guaranteed to render with any font) ---- */
static const char *header_battery_text_for_percent(int percent)
{
    if (percent >= 90) return "[####]";
    if (percent >= 65) return "[### ]";
    if (percent >= 40) return "[##  ]";
    if (percent >= P4_CONFIG_HEADER_BAT_LOW_PCT) return "[#   ]";
    return "[    ]";
}

/* ---- Presentation style + tone -> color ---------------------------------- */

/** True when the compact colored-glyph style is configured. */
static bool header_glyph_style(void)
{
    return P4_CONFIG_HEADER_STATUS_STYLE == P4_CONFIG_HEADER_STATUS_GLYPH;
}

/** Map a semantic tone from header_status.c onto the active theme color. */
static lv_color_t header_tone_color(header_tone_t tone)
{
    switch (tone) {
    case HEADER_TONE_OK:   return lv_color_hex(HEADER_ACCENT_COLOR);
    case HEADER_TONE_WARN: return lv_color_hex(HEADER_WARN_COLOR);
    case HEADER_TONE_ERR:  return lv_color_hex(HEADER_ERROR_COLOR);
    case HEADER_TONE_MUTED:
    default:               return lv_color_hex(HEADER_MUTED_COLOR);
    }
}

/** Map a notification severity onto the active theme color. */
static lv_color_t header_notify_color(header_notify_level_t level)
{
    switch (level) {
    case HEADER_NOTIFY_WARN: return lv_color_hex(HEADER_WARN_COLOR);
    case HEADER_NOTIFY_ERR:  return lv_color_hex(HEADER_ERROR_COLOR);
    case HEADER_NOTIFY_INFO:
    default:                 return lv_color_hex(HEADER_TEXT_COLOR);
    }
}

/* ---- Notification queue + display timer ----
 *
 * The display timer is created once in header_init() and is PERSISTENT
 * (repeat_count = -1), paused while idle. Because it is never auto-freed, the
 * historical O7 hazard — calling lv_timer_set_* on a node that lv_timer_exec
 * had already deleted — cannot occur. header_deinit() deletes it. */

static void header_render(void);

/** Arm (timeout_ms > 0) or pause (timeout_ms == 0, sticky) the display timer. */
static void header_notify_arm(uint32_t timeout_ms)
{
    if (s_notification_timer == NULL) {
        return;
    }
    if (timeout_ms > 0) {
        lv_timer_set_period(s_notification_timer, timeout_ms);
        lv_timer_set_repeat_count(s_notification_timer, -1);
        lv_timer_reset(s_notification_timer);
        lv_timer_resume(s_notification_timer);
    } else {
        lv_timer_pause(s_notification_timer);
    }
}

/** Pop the next queued notification into the displayed slot. */
static bool header_notify_show_next(void)
{
    header_notify_item_t item;

    if (header_notify_queue_pop(&s_header_state.notify_queue, &item)) {
        snprintf(s_header_state.notification, sizeof(s_header_state.notification),
                 "%s", item.text);
        s_header_state.notification_level = item.level;
        s_header_state.notification_timeout_ms = item.timeout_ms;
        s_header_state.notification_active = true;
        header_notify_arm(item.timeout_ms);
        return true;
    }

    s_header_state.notification_active = false;
    s_header_state.notification[0] = '\0';
    header_notify_arm(0);
    return false;
}

/** Apply one notification update. Must run on the LVGL task / under the port lock. */
static void header_notify_apply(const header_notification_update_t *update)
{
    if (update == NULL) {
        return;
    }
    if (update->text[0] == '\0') {
        /* Explicit clear (`notify -`): drop the queue and the displayed item. */
        header_notify_queue_clear(&s_header_state.notify_queue);
        s_header_state.notification_active = false;
        s_header_state.notification[0] = '\0';
        header_notify_arm(0);
        return;
    }
    if (header_notify_queue_push(&s_header_state.notify_queue, update->text,
                                 update->level, update->timeout_ms) &&
        !s_header_state.notification_active) {
        (void)header_notify_show_next();
    }
}

/* ---- Notification timeout callback ---- */
static void header_notification_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    /* The displayed message expired: advance to the next queued one, or clear
     * and fall back to the idle clock. The timer stays alive (persistent). */
    (void)header_notify_show_next();
    header_render();
}

/* ---- Status text + tone, level-aware (FULL/SHORT/MIN) ---- */

/** True when the activity indicator should be shown (allowed and something runs). */
static bool header_activity_active(void)
{
    return P4_CONFIG_HEADER_ACTIVITY &&
           (s_header_state.c6ota_busy || s_header_state.bg_jobs_running);
}

/** Build the status text for an indicator at a compactness level. */
static void header_status_text(header_status_kind_t type, header_level_t level,
                               char *buffer, size_t buffer_size)
{
    const char *state = "?";
    const char *prefix_full = "";
    const char *prefix_short = "";

    /* The activity indicator is conditional: inactive contributes no text, so
     * the measurement pass gives it zero width and hides the label. */
    if (type == HEADER_STATUS_ACTIVITY && !header_activity_active()) {
        buffer[0] = '\0';
        return;
    }

    if (header_glyph_style()) {
        /* One fixed token for every level, so the panel width no longer grows
         * with the requested detail level; the reclaimed width goes to the
         * center notification area. Classification stays in header_status.c. */
        snprintf(buffer, buffer_size, "%s", header_status_glyph(type));
        return;
    }

    switch (type) {
    case HEADER_STATUS_WIFI:
        state = s_header_state.wifi_connected
                    ? header_status_wifi_label(s_header_state.wifi_rssi)
                    : "OFF";
        prefix_full = "WiFi ";
        prefix_short = "W:";
        break;
    case HEADER_STATUS_BLUETOOTH:
        state = s_header_state.bluetooth_connected ? "ON"
                : s_header_state.bluetooth_enabled ? "IDLE" : "OFF";
        prefix_full = "BT ";
        prefix_short = "B:";
        break;
    case HEADER_STATUS_USB:
        state = s_header_state.usb_connected ? "ON" : "OFF";
        prefix_full = "USB ";
        prefix_short = "U:";
        break;
    case HEADER_STATUS_SD:
        switch (s_header_state.sd_state) {
        case HEADER_SD_INSERTED: state = "INS"; break;
        case HEADER_SD_MOUNTED:  state = "ON";  break;
        case HEADER_SD_ERROR:    state = "ERR"; break;
        case HEADER_SD_NONE:
        default:                 state = "NO";  break;
        }
        prefix_full = "SD ";
        prefix_short = "SD:";
        break;
    case HEADER_STATUS_ACTIVITY:
        state = "ACT";
        break;
    default:
        state = "?";
        break;
    }

    if (level == HEADER_LEVEL_FULL) {
        snprintf(buffer, buffer_size, "%s%s", prefix_full, state);
    } else if (level == HEADER_LEVEL_SHORT) {
        snprintf(buffer, buffer_size, "%s%s", prefix_short, state);
    } else {
        snprintf(buffer, buffer_size, "%s", state);
    }
}

/** Tone for a peripheral indicator (state-driven, level-independent). */
static header_tone_t header_status_tone_for(header_status_kind_t type)
{
    switch (type) {
    case HEADER_STATUS_WIFI:
        return header_status_wifi_tone(s_header_state.wifi_connected,
                                       s_header_state.wifi_rssi);
    case HEADER_STATUS_BLUETOOTH:
        return header_status_bluetooth_tone(s_header_state.bluetooth_enabled,
                                            s_header_state.bluetooth_connected);
    case HEADER_STATUS_USB:
        return header_status_usb_tone(s_header_state.usb_connected);
    case HEADER_STATUS_SD:
        return header_status_sd_tone(s_header_state.sd_state);
    case HEADER_STATUS_ACTIVITY:
        return header_activity_active() ? HEADER_TONE_WARN : HEADER_TONE_MUTED;
    default:
        return HEADER_TONE_MUTED;
    }
}

/* ---- Render a single icon by type at the requested level ---- */
static void header_render_icon(header_status_kind_t type, header_level_t level)
{
    char buffer[64];
    lv_obj_t *label;

    if (type < 0 || type >= HEADER_STATUS_PANEL_COUNT) {
        return;
    }
    label = s_status_icons[type];
    if (label == NULL) {
        return;
    }
    header_status_text(type, level, buffer, sizeof(buffer));
    if (buffer[0] == '\0') {
        /* Conditional indicator (activity) with nothing to show. */
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(label, buffer);
    lv_obj_set_style_text_color(label, header_tone_color(header_status_tone_for(type)), 0);
}

/* ---- Indicator tap / long-press detail ---------------------------------- */

/* Registered by command_init(): long-press runs the matching status command. */
static header_status_action_cb_t s_status_action_cb;

/** Show a one-line summary for @p kind in the header's own notification area.
 *  Everything comes from the state the header already caches, so no module is
 *  re-queried and no second status text is duplicated. */
static void header_show_status_detail(header_status_kind_t kind)
{
    char text[P4_CONFIG_HEADER_NOTIFICATION_BYTES];

    switch (kind) {
    case HEADER_STATUS_WIFI:
        if (s_header_state.wifi_connected) {
            snprintf(text, sizeof(text), "WiFi %s  %d dBm",
                     header_status_wifi_label(s_header_state.wifi_rssi),
                     s_header_state.wifi_rssi);
        } else {
            snprintf(text, sizeof(text), "WiFi off");
        }
        break;
    case HEADER_STATUS_BLUETOOTH:
        snprintf(text, sizeof(text), "Bluetooth %s",
                 s_header_state.bluetooth_connected ? "connected"
                 : s_header_state.bluetooth_enabled ? "ready (not connected)"
                                                    : "off");
        break;
    case HEADER_STATUS_USB:
        snprintf(text, sizeof(text), "USB %s",
                 s_header_state.usb_connected ? "device connected" : "not connected");
        break;
    case HEADER_STATUS_SD:
        switch (s_header_state.sd_state) {
        case HEADER_SD_MOUNTED:  snprintf(text, sizeof(text), "SD mounted"); break;
        case HEADER_SD_INSERTED: snprintf(text, sizeof(text), "SD inserted (not mounted)"); break;
        case HEADER_SD_ERROR:    snprintf(text, sizeof(text), "SD error"); break;
        case HEADER_SD_NONE:
        default:                 snprintf(text, sizeof(text), "No SD card"); break;
        }
        break;
    case HEADER_STATUS_ACTIVITY:
        if (s_header_state.c6ota_busy && s_header_state.bg_jobs_running) {
            snprintf(text, sizeof(text), "C6 OTA + background job running");
        } else if (s_header_state.c6ota_busy) {
            snprintf(text, sizeof(text), "C6 OTA update in progress");
        } else {
            snprintf(text, sizeof(text), "Background job running");
        }
        break;
    case HEADER_STATUS_MEM: {
        uint32_t total = s_header_state.total_heap_bytes;
        int pct = (total > 0)
                      ? (int)((s_header_state.free_heap_bytes * 100u) / total) : 0;
        snprintf(text, sizeof(text), "MEM %d%% free", pct);
        break;
    }
    case HEADER_STATUS_CPU:
        snprintf(text, sizeof(text), "CPU %d%%  %" PRIu32 " tasks",
                 s_header_state.cpu_percent, s_header_state.task_count);
        break;
    case HEADER_STATUS_BATTERY:
        if (s_header_state.battery_adc_ready) {
            snprintf(text, sizeof(text), "Battery %d%%", s_header_state.battery_percent);
        } else {
            snprintf(text, sizeof(text), "Battery N/C (ADC not connected)");
        }
        break;
    default:
        return;
    }
    header_set_notification(text, P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS);
}

static void header_status_click_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);
    header_status_kind_t kind =
        (header_status_kind_t)(intptr_t)lv_obj_get_user_data(target);

    /* Stop the long-press from also bubbling to the root build-identity
     * banner: the per-indicator action takes precedence. */
    lv_event_stop_bubbling(event);

    if (code == LV_EVENT_LONG_PRESSED) {
        if (s_status_action_cb != NULL) {
            s_status_action_cb(kind);
        }
        return;
    }
    if (P4_CONFIG_HEADER_DETAIL_ON_TAP) {
        header_show_status_detail(kind);
    }
}

/** Make one indicator tappable/long-pressable. No-op when the feature is off. */
static void header_bind_status_action(lv_obj_t *obj, header_status_kind_t kind)
{
    if (obj == NULL || !P4_CONFIG_HEADER_DETAIL_ON_TAP) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(obj, 6);
    lv_obj_set_user_data(obj, (void *)(intptr_t)kind);
    lv_obj_add_event_cb(obj, header_status_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(obj, header_status_click_cb, LV_EVENT_LONG_PRESSED, NULL);
}

void header_register_status_action(header_status_action_cb_t cb)
{
    s_status_action_cb = cb;
}

/* ---- Notification **bold** subset -> LVGL recolor ----
 * Converts `**text**` (non-space adjacent) to bright-white recolor spans.
 * Also escapes literal `#` (recolor control char) so notifications
 * containing it render instead of swallowing text. Output needs ~2x input;
 * callers size accordingly. Plain text passes through byte-identical. */
static void header_notification_style(const char *in, char *out, size_t out_size)
{
    size_t o = 0;

    if (in == NULL || out == NULL || out_size == 0) {
        return;
    }
    while (*in != '\0' && o + 1 < out_size) {
        if (in[0] == '*' && in[1] == '*' && in[2] != '\0' && in[2] != ' ') {
            const char *close = strstr(in + 2, "**");
            if (close != NULL && close > in + 2 && close[-1] != ' ') {
                static const char open[] = "#FFFFFF";
                size_t inner = (size_t)(close - (in + 2));
                size_t need = sizeof(open) - 1 + inner + 1;
                if (o + need + 1 >= out_size) {
                    break;
                }
                memcpy(out + o, open, sizeof(open) - 1);
                o += sizeof(open) - 1;
                memcpy(out + o, in + 2, inner);
                o += inner;
                out[o++] = '#';
                in = close + 2;
                continue;
            }
        }
        if (*in == '#') {
            if (o + 2 >= out_size) {
                break;
            }
            out[o++] = '#';
            out[o++] = '#';
            in++;
            continue;
        }
        out[o++] = *in++;
    }
    out[o < out_size ? o : out_size - 1] = '\0';
}

/* ---- Format free heap for display (level-aware) ---- */
static void header_format_mem(header_level_t level, char *buf, size_t buf_size)
{
    uint32_t free_heap = s_header_state.free_heap_bytes;
    uint32_t total_heap = s_header_state.total_heap_bytes;
    /* Derive the words-style prefix from the SAME classification the color
     * uses, so the "LOW" label and the amber/red tone can never disagree. */
    const char *prefix =
        header_status_mem_tone(free_heap, total_heap) == HEADER_TONE_OK ? "MEM" : "LOW";
    char num[24];

    if (free_heap >= 1048576) {
        /* Integer-only: float printf pulls in _dtoa_r (see shell_sd_format_size). */
        uint32_t whole = free_heap / 1048576u;
        uint32_t tenth = ((free_heap % 1048576u) * 10u + 524288u) / 1048576u;
        if (tenth >= 10u) {
            whole += 1u;
            tenth = 0u;
        }
        snprintf(num, sizeof(num), "%u.%uM", (unsigned int)whole, (unsigned int)tenth);
    } else if (free_heap >= 1024) {
        snprintf(num, sizeof(num), "%" PRIu32 "K", free_heap / 1024);
    } else {
        snprintf(num, sizeof(num), "%" PRIu32, free_heap);
    }

    if (header_glyph_style()) {
        /* Compact: the glyph carries the identity, the value carries the size.
         * Keep a separator between them so MEM matches the CPU/battery glyph
         * labels, which are separate widgets with a flex gap. */
        snprintf(buf, buf_size, "%s %s", header_status_glyph(HEADER_STATUS_MEM), num);
    } else if (level == HEADER_LEVEL_FULL) {
        snprintf(buf, buf_size, "%s %s", prefix, num);
    } else if (level == HEADER_LEVEL_SHORT) {
        snprintf(buf, buf_size, "%c%s", prefix[0], num);
    } else {
        snprintf(buf, buf_size, "%s", num);
    }
}

/* ---- Format uptime (rendered in the system panel, FULL level only) ---- */
static void header_format_uptime(char *buf, size_t buf_size)
{
    uint32_t s = s_header_state.uptime_seconds;

    if (s >= 86400u) {
        snprintf(buf, buf_size, "UP %" PRIu32 "d%" PRIu32 "h", s / 86400u, (s % 86400u) / 3600u);
    } else if (s >= 3600u) {
        snprintf(buf, buf_size, "UP %" PRIu32 "h%02" PRIu32 "m", s / 3600u, (s % 3600u) / 60u);
    } else if (s >= 60u) {
        snprintf(buf, buf_size, "UP %" PRIu32 "m%02" PRIu32 "s", s / 60u, s % 60u);
    } else {
        snprintf(buf, buf_size, "UP %" PRIu32 "s", s);
    }
}

/* ---- Format CPU for display (label only; the % lives in the value label) ---- */
static void header_format_cpu(header_level_t level, char *buf, size_t buf_size)
{
    if (header_glyph_style()) {
        snprintf(buf, buf_size, "%s", header_status_glyph(HEADER_STATUS_CPU));
    } else if (level == HEADER_LEVEL_FULL) {
        snprintf(buf, buf_size, "CPU");
    } else if (level == HEADER_LEVEL_SHORT) {
        snprintf(buf, buf_size, "C");
    } else {
        buf[0] = '\0';
    }
}

/* ========================================================================
 * RESPONSIVE LAYOUT (measurement + policy + dynamic font)
 * ======================================================================== */

/* Panel geometry (kept in sync between measurement and application). */
#define HEADER_STATUS_GAP   6
#define HEADER_SYS_GAP      5
#define HEADER_PANEL_PAD    6
#define HEADER_CPU_BAR_W    24
#define HEADER_BAT_BAR_W    24

static lv_coord_t s_header_pad_h = 6;

/** Natural width of a text run in @p font (0 for NULL/empty). */
static int header_text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;

    if (text == NULL || text[0] == '\0' || font == NULL) {
        return 0;
    }
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (int)size.x;
}

/** Font for a step: 0 = the UI role font, 1 = the smaller compact fallback. */
static const lv_font_t *header_font_for_step(int step)
{
    return (step <= 0) ? windows_get_ui_font() : &lv_font_montserrat_14;
}

/** Apply a font step to every header label (no-op when unchanged). */
static void header_set_font_step(int step)
{
    const lv_font_t *font;
    int i;

    if (step == s_header_font_step) {
        return;
    }
    s_header_font_step = step;
    font = header_font_for_step(step);
    for (i = 0; i < HEADER_STATUS_PANEL_COUNT; i++) {
        if (s_status_icons[i] != NULL) lv_obj_set_style_text_font(s_status_icons[i], font, 0);
    }
    if (s_notification_icon != NULL) lv_obj_set_style_text_font(s_notification_icon, font, 0);
    if (s_notification_label != NULL) lv_obj_set_style_text_font(s_notification_label, font, 0);
    if (s_mem_label != NULL) lv_obj_set_style_text_font(s_mem_label, font, 0);
    if (s_cpu_label != NULL) lv_obj_set_style_text_font(s_cpu_label, font, 0);
    if (s_cpu_value_label != NULL) lv_obj_set_style_text_font(s_cpu_value_label, font, 0);
    if (s_sep1 != NULL) lv_obj_set_style_text_font(s_sep1, font, 0);
    if (s_sep2 != NULL) lv_obj_set_style_text_font(s_sep2, font, 0);
    if (s_battery_icon_label != NULL) lv_obj_set_style_text_font(s_battery_icon_label, font, 0);
    if (s_battery_value_label != NULL) lv_obj_set_style_text_font(s_battery_value_label, font, 0);
    if (s_uptime_label != NULL) lv_obj_set_style_text_font(s_uptime_label, font, 0);
}

/** Measure the left (status) and right (system) panel widths per level. */
static void header_measure_sides(int status_w[HEADER_LEVEL_COUNT],
                                 int sys_w[HEADER_LEVEL_COUNT],
                                 const lv_font_t *font)
{
    int level;
    int i;

    for (level = 0; level < HEADER_LEVEL_COUNT; level++) {
        int w = 2 * HEADER_PANEL_PAD;
        bool first_visible = true;

        for (i = 0; i < HEADER_STATUS_PANEL_COUNT; i++) {
            char b[64];
            header_status_text((header_status_kind_t)i, (header_level_t)level, b, sizeof(b));
            if (b[0] == '\0') {
                /* Hidden conditional indicator (activity): no width, no gap.
                 * This mirrors LVGL flex, which skips hidden children. */
                continue;
            }
            if (!first_visible) w += HEADER_STATUS_GAP;
            w += header_text_width(b, font);
            first_visible = false;
        }
        status_w[level] = w;
    }

    for (level = 0; level < HEADER_LEVEL_COUNT; level++) {
        char mem[32];
        char cpu[8];
        bool glyph = header_glyph_style();
        int w = 2 * HEADER_PANEL_PAD;

        header_format_mem((header_level_t)level, mem, sizeof(mem));
        header_format_cpu((header_level_t)level, cpu, sizeof(cpu));
        w += header_text_width(mem, font) + HEADER_SYS_GAP;
        if (!glyph && level == HEADER_LEVEL_FULL) {
            w += header_text_width("|", font) + HEADER_SYS_GAP;
        }
        w += header_text_width(cpu, font) + HEADER_SYS_GAP;
        if (!glyph) {
            if (level == HEADER_LEVEL_FULL) {
                w += HEADER_CPU_GRAPH_WIDTH_PX + HEADER_SYS_GAP;
            } else if (level == HEADER_LEVEL_SHORT) {
                w += HEADER_CPU_BAR_W + HEADER_SYS_GAP;
            }
        }
        w += header_text_width("100%", font) + HEADER_SYS_GAP;
        if (glyph) {
            /* Compact: a colored battery glyph, no bar or ASCII block. */
            w += header_text_width(header_status_glyph(HEADER_STATUS_BATTERY), font) +
                 HEADER_SYS_GAP;
        } else {
            if (level == HEADER_LEVEL_FULL) {
                w += header_text_width("|", font) + HEADER_SYS_GAP;
            }
            if (level <= HEADER_LEVEL_SHORT) {
                w += header_text_width(header_battery_text_for_percent(
                                           s_header_state.battery_adc_ready
                                               ? s_header_state.battery_percent : 0),
                                       font) + HEADER_SYS_GAP;
                w += HEADER_BAT_BAR_W + HEADER_SYS_GAP;
            }
        }
        w += header_text_width("100%", font);
        if (!glyph && level == HEADER_LEVEL_FULL) {
            char up[24];
            header_format_uptime(up, sizeof(up));
            w += HEADER_SYS_GAP + header_text_width(up, font);
        }
        sys_w[level] = w;
    }
}

/** Set every label's text/visibility for the resolved levels. */
static void header_apply_levels(const header_layout_t *lay)
{
    char buf[32];
    int i;

    for (i = 0; i < HEADER_STATUS_PANEL_COUNT; i++) {
        header_render_icon((header_status_kind_t)i, lay->status_level);
    }

    /* MEM */
    header_format_mem(lay->sys_level, buf, sizeof(buf));
    if (s_mem_label != NULL) lv_label_set_text(s_mem_label, buf);

    /* CPU label + value */
    header_format_cpu(lay->sys_level, buf, sizeof(buf));
    if (s_cpu_label != NULL) lv_label_set_text(s_cpu_label, buf);
    snprintf(buf, sizeof(buf), "%d%%", s_header_state.cpu_percent);
    if (s_cpu_value_label != NULL) lv_label_set_text(s_cpu_value_label, buf);

    /* The glyph style drops every decoration (separators, graph, bars) and
     * keeps only the compact glyph + value, so the system panel also yields
     * width back to the notification area. */
    bool glyph = header_glyph_style();

    /* Separators */
    if (s_sep1 != NULL) {
        if (!glyph && lay->show_sep) lv_obj_clear_flag(s_sep1, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_sep1, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_sep2 != NULL) {
        if (!glyph && lay->show_sep) lv_obj_clear_flag(s_sep2, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_sep2, LV_OBJ_FLAG_HIDDEN);
    }

    /* CPU indicator: graph (FULL) / bar (SHORT) / none (MIN or glyph). */
    if (s_cpu_graph != NULL) {
        if (!glyph && lay->show_cpu_graph) lv_obj_clear_flag(s_cpu_graph, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_cpu_graph, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_cpu_bar != NULL) {
        bool show_bar = (!glyph && lay->sys_level == HEADER_LEVEL_SHORT);
        if (show_bar) lv_obj_clear_flag(s_cpu_bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_cpu_bar, LV_OBJ_FLAG_HIDDEN);
    }

    /* Battery icon (glyph always; words at FULL/SHORT) + bar (words FULL only). */
    if (s_battery_icon_label != NULL) {
        if (glyph || lay->sys_level <= HEADER_LEVEL_SHORT) {
            lv_obj_clear_flag(s_battery_icon_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_battery_icon_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_battery_bar != NULL) {
        if (!glyph && lay->show_battery_bar) lv_obj_clear_flag(s_battery_bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_battery_bar, LV_OBJ_FLAG_HIDDEN);
    }

    /* Uptime: words FULL level only (the first thing to yield; never in glyph). */
    if (s_uptime_label != NULL) {
        if (!glyph && lay->sys_level == HEADER_LEVEL_FULL) lv_obj_clear_flag(s_uptime_label, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_uptime_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/** Apply exact panel widths + center width from the resolved layout. */
static void header_apply_widths(const header_layout_t *lay, int gap, int avail_w)
{
    int x = 0;

    if (s_status_panel != NULL) {
        if (lay->show_status) {
            lv_obj_clear_flag(s_status_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(s_status_panel, lay->left_w);
            lv_obj_set_pos(s_status_panel, x, 0);
            x += lay->left_w + gap;
        } else {
            lv_obj_add_flag(s_status_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_notif_container != NULL) {
        if (lay->show_center && lay->center_w > 0) {
            int icon_w = header_text_width("!", header_font_for_step(s_header_font_step));
            int label_w;

            lv_obj_clear_flag(s_notif_container, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(s_notif_container, lay->center_w);
            lv_obj_set_pos(s_notif_container, x, 0);
            label_w = lay->center_w - icon_w - gap;
            if (label_w < 0) label_w = 0;
            if (s_notification_label != NULL) {
                lv_obj_set_width(s_notification_label, label_w);
            }
        } else {
            lv_obj_add_flag(s_notif_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_sys_panel != NULL) {
        if (lay->show_sys) {
            /* Pin the system panel to the right edge of the content area. */
            int right_x = avail_w - lay->right_w;

            if (right_x < x) right_x = x; /* never overlap the center */
            lv_obj_clear_flag(s_sys_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(s_sys_panel, lay->right_w);
            lv_obj_set_pos(s_sys_panel, right_x, 0);
        } else {
            lv_obj_add_flag(s_sys_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/** Recompute and apply the responsive header layout for the current state. */
static void header_apply_layout(void)
{
    lv_display_t *display = lv_display_get_default();
    int screen_w = (display != NULL)
                       ? (int)lv_display_get_horizontal_resolution(display)
                       : 1024;
    /* Measure against the root's REAL content area (screen minus the window
     * margin and the header's own padding), not the raw panel width; using the
     * latter let the panels overlap when the window margin was non-zero. */
    int avail_w = (s_header_root != NULL) ? (int)lv_obj_get_content_width(s_header_root)
                                          : 0;
    int status_w[HEADER_LEVEL_COUNT];
    int sys_w[HEADER_LEVEL_COUNT];
    const lv_font_t *font;
    header_layout_t lay;
    int center_min = P4_CONFIG_HEADER_CENTER_MIN_PX;
    const int gap = 6;
    bool allow_font = P4_CONFIG_HEADER_DYNAMIC_FONT;

    if (avail_w < 0) avail_w = 0;
    if (avail_w == 0) {
        /* Not laid out yet: fall back to the display width minus our pads. */
        avail_w = screen_w - 2 * (int)s_header_pad_h;
        if (avail_w < 0) avail_w = 0;
    }

    /* Step 0 (primary UI font) first so the layout auto-restores when room
     * returns. If even the compact layout cannot fit and dynamic fonts are
     * enabled, step down once and re-run the policy. */
    font = header_font_for_step(0);
    header_measure_sides(status_w, sys_w, font);
    if (s_header_mode == HEADER_MODE_COMPACT) {
        /* Collapse FULL to SHORT so the policy can never pick the full labels. */
        status_w[HEADER_LEVEL_FULL] = status_w[HEADER_LEVEL_SHORT];
        sys_w[HEADER_LEVEL_FULL] = sys_w[HEADER_LEVEL_SHORT];
    }
    header_layout_compute(&lay, avail_w, gap, status_w, sys_w, center_min);

    if (lay.smaller_font && allow_font) {
        font = header_font_for_step(1);
        header_measure_sides(status_w, sys_w, font);
        if (s_header_mode == HEADER_MODE_COMPACT) {
            status_w[HEADER_LEVEL_FULL] = status_w[HEADER_LEVEL_SHORT];
            sys_w[HEADER_LEVEL_FULL] = sys_w[HEADER_LEVEL_SHORT];
        }
        header_layout_compute(&lay, avail_w, gap, status_w, sys_w, center_min);
        header_set_font_step(1);
    } else {
        header_set_font_step(0);
    }

    /* COMPACT: the FULL widths were collapsed to SHORT before the policy, so a
     * returned FULL level means "the SHORT width fit" -- render SHORT text. */
    if (s_header_mode == HEADER_MODE_COMPACT) {
        if (lay.status_level == HEADER_LEVEL_FULL) {
            lay.status_level = HEADER_LEVEL_SHORT;
        }
        if (lay.sys_level == HEADER_LEVEL_FULL) {
            lay.sys_level = HEADER_LEVEL_SHORT;
        }
        lay.show_sep = (lay.sys_level == HEADER_LEVEL_FULL);
        lay.show_cpu_graph = (lay.sys_level == HEADER_LEVEL_FULL);
        lay.show_battery_bar = (lay.sys_level <= HEADER_LEVEL_SHORT);
    }

    header_apply_levels(&lay);
    header_apply_widths(&lay, gap, avail_w);

    /* Record the resolved policy (actual widget widths are captured after the
     * layout settles, in header_record_actual()). */
    s_last_layout = lay;
    s_last_screen_w = screen_w;
}

/** Capture the settled widget widths for `header status` (after layout). */
static void header_record_actual(void)
{
    s_last_actual_left = (s_status_panel != NULL &&
                          !lv_obj_has_flag(s_status_panel, LV_OBJ_FLAG_HIDDEN))
                             ? (int)lv_obj_get_width(s_status_panel) : 0;
    s_last_actual_center = (s_notif_container != NULL &&
                            !lv_obj_has_flag(s_notif_container, LV_OBJ_FLAG_HIDDEN) &&
                            s_last_layout.show_center)
                               ? (int)lv_obj_get_width(s_notif_container) : 0;
    s_last_actual_right = (s_sys_panel != NULL &&
                           !lv_obj_has_flag(s_sys_panel, LV_OBJ_FLAG_HIDDEN))
                              ? (int)lv_obj_get_width(s_sys_panel) : 0;
}


/* ---- Main render: state -> colors/text, then the responsive layout pass ---- */
static void header_render(void)
{
    lv_color_t battery_color;
    lv_color_t cpu_color;
    char buf[32];

    if (s_header_root == NULL) {
        return;
    }
    /* Header rendering touches LVGL objects directly. Some callers run on the
     * LVGL task, others (fallback paths, the periodic timer, `header` verb)
     * do not; take the recursive port lock so every call serialises with the
     * LVGL render cycle. Release it on EVERY exit path. */
    if (!lvgl_port_lock(0)) {
        return;
    }
    if (s_header_root == NULL) {
        lvgl_port_unlock();
        return;
    }

    /* Notification (queue-driven). Severity sets the base color; the label
     * keeps recolor enabled so the `**bold**` subset still renders bright.
     * When nothing is displayed, the idle center shows the local clock. */
    if (s_header_state.notification_active && s_header_state.notification[0] != '\0') {
        char *styled = malloc(HEADER_NOTIFICATION_BYTES * 2);
        if (styled != NULL) {
            header_notification_style(s_header_state.notification,
                                      styled, HEADER_NOTIFICATION_BYTES * 2);
            lv_label_set_text(s_notification_label, styled);
            free(styled);
        } else {
            lv_label_set_text(s_notification_label, s_header_state.notification);
        }
        lv_obj_set_style_text_color(
            s_notification_label,
            header_notify_color(s_header_state.notification_level), 0);
        lv_obj_set_style_text_color(
            s_notification_icon,
            header_notify_color(s_header_state.notification_level), 0);
        lv_obj_clear_flag(s_notification_icon, LV_OBJ_FLAG_HIDDEN);
    } else if (P4_CONFIG_HEADER_CLOCK && s_header_state.clock_text[0] != '\0') {
        lv_label_set_text(s_notification_label, s_header_state.clock_text);
        lv_obj_set_style_text_color(s_notification_label,
                                    lv_color_hex(HEADER_MUTED_COLOR), 0);
        lv_obj_add_flag(s_notification_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_notification_label, "");
        lv_obj_add_flag(s_notification_icon, LV_OBJ_FLAG_HIDDEN);
    }

    /* Battery (always present; N/C when the ADC is unavailable). The color
     * comes from the shared tone classification, so the words and glyph
     * styles can never disagree about low/critical. */
    battery_color = header_tone_color(
        header_status_battery_tone(s_header_state.battery_adc_ready
                                       ? s_header_state.battery_percent : 0,
                                   s_header_state.battery_adc_ready));
    lv_label_set_text(s_battery_icon_label,
                      header_glyph_style()
                          ? header_status_glyph(HEADER_STATUS_BATTERY)
                          : (s_header_state.battery_adc_ready
                                 ? header_battery_text_for_percent(s_header_state.battery_percent)
                                 : "[    ]"));
    lv_obj_set_style_text_color(s_battery_icon_label, battery_color, 0);
    lv_bar_set_value(s_battery_bar,
                     s_header_state.battery_adc_ready ? s_header_state.battery_percent : 0,
                     LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_battery_bar, battery_color, LV_PART_INDICATOR);
    if (s_header_state.battery_adc_ready) {
        snprintf(buf, sizeof(buf), "%d%%", s_header_state.battery_percent);
    } else {
        snprintf(buf, sizeof(buf), "%s", "N/C");
    }
    lv_label_set_text(s_battery_value_label, buf);
    lv_obj_set_style_text_color(s_battery_value_label, battery_color, 0);

    /* Memory / CPU label colors (text is set by the layout pass; color from
     * the shared tone classification). */
    lv_obj_set_style_text_color(s_mem_label,
        header_tone_color(header_status_mem_tone(s_header_state.free_heap_bytes,
                                                 s_header_state.total_heap_bytes)), 0);
    cpu_color = header_tone_color(header_status_cpu_tone(s_header_state.cpu_percent));
    lv_obj_set_style_text_color(s_cpu_label, cpu_color, 0);
    lv_obj_set_style_text_color(s_cpu_value_label, cpu_color, 0);

    if (HEADER_CPU_GRAPH && s_cpu_graph != NULL) {
        lv_chart_set_next_value(s_cpu_graph, s_cpu_graph_series, s_header_state.cpu_percent);
        if (s_cpu_graph_series != NULL) {
            lv_chart_set_series_color(s_cpu_graph, s_cpu_graph_series, cpu_color);
        }
    } else if (s_cpu_bar != NULL) {
        lv_bar_set_value(s_cpu_bar, s_header_state.cpu_percent, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cpu_bar, cpu_color, LV_PART_INDICATOR);
    }

    /* Uptime (visibility handled by the layout pass). */
    header_format_uptime(buf, sizeof(buf));
    lv_label_set_text(s_uptime_label, buf);
    lv_obj_set_style_text_color(s_uptime_label, lv_color_hex(HEADER_MUTED_COLOR), 0);

    /* Resolve the root's real content width before measuring. */
    lv_obj_update_layout(s_header_root);

    /* Responsive pass: level texts, exact panel widths, dynamic font. */
    header_apply_layout();

    lv_obj_update_layout(s_header_root);
    header_record_actual();
    lvgl_port_unlock();
}

/* ---- Async dispatch helper ----
 * Ownership: the caller allocates the payload and owns it on failure (it frees
 * it and re-renders); on success the async callback owns and frees it. This
 * helper must NOT free the payload on failure: doing so double-frees it at
 * every caller and corrupts the heap free list (the corruption then surfaces
 * as an LVGL timer-list crash). */
static bool header_schedule(lv_async_cb_t cb, void *payload)
{
    /* No header surface yet (unit tests, very early boot, or between a
     * header_deinit() and the next header_init()): never touch LVGL. The
     * caller's fallback header_render() is a safe no-op in that state.
     * Calling lv_async_call() before LVGL is initialized corrupts the TLSF
     * pool (the unit-test app crashed here). */
    if (s_header_root == NULL) {
        return false;
    }

    /* lv_async_call() creates a one-shot LVGL timer, mutating the global timer
     * list that the LVGL task walks in lv_timer_handler(). With
     * CONFIG_LV_OS_NONE the lv_lock() inside lv_async_call() is a no-op, so
     * the esp_lvgl_port mutex is the only serialisation point; a foreign-task
     * call made without it can interleave with the timer walk and leave a
     * freed timer node linked, which later faults in lv_timer_exec (the
     * 0xcececece Core 0 panic). Every caller of header_schedule() is either
     * the worker task or an event task, so take the (recursive) port lock for
     * the duration of the call. */
    bool ok = false;

    if (lvgl_port_lock(portMAX_DELAY)) {
        ok = lv_async_call(cb, payload) == LV_RESULT_OK;
        lvgl_port_unlock();
    }
    return ok;
}

/* ---- Async callbacks ---- */
static void header_async_refresh(void *user_data)     { (void)user_data; header_render(); }
static void header_async_notification(void *user_data) {
    header_notification_update_t *u = (header_notification_update_t *)user_data;
    if (u != NULL) {
        header_notify_apply(u);
        free(u);
    }
    header_render();
}
static void header_async_wifi(void *u_data)       { free(u_data); header_render(); }
static void header_async_battery(void *u_data)    { free(u_data); header_render(); }
static void header_async_bluetooth(void *u_data)  { free(u_data); header_render(); }
static void header_async_usb(void *u_data)        { free(u_data); header_render(); }
static void header_async_sd(void *u_data)         { free(u_data); header_render(); }
static void header_async_mem(void *u_data)        { free(u_data); header_render(); }
static void header_async_cpu(void *u_data)        { free(u_data); header_render(); }
static void header_async_uptime(void *u_data)     { free(u_data); header_render(); }
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
    /* Chained UI font (terminal + icon fallback): status text and the
     * battery/notify icons share one font. */
    const lv_font_t *status_font = windows_get_ui_font();
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
    s_header_pad_h = horz_pad;

    /* ====================================================================
     * HEADER ROOT: full-width flex row, items centered vertically
     * Tight padding to maximize space for content panels.
     * ==================================================================== */
    s_header_root = lv_obj_create(screen);
    lv_obj_set_width(s_header_root, LV_PCT(100));
    lv_obj_set_height(s_header_root, s_header_height);
    /* Panels are positioned absolutely by header_apply_widths() using the
     * widths the layout policy resolved, so the render is deterministic and
     * independent of flex timing. */
    lv_obj_set_layout(s_header_root, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_left(s_header_root, horz_pad, 0);
    lv_obj_set_style_pad_right(s_header_root, horz_pad, 0);
    lv_obj_set_style_pad_top(s_header_root, vert_pad, 0);
    lv_obj_set_style_pad_bottom(s_header_root, vert_pad, 0);
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
    lv_obj_set_height(s_status_panel, LV_PCT(100));
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
    /* Width is set explicitly each render by the responsive layout pass
     * (header_apply_layout); no fixed min-width to clip against. */

    /* Create icon labels inside status panel — compact letter spacing */
    for (i = 0; i < HEADER_STATUS_PANEL_COUNT; i++) {
        s_status_icons[i] = lv_label_create(s_status_panel);
        lv_obj_set_style_text_font(s_status_icons[i], status_font, 0);
        lv_obj_set_style_text_letter_space(s_status_icons[i], -1, 0);
        lv_obj_clear_flag(s_status_icons[i], LV_OBJ_FLAG_SCROLLABLE);
        header_bind_status_action(s_status_icons[i], (header_status_kind_t)i);
    }

    /* ====================================================================
     * CENTER: NOTIFICATION AREA — icon + bounded scrolling text
     * Width is assigned by the responsive layout pass; the label is pinned
     * to the container width so LV_LABEL_LONG_SCROLL_CIRCULAR actually
     * scrolls within its box and can never overlap the side panels.
     * ==================================================================== */
    s_notif_container = lv_obj_create(s_header_root);
    lv_obj_set_height(s_notif_container, LV_PCT(100));
    lv_obj_set_style_bg_opa(s_notif_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_notif_container, 0, 0);
    lv_obj_set_style_pad_all(s_notif_container, 0, 0);
    lv_obj_set_style_flex_grow(s_notif_container, 0, 0);
    lv_obj_set_layout(s_notif_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_notif_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_notif_container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_notif_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_notif_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Notification icon (bell text, hidden when no notification) */
    s_notification_icon = lv_label_create(s_notif_container);
    lv_label_set_text(s_notification_icon, "!");
    lv_obj_set_style_text_color(s_notification_icon, lv_color_hex(HEADER_WARN_COLOR), 0);
    lv_obj_set_style_text_font(s_notification_icon, status_font, 0);
    lv_obj_add_flag(s_notification_icon, LV_OBJ_FLAG_HIDDEN);

    /* Notification text — width is bounded by the layout pass each render. */
    s_notification_label = lv_label_create(s_notif_container);
    lv_obj_set_width(s_notification_label, LV_PCT(100));
    lv_label_set_long_mode(s_notification_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_notification_label, lv_color_hex(HEADER_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(s_notification_label, status_font, 0);
    /* Recolor markup for the **bold** notification subset (see below). */
    lv_label_set_recolor(s_notification_label, true);
    lv_obj_set_scrollbar_mode(s_notification_label, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_notification_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(s_notification_label, "");

    /* ====================================================================
     * RIGHT: SYSTEM PANEL — MEM | CPU | BAT (pinned to far right)
     * Compact layout: tight gaps, smaller bars, generous min-width.
     * ==================================================================== */
    s_sys_panel = lv_obj_create(s_header_root);
    lv_obj_set_height(s_sys_panel, LV_PCT(100));
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
    /* Width is set explicitly each render by the responsive layout pass. */

    /* ---- MEM label ---- */
    s_mem_label = lv_label_create(s_sys_panel);
    lv_obj_set_style_text_font(s_mem_label, status_font, 0);
    lv_obj_set_style_text_letter_space(s_mem_label, -1, 0);
    lv_obj_set_style_text_color(s_mem_label, lv_color_hex(HEADER_TEXT_COLOR), 0);
    header_bind_status_action(s_mem_label, HEADER_STATUS_MEM);

    /* Separator MEM|CPU */
    s_sep1 = lv_label_create(s_sys_panel);
    lv_label_set_text(s_sep1, "|");
    lv_obj_set_style_text_color(s_sep1, lv_color_hex(HEADER_MUTED_COLOR), 0);

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
    header_bind_status_action(s_cpu_value_label, HEADER_STATUS_CPU);

    /* Separator CPU|BAT */
    s_sep2 = lv_label_create(s_sys_panel);
    lv_label_set_text(s_sep2, "|");
    lv_obj_set_style_text_color(s_sep2, lv_color_hex(HEADER_MUTED_COLOR), 0);

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
    header_bind_status_action(s_battery_value_label, HEADER_STATUS_BATTERY);

    /* Uptime label (shown only in the FULL layout level). */
    s_uptime_label = lv_label_create(s_sys_panel);
    lv_obj_set_style_text_font(s_uptime_label, status_font, 0);
    lv_obj_set_style_text_letter_space(s_uptime_label, -1, 0);
    lv_obj_set_style_text_color(s_uptime_label, lv_color_hex(HEADER_MUTED_COLOR), 0);

    /* Any long-press anywhere on the status bar bubbles to the root handler. */
    header_enable_event_bubble(s_header_root);

    /* Persistent notification-display timer (paused until a message shows).
     * Persistent by design: because it is never auto-freed, the O7 hazard of
     * touching a timer node that lv_timer_exec already deleted cannot occur. */
    s_notification_timer = lv_timer_create(header_notification_timeout_cb,
                                           P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS, NULL);
    if (s_notification_timer != NULL) {
        lv_timer_set_repeat_count(s_notification_timer, -1);
        lv_timer_pause(s_notification_timer);
        /* A notification queued before the UI existed is already displayed. */
        if (s_header_state.notification_active) {
            header_notify_arm(s_header_state.notification_timeout_ms);
        }
    }

    /* Resolve the root's geometry BEFORE the first render so the responsive
     * layout never runs against a zero content width (which could place a
     * panel at a transient position and leave stale pixels there). */
    lv_obj_update_layout(s_header_root);

    /* Initial render */
    header_render();
}

/** Re-resolve label fonts after a `font set/size` switch. Styles snapshot
 * the chain pointer at creation, so without this the header renders the
 * orphaned copy until recreated. Port lock is recursive. */
void header_refresh_fonts(void)
{
    if (s_header_root == NULL) {
        return;
    }
    /* Invalidate the cached step so header_set_font_step() re-applies the
     * (possibly new) UI font, then re-measure/re-layout with the new metrics. */
    s_header_font_step = -1;
    header_render();
}

/** Re-apply the active theme to the header: bar/panel backgrounds here, then
 * a full re-render for the text/accent/muted/warn labels. Port lock is
 * recursive; callers are the worker task (via `theme set`). */
void header_refresh_theme(void)
{
    if (s_header_root == NULL) {
        return;
    }
    if (!lvgl_port_lock(0)) {
        return;
    }
    lv_obj_set_style_bg_color(s_header_root, lv_color_hex(HEADER_BG_COLOR), 0);
    if (s_status_panel != NULL) {
        lv_obj_set_style_bg_color(s_status_panel, lv_color_hex(HEADER_PANEL_BG), 0);
    }
    if (s_sys_panel != NULL) {
        lv_obj_set_style_bg_color(s_sys_panel, lv_color_hex(HEADER_SYS_PANEL_BG), 0);
    }
    lvgl_port_unlock();
    header_render();
}

void header_update_status(void)
{
    (void)header_schedule(header_async_refresh, NULL);
}

void header_notify(header_notify_level_t level, const char *text, uint32_t timeout_ms)
{
    header_notification_update_t *update = calloc(1, sizeof(*update));
    if (update == NULL) return;
    snprintf(update->text, sizeof(update->text), "%s", text != NULL ? text : "");
    update->timeout_ms = timeout_ms;
    update->level = level;
    if (!header_schedule(header_async_notification, update)) {
        /* Fall back to a synchronous apply under the LVGL lock, then render. */
        if (lvgl_port_lock(0)) {
            header_notify_apply(update);
            lvgl_port_unlock();
            header_render();
        }
        free(update);
    }
}

void header_set_notification(const char *text, uint32_t timeout_ms)
{
    header_notify(HEADER_NOTIFY_INFO, text, timeout_ms);
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

/* ========================================================================
 * LAYOUT MODE
 * ======================================================================== */

const char *header_mode_name(header_mode_t mode)
{
    switch (mode) {
    case HEADER_MODE_FULL:    return "full";
    case HEADER_MODE_COMPACT: return "compact";
    case HEADER_MODE_AUTO:
    default:                  return "auto";
    }
}

bool header_mode_parse(const char *text, header_mode_t *out)
{
    header_mode_t mode;

    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcasecmp(text, "auto") == 0) {
        mode = HEADER_MODE_AUTO;
    } else if (strcasecmp(text, "full") == 0) {
        mode = HEADER_MODE_FULL;
    } else if (strcasecmp(text, "compact") == 0) {
        mode = HEADER_MODE_COMPACT;
    } else {
        return false;
    }
    *out = mode;
    return true;
}

void header_set_mode(header_mode_t mode)
{
    if (mode < 0 || mode >= HEADER_MODE_COUNT) {
        return;
    }
    s_header_mode = mode;
    /* Force a re-apply (the step cache is only used when the font is the
     * same; the levels change with the mode). */
    header_render();
}

header_mode_t header_get_mode(void)
{
    return s_header_mode;
}

void header_relayout(void)
{
    (void)header_schedule(header_async_refresh, NULL);
}

void header_get_metrics(header_metrics_t *out)
{
    if (out == NULL) {
        return;
    }
    out->status_level = (int)s_last_layout.status_level;
    out->sys_level = (int)s_last_layout.sys_level;
    out->show_center = s_last_layout.show_center;
    out->font_step = s_header_font_step < 0 ? 0 : s_header_font_step;
    out->left_w = s_last_layout.left_w;
    out->center_w = s_last_layout.center_w;
    out->right_w = s_last_layout.right_w;
    out->actual_left = s_last_actual_left;
    out->actual_center = s_last_actual_center;
    out->actual_right = s_last_actual_right;
    out->screen_w = s_last_screen_w;
    out->glyph_style = header_glyph_style();
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

void header_update_batch(const header_batch_t *in)
{
    if (in == NULL) {
        return;
    }

    /* Set all state directly (atomic on this platform) then schedule
     * a single async render. No dynamic allocation needed — avoids
     * heap corruption from deferred async call payloads. clock_text is
     * copied; the caller's buffer may be transient. */
    s_header_state.wifi_connected = in->wifi_connected;
    s_header_state.wifi_rssi = in->wifi_rssi;
    s_header_state.battery_percent =
        (in->battery_percent < 0) ? 0 : (in->battery_percent > 100) ? 100 : in->battery_percent;
    s_header_state.battery_adc_ready = in->battery_adc_ready;
    s_header_state.bluetooth_enabled = in->bt_enabled;
    s_header_state.bluetooth_connected = in->bt_connected;
    s_header_state.usb_connected = in->usb_connected;
    s_header_state.sd_state = in->sd_state;
    s_header_state.free_heap_bytes = in->free_heap;
    s_header_state.total_heap_bytes = in->total_heap;
    s_header_state.cpu_percent =
        (in->cpu_percent < 0) ? 0 : (in->cpu_percent > 100) ? 100 : in->cpu_percent;
    s_header_state.task_count = in->task_count;
    s_header_state.uptime_seconds = in->uptime_seconds;
    snprintf(s_header_state.clock_text, sizeof(s_header_state.clock_text), "%s",
             in->clock_text != NULL ? in->clock_text : "");
    s_header_state.c6ota_busy = in->c6ota_busy;
    s_header_state.bg_jobs_running = in->bg_jobs_running;

    /* Schedule a single async render (no payload needed) */
    (void)header_schedule(header_async_refresh, NULL);
}

void header_set_visible(bool visible)
{
    s_header_visible = visible;
    if (s_header_root == NULL) {
        return;
    }
    /* Called from the command worker (config/header verbs) and the boot path,
     * not only the LVGL task; hold the recursive port lock while mutating the
     * widget so the change serialises with the render cycle. */
    if (!lvgl_port_lock(0)) {
        return;
    }
    if (s_header_root != NULL) {
        if (visible) {
            lv_obj_clear_flag(s_header_root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_header_root, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
}

bool header_get_visible(void)
{
    return s_header_visible;
}

void header_deinit(void)
{
    /* The notification-display timer is an LVGL timer, not a widget: it is
     * NOT reclaimed by lv_obj_clean(). Delete it here (this runs on the LVGL
     * task under the port lock) so it cannot fire after teardown; header_init()
     * recreates it. */
    if (s_notification_timer != NULL) {
        lv_timer_delete(s_notification_timer);
        s_notification_timer = NULL;
    }

    /* Clear all widget handles — the screen will be cleaned by shell_build_ui
     * via lv_obj_clean(), which deletes all children including our widgets.
     * Reset state to defaults for fresh init. */
    s_header_root = NULL;
    s_notification_label = NULL;
    s_notification_icon = NULL;
    s_notif_container = NULL;
    s_status_panel = NULL;
    s_sys_panel = NULL;
    s_sep1 = NULL;
    s_sep2 = NULL;
    s_battery_icon_label = NULL;
    s_battery_bar = NULL;
    s_battery_value_label = NULL;
    s_mem_label = NULL;
    s_cpu_label = NULL;
    s_cpu_bar = NULL;
    s_cpu_graph = NULL;
    s_cpu_graph_series = NULL;
    s_cpu_value_label = NULL;
    s_uptime_label = NULL;
    s_header_font_step = 0;
    for (int i = 0; i < HEADER_STATUS_PANEL_COUNT; i++) {
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
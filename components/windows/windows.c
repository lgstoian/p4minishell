/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file windows.c
 * @brief Window manager implementation for P4MiniShell LVGL shell UI.
 *
 * Owns the LVGL screen layout: header, transcript, input row, and keyboard.
 * All dimensions are resolution-aware and rotation-aware, computed dynamically
 * from the display manager's current resolution. Works together with display.c
 * for resolution queries and header.c for the top status bar.
 *
 * Thread safety:
 *   - All LVGL object creation/destruction must happen on the LVGL task.
 *   - Public accessors return raw LVGL object pointers (caller must use
 *     from LVGL task context or via lv_async_call).
 *   - Dimension queries read from display.c which is thread-safe.
 */

#include "windows.h"
#include "display.h"
#include "font.h"
#include "theme.h"
#include "header.h"
#include "keyboard.h"
#include "ansi.h"

extern void tui_hide_for_modal(void);
extern void tui_show_after_modal(void);
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p4heap.h"
#include "board_config.h"
#include "p4minishell_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Backward-compatibility aliases */
#define WINDOWS_TAG                     P4_CONFIG_SHELL_TAG
#define WINDOWS_KEYBOARD_HEIGHT         P4_CONFIG_KEYBOARD_HEIGHT
#define WINDOWS_INPUT_ROW_HEIGHT        P4_CONFIG_INPUT_ROW_HEIGHT

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

/** Window manager internal state. All LVGL objects owned here. */
static struct {
    bool initialized;
    lv_obj_t *screen;
    lv_obj_t *transcript;
    lv_obj_t *transcript_spans;
    lv_obj_t *input_row;
    lv_obj_t *input_line;
    lv_obj_t *prev_button;
    lv_obj_t *prev_label;
    lv_obj_t *next_button;
    lv_obj_t *next_label;
    lv_obj_t *scroll_up_button;
    lv_obj_t *up_label;
    lv_obj_t *scroll_down_button;
    lv_obj_t *down_label;
    lv_obj_t *tab_button;
    lv_obj_t *tab_label;
    lv_obj_t *stop_button;
    lv_obj_t *stop_label;
    bool stop_wanted;         /* Last windows_set_stop_visible() request. */
    lv_obj_t *input_ghost;      /* Inline completion ghost (child of input_line) */
    lv_obj_t *search_label;     /* Reverse-history search query (screen child) */
    /* Editor mode: the transcript region hosts a modal editor surface. */
    bool editor_mode;
    lv_obj_t *editor_surface;
    lv_obj_t *editor_status;
    /* App mode: the shell input widgets are hidden for a full-screen app
     * surface (kept separate from editor mode; both hide the input row). */
    bool app_mode;
    /* TUI mode: like editor mode but for the TUI cell buffer. */
    bool tui_mode;
    lv_obj_t *tui_surface;
    /* App surface: a foreground app (TUI cell buffer or gfx canvas) owns the
     * transcript region. The OSK is auto-hidden (and restored on exit), the
     * transcript follow-to-bottom is suppressed, and the container is pinned
     * to the surface origin so the app is visible immediately instead of being
     * dragged off-screen by the scrollback (V1 class). */
    bool app_surface_active;
    bool app_surface_kb_was_visible;
    lv_coord_t app_surface_saved_pad;
    void (*app_surface_layout_cb)(void);
    bool fullscreen;
    /* Shadow of the transcript span group's HIDDEN flag (maintained by
     * windows_shell_spans_set_hidden) so non-LVGL tasks can query it without
     * touching the LVGL object (bugs.md F26 worker/render decoupling). */
    bool shell_spans_hidden;
    /* Windowed-transcript objects (bugs.md F26): a full-height invisible
     * spacer that establishes the container's scroll range from the row model,
     * and a hidden scratch spangroup used to measure per-row wrapped heights.
     * Both are children of the transcript container. */
    lv_obj_t *transcript_spacer;
    lv_obj_t *transcript_measure;
} s_windows = {
    .initialized = false,
    .screen = NULL,
    .transcript = NULL,
    .transcript_spans = NULL,
    .input_row = NULL,
    .input_line = NULL,
    .prev_button = NULL,
    .prev_label = NULL,
    .next_button = NULL,
    .next_label = NULL,
    .scroll_up_button = NULL,
    .up_label = NULL,
    .scroll_down_button = NULL,
    .down_label = NULL,
    .tab_button = NULL,
    .tab_label = NULL,
    .stop_button = NULL,
    .stop_label = NULL,
    .stop_wanted = false,
    .input_ghost = NULL,
    .search_label = NULL,
    .editor_mode = false,
    .editor_surface = NULL,
    .editor_status = NULL,
    .app_mode = false,
    .tui_mode = false,
    .tui_surface = NULL,
    .app_surface_active = false,
    .app_surface_kb_was_visible = false,
    .app_surface_saved_pad = 0,
    .app_surface_layout_cb = NULL,
    .fullscreen = false,
    .shell_spans_hidden = false,
    .transcript_spacer = NULL,
    .transcript_measure = NULL,
};

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static void windows_create_header(void);
static void windows_create_transcript(void);
static void windows_create_input_row(void);
static void windows_apply_screen_style(void);

/* ========================================================================
 * SCALING HELPERS
 * ======================================================================== */

lv_coord_t windows_get_display_width(void)
{
    return (lv_coord_t)display_get_width();
}

lv_coord_t windows_get_display_height(void)
{
    return (lv_coord_t)display_get_height();
}

lv_coord_t windows_scale_height_percent(int percent, lv_coord_t min_h, lv_coord_t max_h)
{
    lv_coord_t h = (lv_coord_t)(windows_get_display_height() * percent / 100);
    if (h < min_h) h = min_h;
    if (h > max_h) h = max_h;
    return h;
}

lv_coord_t windows_scale_width_percent(int percent, lv_coord_t min_w, lv_coord_t max_w)
{
    lv_coord_t w = (lv_coord_t)(windows_get_display_width() * percent / 100);
    if (w < min_w) w = min_w;
    if (w > max_w) w = max_w;
    return w;
}

window_rect_t windows_get_rect(window_region_t region)
{
    window_rect_t rect = {0};
    lv_coord_t disp_w = windows_get_display_width();
    lv_coord_t disp_h = windows_get_display_height();
    lv_coord_t header_h = 0;

    if (header_get_visible()) {
        /* Single source of truth: the header owns its height for the live
         * resolution/rotation, so the reserved region can never disagree
         * (which previously caused a gap or an overlap on some boards). */
        header_h = header_get_height();
    }
    lv_coord_t input_h = windows_scale_height_percent(P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_PCT,
                                                       P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_MIN,
                                                       P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_MAX);
    /* Keyboard height depends on visibility - 0 when hidden */
    lv_coord_t kb_h = keyboard_is_visible() ? keyboard_get_height() : 0;

    switch (region) {
    case WINDOW_REGION_HEADER:
        rect.width = disp_w;
        rect.height = header_h;
        break;
    case WINDOW_REGION_TRANSCRIPT:
        rect.y = header_h;
        rect.width = disp_w;
        /* Transcript fills remaining space between header and input row.
         * When keyboard is hidden, transcript expands to use that space. */
        rect.height = disp_h - header_h - input_h - kb_h;
        if (rect.height < P4_CONFIG_WINDOW_TRANSCRIPT_HEIGHT_MIN) {
            rect.height = P4_CONFIG_WINDOW_TRANSCRIPT_HEIGHT_MIN;
        }
        break;
    case WINDOW_REGION_INPUT_ROW:
        rect.y = disp_h - input_h - kb_h;
        rect.width = disp_w;
        rect.height = input_h;
        break;
    case WINDOW_REGION_KEYBOARD:
        rect.y = disp_h - kb_h;
        rect.width = disp_w;
        rect.height = kb_h;
        break;
    default:
        break;
    }

    return rect;
}

/* ========================================================================
 * COLOR & STYLE HELPERS
 * ======================================================================== */

lv_color_t windows_get_color(const char *name)
{
    if (name == NULL) {
        return lv_color_hex(0x000000);
    }

    if (strcmp(name, WINDOWS_COLOR_BG_SCREEN) == 0) {
        return lv_color_hex(theme_current()->bg_screen);
    }
    if (strcmp(name, WINDOWS_COLOR_BG_TRANSCRIPT) == 0) {
        return lv_color_hex(theme_current()->bg_transcript);
    }
    if (strcmp(name, WINDOWS_COLOR_BG_INPUT_ROW) == 0) {
        return lv_color_hex(theme_current()->bg_input_row);
    }
    if (strcmp(name, WINDOWS_COLOR_BG_KEYBOARD) == 0) {
        return lv_color_hex(theme_current()->bg_keyboard);
    }
    if (strcmp(name, WINDOWS_COLOR_TEXT) == 0) {
        return lv_color_hex(theme_current()->text);
    }
    if (strcmp(name, WINDOWS_COLOR_TEXT_MUTED) == 0) {
        return lv_color_hex(theme_current()->text_muted);
    }

    return lv_color_hex(0x000000);
}

const lv_font_t *windows_get_terminal_font(void)
{
    /* Delegates to the font registry (Phase 1): `font set terminal <name>`
     * takes effect through here. The registry default is the in-tree
     * extended unscii_16, always compiled (see the stale-sdkconfig lesson
     * in the original comment, kept in git history). */
    return font_get(FONT_ROLE_TERMINAL);
}

const lv_font_t *windows_get_ui_font(void)
{
    return font_get(FONT_ROLE_UI);
}

const lv_font_t *windows_get_reading_font(void)
{
    /* Proportional reading face for the viewer and the editor markdown
     * preview. Defaults to the UI font when no serif TTF is configured;
     * never used for cell-metric surfaces (transcript/TUI/editor). */
    return font_get(FONT_ROLE_READING);
}

/** Re-resolve owned widget fonts after a `font set/size` switch. Styles
 * snapshot the chain pointer at creation, so without this everything
 * created before the switch renders the orphaned copy. No-op before init.
 * Takes the port lock (callers are the worker task). */
void windows_refresh_fonts(void)
{
    const lv_font_t *term;
    const lv_font_t *ui;

    if (!s_windows.initialized) {
        return;
    }
    if (!lvgl_port_lock(0)) {
        return;
    }
    term = windows_get_terminal_font();
    ui = windows_get_ui_font();
    if (s_windows.transcript_spans != NULL) {
        lv_obj_set_style_text_font(s_windows.transcript_spans, term, 0);
    }
    if (s_windows.input_line != NULL) {
        lv_obj_set_style_text_font(s_windows.input_line, ui, 0);
    }
    if (s_windows.prev_label != NULL) {
        lv_obj_set_style_text_font(s_windows.prev_label, ui, 0);
    }
    if (s_windows.next_label != NULL) {
        lv_obj_set_style_text_font(s_windows.next_label, ui, 0);
    }
    if (s_windows.up_label != NULL) {
        lv_obj_set_style_text_font(s_windows.up_label, ui, 0);
    }
    if (s_windows.down_label != NULL) {
        lv_obj_set_style_text_font(s_windows.down_label, ui, 0);
    }
    if (s_windows.tab_label != NULL) {
        lv_obj_set_style_text_font(s_windows.tab_label, ui, 0);
    }
    if (s_windows.editor_status != NULL) {
        lv_obj_set_style_text_font(s_windows.editor_status, term, 0);
    }
    lvgl_port_unlock();
}

/** Re-apply the active theme's background colors to the screen, transcript
 * and input row after `theme set`. Font-free; the header/keyboard/modal each
 * have their own theme refresh. Port lock is recursive. */
void windows_refresh_theme(void)
{
    if (!s_windows.initialized) {
        return;
    }
    if (!lvgl_port_lock(0)) {
        return;
    }
    if (s_windows.screen != NULL) {
        lv_obj_set_style_bg_color(s_windows.screen,
                                  windows_get_color(WINDOWS_COLOR_BG_SCREEN), 0);
    }
    if (s_windows.transcript != NULL) {
        lv_obj_set_style_bg_color(s_windows.transcript,
                                  windows_get_color(WINDOWS_COLOR_BG_TRANSCRIPT), 0);
    }
    if (s_windows.input_row != NULL) {
        lv_obj_set_style_bg_color(s_windows.input_row,
                                  windows_get_color(WINDOWS_COLOR_BG_INPUT_ROW), 0);
    }
    lvgl_port_unlock();
}

/* ========================================================================
 * WINDOW CREATION
 * ======================================================================== */

static void windows_transcript_scroll_event_cb(lv_event_t *event);

static void windows_apply_screen_style(void)
{
    lv_obj_t *screen = s_windows.screen;

    lv_obj_set_style_bg_color(screen, windows_get_color(WINDOWS_COLOR_BG_SCREEN), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_layout(screen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    /* Horizontal margin keeps every full-width region off the display edges,
     * so shell text and the header are never clipped at the margins. */
    lv_obj_set_style_pad_left(screen, P4_CONFIG_WINDOW_SCREEN_PAD_HOR, 0);
    lv_obj_set_style_pad_right(screen, P4_CONFIG_WINDOW_SCREEN_PAD_HOR, 0);
    lv_obj_set_style_pad_top(screen, 0, 0);
    lv_obj_set_style_pad_bottom(screen, 0, 0);
    lv_obj_set_style_pad_row(screen, 0, 0);
}

static void windows_create_header(void)
{
    /* Header module: fixed top bar for notifications plus Wi-Fi, battery,
     * Bluetooth, USB, and SD status. Created first so flex-column layout
     * places it above transcript. */
    header_init();
}

static void windows_create_transcript(void)
{
    const lv_font_t *font = windows_get_terminal_font();
    lv_obj_t *screen = s_windows.screen;

    /* The transcript is an LVGL span group. Raw ANSI SGR escape sequences
     * (as produced by the shell's semantic helpers) are parsed into one
     * coloured span per run, so the on-screen transcript matches the UART
     * console. This is the documented architecture: no recolor markup is
     * ever set on the widget, so `#RRGGBB` control tokens can never leak
     * into the visible text.
     *
     * The actual widget update (rebuild spans, force layout, scroll to end)
     * is DEFERRED to the LVGL task through the coalesced apply timer (see
     * windows_transcript_schedule_apply): rebuilding spans synchronously from a
     * non-LVGL task races with the LVGL render cycle and hangs
     * lv_timer_handler on the LVGL task, freezing the whole UI. The apply is
     * coalesced so bursty output paints once per handler pass.
     */
    /* The transcript is a scrollable CONTAINER whose scroll range comes from a
     * full-height invisible spacer child, not from the span group (bugs.md
     * F26): a spangroup clips its own spans to its widget height
     * (LV_SPAN_OVERFLOW_CLIP), so a bounded spangroup reports no overflow and
     * can never scroll. The container owns the fixed height, background and
     * padding; the spacer establishes the range, and only the rows overlapping
     * the viewport (plus a margin) are materialized as spans on a window-sized
     * spangroup. The container therefore sees a taller-than-itself spacer and
     * scrolls through it, exactly as it used to scroll through a
     * content-sized spangroup.
     *
     * The actual widget update (row measurement, rebuild the window spans,
     * force layout, scroll to end) is DEFERRED to the LVGL task through the
     * coalesced apply timer (see windows_transcript_schedule_apply): rebuilding
     * spans synchronously from a non-LVGL task races with the LVGL render
     * cycle and hangs lv_timer_handler on the LVGL task, freezing the whole
     * UI. The apply is coalesced so bursty output paints once per handler
     * pass.
     */
    s_windows.transcript = lv_obj_create(screen);
    lv_obj_set_width(s_windows.transcript, LV_PCT(100));
    /* The height is set explicitly to the computed transcript slot by
     * windows_apply_transcript_height() after the window layout is built, and
     * re-applied on keyboard visibility changes. */
    lv_obj_set_style_bg_color(s_windows.transcript,
                               windows_get_color(WINDOWS_COLOR_BG_TRANSCRIPT), 0);
    lv_obj_set_style_bg_opa(s_windows.transcript, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_windows.transcript, 0, 0);
    lv_obj_set_style_pad_all(s_windows.transcript, 16, 0);
    lv_obj_set_style_radius(s_windows.transcript, 0, 0);
    /* Make the transcript a vertical scroll container: touch pan, USB mouse
     * wheel, and USB keyboard Up/Down all drive it via LVGL's built-in scroll
     * handling. */
    lv_obj_add_flag(s_windows.transcript,
                    LV_OBJ_FLAG_SCROLLABLE |
                    LV_OBJ_FLAG_SCROLL_WITH_ARROW |
                    LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s_windows.transcript, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_windows.transcript, LV_SCROLLBAR_MODE_AUTO);

    /* Full-height invisible spacer establishing the scroll range. Created
     * FIRST so it sits behind the spans. It must stay visually empty (no
     * background, no border) and cannot carry LV_OBJ_FLAG_HIDDEN: hidden
     * children are excluded from the scroll-extent computation and would
     * collapse the range. */
    s_windows.transcript_spacer = lv_obj_create(s_windows.transcript);
    lv_obj_set_width(s_windows.transcript_spacer, LV_PCT(100));
    lv_obj_set_height(s_windows.transcript_spacer, 0);
    lv_obj_clear_flag(s_windows.transcript_spacer,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_windows.transcript_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_windows.transcript_spacer, 0, 0);
    lv_obj_set_style_radius(s_windows.transcript_spacer, 0, 0);
    lv_obj_set_style_pad_all(s_windows.transcript_spacer, 0, 0);

    s_windows.transcript_spans = lv_spangroup_create(s_windows.transcript);
    lv_obj_set_width(s_windows.transcript_spans, LV_PCT(100));
    /* The span group's height is set explicitly to the materialized window
     * height by the renderer - never LV_SIZE_CONTENT. A content-sized child
     * inside a scrollable container is a known LVGL hazard: every layout pass
     * recomputes the self size, and the spangroup's SIZE_CHANGED ->
     * lv_spangroup_refresh() -> refresh_self_size() chain keeps marking the
     * screen layout dirty, so lv_obj_update_layout() never exits its
     * scr->scr_layout_inv loop and the LVGL task spins forever, freezing the
     * whole UI. An explicit height keeps the widget in LV_SPAN_MODE_FIXED
     * where the self size always equals the widget size, so layout converges
     * in one pass while the container still scrolls through the taller
     * spacer. */
    lv_obj_set_height(s_windows.transcript_spans, 0);
    lv_obj_clear_flag(s_windows.transcript_spans, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_windows.transcript_spans, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_windows.transcript_spans, 0, 0);
    lv_obj_set_style_pad_all(s_windows.transcript_spans, 0, 0);
    lv_obj_set_style_text_font(s_windows.transcript_spans, font, 0);

    /* Hidden measure scratch mirroring the span group: row wrapped heights are
     * measured here (windows_transcript_measure_row) without touching the
     * renderer's spans. It is HIDDEN, so the scroll-extent computation skips
     * it. Its styles must match the span group exactly, and the default
     * line_space (0) is REQUIRED: expand_height reads line_space from the
     * group style chain, so customizing it here would diverge measurement from
     * the draw. */
    s_windows.transcript_measure = lv_spangroup_create(s_windows.transcript);
    lv_obj_set_width(s_windows.transcript_measure, LV_PCT(100));
    lv_obj_set_height(s_windows.transcript_measure, 0);
    lv_obj_add_flag(s_windows.transcript_measure, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_windows.transcript_measure, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_windows.transcript_measure, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_windows.transcript_measure, 0, 0);
    lv_obj_set_style_pad_all(s_windows.transcript_measure, 0, 0);
    lv_obj_set_style_text_font(s_windows.transcript_measure, font, 0);

    /* Shift the materialized row window as the visible range scrolls. */
    lv_obj_add_event_cb(s_windows.transcript, windows_transcript_scroll_event_cb,
                        LV_EVENT_SCROLL, NULL);

    s_windows.shell_spans_hidden = false;
}

static void windows_create_input_row(void)
{
    lv_obj_t *screen = s_windows.screen;
    lv_coord_t row_h = windows_scale_height_percent(P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_PCT,
                                                       P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_MIN,
                                                       P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_MAX);

    s_windows.input_row = lv_obj_create(screen);
    lv_obj_set_width(s_windows.input_row, LV_PCT(100));
    lv_obj_set_height(s_windows.input_row, row_h);
    lv_obj_set_layout(s_windows.input_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_windows.input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(s_windows.input_row, 8, 0);
    lv_obj_set_style_pad_ver(s_windows.input_row, 6, 0);
    lv_obj_set_style_pad_column(s_windows.input_row, 8, 0);
    lv_obj_set_style_bg_color(s_windows.input_row,
                               windows_get_color(WINDOWS_COLOR_BG_INPUT_ROW), 0);
    lv_obj_set_style_bg_opa(s_windows.input_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_windows.input_row, 0, 0);

    /* Previous history button */
    s_windows.prev_button = lv_button_create(s_windows.input_row);
    lv_obj_set_size(s_windows.prev_button, 82, LV_PCT(100));
    lv_obj_t *prev_label = lv_label_create(s_windows.prev_button);
    lv_label_set_text(prev_label, "Prev");
    lv_obj_set_style_text_font(prev_label, windows_get_ui_font(), 0);
    lv_obj_center(prev_label);
    s_windows.prev_label = prev_label;

    /* Next history button */
    s_windows.next_button = lv_button_create(s_windows.input_row);
    lv_obj_set_size(s_windows.next_button, 82, LV_PCT(100));
    lv_obj_t *next_label = lv_label_create(s_windows.next_button);
    lv_label_set_text(next_label, "Next");
    lv_obj_set_style_text_font(next_label, windows_get_ui_font(), 0);
    lv_obj_center(next_label);
    s_windows.next_label = next_label;

    /* Transcript scroll buttons. Always visible (independent of the
     * on-screen keyboard) for touch paging. LV_SYMBOL_UP/DOWN resolve via
     * the chained UI font's Montserrat fallback. */
    s_windows.scroll_up_button = lv_button_create(s_windows.input_row);
    lv_obj_set_size(s_windows.scroll_up_button,
                    P4_CONFIG_WINDOW_SCROLL_BUTTON_WIDTH, LV_PCT(100));
    lv_obj_t *up_label = lv_label_create(s_windows.scroll_up_button);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(up_label, windows_get_ui_font(), 0);
    lv_obj_center(up_label);
    s_windows.up_label = up_label;

    s_windows.scroll_down_button = lv_button_create(s_windows.input_row);
    lv_obj_set_size(s_windows.scroll_down_button,
                    P4_CONFIG_WINDOW_SCROLL_BUTTON_WIDTH, LV_PCT(100));
    lv_obj_t *down_label = lv_label_create(s_windows.scroll_down_button);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(down_label, windows_get_ui_font(), 0);
    lv_obj_center(down_label);
    s_windows.down_label = down_label;

    /* Tab completion button. A touch affordance for the completion the USB
     * keyboard reaches with Tab; shares shell_input_line_tab_complete(). */
    s_windows.tab_button = lv_button_create(s_windows.input_row);
    lv_obj_set_size(s_windows.tab_button,
                    P4_CONFIG_WINDOW_SCROLL_BUTTON_WIDTH, LV_PCT(100));
    lv_obj_t *tab_label = lv_label_create(s_windows.tab_button);
    lv_label_set_text(tab_label, "Tab");
    lv_obj_set_style_text_font(tab_label, windows_get_ui_font(), 0);
    lv_obj_center(tab_label);
    s_windows.tab_label = tab_label;

    /* Input-row Stop button: the touch foreground-break affordance. Hidden
     * until a command runs (driven by windows_set_stop_visible from the
     * main poll timer); suppressed in editor/app/TUI modes like its
     * siblings. Placed before the input line so it never steals typing
     * width when hidden (flex skips hidden children). */
    s_windows.stop_button = lv_button_create(s_windows.input_row);
    lv_obj_set_size(s_windows.stop_button,
                    P4_CONFIG_WINDOW_SCROLL_BUTTON_WIDTH, LV_PCT(100));
    lv_obj_t *stop_label = lv_label_create(s_windows.stop_button);
    lv_label_set_text(stop_label, "Stop");
    lv_obj_set_style_text_font(stop_label, windows_get_ui_font(), 0);
    lv_obj_center(stop_label);
    s_windows.stop_label = stop_label;
    lv_obj_add_flag(s_windows.stop_button, LV_OBJ_FLAG_HIDDEN);

    /* Input line textarea */
    s_windows.input_line = lv_textarea_create(s_windows.input_row);
    lv_obj_set_flex_grow(s_windows.input_line, 1);
    lv_obj_set_height(s_windows.input_line, LV_PCT(100));
    lv_textarea_set_one_line(s_windows.input_line, true);
    lv_textarea_set_cursor_click_pos(s_windows.input_line, false);
    /* The vertical padding makes the content box a hair shorter than one text
     * line, so LVGL's AUTO scrollbar mode draws a spurious vertical bar on the
     * right edge. The input is one line and never scrolls vertically; turn the
     * scrollbar off (horizontal text scrolling still follows the cursor). */
    lv_obj_set_scrollbar_mode(s_windows.input_line, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_windows.input_line,
                               windows_get_color(WINDOWS_COLOR_BG_TRANSCRIPT), 0);
    lv_obj_set_style_bg_opa(s_windows.input_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_windows.input_line, 0, 0);
    lv_obj_set_style_pad_hor(s_windows.input_line, 12, 0);
    lv_obj_set_style_pad_ver(s_windows.input_line, 10, 0);
    lv_obj_set_style_text_color(s_windows.input_line,
                                 windows_get_color(WINDOWS_COLOR_TEXT), 0);
    /* Chained UI font (not pure terminal): typed symbols outside unscii's
     * ranges still render via the fallback instead of tofu. */
    lv_obj_set_style_text_font(s_windows.input_line, windows_get_ui_font(), 0);
    /* Editor-like block cursor (see windows_input_cursor_style): full-cell
     * rect via zero border/pad, filled with the text color; the glyph
     * redraws in the input background color for contrast. */
    windows_input_cursor_style(true);

    /* Inline completion ghost: a muted label child of the input line, placed
     * right after the typed text. Managed by shell_input_line_ghost_refresh(). */
    s_windows.input_ghost = lv_label_create(s_windows.input_line);
    lv_obj_set_style_text_font(s_windows.input_ghost, windows_get_ui_font(), 0);
    lv_obj_set_style_text_color(s_windows.input_ghost,
                                windows_get_color(WINDOWS_COLOR_TEXT_MUTED), 0);
    lv_label_set_text(s_windows.input_ghost, "");
    lv_obj_add_flag(s_windows.input_ghost, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_windows.input_ghost, LV_OBJ_FLAG_CLICKABLE);

    /* Reverse-history search query: floats just above the input row while
     * Ctrl+R search is active (positioned by the shell when shown). */
    s_windows.search_label = lv_label_create(s_windows.screen);
    lv_obj_set_style_text_font(s_windows.search_label, windows_get_ui_font(), 0);
    lv_obj_set_style_text_color(s_windows.search_label,
                                windows_get_color(WINDOWS_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_bg_color(s_windows.search_label,
                              windows_get_color(WINDOWS_COLOR_BG_INPUT_ROW), 0);
    lv_obj_set_style_bg_opa(s_windows.search_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_windows.search_label, 6, 0);
    lv_label_set_text(s_windows.search_label, "");
    lv_obj_add_flag(s_windows.search_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_windows.search_label, LV_OBJ_FLAG_CLICKABLE);
}

/** Style the input-line cursor as a block (true, editor-like) or a thin
 * bar (false). Takes the port lock; safe from any task. */
void windows_input_cursor_style(bool block)
{
    lv_obj_t *input;

    /* Read the input-line pointer only with the port lock held: a concurrent
     * UI rebuild (rotation / saved font / window teardown) frees the old input
     * row and NULLs this field on the LVGL task, so a pointer sampled before
     * the lock can be dangling by the time it is styled (boot-time crash). */
    if (!lvgl_port_lock(0)) {
        return;
    }
    input = s_windows.input_line;
    if (input == NULL) {
        lvgl_port_unlock();
        return;
    }
    if (block) {
        lv_obj_set_style_border_width(input, 0, LV_PART_CURSOR);
        lv_obj_set_style_pad_all(input, 0, LV_PART_CURSOR);
        lv_obj_set_style_bg_color(input,
                                  windows_get_color(WINDOWS_COLOR_TEXT),
                                  LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(input, LV_OPA_COVER, LV_PART_CURSOR);
        lv_obj_set_style_text_color(input,
                                    windows_get_color(WINDOWS_COLOR_BG_TRANSCRIPT),
                                    LV_PART_CURSOR);
    } else {
        lv_obj_set_style_border_width(input, 2, LV_PART_CURSOR);
        lv_obj_set_style_border_side(input, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
        lv_obj_set_style_border_color(input,
                                      windows_get_color(WINDOWS_COLOR_TEXT),
                                      LV_PART_CURSOR);
        lv_obj_set_style_pad_all(input, 0, LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(input, LV_OPA_TRANSP, LV_PART_CURSOR);
        lv_obj_set_style_text_color(input,
                                    windows_get_color(WINDOWS_COLOR_TEXT),
                                    LV_PART_CURSOR);
    }
    lv_obj_set_style_anim_duration(input, P4_CONFIG_CURSOR_BLINK_MS,
                                   LV_PART_CURSOR);
    lvgl_port_unlock();
}

/** Set the input-line cursor blink period live (0 = steady, no blink).
 * Takes the port lock; safe from any task. No-op before the input exists. */
void windows_input_cursor_blink(uint32_t blink_ms)
{
    lv_obj_t *input;

    if (!lvgl_port_lock(0)) {
        return;
    }
    input = s_windows.input_line;
    if (input == NULL) {
        lvgl_port_unlock();
        return;
    }
    lv_obj_set_style_anim_duration(input, blink_ms, LV_PART_CURSOR);
    lvgl_port_unlock();
}

static void windows_create_keyboard(void)
{
    /* Delegate keyboard creation to the keyboard component.
     * The keyboard component owns the LVGL keyboard widget and its visibility.
     * We register a callback so the window manager can reflow when
     * keyboard visibility changes. */
    lv_obj_t *kb = keyboard_init(s_windows.screen);
    if (kb != NULL && s_windows.input_line != NULL) {
        keyboard_bind_textarea(s_windows.input_line);
    }
}
/* ========================================================================
 * WINDOW OBJECT ACCESSORS
 * ========================================================================
 */

lv_obj_t *windows_get_transcript(void)
{
    return s_windows.transcript;
}

lv_obj_t *windows_get_transcript_spans(void)
{
    return s_windows.transcript_spans;
}

/* ---- LVGL span-group transcript implementation --------------------------
 * The transcript renders coloured shell output on the display. Raw ANSI SGR
 * escape sequences (as produced by the shell's semantic helpers) are parsed
 * into one coloured span per run. No recolor markup is ever generated, so
 * `#RRGGBB` control tokens can never appear as visible characters.
 *
 * The widget update itself is DEFERRED to the LVGL task. Rebuilding spans and
 * forcing the flex layout synchronously from a non-LVGL task (the command
 * worker, UART console, or networking background task) while the LVGL task is
 * mid-render hangs lv_timer_handler and freezes the whole UI. Every request
 * stages the raw ANSI text into a persistent buffer (guarded by its own
 * mutex, NOT the port lock, so the worker never serializes behind a render -
 * bugs.md F26) and wakes the LVGL task; the apply-timer callback rebuilds the
 * spans from the newest staged content and scrolls to the end. Bursts of
 * output coalesce into one apply per tick, which also keeps the render cost
 * bounded.
 */

static bool s_transcript_apply_pending = false;
/* When set, the next transcript repaint pins the view to the bottom
 * unconditionally (used when a command is submitted), instead of only
 * following when the view is already near the bottom. Cleared after the first
 * apply consumes it, so background output later reverts to near-bottom
 * following. Simple bool: atomic on this platform, written by the submit paths
 * (LVGL event or UART console task) and read on the LVGL task, both of which
 * serialize through the LVGL port lock. */
static bool s_transcript_force_follow = false;
/* Staged ANSI text awaiting span conversion. It lives in PSRAM (not internal
 * DRAM): at P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES it would otherwise consume a
 * scrollback of internal heap and force the very memory-pressure trims it
 * exists to survive. Allocated on first transcript creation with an
 * internal-heap fallback; every use NULL-checks.
 *
 * The staging buffer is guarded by its own mutex, NOT the LVGL port lock:
 * the command worker only pays for the (sub-millisecond) copy and a
 * non-blocking render wake, never for the LVGL-side apply + redraw that
 * happens on the LVGL task within one apply-tick (bugs.md F26: the worker
 * used to serialize behind the whole port-lock hold, costing 300+ ms per
 * command). The lock order is always PORT outer, STAGE inner. */
static char *s_transcript_staged = NULL;
static SemaphoreHandle_t s_transcript_stage_lock = NULL;
static lv_timer_t *s_transcript_apply_timer = NULL;

/** Take/release the staging mutex (created in windows_init; unit-test paths
 *  run before the UI exists and never reach the staged buffer). */
static void windows_stage_lock(void)
{
    if (s_transcript_stage_lock != NULL) {
        xSemaphoreTake(s_transcript_stage_lock, portMAX_DELAY);
    }
}

static void windows_stage_unlock(void)
{
    if (s_transcript_stage_lock != NULL) {
        xSemaphoreGive(s_transcript_stage_lock);
    }
}

/* Incremental-render bookkeeping (bugs.md F26). s_transcript_staged_len and
 * s_transcript_staged_epoch are written by the staging path (under the LVGL
 * port lock) from the shell's length-tracked buffer; s_transcript_rendered_len
 * is the byte count of s_transcript_staged already converted into spans, and
 * s_transcript_rendered_epoch the scrollback generation that covers. The two
 * epochs differing means the shell dropped the oldest bytes (reset, trim,
 * buffer-full truncation), so the apply rebuilds; equality makes truncation
 * detection O(1) with no prefix snapshot and no whole-buffer compares.
 * s_transcript_last_fg carries the ANSI foreground across an append that
 * splits a colour run, so the next fragment keeps its hue instead of
 * restarting at the default colour. */
static size_t s_transcript_staged_len = 0;
static uint32_t s_transcript_staged_epoch = 0;
static uint32_t s_transcript_rendered_epoch = 0;
static size_t s_transcript_rendered_len = 0;
static uint32_t s_transcript_last_fg = 0;
static uint32_t s_transcript_seen_fg = 0;

/* ---- Windowed row model (bugs.md F26) ------------------------------------
 * The scrollback is retained as a table of '\n'-delimited rows (byte offsets
 * into the persistent staged buffer) with per-row measured wrapped heights and
 * a pixel prefix used to anchor scrolling. Only the rows overlapping the
 * viewport are materialized as LVGL spans; a full-height invisible spacer sets
 * the container's scroll range to the whole scrollback. All of this lives in
 * PSRAM (never the internal DMA heap, which is the scarce resource the span
 * cap and the trim guard protect). */

/** One '\n'-delimited row of the transcript scrollback. A closed row includes
 *  its trailing '\n'; the final row may be open (no '\n' yet, see
 *  s_transcript_tail_open). .fg is the ANSI foreground carried into the row's
 *  start (0 = the default), so a window assembled from arbitrary rows renders
 *  byte-identically to the full contiguous stream the measurements assume. */
typedef struct {
    size_t off;          /**< Byte offset of the row inside s_transcript_staged. */
    size_t len;          /**< Row byte length (closed rows include '\n'). */
    int32_t h;           /**< Measured wrapped pixel height. */
    uint32_t fg;         /**< ANSI foreground colour carried into the row start. */
} windows_transcript_row_t;

static windows_transcript_row_t *s_transcript_rows = NULL;
static int32_t *s_transcript_prefix = NULL;   /**< prefix[i] = pixel top of row i; prefix[count] = total. */
static uint32_t s_transcript_row_count = 0;
static bool s_transcript_tail_open = false;   /**< Last row has no trailing '\n'. */

static char *s_transcript_work = NULL;        /**< Transient concat/measure scratch (PSRAM). */
static int32_t s_transcript_measure_width = 0; /**< Wrap width used for measurement. */
static bool s_transcript_metrics_dirty = true; /**< Font or wrap width changed: remeasure all rows. */
static bool s_transcript_render_busy = false;  /**< Reentrancy guard for the scroll handler. */
static uint32_t s_transcript_render_first = 0; /**< First row of the materialized window. */
static uint32_t s_transcript_render_count = 0; /**< Row count of the materialized window. */

/**
 * Ensure the PSRAM staging buffer exists. Called from the staging paths and
 * the apply; the caller must hold the staging mutex (allocation happens once,
 * and the buffer must never be freed while a copy reads it). Returns false
 * when PSRAM could not satisfy the request; callers then skip the render
 * update rather than touching a NULL buffer.
 */
static bool windows_transcript_staging_ensure(void)
{
    if (s_transcript_staged != NULL) {
        return true;
    }
    /* All-or-nothing: the staged buffer alone is not enough for the windowed
     * renderer (rows, prefix, work), so a partial failure backs out rather
     * than running with a half-initialized model. PSRAM is plentiful on both
     * boards; the fallback skips the on-screen render only. */
    s_transcript_staged = p4heap_alloc_psram(P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES);
    if (s_transcript_staged != NULL) {
        s_transcript_staged[0] = '\0';
    }
    if (s_transcript_staged != NULL) {
        s_transcript_rows = (windows_transcript_row_t *)p4heap_alloc_psram(
            (size_t)P4_CONFIG_TRANSCRIPT_ROW_CAP * sizeof(windows_transcript_row_t));
        s_transcript_prefix = (int32_t *)p4heap_alloc_psram(
            (size_t)(P4_CONFIG_TRANSCRIPT_ROW_CAP + 1) * sizeof(int32_t));
        s_transcript_work = p4heap_alloc_psram(P4_CONFIG_TRANSCRIPT_WORK_BYTES);
    }
    if (s_transcript_staged == NULL || s_transcript_rows == NULL ||
        s_transcript_prefix == NULL || s_transcript_work == NULL) {
        if (s_transcript_staged != NULL) {
            heap_caps_free(s_transcript_staged);
            s_transcript_staged = NULL;
        }
        if (s_transcript_rows != NULL) {
            heap_caps_free(s_transcript_rows);
            s_transcript_rows = NULL;
        }
        if (s_transcript_prefix != NULL) {
            heap_caps_free(s_transcript_prefix);
            s_transcript_prefix = NULL;
        }
        if (s_transcript_work != NULL) {
            heap_caps_free(s_transcript_work);
            s_transcript_work = NULL;
        }
        return false;
    }
    return true;
}

/** Release the PSRAM staging + row-model buffers (windows_deinit). The staging
 *  mutex serializes this against any in-flight copy on the command worker; it
 *  is created with the UI and outlives rebuilds. */
static void windows_transcript_staging_free(void)
{
    windows_stage_lock();
    if (s_transcript_staged != NULL) {
        heap_caps_free(s_transcript_staged);
        s_transcript_staged = NULL;
    }
    if (s_transcript_rows != NULL) {
        heap_caps_free(s_transcript_rows);
        s_transcript_rows = NULL;
    }
    if (s_transcript_prefix != NULL) {
        heap_caps_free(s_transcript_prefix);
        s_transcript_prefix = NULL;
    }
    if (s_transcript_work != NULL) {
        heap_caps_free(s_transcript_work);
        s_transcript_work = NULL;
    }
    s_transcript_staged_len = 0;
    s_transcript_staged_epoch = 0;
    s_transcript_rendered_len = 0;
    s_transcript_rendered_epoch = 0;
    s_transcript_last_fg = 0;
    s_transcript_seen_fg = 0;
    s_transcript_row_count = 0;
    s_transcript_tail_open = false;
    s_transcript_measure_width = 0;
    s_transcript_metrics_dirty = true;
    s_transcript_render_busy = false;
    s_transcript_render_first = 0;
    s_transcript_render_count = 0;
    windows_stage_unlock();
    s_transcript_apply_pending = false;
}

/**
 * Build one coloured span holding @p text with the ANSI state's foreground
 * colour into @p group. Shared by the measure and render paths; the two call
 * it with different groups and only the measure wrapper records the colour
 * carry (s_transcript_seen_fg).
 */
static void windows_span_segment(const char *text, const ansi_state_t *state, void *user_data)
{
    lv_obj_t *group = (lv_obj_t *)user_data;
    lv_span_t *span;
    lv_style_t *style;

    if (group == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    span = lv_spangroup_add_span(group);
    if (span == NULL) {
        return;
    }

    lv_span_set_text(span, text);
    style = lv_span_get_style(span);
    if (state != NULL) {
        /* Full attribute mapping (bold/italic via TTF variants with bright
         * fallback, underline/strike decor). Serial terminals render the
         * same SGR natively through the UART mirror. */
        font_span_style(style, FONT_ROLE_TERMINAL, (unsigned)state->attrs,
                        state->fg_index, state->fg_color);
    } else {
        lv_style_set_text_font(style, windows_get_terminal_font());
    }
}

/**
 * Measure variant: adds the span to the scratch measure group and records the
 * colour active at the end of the fragment (the carry into the next row,
 * mirrored by the render path so heights match what is drawn).
 */
static void windows_measure_segment(const char *text, const ansi_state_t *state, void *user_data)
{
    if (state != NULL) {
        s_transcript_seen_fg = state->fg_color;
    }
    windows_span_segment(text, state, user_data);
}

/**
 * Render variant: pure window-span builder. MUST NOT touch the measurement
 * carry statics: a window rebuild for an arbitrary scroll position would
 * otherwise corrupt the running-colour bookkeeping of the measure pass.
 */
static void windows_render_segment(const char *text, const ansi_state_t *state, void *user_data)
{
    windows_span_segment(text, state, user_data);
}

/** Delete every span currently held by the transcript span group. */
static void windows_transcript_clear_spans(lv_obj_t *transcript)
{
    uint32_t count = lv_spangroup_get_span_count(transcript);
    uint32_t index;

    for (index = 0; index < count; index++) {
        lv_span_t *span = lv_spangroup_get_child(transcript, 0);

        if (span != NULL) {
            lv_spangroup_delete_span(transcript, span);
        }
    }
}

/**
 * Map a 24-bit RGB colour back to the closest 16-colour SGR foreground code
 * (30-37 / 90-97) by comparing against the ANSI palette. Used to re-emit a
 * carried colour as a real SGR prefix when an append splits a colour run.
 * Returns -1 when the colour is not one of the 16 palette entries.
 */
static int windows_ansi_sgr_for_color(uint32_t color)
{
    int index;

    if (color == 0) {
        return -1;
    }
    for (index = 0; index < ANSI_COLOR_COUNT; index++) {
        if (ansi_get_palette_color((ansi_color_index_t)index) == color) {
            return (index < 8) ? (30 + index) : (90 + (index - 8));
        }
    }
    return -1;
}

/**
 * Emit the SGR foreground-carry prefix that @p bytes needs from @p carry_fg, or
 * nothing. The rule must be byte-identical between the measure and render
 * paths: a row that starts with its own escape sequence already re-arms the
 * SGR state, and the default foreground carries nothing.
 * @return Number of bytes written to @p buf (0 = no carry).
 */
static size_t windows_transcript_carry(const char *bytes, uint32_t carry_fg,
                                       char *buf, size_t buf_size)
{
    int code;

    if (carry_fg == 0 || carry_fg == ansi_get_default_fg() ||
        bytes[0] == '\x1B' || buf_size < 12) {
        return 0;
    }
    code = windows_ansi_sgr_for_color(carry_fg);
    if (code <= 0) {
        return 0;
    }
    return (size_t)snprintf(buf, buf_size, "\x1B[%dm", code);
}

/**
 * Measure the wrapped pixel height of one row end-to-end. The row text plus
 * its carry prefix are assembled into the transient work buffer and parsed
 * into the hidden scratch spangroup; the height is that group's expand height
 * at the measurement width (the window group's content width, so the numbers
 * match the draw exactly).
 */
static int32_t windows_transcript_measure_row(const char *bytes, size_t len,
                                              uint32_t carry_fg)
{
    char *work = s_transcript_work;
    char carry_buf[16];
    size_t head = 0;
    int32_t width = s_transcript_measure_width;

    if (work == NULL || s_windows.transcript_measure == NULL || width <= 0 ||
        bytes == NULL) {
        return 0;
    }
    if (len + 16 > P4_CONFIG_TRANSCRIPT_WORK_BYTES) {
        len = P4_CONFIG_TRANSCRIPT_WORK_BYTES - 16;
    }

    head = windows_transcript_carry(bytes, carry_fg, carry_buf, sizeof(carry_buf));
    if (head > 0) {
        memcpy(work, carry_buf, head);
    }
    memcpy(work + head, bytes, len);
    work[head + len] = '\0';

    s_transcript_seen_fg = 0;
    windows_transcript_clear_spans(s_windows.transcript_measure);
    ansi_process_text(work, windows_measure_segment, s_windows.transcript_measure);
    return lv_spangroup_get_expand_height(s_windows.transcript_measure, width);
}

/**
 * Rebuild the pixel prefix from the current row table and size the spacer to
 * the total content height (the full scroll range, editor-windowing pattern).
 */
static void windows_transcript_prefix_rebuild(void)
{
    uint32_t i;
    int32_t acc = 0;

    if (s_transcript_prefix == NULL) {
        return;
    }
    s_transcript_prefix[0] = 0;
    for (i = 0; i < s_transcript_row_count; i++) {
        acc += s_transcript_rows[i].h;
        s_transcript_prefix[i + 1] = acc;
    }
    if (s_windows.transcript_spacer != NULL) {
        lv_obj_set_height(s_windows.transcript_spacer, acc);
    }
}

/**
 * Parse @p len bytes (a suffix of s_transcript_staged starting at the staged
 * offset @p base_off) into rows, measuring each wrapped height as it goes,
 * and append them to the row table. @p start_fg is the ANSI foreground active
 * before the first byte (the running carry). Runs on the LVGL task (reads the
 * claimed staged suffix, which is append-stable for the epoch).
 */
static void windows_transcript_rows_add(const char *data, size_t len,
                                        size_t base_off, uint32_t start_fg)
{
    uint32_t run = start_fg;
    size_t pos = 0;
    size_t row_start = 0;
    size_t closed = 0;
    size_t new_rows_bound;
    size_t new_bottom;
    bool open = false;
    uint32_t i;

    if (s_transcript_rows == NULL || data == NULL) {
        return;
    }
    if (len == 0) {
        s_transcript_tail_open = false;
        return;
    }

    /* Bound the number of new rows: every '\n' closes one row, plus possibly
     * one open tail row if the region does not end with '\n'. */
    for (i = 0; i < len; i++) {
        if (data[i] == '\n') {
            closed++;
        }
    }
    new_rows_bound = closed + (data[len - 1] != '\n' ? 1 : 0);

    /* Evict the oldest rows once if adding these would overflow the table. */
    if (s_transcript_row_count + new_rows_bound > P4_CONFIG_TRANSCRIPT_ROW_CAP &&
        s_transcript_row_count > 0) {
        size_t keep = s_transcript_row_count > new_rows_bound
                          ? (P4_CONFIG_TRANSCRIPT_ROW_CAP > new_rows_bound
                                 ? P4_CONFIG_TRANSCRIPT_ROW_CAP - new_rows_bound
                                 : 0)
                          : 0;
        size_t drop = s_transcript_row_count - keep;

        if (drop < s_transcript_row_count && keep > 0) {
            memmove(s_transcript_rows, s_transcript_rows + drop,
                    keep * sizeof(windows_transcript_row_t));
            s_transcript_row_count = (uint32_t)keep;
        } else {
            s_transcript_row_count = 0;
        }
    }

    while (pos < len) {
        const char *nl = memchr(data + pos, '\n', len - pos);

        if (nl == NULL) {
            open = true;
            break;
        }
        new_bottom = (size_t)(nl - data) + 1; /* include the trailing '\n' */
        if (s_transcript_row_count < P4_CONFIG_TRANSCRIPT_ROW_CAP) {
            windows_transcript_row_t *row = &s_transcript_rows[s_transcript_row_count];
            int32_t h = windows_transcript_measure_row(data + row_start,
                                                       new_bottom - row_start, run);

            row->off = base_off + row_start;
            row->len = new_bottom - row_start;
            row->h = h < 0 ? 0 : h;
            row->fg = run;
            s_transcript_row_count++;
        }
        if (s_transcript_seen_fg != 0) {
            run = s_transcript_seen_fg;
        }
        pos = new_bottom;
        row_start = new_bottom;
    }

    if (open && row_start < len) {
        if (s_transcript_row_count < P4_CONFIG_TRANSCRIPT_ROW_CAP) {
            windows_transcript_row_t *row = &s_transcript_rows[s_transcript_row_count];
            int32_t h = windows_transcript_measure_row(data + row_start,
                                                       len - row_start, run);

            row->off = base_off + row_start;
            row->len = len - row_start;
            row->h = h < 0 ? 0 : h;
            row->fg = run;
            s_transcript_row_count++;
        }
        s_transcript_tail_open = true;
    } else {
        s_transcript_tail_open = false;
    }

    windows_transcript_prefix_rebuild();
}

/**
 * Drop the whole windowed row model: no rows, no rendered window, zero-height
 * spacer. Called on scrollback resets (epoch moved / explicit trim) where the
 * staged bytes the rows pointed at no longer describe the scrollback.
 */
static void windows_transcript_rows_reset(void)
{
    s_transcript_row_count = 0;
    s_transcript_tail_open = false;
    s_transcript_render_first = 0;
    s_transcript_render_count = 0;
    if (s_windows.transcript_spans != NULL) {
        windows_transcript_clear_spans(s_windows.transcript_spans);
        lv_obj_set_y(s_windows.transcript_spans, 0);
    }
    if (s_windows.transcript_measure != NULL) {
        windows_transcript_clear_spans(s_windows.transcript_measure);
    }
    if (s_windows.transcript_spacer != NULL) {
        lv_obj_set_height(s_windows.transcript_spacer, 0);
    }
}

/** Remeasure every retained row after a font or wrap-width change. Runs on the
 *  LVGL task; reads the claimed staged bytes only. */
static void windows_transcript_remeasure_all(void)
{
    uint32_t i;

    if (s_transcript_rows == NULL || s_transcript_staged == NULL) {
        return;
    }
    for (i = 0; i < s_transcript_row_count; i++) {
        windows_transcript_row_t *row = &s_transcript_rows[i];
        int32_t h = windows_transcript_measure_row(s_transcript_staged + row->off,
                                                   row->len, row->fg);

        row->h = h < 0 ? 0 : h;
    }
    windows_transcript_prefix_rebuild();
}

/**
 * Refresh the measurement wrap width (the span group's content width, i.e. the
 * width the span draw uses) and re-measure all rows when it changed. The
 * container's layout may be pending when this runs, so a change first seen
 * here is picked up on the next pass; the metric converges within one apply.
 */
static void windows_transcript_sync_metrics(void)
{
    lv_obj_t *container = s_windows.transcript;
    lv_obj_t *spans = s_windows.transcript_spans;
    int32_t width;

    if (container == NULL || spans == NULL) {
        return;
    }
    width = lv_obj_get_content_width(spans);
    if (width <= 0) {
        width = lv_obj_get_content_width(container);
    }
    if (width <= 0) {
        width = (int32_t)P4_CONFIG_TRANSCRIPT_WINDOW_MARGIN_PX;
    }
    if (width != s_transcript_measure_width) {
        s_transcript_measure_width = width;
        s_transcript_metrics_dirty = true;
    }
    if (s_transcript_metrics_dirty) {
        windows_transcript_remeasure_all();
        s_transcript_metrics_dirty = false;
    }
}

/** First row whose top pixel (prefix[i]) is at or below @p px. Returns
 *  s_transcript_row_count when @p px is at or beyond the scrollback bottom. */
static uint32_t windows_transcript_start_row(int32_t px)
{
    uint32_t lo = 0;
    uint32_t hi = s_transcript_row_count;

    if (s_transcript_row_count == 0) {
        return 0;
    }
    if (px <= 0) {
        return 0;
    }
    if (px >= s_transcript_prefix[s_transcript_row_count]) {
        return s_transcript_row_count;
    }
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (s_transcript_prefix[mid] >= px) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

/**
 * Materialize the rows overlapping the current viewport as spans.
 *
 * The rows [first, last) are assembled from the staged buffer (with per-row
 * SGR carry prefixes that reproduce the exact byte stream the measurements
 * assumed) under the staging mutex, with an epoch guard so the window can never
 * be built from a scrollback generation that no longer matches the rows. When
 * the scroll moves within the materialized margin the window is left untouched
 * (editor-windowing pattern); @p force overrides that reuse.
 *
 * @p pin_bottom renders the window for the very bottom of the scrollback and
 * scrolls the container there (used when following new output).
 */
static void windows_transcript_render_window(bool pin_bottom, bool force)
{
    lv_obj_t *container = s_windows.transcript;
    lv_obj_t *spans = s_windows.transcript_spans;
    uint32_t count = s_transcript_row_count;
    lv_coord_t view_h;
    int32_t total;
    int32_t scroll_y;
    int32_t px_top;
    int32_t px_bottom;
    uint32_t first;
    uint32_t last;
    uint32_t i;
    uint32_t span_count;

    if (container == NULL || spans == NULL || s_transcript_work == NULL ||
        count == 0) {
        return;
    }

    view_h = lv_obj_get_height(container);
    total = s_transcript_prefix[count];
    if (total <= 0) {
        return;
    }
    /* For a pinned rebuild the window covers the bottom rows (scroll_y is
     * approximated; the real clamp happens via LV_COORD_MAX below). */
    scroll_y = pin_bottom ? (total - view_h) : lv_obj_get_scroll_y(container);
    if (scroll_y < 0) {
        scroll_y = 0;
    }

    px_top = scroll_y - (int32_t)P4_CONFIG_TRANSCRIPT_WINDOW_MARGIN_PX;
    if (px_top < 0) {
        px_top = 0;
    }
    px_bottom = scroll_y + view_h + (int32_t)P4_CONFIG_TRANSCRIPT_WINDOW_MARGIN_PX;
    if (px_bottom > total) {
        px_bottom = total;
    }

    first = windows_transcript_start_row(px_top);
    if (first > 0 && s_transcript_prefix[first] > px_top) {
        first--; /* the row split by the top edge is partially visible */
    }
    last = windows_transcript_start_row(px_bottom);
    if (last <= first) {
        last = first + 1; /* never materialize an empty window */
    }
    if (last - first > P4_CONFIG_TRANSCRIPT_WINDOW_ROWS) {
        last = first + P4_CONFIG_TRANSCRIPT_WINDOW_ROWS;
    }

    if (!force && first == s_transcript_render_first &&
        last - first == s_transcript_render_count) {
        return;
    }

    /* Assemble the window's byte stream. Offsets are slices of the persistent
     * staged buffer, so the epoch must not have moved while we read them. */
    windows_stage_lock();
    if (s_transcript_staged == NULL ||
        s_transcript_staged_epoch != s_transcript_rendered_epoch) {
        windows_stage_unlock();
        return;
    }
    {
        char *out = s_transcript_work;
        size_t out_off = 0;
        char carry_buf[16];
        bool truncated = false;

        for (i = first; i < last; i++) {
            const windows_transcript_row_t *row = &s_transcript_rows[i];
            const char *bytes = s_transcript_staged + row->off;
            size_t carry = windows_transcript_carry(bytes, row->fg,
                                                    carry_buf, sizeof(carry_buf));

            if (out_off + carry + row->len + 1 > P4_CONFIG_TRANSCRIPT_WORK_BYTES) {
                truncated = true;
                break;
            }
            if (carry > 0) {
                memcpy(out + out_off, carry_buf, carry);
                out_off += carry;
            }
            memcpy(out + out_off, bytes, row->len);
            out_off += row->len;
        }
        out[out_off] = '\0';
        (void)truncated; /* window is bounded by WINDOW_ROWS; carry+len headroom holds */
    }
    windows_stage_unlock();

    /* Rebuild the window spans from the assembled stream. */
    s_transcript_render_busy = true;
    windows_transcript_clear_spans(spans);
    ansi_process_text(s_transcript_work, windows_render_segment, spans);

    /* Hard bound on the per-window span count (internal-heap reclamation; the
     * per-window cap is far below the old whole-scrollback consumption). */
    span_count = lv_spangroup_get_span_count(spans);
    while (span_count > P4_CONFIG_TRANSCRIPT_MAX_SPANS) {
        lv_span_t *oldest = lv_spangroup_get_child(spans, 0);

        if (oldest == NULL) {
            break;
        }
        lv_spangroup_delete_span(spans, oldest);
        span_count--;
    }

    lv_obj_set_y(spans, s_transcript_prefix[first]);
    lv_obj_set_height(spans, s_transcript_prefix[last] - s_transcript_prefix[first]);
    s_transcript_render_first = first;
    s_transcript_render_count = last - first;
    s_transcript_render_busy = false;

    if (pin_bottom) {
        /* LV_COORD_MAX clamps to the container's true scrollable maximum
         * (spacer extent plus padding), mirroring the pre-windowed follow. */
        lv_obj_update_layout(container);
        lv_obj_scroll_to_y(container, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

/** Container scroll (touch drag, mouse wheel, arrow keys, scroll buttons):
 *  shift the materialized row window as the visible range moves out of it. */
static void windows_transcript_scroll_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_transcript_render_busy || s_windows.shell_spans_hidden ||
        s_windows.editor_mode || s_windows.app_surface_active ||
        s_windows.tui_mode || s_windows.transcript == NULL ||
        s_transcript_row_count == 0) {
        return;
    }
    windows_transcript_render_window(false, false);
}

/**
 * Reconcile the staged ANSI scrollback against the windowed row model and the
 * on-screen span window. Runs ONLY on the LVGL task (the coalescing apply timer
 * callback in the handler pass), so it is inherently serialized with the render
 * cycle. The staging buffer is read under its own mutex, which the command
 * worker holds briefly while copying; the expensive work (row measurement, span
 * parse, layout) happens after that mutex is released (bugs.md F26).
 *
 * Measurement and drawing never touch the whole scrollback: only the rows
 * overlapping the viewport (plus a margin) are materialized as spans, so the
 * per-command cost stays O(viewport) however much history has accumulated
 * (see windows_transcript_render_window).
 */
static void windows_transcript_apply(void)
{
    lv_obj_t *container = s_windows.transcript;
    lv_obj_t *spans = s_windows.transcript_spans;
    size_t new_len;
    size_t add_len = 0;
    size_t base_off = 0;
    uint32_t carry_fg = 0;
    char *fragment = NULL;
    bool rebuild_all = false;
    bool rebuild_win;
    bool follow_bottom;
    uint32_t prev_row_count;

    if (container == NULL || spans == NULL) {
        return;
    }

    /* While an app surface (gfx canvas, fullscreen app) owns the transcript,
     * the span group is hidden and its exact height is irrelevant. Skip the
     * reconcile entirely: the staged text keeps accumulating, and an app-surface
     * exit schedules a single catch-up apply that consumes the whole delta in
     * one pass (bugs.md F25). */
    if (s_windows.app_surface_active) {
        return;
    }

    /* Follow the newest output when already at/near the bottom (so reading
     * earlier history is not yanked down by new output), or when a submitted
     * command requested a forced jump to its output. The force flag is
     * consumed here so it affects only the output of the just-submitted
     * command.
     *
     * Never follow while a modal surface owns the transcript (editor mode):
     * the modal panel is pinned at the content origin, and scrolling to the
     * newest shell output would drag the panel off-screen above the viewport
     * (V1). The deferred apply can run right after the modal opens, so the
     * guard must live here, not only at panel creation. */
    follow_bottom = !s_windows.editor_mode && !s_windows.app_surface_active &&
                    (s_transcript_force_follow ||
                     lv_obj_get_scroll_bottom(container) < P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX);
    s_transcript_force_follow = false;

    /* Snapshot the staged delta under the staging mutex. Only the staged bytes
     * and the render bookkeeping are touched here; the expensive work below
     * (row measurement, span parse, layout) runs unlocked against a private
     * PSRAM fragment copy. Lock order is PORT outer, STAGE inner: apply runs on
     * the LVGL task which holds the port lock, and the worker never takes the
     * port lock at all (bugs.md F26 decoupling). */
    windows_stage_lock();

    /* Staging lives in PSRAM; if it could not be allocated, there is nothing
     * to render (the UART console still received the text). */
    if (!windows_transcript_staging_ensure()) {
        windows_stage_unlock();
        return;
    }
    new_len = s_transcript_staged_len;

    /* Scrollback truncation: the shell replaced the buffer content (its epoch
     * moved) or the staged text somehow shrank. Every row offset is invalid, so
     * the whole windowed model is rebuilt. */
    if (s_transcript_rendered_epoch != s_transcript_staged_epoch ||
        s_transcript_rendered_len > new_len) {
        rebuild_all = true;
        s_transcript_rendered_len = 0;
        s_transcript_rendered_epoch = s_transcript_staged_epoch;
        s_transcript_last_fg = 0;
    }

    prev_row_count = s_transcript_row_count;

    /* Locate the append region. When the last row is still open (no trailing
     * '\n'), the previous bytes and the new bytes are adjacent in the staged
     * buffer, so the combined row is re-measured from its first byte with the
     * colour it carried. */
    if (new_len > s_transcript_rendered_len) {
        if (rebuild_all) {
            base_off = 0;
            carry_fg = 0;
        } else if (s_transcript_tail_open && s_transcript_row_count > 0) {
            base_off = s_transcript_rows[s_transcript_row_count - 1].off;
            carry_fg = s_transcript_rows[s_transcript_row_count - 1].fg;
        } else {
            base_off = s_transcript_rendered_len;
            carry_fg = s_transcript_last_fg;
        }
        add_len = new_len - base_off;

        /* Copy the append region: the worker may re-copy the whole staged
         * buffer at any moment, and the per-row measurement below must not run
         * under the mutex. The fragment holds the same bytes the offsets will
         * describe for this epoch, so slice-based windows stay consistent. */
        if (add_len > 0) {
            fragment = p4heap_alloc_psram(add_len + 1);
            if (fragment != NULL) {
                memcpy(fragment, s_transcript_staged + base_off, add_len);
                fragment[add_len] = '\0';
            } else {
                ESP_LOGW(WINDOWS_TAG, "transcript: OOM for append fragment");
            }
        }
        if (fragment != NULL) {
            if (s_transcript_tail_open && s_transcript_row_count > 0 &&
                !rebuild_all) {
                s_transcript_row_count--; /* pop the re-measured open tail */
            }
            s_transcript_rendered_len = new_len;
            s_transcript_rendered_epoch = s_transcript_staged_epoch;
        }
    }
    windows_stage_unlock();

    /* Off-mutex model update. A pop/claim happened only when the fragment copy
     * succeeded, so a failed copy leaves everything untouched for the next tick
     * to retry. */
    if (rebuild_all) {
        windows_transcript_rows_reset();
    }
    if (fragment != NULL) {
        windows_transcript_rows_add(fragment, add_len, base_off, carry_fg);
        if (s_transcript_seen_fg != 0) {
            s_transcript_last_fg = s_transcript_seen_fg;
        }
        heap_caps_free(fragment);
    }

    /* Re-measure on font or wrap-width changes (the window group's content
     * width is the width the span draw uses, so measurements match the draw). */
    windows_transcript_sync_metrics();

    /* Rebuild the materialized window when the model changed under it: fresh
     * model, nothing rendered yet, or the previous window reached the old tail
     * (its rows were re-measured and new rows follow it). Otherwise the window
     * is left untouched and the scroll handler shifts it as the user moves. */
    rebuild_win = rebuild_all || s_transcript_render_count == 0 ||
                  (s_transcript_render_count > 0 && prev_row_count > 0 &&
                   s_transcript_render_first + s_transcript_render_count >=
                       prev_row_count);

    windows_transcript_render_window(follow_bottom, rebuild_win);
}
/**
 * Apply-timer tick (LVGL task, inside the handler pass with the port lock
 * held). The pending flag is the coalescing token set by every staging write;
 * bursts of lines consume as one apply.
 */
static void windows_transcript_apply_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_transcript_apply_pending) {
        return;
    }
    s_transcript_apply_pending = false;
    windows_transcript_apply();
}

/**
 * Reset the windowed transcript after the shell dropped the oldest scrollback
 * bytes (see shell_transcript_guard_internal). Runs under memory pressure and
 * must free the materialized spans synchronously before a command prints; the
 * caller holds the LVGL port lock.
 *
 * The staged buffer is NOT modified here; the guard re-stages the trimmed text
 * (which already contains the trim marker) BEFORE calling this, so we only
 * reset the row model and drop every span. The sentinel epoch forces the next
 * apply to rebuild all rows from the staged head. UINT32_MAX never equals a
 * real shell epoch (0xFFFFFFFF trims would need ~270 years at the trim rate).
 */
void windows_transcript_trim(void)
{
    windows_stage_lock();
    if (!windows_transcript_staging_ensure()) {
        windows_stage_unlock();
        return;
    }

    windows_transcript_rows_reset();
    s_transcript_rendered_len = 0;
    s_transcript_last_fg = 0;
    s_transcript_seen_fg = 0;
    s_transcript_rendered_epoch = UINT32_MAX;
    windows_stage_unlock();
}

/**
 * Schedule a transcript repaint. Coalesces: if an apply is already pending,
 * the staging buffer already holds the newest text and the apply timer paints
 * it; no second wake is needed. The LVGL task consumes the flag through the
 * apply timer (P4_CONFIG_TRANSCRIPT_APPLY_TICK_MS), and the port-task wake
 * short-cuts the sleep between handler passes so output appears within one
 * tick. This path takes NO LVGL lock: it is called from the command worker
 * between staging copies, and must never serialize behind a long render
 * (bugs.md F26). Without a live apply timer (UI torn down between rebuilds)
 * the staged text simply stays pending; the next init + repaint-pending pass
 * (shell_transcript_repaint_if_pending / app-surface exit) consumes it.
 */
static void windows_transcript_schedule_apply(void)
{
    if (s_transcript_apply_timer == NULL) {
        return;
    }
    if (s_transcript_apply_pending) {
        return;
    }
    s_transcript_apply_pending = true;
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

/**
 * Run a pending repaint synchronously. Explicit synchronization points
 * (screenshots, key waits, batch segment end) call this while holding the
 * LVGL port lock (or on the LVGL task) so the widget state provably includes
 * everything staged before the call; the normal per-append path never waits.
 */
void windows_transcript_sync_apply(void)
{
    if (!s_transcript_apply_pending) {
        return;
    }
    s_transcript_apply_pending = false;
    windows_transcript_apply();
}

void windows_set_transcript_text(const char *text)
{
    /* Writers that do not track the shell's scrollback generation replace the
     * whole content, so each one gets a fresh (always-different) epoch and the
     * next apply rebuilds spans from scratch. */
    static uint32_t s_external_epoch = 0x80000000u;

    if (text == NULL) {
        text = "";
    }
    windows_set_transcript_text_len(text, strlen(text), ++s_external_epoch);
}

void windows_set_transcript_text_len(const char *text, size_t len, uint32_t epoch)
{
    if (text == NULL) {
        text = "";
        len = 0;
    }
    if (len >= P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES) {
        len = P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES - 1;
    }

    /* Stage the raw ANSI text for the deferred span render. This takes only
     * the staging mutex (sub-millisecond copy), never the LVGL port lock: the
     * command worker must not serialize behind a long render pass (bugs.md
     * F26). The UI-down checks live under the same mutex because teardown
     * clears the pointers and frees the buffer under it too. The apply timer
     * consumes the copy on the LVGL task. */
    windows_stage_lock();
    if (s_windows.transcript == NULL || s_windows.transcript_spans == NULL) {
        windows_stage_unlock();
        return;
    }
    if (!windows_transcript_staging_ensure()) {
        windows_stage_unlock();
        return;
    }
    memcpy(s_transcript_staged, text, len);
    s_transcript_staged[len] = '\0';
    s_transcript_staged_len = len;
    s_transcript_staged_epoch = epoch;
    windows_stage_unlock();

    windows_transcript_schedule_apply();
}

void windows_scroll_transcript_to_end(void)
{
    if (s_windows.transcript == NULL) {
        return;
    }

    /* Repaint from the staged text, which repaints and scrolls to the end.
     * Runs on the LVGL task via the deferred apply. */
    windows_transcript_schedule_apply();
}

void windows_force_scroll_transcript_to_end(void)
{
    if (s_windows.transcript == NULL) {
        return;
    }

    /* The next apply pins the view to the bottom regardless of the current
     * position, so the output of a just-submitted command is always visible.
     * The flag is cleared when that apply runs. */
    s_transcript_force_follow = true;
    windows_transcript_schedule_apply();
}

void windows_scroll_transcript_by(int32_t pixels)
{
    lv_obj_t *container = s_windows.transcript;

    if (container == NULL) {
        return;
    }

    /* Positive pixels scroll toward newer output (scroll.y increases), negative
     * toward older output. The bounded variant clamps to the scroll range. */
    lv_obj_scroll_by_bounded(container, 0, pixels, LV_ANIM_OFF);
}

void windows_scroll_transcript_to_top(void)
{
    lv_obj_t *container = s_windows.transcript;

    if (container == NULL) {
        return;
    }

    lv_obj_scroll_to_y(container, 0, LV_ANIM_OFF);
}

/**
 * Apply the transcript's computed region height.
 *
 * The transcript container must have an explicit, bounded height (not
 * LV_SIZE_CONTENT) so its spacer overflows the container and becomes
 * vertically scrollable. When the slot changes (keyboard visibility, rotation
 * rebuild) the child's wrap width may change too, so the rows are re-measured
 * and the materialized window rebuilt. Runs on the LVGL task.
 */
void windows_apply_transcript_height(void)
{
    lv_obj_t *container = s_windows.transcript;

    if (container == NULL) {
        return;
    }

    lv_obj_set_height(container, windows_get_rect(WINDOW_REGION_TRANSCRIPT).height);
    /* Lay out first so the span group's wrap width is current, re-measure when
     * it changed, then rebuild the materialized window and lay out again to
     * refresh the scroll range. */
    lv_obj_update_layout(container);
    windows_transcript_sync_metrics();
    windows_transcript_render_window(false, true);
    lv_obj_update_layout(container);
}

lv_obj_t *windows_get_input_line(void)
{
    return s_windows.input_line;
}

lv_obj_t *windows_get_keyboard(void)
{
    return keyboard_get_widget();
}

lv_obj_t *windows_get_prev_button(void)
{
    return s_windows.prev_button;
}

lv_obj_t *windows_get_next_button(void)
{
    return s_windows.next_button;
}

lv_obj_t *windows_get_scroll_up_button(void)
{
    return s_windows.scroll_up_button;
}

lv_obj_t *windows_get_scroll_down_button(void)
{
    return s_windows.scroll_down_button;
}

/** Get the input-row Tab completion button. */
lv_obj_t *windows_get_tab_button(void)
{
    return s_windows.tab_button;
}

/** Get the input-row Stop (foreground-break) button. */
lv_obj_t *windows_get_stop_button(void)
{
    return s_windows.stop_button;
}

bool windows_transcript_is_hidden(void)
{
    /* Shadow flag, safe to read from any task (the shell's append path asks
     * this from the command worker without the LVGL port lock). */
    if (s_windows.transcript_spans == NULL) {
        return true;
    }
    return s_windows.shell_spans_hidden;
}

void windows_shell_spans_set_hidden(bool hidden)
{
    if (s_windows.transcript_spans == NULL) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(s_windows.transcript_spans, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_windows.transcript_spans, LV_OBJ_FLAG_HIDDEN);
    }
    /* The spacer defines the scroll range; hide it with the spans so a stale
     * range cannot swallow scroll input on an overlaid transcript, and restore
     * the range on the way back out. It is never hidden otherwise. */
    if (s_windows.transcript_spacer != NULL) {
        if (hidden) {
            lv_obj_add_flag(s_windows.transcript_spacer, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_windows.transcript_spacer, LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_windows.shell_spans_hidden = hidden;
}

/** Re-apply the Stop visibility rule (must run on the LVGL task). */
static void windows_apply_stop_visibility(void)
{
    bool show;

    if (s_windows.stop_button == NULL) {
        return;
    }
    /* The shell input row hosts Stop; editor/app/TUI modes repurpose or
     * hide the row's widgets, so Stop shows in shell mode only. */
    show = s_windows.stop_wanted && !s_windows.editor_mode &&
           !s_windows.app_mode && !s_windows.tui_mode;
    if (show) {
        lv_obj_remove_flag(s_windows.stop_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_windows.stop_button, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * Request Stop-button visibility (the main poll timer drives this from the
 * command-worker busy state). The mode rule above decides what shows.
 * Must run on the LVGL task.
 */
void windows_set_stop_visible(bool visible)
{
    s_windows.stop_wanted = visible;
    windows_apply_stop_visibility();
}

lv_obj_t *windows_get_input_ghost(void)
{
    return s_windows.input_ghost;
}

lv_obj_t *windows_get_search_label(void)
{
    return s_windows.search_label;
}

lv_obj_t *windows_get_input_row(void)
{
    return s_windows.input_row;
}

lv_obj_t *windows_get_screen(void)
{
    return s_windows.screen;
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

esp_err_t windows_init(void)
{
    lv_obj_t *screen;

    if (s_windows.initialized) {
        ESP_LOGW(WINDOWS_TAG, "Window manager already initialized; deinitializing first");
        windows_deinit();
    }

    screen = lv_screen_active();
    if (screen == NULL) {
        ESP_LOGE(WINDOWS_TAG, "No active LVGL screen");
        return ESP_FAIL;
    }

    /* Font registry first: every surface below resolves its role font here. */
    font_init();

    /* Transcript staging mutex: created once, survives rebuilds (teardown
     * never deletes it, so no task can race its creation). */
    if (s_transcript_stage_lock == NULL) {
        s_transcript_stage_lock = xSemaphoreCreateMutex();
    }

    /* Clean the screen and apply root layout */
    lv_obj_clean(screen);
    s_windows.screen = screen;
    windows_apply_screen_style();

    /* Build all window regions in order */
    windows_create_header();
    windows_create_transcript();
    windows_create_input_row();
    windows_create_keyboard();

    /* Transcript apply pump: a periodic LVGL timer consumed by the staging
     * path's schedule call. Created here (windows_init runs on the LVGL
     * task, inside the port lock) and deleted in windows_deinit before the
     * objects it paints go away (bugs.md F26: replaces the lv_async_call
     * scheduling whose dispatch required the port lock from every worker
     * append). */
    s_windows.shell_spans_hidden = false;
    s_transcript_apply_pending = false;
    s_transcript_apply_timer = lv_timer_create(windows_transcript_apply_timer_cb,
                                               P4_CONFIG_TRANSCRIPT_APPLY_TICK_MS, NULL);

    /* Reflow the transcript when the on-screen keyboard is shown/hidden, so
     * hiding the OSK expands the transcript into the freed space. This wires
     * the otherwise-dead visibility callback. */
    keyboard_register_visibility_callback(windows_notify_keyboard_visibility);

    /* Bound the transcript to its computed slot so its content overflows and
     * becomes scrollable. Must run after the keyboard exists (its height
     * factors into the slot). */
    windows_apply_transcript_height();

    s_windows.initialized = true;

    ESP_LOGI(WINDOWS_TAG, "Window manager initialized: %" PRId32 "x%" PRId32,
             (int32_t)windows_get_display_width(),
             (int32_t)windows_get_display_height());

    return ESP_OK;
}

/**
 * Recursively delete all LVGL animations on @p obj and its descendants.
 * Style-transition animations are created on style changes; if the target
 * object is deleted while such an animation is pending, the start callback
 * fires on freed memory and crashes in lv_obj_get_style_prop. Killing every
 * animation before lv_obj_clean() removes that window.
 */
static void windows_kill_animations(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }

    /* Delete every animation that targets this object. */
    lv_anim_del(obj, NULL);

    /* Recurse into children. */
    uint32_t child_cnt = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < child_cnt; i++) {
        windows_kill_animations(lv_obj_get_child(obj, i));
    }
}

void windows_deinit(void)
{
    if (!s_windows.initialized) {
        return;
    }

    /* Deinitialize the header and keyboard before cleaning the screen
     * so they can be re-initialized fresh ΓÇö needed for display rotation
     * support. */
    header_deinit();
    keyboard_deinit();

    /* Detach the visibility callback (the keyboard is being torn down). */
    keyboard_register_visibility_callback(NULL);

    /* Stop the transcript apply pump before the widgets disappear; the
     * staging path degrades to no-op scheduling until the next windows_init
     * recreates it (a timer tick against cleaned objects must never run). */
    if (s_transcript_apply_timer != NULL) {
        lv_timer_delete(s_transcript_apply_timer);
        s_transcript_apply_timer = NULL;
    }
    s_transcript_apply_pending = false;

    if (s_windows.screen != NULL) {
        /* Kill every pending animation on the screen and its descendants
         * BEFORE deleting any object, so no transition start callback can
         * fire on freed memory afterwards. */
        windows_kill_animations(s_windows.screen);
        lv_obj_clean(s_windows.screen);
    }

    s_windows.screen = NULL;
    s_windows.transcript = NULL;
    s_windows.transcript_spans = NULL;
    s_windows.transcript_spacer = NULL;
    s_windows.transcript_measure = NULL;
    s_windows.input_row = NULL;
    s_windows.input_line = NULL;
    s_windows.prev_button = NULL;
    s_windows.prev_label = NULL;
    s_windows.next_button = NULL;
    s_windows.next_label = NULL;
    s_windows.scroll_up_button = NULL;
    s_windows.up_label = NULL;
    s_windows.scroll_down_button = NULL;
    s_windows.down_label = NULL;
    s_windows.tab_button = NULL;
    s_windows.tab_label = NULL;
    s_windows.stop_button = NULL;
    s_windows.stop_label = NULL;
    s_windows.stop_wanted = false;
    /* Reset the surface-mode state too: a rotation rebuild deinits and
     * reinits, and a stale tui_mode/editor_mode would hand back dangling
     * surface pointers to the next session. */
    s_windows.input_ghost = NULL;
    s_windows.search_label = NULL;
    s_windows.editor_mode = false;
    s_windows.editor_surface = NULL;
    s_windows.editor_status = NULL;
    s_windows.app_mode = false;
    s_windows.tui_mode = false;
    s_windows.tui_surface = NULL;
    s_windows.fullscreen = false;
    s_windows.initialized = false;

    /* Release the PSRAM staging buffers so a rebuild starts clean. They are
     * re-allocated on the next transcript creation. */
    windows_transcript_staging_free();
}

bool windows_is_initialized(void)
{
    return s_windows.initialized;
}

void windows_show_boot_banner(const char *message)
{
    if (s_windows.transcript == NULL || message == NULL) {
        return;
    }

    /* Render the banner via the label-with-recolor transcript. ANSI codes in
     * the message are converted to recolor markup and set directly. */
    windows_set_transcript_text(message);
}

void windows_reset_input_line(const char *prompt)
{
    if (s_windows.input_line == NULL) {
        return;
    }

    lv_textarea_set_text(s_windows.input_line, prompt != NULL ? prompt : "");
    if (prompt != NULL && strlen(prompt) > 0) {
        lv_textarea_set_cursor_pos(s_windows.input_line, (int32_t)strlen(prompt));
    }
}

void windows_notify_keyboard_visibility(bool visible)
{
    (void)visible;

    /* The keyboard fires this after it releases the port lock, from the
     * command worker; every call below touches LVGL widgets/layout, so take
     * the (recursive) port lock here. */
    if (!lvgl_port_lock(0)) {
        return;
    }

    /* The screen is a flex column; the header, transcript, input row and
     * keyboard are already positioned by flex in creation order. Do NOT set
     * manual y coordinates on any flex child ΓÇö that fights the flex layout
     * and throws the keyboard to the top with a black void below. The only
     * region that needs re-bounding when the keyboard shows/hides is the
     * transcript, whose height is explicit (not flex-grow), so it must be
     * re-applied to fill the space the keyboard just used or freed. */
    if (s_windows.transcript != NULL) {
        windows_apply_transcript_height();
        lv_obj_update_layout(s_windows.transcript);
    }

    if (s_windows.editor_mode) {
        /* The editor surface tracks the transcript slot explicitly; resize it
         * so it fills the space freed/used by the keyboard, using the same
         * rect computation as the shell transcript. */
        windows_refresh_editor_surface();
        if (!windows_editor_surface_height_ok()) {
            ESP_LOGW(WINDOWS_TAG, "editor surface collapsed to %d px",
                     (int)lv_obj_get_height(s_windows.editor_surface));
        }
    }

    if (s_windows.tui_mode) {
        windows_refresh_tui_surface();
        if (!windows_tui_surface_height_ok()) {
            ESP_LOGW(WINDOWS_TAG, "tui surface collapsed to %d px",
                     (int)lv_obj_get_height(s_windows.tui_surface));
        }
    }

    /* An app surface (TUI/gfx) must re-map its cells/scale to the slot the
     * keyboard just freed or used. */
    if (s_windows.app_surface_active) {
        windows_refresh_app_surface();
    }

    lvgl_port_unlock();
}

/* ========================================================================
 * EDITOR MODE
 * ======================================================================== */

lv_obj_t *windows_get_editor_surface(void)
{
    return s_windows.editor_surface;
}

/** Re-apply the transcript-region height to the editor surface. */
void windows_refresh_editor_surface(void)
{
    if (!s_windows.editor_mode || s_windows.editor_surface == NULL) {
        return;
    }
    lv_obj_set_height(s_windows.editor_surface,
                      windows_get_rect(WINDOW_REGION_TRANSCRIPT).height);
    lv_obj_update_layout(s_windows.editor_surface);
}

/** Report whether the editor surface is at least as tall as its region. */
bool windows_editor_surface_height_ok(void)
{
    if (!s_windows.editor_mode || s_windows.editor_surface == NULL) {
        return false;
    }
    return lv_obj_get_height(s_windows.editor_surface) >=
           windows_get_rect(WINDOW_REGION_TRANSCRIPT).height;
}

/** Log the editor surface, transcript-region, and keyboard rectangles. */
void windows_debug_editor_layout(void)
{
    window_rect_t region = windows_get_rect(WINDOW_REGION_TRANSCRIPT);
    lv_coord_t surface_h = s_windows.editor_surface != NULL
                               ? lv_obj_get_height(s_windows.editor_surface)
                               : 0;
    lv_coord_t kb_h = keyboard_is_visible() ? keyboard_get_height() : 0;

    /* A healthy layout is the normal case and must not spam the serial log
     * (the firmware logs at WARN by default). Only a collapsed surface ΓÇö the
     * real failure this guards against ΓÇö is worth a warning; the full geometry
     * is available at debug level. */
    if (surface_h < region.height) {
        ESP_LOGW(WINDOWS_TAG,
                 "editor layout diagnostic: surface h=%d px, transcript region "
                 "h=%d px (y=%d), keyboard h=%d px",
                 (int)surface_h, (int)region.height, (int)region.y, (int)kb_h);
    } else {
        ESP_LOGD(WINDOWS_TAG,
                 "editor layout: surface h=%d px, transcript region h=%d px "
                 "(y=%d), keyboard h=%d px",
                 (int)surface_h, (int)region.height, (int)region.y, (int)kb_h);
    }
}

/**
 * Enter modal editor mode. Hides the shell input surface (transcript, input
 * row buttons and line) and creates a full-height editor surface in the
 * transcript region. The input row becomes a status bar.
 *
 * @return The editor surface container, or NULL on failure. LVGL task.
 */
lv_obj_t *windows_enter_editor_mode(void)
{
    if (s_windows.editor_mode) {
        return s_windows.editor_surface;
    }
    if (s_windows.screen == NULL) {
        return NULL;
    }
    if (s_windows.tui_mode) {
        tui_hide_for_modal();
    }

    /* Hide the shell input widgets. The transcript container is NOT hidden:
     * it becomes the editor surface, so it must stay visible and keep the
     * transcript-region height (hiding it makes LVGL flex skip it, collapsing
     * the editor area to ~one line and dragging the keyboard up under it). */
    if (s_windows.prev_button != NULL) {
        lv_obj_add_flag(s_windows.prev_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.next_button != NULL) {
        lv_obj_add_flag(s_windows.next_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.scroll_up_button != NULL) {
        lv_obj_add_flag(s_windows.scroll_up_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.scroll_down_button != NULL) {
        lv_obj_add_flag(s_windows.scroll_down_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.tab_button != NULL) {
        lv_obj_add_flag(s_windows.tab_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.input_line != NULL) {
        lv_obj_add_flag(s_windows.input_line, LV_OBJ_FLAG_HIDDEN);
    }

    /* The editor renders into the EXISTING transcript widget (proven,
     * working scrollable ANSI spangroup). The transcript container becomes
     * the editor surface; the input row becomes a status bar. */
    s_windows.editor_surface = s_windows.transcript;

    /* Status bar: a label in the (now content-free) input row. */
    if (s_windows.input_row != NULL) {
        s_windows.editor_status = lv_label_create(s_windows.input_row);
        lv_obj_set_width(s_windows.editor_status, LV_PCT(100));
        lv_obj_set_style_text_font(s_windows.editor_status,
                                   windows_get_terminal_font(), 0);
        lv_obj_set_style_text_color(s_windows.editor_status,
                                    windows_get_color(WINDOWS_COLOR_TEXT_MUTED), 0);
    }

    s_windows.editor_mode = true;
    windows_apply_stop_visibility();

    /* Size the editor surface to exactly the transcript slot and verify it
     * did not collapse (a regression guard for the "1-line editor" bug). */
    windows_refresh_editor_surface();
    if (!windows_editor_surface_height_ok()) {
        ESP_LOGW(WINDOWS_TAG, "editor surface collapsed to %d px",
                 (int)lv_obj_get_height(s_windows.editor_surface));
    }
    windows_debug_editor_layout();

    return s_windows.editor_surface;
}

/** Leave modal editor mode and restore the shell input surface. LVGL task. */
void windows_exit_editor_mode(void)
{
    if (!s_windows.editor_mode) {
        return;
    }
    if (s_windows.tui_mode) {
        tui_show_after_modal();
    }

    /* The editor surface aliases the transcript container; do not delete it.
     * The transcript was kept visible for the session; re-apply the region
     * height and re-show the shell output on the way out. */
    s_windows.editor_surface = NULL;

    if (s_windows.transcript != NULL) {
        lv_obj_remove_flag(s_windows.transcript, LV_OBJ_FLAG_HIDDEN);
        windows_apply_transcript_height();
    }

    if (s_windows.editor_status != NULL) {
        lv_obj_delete(s_windows.editor_status);
        s_windows.editor_status = NULL;
    }

    if (s_windows.prev_button != NULL) {
        lv_obj_remove_flag(s_windows.prev_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.next_button != NULL) {
        lv_obj_remove_flag(s_windows.next_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.scroll_up_button != NULL) {
        lv_obj_remove_flag(s_windows.scroll_up_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.scroll_down_button != NULL) {
        lv_obj_remove_flag(s_windows.scroll_down_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.tab_button != NULL) {
        lv_obj_remove_flag(s_windows.tab_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.input_line != NULL) {
        lv_obj_remove_flag(s_windows.input_line, LV_OBJ_FLAG_HIDDEN);
    }

    s_windows.editor_mode = false;
    windows_apply_stop_visibility();
}

/** Get the status-bar label created by windows_enter_editor_mode(). */
lv_obj_t *windows_get_editor_status(void)
{
    return s_windows.editor_status;
}

/** Report whether the editor modal surface is currently active. */
bool windows_editor_mode_active(void)
{
    return s_windows.editor_mode;
}

/**
 * Enter app mode: hide the shell input widgets (input line and the
 * prev/next/scroll buttons) so the transcript becomes a clean full-screen app
 * surface. The on-screen keyboard can still be shown for app input. The
 * transcript container is NOT hidden (LVGL flex skips hidden children, which
 * would collapse the area). LVGL task.
 */
void windows_enter_app_mode(void)
{
    if (s_windows.app_mode || s_windows.screen == NULL) {
        return;
    }
    if (s_windows.prev_button != NULL) {
        lv_obj_add_flag(s_windows.prev_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.next_button != NULL) {
        lv_obj_add_flag(s_windows.next_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.scroll_up_button != NULL) {
        lv_obj_add_flag(s_windows.scroll_up_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.scroll_down_button != NULL) {
        lv_obj_add_flag(s_windows.scroll_down_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.tab_button != NULL) {
        lv_obj_add_flag(s_windows.tab_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.input_line != NULL) {
        lv_obj_add_flag(s_windows.input_line, LV_OBJ_FLAG_HIDDEN);
    }
    s_windows.app_mode = true;
    windows_apply_stop_visibility();
}

/** Leave app mode and restore the shell input widgets. LVGL task. */
void windows_exit_app_mode(void)
{
    if (!s_windows.app_mode) {
        return;
    }
    if (s_windows.prev_button != NULL) {
        lv_obj_remove_flag(s_windows.prev_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.next_button != NULL) {
        lv_obj_remove_flag(s_windows.next_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.scroll_up_button != NULL) {
        lv_obj_remove_flag(s_windows.scroll_up_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.scroll_down_button != NULL) {
        lv_obj_remove_flag(s_windows.scroll_down_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.tab_button != NULL) {
        lv_obj_remove_flag(s_windows.tab_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_windows.input_line != NULL) {
        lv_obj_remove_flag(s_windows.input_line, LV_OBJ_FLAG_HIDDEN);
    }
    s_windows.app_mode = false;
    windows_apply_stop_visibility();
}

/* ========================================================================
 * APP SURFACE (foreground TUI cell buffer / gfx canvas viewport)
 * ======================================================================== */

window_rect_t windows_get_app_viewport(void)
{
    return windows_get_rect(WINDOW_REGION_TRANSCRIPT);
}

bool windows_app_surface_active(void)
{
    return s_windows.app_surface_active;
}

void windows_set_surface_layout_cb(void (*cb)(void))
{
    s_windows.app_surface_layout_cb = cb;
}

void windows_refresh_app_surface(void)
{
    if (!s_windows.app_surface_active || s_windows.transcript == NULL) {
        return;
    }
    if (s_windows.app_surface_layout_cb != NULL) {
        s_windows.app_surface_layout_cb();
    }
    lv_obj_update_layout(s_windows.transcript);
    /* Pin the surface to the content origin: the scrollback may be parked at
     * its bottom, which would draw the app surface above the viewport. */
    lv_obj_scroll_to_y(s_windows.transcript, 0, LV_ANIM_OFF);
}

lv_obj_t *windows_enter_app_surface(void)
{
    if (s_windows.app_surface_active) {
        return s_windows.transcript;
    }
    if (s_windows.screen == NULL || s_windows.transcript == NULL) {
        return NULL;
    }
    if (s_windows.transcript_spans != NULL) {
        windows_shell_spans_set_hidden(true);
    }
    /* Drop the transcript padding so the app surface gets the full region; the
     * shell transcript keeps its inset when the surface exits. */
    s_windows.app_surface_saved_pad =
        lv_obj_get_style_pad_top(s_windows.transcript, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_windows.transcript, 0, LV_PART_MAIN);
    s_windows.app_surface_active = true;
    /* Free the ~240 px the on-screen keyboard occupies for the app; restore it
     * on exit if it was visible. */
    s_windows.app_surface_kb_was_visible = keyboard_is_visible();
    if (s_windows.app_surface_kb_was_visible) {
        keyboard_hide();
    }
    windows_apply_stop_visibility();
    windows_refresh_app_surface();
    return s_windows.transcript;
}

void windows_exit_app_surface(void)
{
    if (!s_windows.app_surface_active) {
        return;
    }
    s_windows.app_surface_active = false;
    s_windows.app_surface_layout_cb = NULL;
    if (s_windows.transcript != NULL) {
        lv_obj_set_style_pad_all(s_windows.transcript,
                                 s_windows.app_surface_saved_pad, LV_PART_MAIN);
    }
    if (s_windows.transcript_spans != NULL) {
        windows_shell_spans_set_hidden(false);
    }
    if (s_windows.transcript != NULL) {
        windows_apply_transcript_height();
    }
    /* Catch up the span group in one pass: while the surface was active the
     * transcript applies were suspended (F25), so the staged text advanced
     * past the rendered prefix. */
    windows_transcript_schedule_apply();
    if (s_windows.app_surface_kb_was_visible) {
        keyboard_show();
        s_windows.app_surface_kb_was_visible = false;
    }
    windows_apply_stop_visibility();
}

/* ========================================================================
 * TUI MODE
 * ======================================================================== */

lv_obj_t *windows_enter_tui_mode(void)
{
    if (s_windows.tui_mode) return s_windows.tui_surface;
    if (s_windows.screen == NULL) return NULL;
    if (s_windows.editor_mode) return NULL;
    if (windows_enter_app_surface() == NULL) return NULL;
    s_windows.tui_surface = s_windows.transcript;
    s_windows.tui_mode = true;
    windows_apply_stop_visibility();
    windows_refresh_tui_surface();
    return s_windows.tui_surface;
}

void windows_exit_tui_mode(void)
{
    if (!s_windows.tui_mode) return;
    s_windows.tui_surface = NULL;
    s_windows.tui_mode = false;
    windows_exit_app_surface();
    if (s_windows.transcript) lv_obj_remove_flag(s_windows.transcript, LV_OBJ_FLAG_HIDDEN);
    windows_apply_stop_visibility();
}

bool windows_tui_mode_active(void) { return s_windows.tui_mode; }
lv_obj_t *windows_get_tui_surface(void) { return s_windows.tui_surface; }

void windows_refresh_tui_surface(void)
{
    if (!s_windows.tui_mode || !s_windows.tui_surface) return;
    lv_obj_set_height(s_windows.tui_surface, windows_get_rect(WINDOW_REGION_TRANSCRIPT).height);
    lv_obj_update_layout(s_windows.tui_surface);
}

bool windows_tui_surface_height_ok(void)
{
    if (!s_windows.tui_mode || !s_windows.tui_surface) return false;
    return lv_obj_get_height(s_windows.tui_surface) >= windows_get_rect(WINDOW_REGION_TRANSCRIPT).height;
}

void windows_set_fullscreen(bool fullscreen)
{
    if (s_windows.fullscreen == fullscreen) return;
    s_windows.fullscreen = fullscreen;
    header_set_visible(!fullscreen);
    if (s_windows.input_row) {
        if (fullscreen) lv_obj_add_flag(s_windows.input_row, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_windows.input_row, LV_OBJ_FLAG_HIDDEN);
    }
    // Keyboard auto-hidden in fullscreen; will be restored on exit if needed
    if (fullscreen) {
        extern void keyboard_hide(void);
        keyboard_hide();
    }
    if (s_windows.transcript) {
        windows_apply_transcript_height();
        lv_obj_update_layout(s_windows.transcript);
    }
    if (s_windows.tui_mode) windows_refresh_tui_surface();
    if (s_windows.editor_mode) windows_refresh_editor_surface();
    if (s_windows.app_surface_active) windows_refresh_app_surface();
}

bool windows_is_fullscreen(void) { return s_windows.fullscreen; }

void windows_set_editor_focus(bool focus)
{
    /* Writerdeck focus mode: hide the header + on-screen keyboard and give the
     * space to the editor surface, mirroring windows_set_fullscreen() but
     * keeping the input row (the editor status bar with the word count) and
     * WITHOUT a full UI rebuild ΓÇö a rebuild would tear down the open editor. */
    header_set_visible(!focus);
    if (focus) {
        keyboard_hide();
    } else {
        /* Touch users need the OSK back to type; USB auto-detect may re-hide
         * it when an external keyboard is attached. */
        keyboard_show();
    }
    if (s_windows.transcript) {
        windows_apply_transcript_height();
        lv_obj_update_layout(s_windows.transcript);
    }
    if (s_windows.editor_mode) {
        windows_refresh_editor_surface();
    }
}

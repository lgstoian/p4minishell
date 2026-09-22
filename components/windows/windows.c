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
    /* The transcript is a scrollable CONTAINER holding a span group. The
     * spangroup must not be the scrollable object itself: a spangroup clips its
     * own spans to its widget height (LV_SPAN_OVERFLOW_CLIP), so a bounded
     * spangroup reports no overflow and can never scroll. Instead the container
     * owns the fixed height, background and padding, and the span group child
     * is sized to exactly its wrapped content height by
     * windows_transcript_update_content_size() on every change. The container
     * then sees a taller-than-itself child and scrolls through it.
     *
     * The actual widget update (rebuild spans, size the child, force layout,
     * scroll to end) is DEFERRED to the LVGL task through the coalesced apply timer (see
     * windows_transcript_schedule_apply): rebuilding spans synchronously from a
     * non-LVGL task races with the LVGL render cycle and hangs
     * lv_timer_handler on the LVGL task, freezing the whole UI. The apply is
     * coalesced so bursty output paints once per handler pass.
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

    s_windows.transcript_spans = lv_spangroup_create(s_windows.transcript);
    lv_obj_set_width(s_windows.transcript_spans, LV_PCT(100));
    /* The span group's height is set explicitly to its wrapped content height
     * (windows_transcript_update_content_size) - never LV_SIZE_CONTENT. A
     * content-sized child inside a scrollable container is a known LVGL hazard:
     * every layout pass recomputes the self size, and the spangroup's
     * SIZE_CHANGED -> lv_spangroup_refresh() -> refresh_self_size() chain keeps
     * marking the screen layout dirty, so lv_obj_update_layout() never exits
     * its scr->scr_layout_inv loop and the LVGL task spins forever, freezing
     * the whole UI. An explicit height keeps the widget in LV_SPAN_MODE_FIXED
     * where the self size always equals the widget size, so layout converges
     * in one pass while the container still scrolls through the taller
     * child. */
    lv_obj_set_height(s_windows.transcript_spans, 0);
    lv_obj_clear_flag(s_windows.transcript_spans, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_windows.transcript_spans, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_windows.transcript_spans, 0, 0);
    lv_obj_set_style_pad_all(s_windows.transcript_spans, 0, 0);
    lv_obj_set_style_text_font(s_windows.transcript_spans, font, 0);
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
    lv_obj_t *input = s_windows.input_line;

    if (input == NULL) {
        return;
    }
    if (!lvgl_port_lock(0)) {
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
    lv_obj_t *input = s_windows.input_line;

    if (input == NULL) {
        return;
    }
    if (!lvgl_port_lock(0)) {
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
static int32_t s_transcript_content_height = 0;
static int32_t s_transcript_pending_height_delta = 0;

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
    s_transcript_staged = p4heap_alloc_psram(P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES);
    if (s_transcript_staged != NULL) {
        s_transcript_staged[0] = '\0';
    }
    return s_transcript_staged != NULL;
}

/** Release the PSRAM staging buffers (windows_deinit). The staging mutex
 *  serializes this against any in-flight copy on the command worker; it is
 *  created with the UI and outlives rebuilds. */
static void windows_transcript_staging_free(void)
{
    windows_stage_lock();
    if (s_transcript_staged != NULL) {
        heap_caps_free(s_transcript_staged);
        s_transcript_staged = NULL;
    }
    s_transcript_staged_len = 0;
    s_transcript_staged_epoch = 0;
    s_transcript_rendered_len = 0;
    s_transcript_rendered_epoch = 0;
    s_transcript_last_fg = 0;
    s_transcript_seen_fg = 0;
    windows_stage_unlock();
    s_transcript_apply_pending = false;
}

/**
 * Segment callback for ansi_process_text(): append one coloured span holding
 * @p text with the ANSI state's foreground colour. Runs on the LVGL task from
 * windows_transcript_apply().
 */
static void windows_span_segment(const char *text, const ansi_state_t *state, void *user_data)
{
    lv_obj_t *group = (lv_obj_t *)user_data;
    lv_span_t *span;
    lv_style_t *style;

    if (state != NULL) {
        s_transcript_seen_fg = state->fg_color;
    }
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
 * Size the transcript span group to exactly its wrapped content height.
 *
 * The child of the scrollable transcript container must have an explicit pixel
 * height (never LV_SIZE_CONTENT): see windows_create_transcript() for why a
 * content-sized child makes the LVGL layout pass loop forever. The span group
 * stays in LV_SPAN_MODE_FIXED, where GET_SELF_SIZE returns the widget's own
 * size, so growing the content invalidates nothing by itself and layout
 * converges in a single pass.
 *
 * lv_spangroup_get_expand_height() returns the exact height that the span
 * draw code fills (sum of line heights minus one line-space), so a widget
 * sized to it draws every line without clipping or trailing blank space.
 */
static void windows_transcript_update_content_size(lv_obj_t *spans)
{
    int32_t width = lv_obj_get_content_width(spans);
    if (width <= 0 && s_windows.transcript != NULL) {
        width = lv_obj_get_content_width(s_windows.transcript);
    }

    /* Incremental height update: add the pending delta to the tracked height.
     * This avoids the O(retained) lv_spangroup_get_expand_height() call. */
    s_transcript_content_height += s_transcript_pending_height_delta;
    s_transcript_pending_height_delta = 0;

    if (s_transcript_content_height <= 0 && s_windows.transcript != NULL) {
        /* Fallback: compute full height if we don't have a valid cached value. */
        int32_t width = lv_obj_get_content_width(spans);
        if (width <= 0 && s_windows.transcript != NULL) {
            width = lv_obj_get_content_width(s_windows.transcript);
        }
        s_transcript_content_height = lv_spangroup_get_expand_height(spans, width);
    }

    lv_obj_set_height(spans, s_transcript_content_height);
}

/**
 * Paint the staged raw ANSI text onto the transcript span group and scroll to
 * the end. Runs ONLY on the LVGL task (the coalescing apply timer callback in
 * the handler pass), so it is inherently serialized with the render cycle.
 * The staged buffer is read under its own mutex, which the command worker may
 * hold briefly while copying; everything expensive (span parse, re-wrap,
 * layout) happens after that mutex is released.
 *
 * To avoid flicker, the apply renders INCREMENTALLY: when the staged buffer
 * still carries the scrollback generation already turned into spans, only the
 * newly appended fragment is parsed into new spans and existing spans are left
 * untouched. A full teardown/rebuild happens when the epochs differ (the shell
 * dropped the oldest bytes: reset, trim, or buffer-full truncation).
 */
static void windows_transcript_apply(void)
{
    lv_obj_t *container = s_windows.transcript;
    lv_obj_t *spans = s_windows.transcript_spans;
    size_t new_len;
    size_t append_len = 0;
    char *fragment = NULL;
    int carry_code = -1;
    uint32_t default_fg;

    if (container == NULL || spans == NULL) {
        return;
    }

    /* While an app surface (gfx canvas, fullscreen app) owns the transcript,
     * the span group is hidden and its exact height is irrelevant. Skip the
     * reconcile entirely: re-wrapping the whole scrollback
     * (`lv_spangroup_get_expand_height`, O(all spans)) on every command was
     * the dominant per-frame cost of a gfx animation (bugs.md F25). The staged
     * text keeps accumulating; `windows_exit_app_surface()` schedules one
     * catch-up apply that appends the whole delta in a single pass. */
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
    bool follow_bottom = !s_windows.editor_mode && !s_windows.app_surface_active &&
                         (s_transcript_force_follow ||
                          lv_obj_get_scroll_bottom(container) < P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX);
    s_transcript_force_follow = false;

    /* Snapshot the staged delta under the staging mutex. Only the staged
     * bytes and the render bookkeeping are touched here; the expensive work
     * below (span parse, re-wrap, layout) runs unlocked against the private
     * fragment copy. Lock order is PORT outer, STAGE inner: apply runs on the
     * LVGL task which holds the port lock, and the worker never takes the
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
     * moved) or the staged text somehow shrank. Rebuild from scratch. */
    if (s_transcript_rendered_epoch != s_transcript_staged_epoch ||
        s_transcript_rendered_len > new_len) {
        windows_transcript_clear_spans(spans);
        s_transcript_rendered_len = 0;
        s_transcript_last_fg = 0;
    }

    if (new_len > s_transcript_rendered_len) {
        const char *append_start = s_transcript_staged + s_transcript_rendered_len;
        uint32_t head = 0;

        append_len = new_len - s_transcript_rendered_len;

        /* If the new bytes begin mid-colour (the previous append left the SGR
         * state non-default and the fragment does not open with its own SGR
         * sequence), re-emit the carried colour so the first span matches. The
         * fragment is heap-allocated because it can exceed the LVGL task stack. */
        default_fg = ansi_get_default_fg();
        if (s_transcript_last_fg != 0 && s_transcript_last_fg != default_fg &&
            append_start[0] != '\x1B') {
            carry_code = windows_ansi_sgr_for_color(s_transcript_last_fg);
        }
        if (carry_code > 0) {
            head = 10; /* room for "\x1B[NNdm" */
        }

        fragment = malloc(append_len + head + 1);
        if (fragment != NULL) {
            if (carry_code > 0) {
                int n = snprintf(fragment, head + 1, "\x1B[%dm", carry_code);
                if (n < 0) {
                    n = 0;
                }
                memcpy(fragment + n, append_start, append_len + 1);
            } else {
                memcpy(fragment, append_start, append_len + 1);
            }
            /* Claim the bytes now: the next staging pass must not be told a
             * smaller rendered length than the delta we already copied. */
            s_transcript_rendered_len = new_len;
            s_transcript_rendered_epoch = s_transcript_staged_epoch;
        } else {
            ESP_LOGW(WINDOWS_TAG, "transcript: out of memory for fragment");
        }
    }
    windows_stage_unlock();

    if (fragment != NULL) {
        s_transcript_seen_fg = 0;
        ansi_process_text(fragment, windows_span_segment, spans);
        if (s_transcript_seen_fg != 0) {
            s_transcript_last_fg = s_transcript_seen_fg;
        }
        free(fragment);
    }

    /* Calculate the height delta for the newly added spans. We measure the
     * height of the span group before and after adding the new spans to get
     * the incremental height delta, avoiding O(retained) get_expand_height calls. */
    if (new_len > s_transcript_rendered_len) {
        int32_t old_height = lv_obj_get_height(spans);
        windows_transcript_update_content_size(spans);
        int32_t new_height = lv_obj_get_height(spans);
        s_transcript_pending_height_delta += (new_height - old_height);
    } else {
        /* Even if no new text, the span bounds might have changed due to
         * span deletions from trimming. Recompute height delta. */
        int32_t old_height = lv_obj_get_height(spans);
        windows_transcript_update_content_size(spans);
        int32_t new_height = lv_obj_get_height(spans);
        s_transcript_pending_height_delta += (new_height - old_height);
    }

    /* Bound the retained span group: the newest spans are kept, the oldest
     * dropped. Without this every append re-wraps the whole scrollback
     * (`windows_transcript_update_content_size`), which degrades to hundreds
     * of ms per command after a long session (bugs.md F25). The rendered-length
     * bookkeeping is intentionally left at the full staged length, so the next
     * append stays incremental. */
    {
        uint32_t span_count = lv_spangroup_get_span_count(spans);

        while (span_count > P4_CONFIG_TRANSCRIPT_MAX_SPANS) {
            lv_span_t *oldest = lv_spangroup_get_child(spans, 0);

            if (oldest == NULL) {
                break;
            }
            lv_spangroup_delete_span(spans, oldest);
            span_count--;
        }
    }

    /* Incremental height update: apply the accumulated height delta to the
     * span group, then clear the pending delta. This avoids the O(retained)
     * lv_spangroup_get_expand_height() call. The container layout will be
     * refreshed on the next LVGL render pass or when follow_bottom requires
     * an immediate scroll. */
    if (s_transcript_pending_height_delta != 0) {
        int32_t new_height = lv_obj_get_height(spans) + s_transcript_pending_height_delta;
        if (new_height < 0) new_height = 0;
        lv_obj_set_height(spans, new_height);
        s_transcript_pending_height_delta = 0;
    }

    /* Force layout only when follow_bottom requires immediate scroll sync.
     * Otherwise the LVGL render pass will pick up the height change naturally. */
    if (follow_bottom) {
        lv_obj_update_layout(container);
        lv_obj_scroll_to_y(container, LV_COORD_MAX, LV_ANIM_OFF);
    }
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
 * Drop the oldest quarter of the rendered scrollback and free its spans.
 *
 * Runs under memory pressure (see shell_transcript_guard_internal): the
 * accumulated span objects live in the internal heap, so this reclaims them
 * synchronously before a command prints. The caller must hold the LVGL port
 * lock.
 *
 * Only the OLDEST quarter of spans is deleted, so the newest content stays
 * on screen without an empty-frame flash. The staged buffer is NOT modified
 * here - it will be synced from the main transcript buffers (which already
 * contain the trim marker) on the next append via
 * windows_set_transcript_text_len(). The render bookkeeping is reset so the
 * next apply reconciles spans against the new staged text in one pass.
 */
void windows_transcript_trim(void)
{
    lv_obj_t *spans = s_windows.transcript_spans;
    uint32_t span_count;
    uint32_t drop_spans;
    uint32_t index;

    windows_stage_lock();
    if (spans == NULL || !windows_transcript_staging_ensure()) {
        windows_stage_unlock();
        return;
    }

    /* Free roughly the oldest quarter of spans immediately (this is the
     * internal-heap reclamation the guard needs), leaving the newest spans
     * visible so the screen never flashes empty. */
    span_count = lv_spangroup_get_span_count(spans);
    drop_spans = span_count / 4;
    for (index = 0; index < drop_spans; index++) {
        lv_span_t *span = lv_spangroup_get_child(spans, 0);

        if (span == NULL) {
            break;
        }
        lv_spangroup_delete_span(spans, span);
    }

s_transcript_rendered_len = 0;
    s_transcript_last_fg = 0;
    s_transcript_seen_fg = 0;
    /* Force a clean rebuild from staged text: the retained spans no longer
     * match its head after the drop. The staged buffer itself is untouched
     * here (the guard re-stages the trimmed text before calling this).
     * UINT32_MAX never equals a real shell epoch (0xFFFFFFFF trims would need
     * ~270 years at the trim rate). */
    s_transcript_rendered_epoch = UINT32_MAX;
    windows_stage_unlock();

    /* Incremental height update: the span group is shorter now, but we don't
     * know the exact new height without a full re-wrap. Set a sentinel to
     * force a full re-measure on the next apply pass. */
    s_transcript_pending_height_delta = INT32_MIN;
    /* No layout update here - the LVGL render pass will pick up the change. */
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
 * LV_SIZE_CONTENT) so its span content overflows the container and becomes
 * vertically scrollable. When the slot changes (keyboard visibility, rotation
 * rebuild) the child's wrap width may change too, so the span group is re-sized
 * to its content height and one layout pass is forced. Runs on the LVGL task.
 */
void windows_apply_transcript_height(void)
{
    lv_obj_t *container = s_windows.transcript;

    if (container == NULL) {
        return;
    }

    lv_obj_set_height(container, windows_get_rect(WINDOW_REGION_TRANSCRIPT).height);
    /* Lay out first so the span group's wrap width is current, then re-size it
     * to the new content height and lay out again to refresh the scroll range. */
    lv_obj_update_layout(container);
    if (s_windows.transcript_spans != NULL) {
        windows_transcript_update_content_size(s_windows.transcript_spans);
    }
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
     * so they can be re-initialized fresh — needed for display rotation
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
     * manual y coordinates on any flex child — that fights the flex layout
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
     * (the firmware logs at WARN by default). Only a collapsed surface — the
     * real failure this guards against — is worth a warning; the full geometry
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
     * WITHOUT a full UI rebuild — a rebuild would tear down the open editor. */
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

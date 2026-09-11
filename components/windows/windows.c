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
    bool fullscreen;
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
    .editor_mode = false,
    .editor_surface = NULL,
    .editor_status = NULL,
    .app_mode = false,
    .tui_mode = false,
    .tui_surface = NULL,
    .fullscreen = false,
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
     * is DEFERRED to the LVGL task via lv_async_call (see
     * windows_set_transcript_text): rebuilding spans synchronously from a
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
     * scroll to end) is DEFERRED to the LVGL task via lv_async_call (see
     * windows_set_transcript_text): rebuilding spans synchronously from a
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

    /* Input line textarea */
    s_windows.input_line = lv_textarea_create(s_windows.input_row);
    lv_obj_set_flex_grow(s_windows.input_line, 1);
    lv_obj_set_height(s_windows.input_line, LV_PCT(100));
    lv_textarea_set_one_line(s_windows.input_line, true);
    lv_textarea_set_cursor_click_pos(s_windows.input_line, false);
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
 * stages the raw ANSI text into a persistent buffer and schedules a single
 * lv_async_call; the callback runs on the LVGL task where it rebuilds the
 * spans from the newest staged content and scrolls to the end. Bursts of
 * output coalesce into one apply per handler pass, which also keeps the
 * render cost bounded.
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
/* Staged ANSI text awaiting span conversion, plus the rendered-prefix
 * snapshot used to detect scrollback truncation. Both live in PSRAM (not
 * internal DRAM): at P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES each they would
 * otherwise consume ~2x scrollback of internal heap and force the very
 * memory-pressure trims they exist to survive. Allocated on first transcript
 * creation with an internal-heap fallback; every use NULL-checks. */
static char *s_transcript_staged = NULL;
static char *s_transcript_rendered_prefix = NULL;

/* Incremental-render bookkeeping. s_transcript_rendered_len is the byte count
 * of s_transcript_staged already converted into spans; the prefix snapshot lets
 * the apply detect scrollback truncation (the shell drops the oldest bytes
 * when its buffer fills). s_transcript_last_fg carries the ANSI foreground
 * across an append that splits a colour run, so the next fragment keeps its
 * hue instead of restarting at the default colour. */
static size_t s_transcript_rendered_len = 0;
static uint32_t s_transcript_last_fg = 0;
static uint32_t s_transcript_seen_fg = 0;

/**
 * Ensure the PSRAM staging buffers exist. Called on every staging-buffer use
 * (all callers hold the LVGL port lock or run on the LVGL task, and allocation
 * happens once, so no race). Returns false when neither PSRAM nor the internal
 * fallback could satisfy the request; callers then skip the render update
 * rather than touching a NULL buffer.
 */
static bool windows_transcript_staging_ensure(void)
{
    if (s_transcript_staged != NULL && s_transcript_rendered_prefix != NULL) {
        return true;
    }
    if (s_transcript_staged == NULL) {
        s_transcript_staged = heap_caps_malloc(P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_transcript_staged == NULL) {
            s_transcript_staged = malloc(P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES);
        }
        if (s_transcript_staged != NULL) {
            s_transcript_staged[0] = '\0';
        }
    }
    if (s_transcript_rendered_prefix == NULL) {
        s_transcript_rendered_prefix = heap_caps_malloc(P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_transcript_rendered_prefix == NULL) {
            s_transcript_rendered_prefix = malloc(P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES);
        }
        if (s_transcript_rendered_prefix != NULL) {
            s_transcript_rendered_prefix[0] = '\0';
        }
    }
    return s_transcript_staged != NULL && s_transcript_rendered_prefix != NULL;
}

/** Release the PSRAM staging buffers (windows_deinit). */
static void windows_transcript_staging_free(void)
{
    if (s_transcript_staged != NULL) {
        heap_caps_free(s_transcript_staged);
        s_transcript_staged = NULL;
    }
    if (s_transcript_rendered_prefix != NULL) {
        heap_caps_free(s_transcript_rendered_prefix);
        s_transcript_rendered_prefix = NULL;
    }
    s_transcript_rendered_len = 0;
    s_transcript_last_fg = 0;
    s_transcript_seen_fg = 0;
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
    lv_obj_set_height(spans, lv_spangroup_get_expand_height(spans, width));
}

/**
 * Paint the staged raw ANSI text onto the transcript span group and scroll to
 * the end. Runs on the LVGL task (from the async apply callback) or, in the
 * lv_async_call failure fallback, on the caller's task while holding the LVGL
 * port lock. Both callers serialize with the render cycle, so the staging
 * buffer is never written and read concurrently.
 *
 * To avoid flicker, the apply renders INCREMENTALLY: when the staged buffer
 * still begins with the bytes already turned into spans, only the newly
 * appended fragment is parsed into new spans and existing spans are left
 * untouched. A full teardown/rebuild happens only when the scrollback
 * truncated (the staged content no longer starts with the rendered prefix).
 */
static void windows_transcript_apply(void)
{
    lv_obj_t *container = s_windows.transcript;
    lv_obj_t *spans = s_windows.transcript_spans;
    size_t new_len;
    const char *append_start;
    uint32_t default_fg;

    if (container == NULL || spans == NULL) {
        return;
    }

    /* Staging lives in PSRAM; if it could not be allocated, there is nothing
     * to render (the UART console still received the text). */
    if (!windows_transcript_staging_ensure()) {
        return;
    }

    /* Follow the newest output when already at/near the bottom (so reading
     * earlier history is not yanked down by new output), or when a submitted
     * command requested a forced jump to its output. The force flag is
     * consumed here so it affects only the output of the just-submitted
     * command. */
    bool follow_bottom = s_transcript_force_follow ||
                         lv_obj_get_scroll_bottom(container) < P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX;
    s_transcript_force_follow = false;

    new_len = strlen(s_transcript_staged);

    /* Scrollback truncation: the staged content shrank, or the first rendered
     * bytes no longer match what we already drew. Rebuild from scratch. */
    if (s_transcript_rendered_len > new_len ||
        strncmp(s_transcript_staged, s_transcript_rendered_prefix,
                s_transcript_rendered_len) != 0) {
        windows_transcript_clear_spans(spans);
        s_transcript_rendered_len = 0;
        s_transcript_last_fg = 0;
    }

    append_start = s_transcript_staged + s_transcript_rendered_len;
    if (*append_start != '\0') {
        size_t append_len = strlen(append_start);
        char *fragment = malloc(append_len + 16);
        int carry_code = -1;

        /* If the new bytes begin mid-colour (the previous append left the SGR
         * state non-default and the fragment does not open with its own SGR
         * sequence), re-emit the carried colour so the first span matches. The
         * fragment is heap-allocated because it can exceed the LVGL task stack. */
        default_fg = ansi_get_default_fg();
        if (s_transcript_last_fg != 0 && s_transcript_last_fg != default_fg &&
            append_start[0] != '\x1B') {
            carry_code = windows_ansi_sgr_for_color(s_transcript_last_fg);
        }

        if (fragment != NULL) {
            if (carry_code > 0) {
                snprintf(fragment, append_len + 16, "\x1B[%dm%s", carry_code, append_start);
            } else {
                snprintf(fragment, append_len + 16, "%s", append_start);
            }

            s_transcript_seen_fg = 0;
            ansi_process_text(fragment, windows_span_segment, spans);
            if (s_transcript_seen_fg != 0) {
                s_transcript_last_fg = s_transcript_seen_fg;
            }
            free(fragment);
        } else {
            ESP_LOGW(WINDOWS_TAG, "transcript: out of memory for fragment");
        }
    }

    /* Record how much of the staged buffer is now rendered. */
    s_transcript_rendered_len = new_len;
    snprintf(s_transcript_rendered_prefix, P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES,
             "%s", s_transcript_staged);

    /* Size the child to its new content height, then force one layout pass so
     * the container's scroll range matches, and pin to the bottom if the user
     * was already following. lv_obj_update_layout on the container also lays
     * out its children (the span group). */
    windows_transcript_update_content_size(spans);
    lv_obj_update_layout(container);
    if (follow_bottom) {
        lv_obj_scroll_to_y(container, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void windows_transcript_apply_cb(void *user_data)
{
    (void)user_data;
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
 * contain the trim marker) on the next append via windows_set_transcript_text().
 * The render bookkeeping is reset so the next apply reconciles spans against
 * the new staged text in one pass.
 */
void windows_transcript_trim(void)
{
    lv_obj_t *spans = s_windows.transcript_spans;
    uint32_t span_count;
    uint32_t drop_spans;
    uint32_t index;

    if (!windows_transcript_staging_ensure()) {
        return;
    }

    if (spans == NULL) {
        return;
    }

    span_count = lv_spangroup_get_span_count(spans);

    /* Free roughly the oldest quarter of spans immediately (this is the
     * internal-heap reclamation the guard needs), leaving the newest spans
     * visible so the screen never flashes empty. */
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
    s_transcript_rendered_prefix[0] = '\0';
    windows_transcript_update_content_size(spans);
}

/**
 * Schedule a transcript repaint on the LVGL task. Coalesces: if an apply is
 * already queued, the staging buffer already holds the newest text and the
 * queued callback paints it; no second async call is needed.
 */
static void windows_transcript_schedule_apply(void)
{
    if (s_transcript_apply_pending) {
        return;
    }

    s_transcript_apply_pending = true;
    if (lv_async_call(windows_transcript_apply_cb, NULL) != LV_RESULT_OK) {
        s_transcript_apply_pending = false;
        // Async dispatch failed: run the apply synchronously on the caller's
        // task. Callers hold the LVGL port lock here and the port mutex is
        // recursive, so take it explicitly to match the documented contract
        // ("on the caller's task while holding the LVGL port lock").
        lvgl_port_lock(0);
        windows_transcript_apply();
        lvgl_port_unlock();
    }
}

void windows_set_transcript_text(const char *text)
{
    if (text == NULL) {
        text = "";
    }
    windows_set_transcript_text_len(text, strlen(text));
}

void windows_set_transcript_text_len(const char *text, size_t len)
{
    lv_obj_t *transcript = s_windows.transcript;

    if (transcript == NULL) {
        return;
    }
    if (text == NULL) {
        text = "";
        len = 0;
    }
    if (!windows_transcript_staging_ensure()) {
        return;
    }

    /* Stage the raw ANSI text for the deferred span render. Callers hold the
     * LVGL port lock, so the staging buffer is never written and read
     * concurrently. The staging buffer is twice the ANSI transcript size, so
     * the accumulated scrollback always fits. Copy by known length so no
     * strlen scan of the (up to 64 KB) transcript is needed. */
    if (len >= P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES) {
        len = P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES - 1;
    }
    memcpy(s_transcript_staged, text, len);
    s_transcript_staged[len] = '\0';

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

    /* Clean the screen and apply root layout */
    lv_obj_clean(screen);
    s_windows.screen = screen;
    windows_apply_screen_style();

    /* Build all window regions in order */
    windows_create_header();
    windows_create_transcript();
    windows_create_input_row();
    windows_create_keyboard();

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

    /* Logged at warn level (the firmware's default log floor) so the editor
     * surface geometry is verifiable on the serial console during an edit
     * session. */
    ESP_LOGW(WINDOWS_TAG,
             "editor layout diagnostic: surface h=%d px, transcript region "
             "h=%d px (y=%d), keyboard h=%d px",
             (int)surface_h, (int)region.height, (int)region.y, (int)kb_h);
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
    if (s_windows.input_line != NULL) {
        lv_obj_remove_flag(s_windows.input_line, LV_OBJ_FLAG_HIDDEN);
    }

    s_windows.editor_mode = false;
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
    if (s_windows.input_line != NULL) {
        lv_obj_add_flag(s_windows.input_line, LV_OBJ_FLAG_HIDDEN);
    }
    s_windows.app_mode = true;
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
    if (s_windows.input_line != NULL) {
        lv_obj_remove_flag(s_windows.input_line, LV_OBJ_FLAG_HIDDEN);
    }
    s_windows.app_mode = false;
}

/* ========================================================================
 * TUI MODE
 * ======================================================================== */

lv_obj_t *windows_enter_tui_mode(void)
{
    if (s_windows.tui_mode) return s_windows.tui_surface;
    if (s_windows.screen == NULL) return NULL;
    if (s_windows.editor_mode) return NULL;
    if (s_windows.prev_button) lv_obj_add_flag(s_windows.prev_button, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.next_button) lv_obj_add_flag(s_windows.next_button, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.scroll_up_button) lv_obj_add_flag(s_windows.scroll_up_button, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.scroll_down_button) lv_obj_add_flag(s_windows.scroll_down_button, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.input_line) lv_obj_add_flag(s_windows.input_line, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.transcript_spans) lv_obj_add_flag(s_windows.transcript_spans, LV_OBJ_FLAG_HIDDEN);
    s_windows.tui_surface = s_windows.transcript;
    s_windows.tui_mode = true;
    windows_refresh_tui_surface();
    return s_windows.tui_surface;
}

void windows_exit_tui_mode(void)
{
    if (!s_windows.tui_mode) return;
    s_windows.tui_surface = NULL;
    if (s_windows.transcript) lv_obj_remove_flag(s_windows.transcript, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.transcript_spans) lv_obj_remove_flag(s_windows.transcript_spans, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.transcript) windows_apply_transcript_height();
    if (s_windows.prev_button) lv_obj_remove_flag(s_windows.prev_button, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.next_button) lv_obj_remove_flag(s_windows.next_button, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.scroll_up_button) lv_obj_remove_flag(s_windows.scroll_up_button, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.scroll_down_button) lv_obj_remove_flag(s_windows.scroll_down_button, LV_OBJ_FLAG_HIDDEN);
    if (s_windows.input_line) lv_obj_remove_flag(s_windows.input_line, LV_OBJ_FLAG_HIDDEN);
    s_windows.tui_mode = false;
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
}

bool windows_is_fullscreen(void) { return s_windows.fullscreen; }

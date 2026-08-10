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
#include "header.h"
#include "keyboard.h"
#include "ansi.h"
#include "esp_lvgl_port.h"
#include "board_config.h"
#include "p4minishell_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

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
    lv_obj_t *input_row;
    lv_obj_t *input_line;
    lv_obj_t *prev_button;
    lv_obj_t *next_button;
} s_windows = {
    .initialized = false,
    .screen = NULL,
    .transcript = NULL,
    .input_row = NULL,
    .input_line = NULL,
    .prev_button = NULL,
    .next_button = NULL,
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
    lv_coord_t header_h = windows_scale_height_percent(P4_CONFIG_WINDOW_HEADER_HEIGHT_PCT,
                                                        P4_CONFIG_WINDOW_HEADER_HEIGHT_MIN,
                                                        P4_CONFIG_WINDOW_HEADER_HEIGHT_MAX);
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
        return lv_color_hex(0x0B0F10);
    }
    if (strcmp(name, WINDOWS_COLOR_BG_TRANSCRIPT) == 0) {
        return lv_color_hex(0x050806);
    }
    if (strcmp(name, WINDOWS_COLOR_BG_INPUT_ROW) == 0) {
        return lv_color_hex(0x111816);
    }
    if (strcmp(name, WINDOWS_COLOR_BG_KEYBOARD) == 0) {
        return lv_color_hex(0x1D2625);
    }
    if (strcmp(name, WINDOWS_COLOR_TEXT) == 0) {
        return lv_color_hex(P4_CONFIG_HEADER_ACCENT_COLOR);
    }
    if (strcmp(name, WINDOWS_COLOR_TEXT_MUTED) == 0) {
        return lv_color_hex(P4_CONFIG_HEADER_MUTED_COLOR);
    }

    return lv_color_hex(0x000000);
}

const lv_font_t *windows_get_terminal_font(void)
{
#if LV_FONT_UNSCII_16
    return &lv_font_unscii_16;
#else
    return LV_FONT_DEFAULT;
#endif
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
    lv_obj_set_style_pad_all(screen, 0, 0);
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
    s_windows.transcript = lv_spangroup_create(screen);
    lv_obj_set_width(s_windows.transcript, LV_PCT(100));
    lv_obj_set_flex_grow(s_windows.transcript, 1);
    lv_obj_set_style_bg_color(s_windows.transcript,
                               windows_get_color(WINDOWS_COLOR_BG_TRANSCRIPT), 0);
    lv_obj_set_style_bg_opa(s_windows.transcript, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_windows.transcript, 0, 0);
    lv_obj_set_style_pad_all(s_windows.transcript, 16, 0);
    lv_obj_set_style_text_font(s_windows.transcript, font, 0);
    lv_obj_set_scrollbar_mode(s_windows.transcript, LV_SCROLLBAR_MODE_ACTIVE);
}

static void windows_create_input_row(void)
{
    const lv_font_t *font = windows_get_terminal_font();
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
    lv_obj_center(prev_label);

    /* Next history button */
    s_windows.next_button = lv_button_create(s_windows.input_row);
    lv_obj_set_size(s_windows.next_button, 82, LV_PCT(100));
    lv_obj_t *next_label = lv_label_create(s_windows.next_button);
    lv_label_set_text(next_label, "Next");
    lv_obj_center(next_label);

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
    lv_obj_set_style_text_font(s_windows.input_line, font, 0);
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
static char s_transcript_staged[P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES];

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
        lv_style_set_text_color(style, lv_color_hex(state->fg_color));
    }
    lv_style_set_text_font(style, windows_get_terminal_font());
}

/**
 * Paint the staged raw ANSI text onto the transcript span group and scroll to
 * the end. Runs on the LVGL task (from the async apply callback) or, in the
 * lv_async_call failure fallback, on the caller's task while holding the LVGL
 * port lock. Both callers serialize with the render cycle, so the staging
 * buffer is never written and read concurrently.
 */
static void windows_transcript_apply(void)
{
    lv_obj_t *transcript = s_windows.transcript;
    uint32_t count;
    uint32_t index;

    if (transcript == NULL) {
        return;
    }

    /* Remove every existing span before rebuilding from the staged ANSI. */
    count = lv_spangroup_get_span_count(transcript);
    for (index = 0; index < count; index++) {
        lv_span_t *span = lv_spangroup_get_child(transcript, 0);

        if (span != NULL) {
            lv_spangroup_delete_span(transcript, span);
        }
    }

    if (s_transcript_staged[0] != '\0') {
        ansi_process_text(s_transcript_staged, windows_span_segment, transcript);
    }

    ESP_LOGD(WINDOWS_TAG, "transcript: %u spans, staged=%u bytes",
             (unsigned int)lv_spangroup_get_span_count(transcript),
             (unsigned int)strlen(s_transcript_staged));

    lv_obj_update_layout(transcript);
    lv_obj_scroll_to_y(transcript, LV_COORD_MAX, LV_ANIM_OFF);
}

static void windows_transcript_apply_cb(void *user_data)
{
    (void)user_data;
    s_transcript_apply_pending = false;
    windows_transcript_apply();
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
        windows_transcript_apply();
    }
}

void windows_set_transcript_text(const char *text)
{
    lv_obj_t *transcript = s_windows.transcript;

    if (transcript == NULL) {
        return;
    }
    if (text == NULL) {
        text = "";
    }

    /* Stage the raw ANSI text for the deferred span rebuild. Callers hold the
     * LVGL port lock, so the staging buffer is never written and read
     * concurrently. The staging buffer is twice the ANSI transcript size, so
     * the accumulated scrollback always fits. */
    snprintf(s_transcript_staged, sizeof(s_transcript_staged), "%s", text);

    windows_transcript_schedule_apply();
}

void windows_scroll_transcript_to_end(void)
{
    if (s_windows.transcript == NULL) {
        return;
    }

    /* Repaint from the staged markup, which repaints and scrolls to the end.
     * Runs on the LVGL task via the deferred apply. */
    windows_transcript_schedule_apply();
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

    /* Clean the screen and apply root layout */
    lv_obj_clean(screen);
    s_windows.screen = screen;
    windows_apply_screen_style();

    /* Build all window regions in order */
    windows_create_header();
    windows_create_transcript();
    windows_create_input_row();
    windows_create_keyboard();

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

    if (s_windows.screen != NULL) {
        /* Kill every pending animation on the screen and its descendants
         * BEFORE deleting any object, so no transition start callback can
         * fire on freed memory afterwards. */
        windows_kill_animations(s_windows.screen);
        lv_obj_clean(s_windows.screen);
    }

    s_windows.screen = NULL;
    s_windows.transcript = NULL;
    s_windows.input_row = NULL;
    s_windows.input_line = NULL;
    s_windows.prev_button = NULL;
    s_windows.next_button = NULL;
    s_windows.initialized = false;
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
    lv_coord_t disp_h = windows_get_display_height();
    lv_coord_t input_h = windows_scale_height_percent(P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_PCT,
                                                       P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_MIN,
                                                       P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_MAX);
    lv_coord_t kb_h = visible ? keyboard_get_height() : 0;

    if (s_windows.input_row != NULL) {
        lv_obj_set_y(s_windows.input_row, disp_h - input_h - kb_h);
    }

    if (s_windows.transcript != NULL) {
        lv_obj_update_layout(s_windows.transcript);
    }

    ESP_LOGI(WINDOWS_TAG, "Keyboard visibility changed: %s (kb_h=%" PRIu32 ")",
             visible ? "visible" : "hidden", (uint32_t)kb_h);
}

/**
 * @file windows.h
 * @brief Window manager for P4MiniShell LVGL shell UI.
 *
 * Central window/layout manager that owns the LVGL screen region partitioning
 * and dynamic scaling. Works together with the display manager (display.c) to
 * query resolution and rotation, and with the header module (header.c) for
 * the top status bar.
 *
 * The window manager divides the screen into named regions:
 *   - HEADER:     Fixed top status bar (delegated to components/header)
 *   - TRANSCRIPT: Scrollable command output area (flex-grow fill)
 *   - INPUT_ROW:  Command prompt line with Prev/Next buttons
 *   - KEYBOARD:   On-screen LVGL keyboard
 *
 * Features:
 *   - Resolution-aware dynamic scaling of all window regions
 *   - Rotation-aware layout (recalculates on rotation change)
 *   - Consistent styling across all windows (colors, fonts, padding)
 *   - Clean init/deinit lifecycle for UI rebuilds
 *   - Public accessors for all window objects (for event callbacks)
 *   - All dimensions derived from config macros, never hardcoded
 *
 * Architecture:
 *   This module OWNS the LVGL screen layout. The shell layer (main.c) calls
 *   windows_init() to build the UI and windows_deinit() before rebuilding.
 *   Individual window objects are accessible via getter functions for
 *   event callback registration. The module queries display.c for resolution
 *   and rotation; it delegates header rendering to header.c.
 *
 * Usage:
 *   1. display_init()         — initialize display hardware
 *   2. windows_init()         — build all UI windows on the LVGL screen
 *   3. windows_get_transcript() etc. — get window objects for event callbacks
 *   4. windows_deinit()       — tear down before rotation rebuild
 *   5. windows_init()         — rebuild at new resolution
 */

#ifndef P4MINISHELL_WINDOWS_H
#define P4MINISHELL_WINDOWS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * WINDOW REGION IDENTIFIERS
 * ======================================================================== */

/** Named window regions on the screen. */
typedef enum {
    WINDOW_REGION_HEADER = 0,       /**< Fixed top status bar */
    WINDOW_REGION_TRANSCRIPT,       /**< Scrollable command output */
    WINDOW_REGION_INPUT_ROW,        /**< Command prompt + Prev/Next buttons */
    WINDOW_REGION_KEYBOARD,         /**< On-screen LVGL keyboard */
    WINDOW_REGION_COUNT             /**< Sentinel count */
} window_region_t;

/* ========================================================================
 * WINDOW DIMENSIONS
 * ======================================================================== */

/** Computed dimensions for a window region. */
typedef struct {
    lv_coord_t x;
    lv_coord_t y;
    lv_coord_t width;
    lv_coord_t height;
} window_rect_t;

/** Get the current bounding rectangle for a named window region.
 *  Returns zeroed rect if windows not initialized. */
window_rect_t windows_get_rect(window_region_t region);

/** Get the current display width (after rotation). */
lv_coord_t windows_get_display_width(void);

/** Get the current display height (after rotation). */
lv_coord_t windows_get_display_height(void);

/* ========================================================================
 * SCALING HELPERS
 * ======================================================================== */

/** Compute a height as a percentage of the current display height.
 *  Clamped to [min_h, max_h]. */
lv_coord_t windows_scale_height_percent(int percent, lv_coord_t min_h, lv_coord_t max_h);

/** Compute a width as a percentage of the current display width.
 *  Clamped to [min_w, max_w]. */
lv_coord_t windows_scale_width_percent(int percent, lv_coord_t min_w, lv_coord_t max_w);

/* ========================================================================
 * WINDOW OBJECT ACCESSORS
 * ======================================================================== */

/** Get the transcript (LVGL label showing coloured command output). */
lv_obj_t *windows_get_transcript(void);

/** Get the transcript's span group (the child holding the coloured spans).
 *  The editor hides it while its own surface owns the container. */
lv_obj_t *windows_get_transcript_spans(void);

/**
 * Set the transcript text, converting ANSI SGR escape sequences to LVGL
 * recolor markup for coloured rendering on the label.
 *
 * @param text  ANSI text to render (may contain ESC[..m sequences).
 */
void windows_set_transcript_text(const char *text);

/**
 * Drop the oldest half of the rendered scrollback and free its spans.
 *
 * Invoked under memory pressure from the shell (which holds the LVGL port
 * lock) to reclaim the internal-heap memory owned by the accumulated span
 * objects. The staged buffer keeps the newest half plus a truncation marker
 * so the next append re-renders only a small tail.
 */
void windows_transcript_trim(void);

/**
 * Apply the transcript's computed region height.
 *
 * The transcript must have an explicit, bounded height (not LV_SIZE_CONTENT)
 * so its span content overflows the widget and becomes vertically scrollable.
 * Call after the window layout is built and again on keyboard-visibility
 * changes so the height tracks the available slot.
 */
void windows_apply_transcript_height(void);

/**
 * Scroll the transcript to the end.
 */
void windows_scroll_transcript_to_end(void);

/**
 * Force the transcript to jump to the newest output. Unlike
 * windows_scroll_transcript_to_end(), which only follows when the view is
 * already near the bottom, this unconditionally pins the next repaint to the
 * bottom. Used when a command is submitted so the user always sees the output
 * of the command they just ran.
 */
void windows_force_scroll_transcript_to_end(void);

/**
 * Scroll the transcript vertically by the given pixel delta. A positive value
 * scrolls toward newer output (the bottom), a negative value toward older
 * output (the top). The scroll is clamped to the transcript's range. Must run
 * on the LVGL task.
 *
 * @param pixels  Signed pixel delta to scroll by.
 */
void windows_scroll_transcript_by(int32_t pixels);

/**
 * Jump the transcript to the top (the oldest retained output). Must run on
 * the LVGL task.
 */
void windows_scroll_transcript_to_top(void);

/** Get the input line textarea (single-line command entry). */
lv_obj_t *windows_get_input_line(void);

/** Get the on-screen keyboard. */
lv_obj_t *windows_get_keyboard(void);

/** Get the Previous history button. */
lv_obj_t *windows_get_prev_button(void);

/** Get the Next history button. */
lv_obj_t *windows_get_next_button(void);

/** Get the transcript scroll-up button (input row). */
lv_obj_t *windows_get_scroll_up_button(void);

/** Get the transcript scroll-down button (input row). */
lv_obj_t *windows_get_scroll_down_button(void);

/** Get the input row container (holds input line + prev/next/scroll buttons). */
lv_obj_t *windows_get_input_row(void);

/** Get the active screen object. */
lv_obj_t *windows_get_screen(void);

/* ========================================================================
 * STYLING
 * ======================================================================== */

/** Get the terminal font selected for the shell UI. */
const lv_font_t *windows_get_terminal_font(void);

/** Get the chained UI font: terminal font primary, Montserrat 14 fallback
 * for FontAwesome PUA icons (keyboard BACKSPACE/OK/arrows, header icons).
 * Use for chrome (keyboard, header, buttons, input line); the transcript and
 * TUI surfaces stay on the pure terminal font so cell metrics are exact.
 * Both delegate to the components/font/ registry. */
const lv_font_t *windows_get_ui_font(void);

/** Re-resolve owned widget fonts after a font switch (see impl note). */
void windows_refresh_fonts(void);

/** Style the input-line cursor as a block (true, editor-like) or thin bar.
 * No-op before the input row exists. Takes the port lock. */
void windows_input_cursor_style(bool block);

/** Set the input-line cursor blink period live (0 = steady, no blink).
 * No-op before the input row exists. Takes the port lock. */
void windows_input_cursor_blink(uint32_t blink_ms);

/** Get a standard shell color by semantic name. */
lv_color_t windows_get_color(const char *name);

/* Predefined color names for windows_get_color() */
#define WINDOWS_COLOR_BG_SCREEN      "bg_screen"
#define WINDOWS_COLOR_BG_TRANSCRIPT  "bg_transcript"
#define WINDOWS_COLOR_BG_INPUT_ROW   "bg_input_row"
#define WINDOWS_COLOR_BG_KEYBOARD    "bg_keyboard"
#define WINDOWS_COLOR_TEXT           "text"
#define WINDOWS_COLOR_TEXT_MUTED     "text_muted"

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/**
 * Initialize the window manager and build all UI windows on the LVGL screen.
 * Must be called after display_init() and from the LVGL task context.
 *
 * This function:
 *   1. Cleans the active screen
 *   2. Sets up the root flex-column layout
 *   3. Creates header (delegated to header_init)
 *   4. Creates transcript textarea
 *   5. Creates input row with prev/next buttons and input line
 *   6. Creates on-screen keyboard
 *   7. Applies consistent styling to all regions
 *
 * @return ESP_OK on success, ESP_FAIL on allocation failure.
 */
esp_err_t windows_init(void);

/**
 * Deinitialize the window manager and release all window objects.
 * Must be called before rebuilding the UI (e.g., after rotation change).
 * Calls header_deinit() internally.
 */
void windows_deinit(void);

/**
 * Check if the window manager is initialized.
 * @return true if windows_init() completed successfully.
 */
bool windows_is_initialized(void);

/**
 * Apply the boot banner text to the transcript window.
 * Called by the shell after windows_init() to show the startup message.
 */
void windows_show_boot_banner(const char *message);

/**
 * Reset the input line to the shell prompt.
 */
void windows_reset_input_line(const char *prompt);

/**
 * Notify the window manager that the keyboard visibility has changed.
 * Called by the keyboard component when keyboard is shown/hidden.
 * Triggers a reflow of window region rectangles.
 * @param visible  true if keyboard is now visible, false if hidden.
 */
void windows_notify_keyboard_visibility(bool visible);

/* ========================================================================
 * EDITOR MODE
 * ======================================================================== */

/**
 * Enter modal editor mode: hide the shell input widgets (prev/next/scroll
 * buttons, input line) and hand the transcript container to the editor as a
 * full-height surface in the transcript region. The transcript container is
 * kept visible (its own span group is hidden by the editor) so the editor
 * area is exactly as large as the normal shell transcript and the on-screen
 * keyboard stays at the bottom. The input row becomes a status bar. Must run
 * on the LVGL task.
 * @return The editor surface container, or NULL on failure.
 */
lv_obj_t *windows_enter_editor_mode(void);

/** Leave modal editor mode and restore the shell input surface. LVGL task. */
void windows_exit_editor_mode(void);

/** Get the status-bar label created by windows_enter_editor_mode(). */
lv_obj_t *windows_get_editor_status(void);

/** Get the editor surface container (the transcript region). */
lv_obj_t *windows_get_editor_surface(void);

/** Report whether the editor modal surface is currently active. */
bool windows_editor_mode_active(void);

/**
 * Enter app mode: hide the shell input widgets (input line and the
 * prev/next/scroll buttons) so the transcript becomes a clean full-screen app
 * surface; the on-screen keyboard can still be shown for app input. Must run
 * on the LVGL task. Paired with `windows_exit_app_mode`.
 */
void windows_enter_app_mode(void);

/** Leave app mode and restore the shell input widgets. Must run on LVGL task. */
void windows_exit_app_mode(void);

/**
 * Re-apply the transcript-region height to the editor surface and force a
 * layout pass. The single source of truth for the editor surface size — it
 * mirrors windows_apply_transcript_height() so the editor always fills the
 * same slot as the shell transcript. Call on editor entry, on keyboard
 * visibility changes, and whenever the layout may have shifted.
 */
void windows_refresh_editor_surface(void);

/**
 * Check whether the editor surface is at least as tall as the transcript
 * region (i.e. it did not collapse). Used to catch a "collapsed editor"
 * layout regression early.
 * @return true when the surface height is sane, false otherwise.
 */
bool windows_editor_surface_height_ok(void);

/**
 * Log the editor surface, transcript-region, and keyboard rectangles. A
 * diagnostic for on-board layout inspection (the editor entry path logs it
 * automatically; call it from a debug command to inspect live state).
 */
void windows_debug_editor_layout(void);

/* ========================================================================
 * TUI MODE
 * ======================================================================== */

/**
 * Enter TUI mode: hide shell input widgets and hand the transcript container
 * to the TUI as a full-height surface (like editor mode). The transcript
 * spangroup is hidden while TUI owns the container.
 * @return Transcript container, or NULL on failure. LVGL task.
 */
lv_obj_t *windows_enter_tui_mode(void);

/** Leave TUI mode and restore shell widgets. LVGL task. */
void windows_exit_tui_mode(void);

/** Report whether TUI mode is active. */
bool windows_tui_mode_active(void);

/** Get TUI surface container (transcript region when TUI active). */
lv_obj_t *windows_get_tui_surface(void);

/**
 * Re-apply transcript-region height to TUI surface (call on entry and on
 * keyboard visibility changes, like windows_refresh_editor_surface).
 */
void windows_refresh_tui_surface(void);

/** Check TUI surface height sane (mirrors editor check). */
bool windows_tui_surface_height_ok(void);

/**
 * Set fullscreen TUI mode: hide header and input row for full display.
 * Call after tui_init or when already in TUI mode.
 */
void windows_set_fullscreen(bool fullscreen);

/** Report whether fullscreen TUI is active. */
bool windows_is_fullscreen(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_WINDOWS_H */

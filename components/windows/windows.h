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

/**
 * Set the transcript text, converting ANSI SGR escape sequences to LVGL
 * recolor markup for coloured rendering on the label.
 *
 * @param text  ANSI text to render (may contain ESC[..m sequences).
 */
void windows_set_transcript_text(const char *text);

/**
 * Scroll the transcript to the end.
 */
void windows_scroll_transcript_to_end(void);

/** Get the input line textarea (single-line command entry). */
lv_obj_t *windows_get_input_line(void);

/** Get the on-screen keyboard. */
lv_obj_t *windows_get_keyboard(void);

/** Get the Previous history button. */
lv_obj_t *windows_get_prev_button(void);

/** Get the Next history button. */
lv_obj_t *windows_get_next_button(void);

/** Get the input row container (holds input line + prev/next buttons). */
lv_obj_t *windows_get_input_row(void);

/** Get the active screen object. */
lv_obj_t *windows_get_screen(void);

/* ========================================================================
 * STYLING
 * ======================================================================== */

/** Get the terminal font selected for the shell UI. */
const lv_font_t *windows_get_terminal_font(void);

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

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_WINDOWS_H */

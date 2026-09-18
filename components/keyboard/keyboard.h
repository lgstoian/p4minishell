/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file keyboard.h
 * @brief On-screen keyboard manager for P4MiniShell LVGL shell UI.
 *
 * Owns the LVGL keyboard widget and all keyboard-related state:
 * visibility, mode, textarea binding, and dynamic scaling.
 * Works together with the window manager (windows.c) for layout
 * and the display manager (display.c) for resolution queries.
 *
 * Features:
 *   - Show/hide keyboard with automatic UI reflow
 *   - Multiple keyboard modes (text_lower, text_upper, number, symbols)
 *   - Dynamic height scaling based on display resolution
 *   - Textarea binding for input routing
 *   - Keyboard visibility state tracking
 *   - Transcript area auto-expansion when keyboard is hidden
 *   - External input mode: auto-hide when USB keyboard is present
 *
 * Architecture:
 *   This module OWNS the LVGL keyboard widget and its visibility state.
 *   The window manager delegates keyboard creation to this module.
 *   When keyboard visibility changes, the window manager is notified
 *   to recalculate region rectangles (transcript expands when keyboard hidden).
 *
 * Usage:
 *   1. keyboard_init()          — create the keyboard widget
 *   2. keyboard_bind_textarea() — attach to input line
 *   3. keyboard_show()/keyboard_hide() — toggle visibility
 *   4. keyboard_is_visible()    — query state for layout calculations
 *   5. keyboard_get_height()    — get current keyboard height for layout
 *   6. keyboard_deinit()        — release keyboard widget
 */

#ifndef P4MINISHELL_KEYBOARD_H
#define P4MINISHELL_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * KEYBOARD MODE
 * ======================================================================== */

/** Keyboard input modes. */
typedef enum {
    KEYBOARD_MODE_TEXT_LOWER = 0,   /**< Lowercase text */
    KEYBOARD_MODE_TEXT_UPPER,       /**< Uppercase text */
    KEYBOARD_MODE_NUMBER,           /**< Numbers only */
    KEYBOARD_MODE_SYMBOLS,          /**< Symbols only */
    KEYBOARD_MODE_NAV,              /**< Editor navigation page (page 1) */
    KEYBOARD_MODE_NAV2,             /**< Editor navigation page (page 2: edit/clipboard) */
    KEYBOARD_MODE_COUNT             /**< Sentinel */
} keyboard_mode_t;

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/**
 * Initialize the keyboard manager and create the LVGL keyboard widget.
 * Must be called after display_init() and from the LVGL task context.
 * The keyboard is created visible by default.
 *
 * @param parent  Parent LVGL object (typically the active screen).
 * @return Pointer to the keyboard LVGL object, or NULL on failure.
 */
lv_obj_t *keyboard_init(lv_obj_t *parent);

/**
 * Deinitialize the keyboard manager and release the keyboard widget.
 * Safe to call even if keyboard is not initialized.
 */
void keyboard_deinit(void);

/** Re-resolve the keyboard font after a font switch (see impl note). */
void keyboard_refresh_fonts(void);
void keyboard_refresh_theme(void);

/**
 * Check if the keyboard manager is initialized.
 * @return true if keyboard_init() completed successfully.
 */
bool keyboard_is_initialized(void);

/**
 * Register a callback to be invoked when keyboard visibility changes.
 * The window manager uses this to recalculate region rectangles.
 * @param cb  Callback function. Pass NULL to clear.
 */
void keyboard_register_visibility_callback(void (*cb)(bool visible));

/* ========================================================================
 * VISIBILITY
 * ======================================================================== */

/**
 * Show the keyboard. If already visible, this is a no-op.
 * Triggers a window manager reflow so the transcript shrinks to
 * accommodate the keyboard.
 * If external input is enabled, this forces the keyboard visible
 * regardless of external input state.
 */
void keyboard_show(void);

/**
 * Hide the keyboard. If already hidden, this is a no-op.
 * Triggers a window manager reflow so the transcript expands to
 * fill the freed space.
 */
void keyboard_hide(void);

/**
 * Toggle keyboard visibility.
 * If external input is enabled, this forces a toggle regardless.
 */
void keyboard_toggle(void);

/**
 * Query whether the keyboard is currently visible.
 * @return true if keyboard is visible.
 */
bool keyboard_is_visible(void);

/* ========================================================================
 * EXTERNAL INPUT MODE (USB KEYBOARD)
 * ======================================================================== */

/**
 * Enable or disable external input mode.
 * When enabled, the on-screen keyboard is automatically hidden
 * (unless manually overridden by the user).
 * @param enabled  true to enable external input, false to disable.
 */
void keyboard_set_external_input(bool enabled);

/**
 * Query whether external input mode is active.
 * @return true if external input is enabled.
 */
bool keyboard_is_external_input_enabled(void);

/**
 * Force the keyboard to stay visible even when external input is enabled.
 * Reset by calling keyboard_set_external_input() again or
 * keyboard_clear_force_visible().
 */
void keyboard_force_visible(void);

/**
 * Clear the force-visible override, allowing external input mode
 * to hide the keyboard again.
 */
void keyboard_clear_force_visible(void);

/* ========================================================================
 * TEXTAREA BINDING
 * ======================================================================== */

/**
 * Bind the keyboard to a textarea for input routing.
 * Passing NULL unbinds the keyboard (its LVGL widget binding is cleared too,
 * so a stray default handler can never type into a hidden textarea).
 * @param textarea  LVGL textarea object to receive keyboard input, or NULL.
 */
void keyboard_bind_textarea(lv_obj_t *textarea);

/**
 * Get the currently bound textarea.
 * @return The bound textarea, or NULL if none.
 */
lv_obj_t *keyboard_get_textarea(void);

/**
 * Report whether a textarea is currently bound to the keyboard.
 * Lets callers assert the intended binding state (e.g. the editor asserts it
 * is unbound while it owns the on-screen keyboard).
 * @return true when a textarea is bound.
 */
bool keyboard_is_textarea_bound(void);

/* ========================================================================
 * OSK INPUT DEDUPLICATION
 * ======================================================================== */

/**
 * Decide whether an on-screen keyboard button press should be processed.
 *
 * The shell routes every OSK button through this guard before acting on it:
 * a re-fire of the same button id within P4_CONFIG_OSK_DEBOUNCE_MS (the same
 * LVGL event reaching a second, stray handler) is dropped, so double input is
 * impossible even if a duplicate handler is ever re-registered. Deliberate
 * fast repeats (auto-repeat, quick double-taps) are far slower than the
 * debounce window and are not merged.
 *
 * @param btn_id  The LVGL button id of the pressed key.
 * @return true when the press should be processed.
 */
bool keyboard_osk_accept(uint32_t btn_id);

/**
 * Pure, injectable-clock variant of keyboard_osk_accept() for unit tests.
 * @param btn_id  The LVGL button id of the pressed key.
 * @param now_ms  Current time in milliseconds (injected).
 * @return true when the press should be processed.
 */
bool keyboard_osk_accept_at(uint32_t btn_id, int64_t now_ms);

/* ========================================================================
 * SITUATIONAL CAPABILITIES
 * ========================================================================
 * Some OSK keys are only meaningful in certain contexts (the symbols page's
 * "Nav" key opens the editor navigation page). A capability is enabled when
 * EITHER the registered context callback reports it (the shell supplies the
 * current context, e.g. an open editor) OR any component has explicitly
 * requested it. Unavailable capability keys are disabled in place (greyed)
 * and never fire.
 */

/** Capability bits. Add new context-specific keys here. */
typedef enum {
    KEYBOARD_CAP_NAV = 1u << 0,  /**< The editor navigation page is reachable. */
} keyboard_capability_t;

/**
 * Context provider. Returns the capabilities available in the current context.
 * Called from the LVGL task while the port lock is held: must be cheap and must
 * not call back into LVGL or the keyboard module.
 */
typedef uint32_t (*keyboard_capabilities_cb_t)(void);

/** Register (or clear, with NULL) the context capabilities provider. */
void keyboard_register_capabilities_callback(keyboard_capabilities_cb_t cb);

/**
 * Request (on) or release (off) a capability bit as an explicit requester.
 *
 * Reference-counted: multiple requesters may hold the same bit and it stays
 * enabled until the last one releases it. Batch apps use the `keyboard nav
 * on|off` command; future modal apps call this on open/close. Balanced calls
 * are the caller's responsibility (an unbalanced `on` keeps the bit until a
 * reboot or a matching `off`).
 */
void keyboard_request_capability(uint32_t caps, bool on);

/** Effective capabilities (context callback OR any active request). */
uint32_t keyboard_effective_capabilities(void);

/** Re-evaluate capability key availability for the current page (idempotent). */
void keyboard_refresh_availability(void);

/** Tri-state of the Nav capability key, for diagnostics/tests. */
typedef enum {
    KEYBOARD_NAV_KEY_ABSENT = -1,   /**< No on-screen keyboard (nothing to report). */
    KEYBOARD_NAV_KEY_ENABLED = 0,   /**< Nav capability granted (key usable). */
    KEYBOARD_NAV_KEY_DISABLED = 1,  /**< Nav capability denied (key greyed out). */
} keyboard_nav_key_state_t;

/** Query the Nav capability state: granted by the shell context (editor open)
 *  or by a reference-counted request (`keyboard nav on`). Matches
 *  `keyboard_effective_capabilities()`, so `ui state nav=` reads `on` while the
 *  editor (which switches to the page that has no literal Nav button) is open. */
keyboard_nav_key_state_t keyboard_nav_key_state(void);

/* Pure helpers (no LVGL; unit-tested): the capability a key label requires
 * (0 = always enabled) and whether that key is enabled for a capability set. */
uint32_t keyboard_button_required_capability(const char *label);
bool keyboard_button_enabled_for_caps(uint32_t effective_caps, const char *label);

/* ========================================================================
 * MODE CONTROL
 * ======================================================================== */

/**
 * Set the keyboard input mode.
 * @param mode  Desired keyboard mode.
 */
void keyboard_set_mode(keyboard_mode_t mode);

/**
 * Stable registry name for a mode ("text_lower", "text_upper", "number",
 * "symbols", "nav", "nav2"). Used by the `keyboard mode` command and tests.
 */
const char *keyboard_mode_name(keyboard_mode_t mode);

/** Parse a page/mode name or common alias into a keyboard mode. */
bool keyboard_mode_parse(const char *text, keyboard_mode_t *out);

/**
 * Get the current keyboard mode.
 * @return Current keyboard mode.
 */
keyboard_mode_t keyboard_get_mode(void);

/**
 * Cycle to the next keyboard mode.
 * Order: TEXT_LOWER → TEXT_UPPER → NUMBER → SYMBOLS → TEXT_LOWER
 */
void keyboard_cycle_mode(void);

/* ========================================================================
 * DIMENSIONS
 * ======================================================================== */

/**
 * Get the current keyboard height in pixels.
 * Returns 0 when keyboard is hidden.
 * @return Keyboard height, or 0 if hidden.
 */
lv_coord_t keyboard_get_height(void);

/**
 * Get the keyboard height that would be used if visible.
 * This is the configured height regardless of current visibility.
 * @return Configured keyboard height.
 */
lv_coord_t keyboard_get_configured_height(void);

/* ========================================================================
 * WIDGET ACCESS
 * ======================================================================== */

/**
 * Get the LVGL keyboard widget for direct manipulation.
 * @return The keyboard LVGL object, or NULL if not initialized.
 */
lv_obj_t *keyboard_get_widget(void);

/* ========================================================================
 * LVGL EVENT CALLBACK
 * ======================================================================== */

/**
 * Register an LVGL event callback on the keyboard widget.
 * The callback receives all LVGL events from the keyboard widget
 * (LV_EVENT_VALUE_CHANGED for mode/button presses, LV_EVENT_READY
 * for OK button, LV_EVENT_CANCEL for keyboard hide button, etc.).
 *
 * This enables richer keyboard interaction such as:
 *   - Detecting mode changes (abc → ABC → 1#)
 *   - Handling the OK/done button press
 *   - Responding to keyboard hide requests
 *
 * Only one callback can be registered at a time. Pass NULL to unregister.
 *
 * @param cb        LVGL event callback function.
 * @param user_data Opaque user data passed to the callback.
 */
void keyboard_register_event_callback(lv_event_cb_t cb, void *user_data);

/**
 * Report how many LVGL event callbacks are currently registered on the
 * keyboard widget. The shell expects exactly one (its own). A value other
 * than 1 means the LVGL default handler survived or a callback was registered
 * twice — the root cause of double OSK input.
 * @return The number of registered LVGL event callbacks (0 when no widget).
 */
uint32_t keyboard_event_callback_count(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_KEYBOARD_H */

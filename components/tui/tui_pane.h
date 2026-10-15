/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tui_pane.h
 * @brief TUI workspace: pane stack, widget layer, HUD region, event multiplexer.
 *
 * Sits on the 80x25 TUI grid. All state is fixed-capacity (no dynamic
 * allocation). Rendering writes through the existing tui_draw_* primitives
 * into the cell buffer, then tui_flush() pushes to LVGL.
 *
 * Layering: leaf inside components/tui, REQUIRES shell, windows, ansi,
 * display (same as tui.c). The command layer resolves environment variables
 * and passes concrete display strings/values into this layer.
 */

#ifndef P4MINISHELL_TUI_PANE_H
#define P4MINISHELL_TUI_PANE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * PANE STACK
 * ======================================================================== */

/** Add a named pane at grid rect (x,y,w,h) 1-based. title may be NULL.
 *  Returns false on duplicate name, invalid geometry, or table full. */
bool tui_pane_add(const char *name, int x, int y, int w, int h, const char *title);

/** Close (remove) a pane by name. */
bool tui_pane_close(const char *name);

/** Set the focused pane by name. Focus ring highlight follows. */
bool tui_pane_focus(const char *name);

/** Advance focus to the next visible pane (wraps). */
bool tui_pane_cycle(void);

/** Hide/show a pane (hidden panes skip render and focus). */
bool tui_pane_hide(const char *name, bool hide);

/** Split a pane horizontally (true) or vertically (false): the original
 *  keeps the top/left half, a new pane gets the bottom/right half. */
bool tui_pane_split(const char *name, bool horizontal);

/** Number of panes currently in the stack. */
int tui_pane_count(void);

/** Copy the pane name at index (0-based) into buf. */
bool tui_pane_get_name(int index, char *buf, size_t buf_size);

/** Index of the focused pane, or -1 when none. */
int tui_pane_get_focus(void);

/** Append a text line to a pane's scrollback (auto-scrolls). */
bool tui_pane_append(const char *name, const char *text);

/** Clear a pane's scrollback. */
bool tui_pane_clear(const char *name);

/** Render all visible panes into the cell buffer. */
void tui_pane_render(void);

/* ========================================================================
 * WIDGET LAYER
 * ======================================================================== */

typedef enum {
    TUI_WIDGET_TEXT = 0,
    TUI_WIDGET_GAUGE,
    TUI_WIDGET_BUTTON,
    TUI_WIDGET_LIST,
    TUI_WIDGET_PANEL
} tui_widget_type_t;

/** Declare or replace a widget. type_str is "text"|"gauge"|"button"|"list"|"panel".
 *  label may be NULL. Returns false on bad args or table full. */
bool tui_widget_add(const char *name, const char *type_str, int x, int y,
                    int w, int h, const char *label);

/** Set the display text for a text/list widget (already env-resolved). */
bool tui_widget_set_text(const char *name, const char *text);

/** Set a numeric value (gauge fill, list selection index 0-based). */
bool tui_widget_set_value(const char *name, int value);

/** Set the /max cap for a gauge widget. */
bool tui_widget_set_max(const char *name, int max_val);

/** Bind a widget to a label (button fires this label when activated). */
bool tui_widget_bind_label(const char *name, const char *label);

/** Remove a widget. */
bool tui_widget_remove(const char *name);

/** Render all widgets into the cell buffer. */
void tui_widget_render(void);

/** Report whether a widget with this name exists. */
bool tui_widget_exists(const char *name);

/* ========================================================================
 * HUD REGION
 * ======================================================================== */

/** Add a HUD cell. position: 0 = left, 1 = right. */
bool tui_hud_add(const char *name, int position);

/** Set the display text for a HUD cell (already env-resolved). */
bool tui_hud_set_text(const char *name, const char *text);

/** Remove a HUD cell. */
bool tui_hud_remove(const char *name);

/** Set the refresh cadence in ms (0 = manual only). */
void tui_hud_set_refresh(uint32_t ms);

/** Get the current refresh cadence. */
uint32_t tui_hud_get_refresh(void);

/** Flash a HUD cell for a transient emphasis (duration in refreshes). */
bool tui_hud_flash(const char *name, int duration);

/** Set the HUD band palette (DOS 0-15 colors, 16 = default). */
void tui_hud_set_colors(uint8_t fg, uint8_t bg);

/** Re-evaluate all HUD cells and redraw only changed cells. */
void tui_hud_refresh(void);

/** Render the HUD band into the cell buffer. */
void tui_hud_render(void);

/* ========================================================================
 * EVENT MULTIPLEXER
 * ======================================================================== */

typedef enum {
    TUI_EVENT_KEY = 0,
    TUI_EVENT_TAP,
    TUI_EVENT_TIMER,
    TUI_EVENT_SERIAL,
    TUI_EVENT_RESIZE,
    TUI_EVENT_TYPE_COUNT
} tui_event_type_t;

/** Post an event to the queue. payload is type-specific (key name, tap label,
 *  timer id, etc). Returns false when the queue is full. */
bool tui_event_post(tui_event_type_t type, const char *payload);

/** Number of events currently queued. */
int tui_event_count(void);

/** Copy event at index (0 = oldest). payload may be NULL. */
bool tui_event_get(int index, tui_event_type_t *type, char *payload,
                   size_t payload_size);

/** Wait up to timeout_ms for an event. Returns the event index or -1 on
 *  timeout. */
int tui_event_wait(int timeout_ms);

/** Clear the event queue. */
void tui_event_clear(void);

/* ========================================================================
 * KEY BINDINGS
 * ======================================================================== */

/** Bind a key name (F1..F12, CTRL+R, etc.) to a batch label. */
bool tui_key_bind(const char *key, const char *label);

/** Remove a key binding. */
bool tui_key_unbind(const char *key);

/** Inject synthetic input into the shell input stream. */
bool tui_key_inject(const char *text);

/** Count of active key bindings. */
int tui_key_count(void);

/** Copy the key name at index into buf. */
bool tui_key_get_name(int index, char *buf, size_t buf_size);

/** Look up the label bound to a key, or NULL. */
const char *tui_key_lookup(const char *key);

/** Set input mode: 0 = line (default), 1 = raw, 2 = passthrough. */
void tui_key_set_mode(int mode);

/** Get the current input mode. */
int tui_key_get_mode(void);

/* ========================================================================
 * TOUCH HOTSPOTS
 * ======================================================================== */

/** Register a touch hotspot rect (x,y,w,h) mapped to a batch label.
 *  Overlaps resolve to the most recently added (topmost). */
bool tui_tap_add(int x, int y, int w, int h, const char *label);

/** Remove hotspots whose rect starts at (x,y). Returns count removed. */
int tui_tap_remove(int x, int y);

/** Clear all hotspots. */
void tui_tap_clear(void);

/** Hit-test a point; returns the topmost label or NULL. */
const char *tui_tap_hit(int x, int y);

/** Count of active hotspots. */
int tui_tap_count(void);

/* ========================================================================
 * WORKSPACE RENDER
 * ======================================================================== */

/** Render the full workspace: HUD + panes + widgets. */
void tui_workspace_render(void);

/** Reset all workspace state (panes, widgets, HUD, events, keys, taps). */
void tui_workspace_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_TUI_PANE_H */

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tui_modal.h
 * @brief Modal layer: mode frames, multi-stroke keymaps, nested dialogs, scopes.
 *
 * Sits on the TUI workspace. The mode stack is a fixed-depth array of typed
 * frames; each frame owns a keymap name, a scope bag, and optional guards.
 * Multi-stroke chords resolve longest-match-wins against the active map with
 * a timeout. Named commands unify bind/palette/macro targets. Dialogs are
 * blocking frames that return a value plus errorlevel.
 *
 * Layering: leaf inside components/tui, REQUIRES shell, windows, ansi,
 * display, freertos. Guard/label execution is registered by the command
 * layer via callback; the modal core never calls shell_execute_command.
 */

#ifndef P4MINISHELL_TUI_MODAL_H
#define P4MINISHELL_TUI_MODAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * TYPES
 * ======================================================================== */

typedef enum {
    MODAL_FRAME_NORMAL = 0,
    MODAL_FRAME_INSERT,
    MODAL_FRAME_DIALOG,
    MODAL_FRAME_PALETTE,
    MODAL_FRAME_MACRO,
    MODAL_FRAME_TYPE_COUNT
} modal_frame_type_t;

typedef enum {
    MODAL_GUARD_ENTER = 0,
    MODAL_GUARD_LEAVE,
    MODAL_GUARD_KIND_COUNT
} modal_guard_kind_t;

/** Callback type for executing a label (guard, binding target, command).
 *  Registered by the command layer. Returns the errorlevel from execution. */
typedef int (*modal_label_exec_fn)(const char *label, void *context);

/** Callback for getting an environment variable. */
typedef const char *(*modal_env_get_fn)(const char *name);

/** Callback for setting an environment variable. */
typedef int (*modal_env_set_fn)(const char *name, const char *value);

/** Callback for rendering the statusline (called on mode change / status update). */
typedef void (*modal_status_render_fn)(const char *text);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/** Initialize the modal layer. Registers default callbacks. */
bool modal_init(modal_label_exec_fn exec_cb, modal_env_get_fn get_cb,
                modal_env_set_fn set_cb, modal_status_render_fn status_cb);

/** Reset all modal state (mode stack, maps, commands, scopes, macros). */
void modal_reset(void);

/** Set the label execution callback. */
void modal_set_exec_callback(modal_label_exec_fn cb);

/** Set the statusline render callback. */
void modal_set_status_render_callback(modal_frame_type_t type, const char *format);

/* ========================================================================
 * MODE STACK
 * ======================================================================== */

/** Push a named mode frame. map_name is the keymap to activate (may NULL).
 *  type decides input ownership, rendering, and Esc behavior. */
bool modal_mode_push(const char *name, modal_frame_type_t type, const char *map_name);

/** Pop the top frame. Runs leave guard on the popped frame. Returns false
 *  if the guard vetoes (errorlevel non-zero). */
bool modal_mode_pop(void);

/** Pop until a named frame is on top (or stack empty). If the named frame
 *  doesn't exist, push it as a normal frame. */
bool modal_mode_switch(const char *name);

/** Get the top frame index (-1 if empty). */
int modal_mode_top(void);

/** Copy the frame name at depth (0 = top) into buf. */
bool modal_mode_get_name(int depth, char *buf, size_t buf_size);

/** Get the frame type at depth. */
modal_frame_type_t modal_mode_get_type(int depth);

/** Number of frames on the stack. */
int modal_mode_count(void);

/** Drop all frames of a given type. */
int modal_mode_drop_by_type(modal_frame_type_t type);

/** Set the chord pending timeout in ms. */
void modal_set_chord_timeout(uint32_t ms);

/** Get the chord pending timeout. */
uint32_t modal_get_chord_timeout(void);

/* ========================================================================
 * KEYMAPS
 * ======================================================================== */

/** Create or get a named keymap. Returns its index or -1. */
int modal_map_create(const char *map_name);

/** Find a keymap by name. Returns index or -1. */
int modal_map_find(const char *map_name);

/** Bind a chord to a label in a named map. Multi-stroke chords are
 *  space-separated keys (e.g. "g g" or "leader f"). */
bool modal_bind(const char *map_name, const char *chord, const char *label);

/** Unbind a chord from a named map. */
bool modal_unbind(const char *map_name, const char *chord);

/** Set the leader key (single character prefix). */
bool modal_set_leader(const char *key);

/** Get the leader key. */
const char *modal_get_leader(void);

/** Count bindings in a map. */
int modal_bind_count(const char *map_name);

/** Copy the chord at index into buf. */
bool modal_bind_get_chord(const char *map_name, int index, char *buf, size_t buf_size);

/** Copy the label at index into buf. */
bool modal_bind_get_label(const char *map_name, int index, char *buf, size_t buf_size);

/* ========================================================================
 * CHORD RESOLUTION
 * ======================================================================== */

/** Push a stroke into the pending buffer. Returns true if a complete
 *  binding was resolved (label_out filled). On partial match, returns
 *  false and the stroke remains pending. On timeout or no match, the
 *  buffer clears. */
bool modal_chord_push(const char *stroke, char *label_out, size_t label_size);

/** Copy the pending prefix into buf (for statusline display). */
bool modal_chord_pending(char *buf, size_t buf_size);

/** Clear the pending chord buffer. */
void modal_chord_clear(void);

/* ========================================================================
 * NAMED COMMANDS
 * ======================================================================== */

/** Define a named command (name -> label). doc may be NULL. */
bool modal_cmd_def(const char *name, const char *label, const char *doc);

/** Undefine a named command. */
bool modal_cmd_undef(const char *name);

/** Find a command by name. Returns label or NULL. */
const char *modal_cmd_find(const char *name);

/** Get the doc string for a command. */
const char *modal_cmd_get_doc(const char *name);

/** Count defined commands. */
int modal_cmd_count(void);

/** Copy the command name at index into buf. */
bool modal_cmd_get_name(int index, char *buf, size_t buf_size);

/** Run a named command by name. Returns errorlevel. */
int modal_cmd_run(const char *name);

/* ========================================================================
 * SCOPE BAGS
 * ======================================================================== */

/** Save a scope bag for a mode: snapshot the named env vars. */
bool modal_scope_save(const char *mode_name, int count, const char **vars);

/** Restore a scope bag for a mode: write saved values back to env. */
bool modal_scope_restore(const char *mode_name);

/** List the variables in a scope bag into a dim-managed array. */
int modal_scope_list(const char *mode_name, char *buf, size_t buf_size);

/** Clear a scope bag. */
bool modal_scope_clear(const char *mode_name);

/* ========================================================================
 * GUARDS
 * ======================================================================== */

/** Register a guard hook (enter or leave) for a mode type. */
bool modal_guard_set(modal_guard_kind_t kind, modal_frame_type_t type, const char *label);

/** Clear a guard hook. */
bool modal_guard_clear(modal_guard_kind_t kind, modal_frame_type_t type);

/** Get the guard label for a kind+type (or NULL). */
const char *modal_guard_get(modal_guard_kind_t kind, modal_frame_type_t type);

/* ========================================================================
 * DIALOGS (nested blocking frames)
 * ======================================================================== */

/** Push a dialog frame. type is the dialog kind ("confirm", "input",
 *  "select", "progress"). title is the dialog title. */
bool modal_dialog_push(const char *type, const char *title);

/** Set the dialog result value (filled by the verb layer). */
void modal_dialog_set_result(const char *value);

/** Get the last dialog result. */
const char *modal_dialog_get_result(void);

/** Pop the dialog frame. Returns the dialog errorlevel. */
int modal_dialog_pop(void);

/** Number of active dialog frames. */
int modal_dialog_count(void);

/* ========================================================================
 * MACROS
 * ======================================================================== */

/** Start recording a macro into a named slot. */
bool modal_macro_record(const char *name);

/** Stop recording. Returns the number of events captured. */
int modal_macro_stop(void);

/** Record a single event (called during recording). */
bool modal_macro_capture(const char *event);

/** Replay a named macro. times = repeat count. Returns errorlevel. */
int modal_macro_replay(const char *name, int times);

/** Count recorded macros. */
int modal_macro_count(void);

/** Copy the macro name at index into buf. */
bool modal_macro_get_name(int index, char *buf, size_t buf_size);

/* ========================================================================
 * REPEAT
 * ======================================================================== */

/** Set the repeat count for the next command. */
void modal_set_repeat(int count);

/** Get and consume the repeat count (returns 1 if none set). */
int modal_get_repeat(void);

/** Clear the repeat count. */
void modal_clear_repeat(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_TUI_MODAL_H */

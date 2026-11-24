/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file modal_surf.h
 * @brief Native modal surfaces for batch apps: dialog, list, ask, filebrowser, viewer.
 *
 * These surfaces use the shared modal runtime (modal.h) to take over the
 * current shell display area and return a result to the batch file.
 */

#ifndef P4MINISHELL_MODAL_SURF_H
#define P4MINISHELL_MODAL_SURF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Show a modal dialog with up to two buttons.
 *
 * @param title       Dialog title (may be NULL).
 * @param message     Body text (may be NULL).
 * @param button1     Label for the first button (default "OK" if NULL).
 * @param button2     Label for the second button (NULL for a single-button
 *                    dialog).
 * @param timeout_ms  Auto-cancel after this many ms (0 = wait forever).
 * @return 0 for button1, 1 for button2, or -1 for cancel/Esc/timeout.
 */
int modal_dialog_run(const char *title, const char *message,
                     const char *button1, const char *button2,
                     uint32_t timeout_ms);

/**
 * @brief Show a scrollable list of items and return the selected index.
 *
 * @param title       List title (may be NULL).
 * @param items       Array of item labels.
 * @param count       Number of items (>= 1).
 * @param timeout_ms  Auto-cancel after this many ms (0 = wait forever).
 * @return 0-based selected index, or -1 for cancel/Esc/timeout.
 */
int modal_list_run(const char *title, const char **items, int count,
                   uint32_t timeout_ms);

/**
 * @brief Prompt the user for a text answer.
 *
 * @param prompt       Prompt text (may be NULL).
 * @param default_text Text prefilled in the input field (may be NULL).
 * @param password     When true the input is masked (password mode).
 * @param timeout_ms   Auto-cancel after this many ms (0 = wait forever).
 * @param result       Buffer receiving the answer string.
 * @param result_size  Capacity of @p result.
 * @return 0 when the user confirmed, -1 for cancel/Esc/timeout.
 */
int modal_ask_run(const char *prompt, const char *default_text, bool password,
                  uint32_t timeout_ms, char *result, size_t result_size);

/**
 * @brief Run a file browser modal surface.
 *
 * @param title          Dialog title (may be NULL).
 * @param start_path     Initial directory path (may be NULL for default /sdcard).
 * @param selected_path  Buffer to receive the selected file/directory path.
 * @param path_size      Capacity of @p selected_path.
 * @param timeout_ms     Auto-cancel after this many ms (0 = wait forever).
 * @return 0 on file/directory selected, -1 on cancel/Esc/timeout.
 */
int modal_filebrowser_run(const char *title, const char *start_path,
                          char *selected_path, size_t path_size,
                          uint32_t timeout_ms);

/**
 * @brief Run a text viewer/pager modal surface.
 *
 * @param title       Dialog title (may be NULL).
 * @param file_path   Path to file to view (may be NULL for stdin).
 * @param timeout_ms  Auto-cancel after this many ms (0 = wait forever).
 * @return 0 on exit, -1 on error.
 */
int modal_viewer_run(const char *title, const char *file_path, uint32_t timeout_ms);

/**
 * @brief Run the viewer with explicit raw control.
 *
 * Same as modal_viewer_run, but @p raw bypasses the Markdown branch
 * (.md files render rendered-plain by default).
 */
int modal_viewer_run_raw(const char *title, const char *file_path, uint32_t timeout_ms, bool raw);

/**
 * @brief Run a BMP image viewer modal surface.
 *
 * Decodes a 24/32-bit BI_RGB BMP (either orientation) straight to a
 * fit-to-screen RGB565 image and shows it centered in the transcript region.
 * Keys: Esc/q/Close (and `/t:secs` auto-close). Reuses the shared modal
 * runtime and the single BMP decoder in components/gfx.
 *
 * @param title       Dialog title (may be NULL).
 * @param file_path   Path to the .bmp file.
 * @param timeout_ms  Auto-cancel after this many ms (0 = wait forever).
 * @param fit         true to aspect-fit to the region (never upscales the
 *                    source past its native size), false for native size.
 * @return 0 on exit, -1 on error (bad file / decode / no surface).
 */
int modal_image_run(const char *title, const char *file_path,
                    uint32_t timeout_ms, bool fit);

/**
 * @brief Set the transparency of every modal panel and its backdrop scrim.
 *
 * Opt-in and per app: the shell applies an app's APPINFO `alpha=`/`scrim=`
 * before it runs and restores the defaults afterwards. Both are 0-255.
 * `panel_alpha` < 255 makes the panel translucent so the page behind shows
 * through; `scrim_alpha` > 0 dims that page behind the panel (black at that
 * opacity). 255/0 is the opaque default: no visible change.
 */
void modal_surf_set_alpha(uint8_t panel_alpha, uint8_t scrim_alpha);
void modal_surf_get_alpha(uint8_t *panel_alpha, uint8_t *scrim_alpha);

/* -------------------------------------------------------------------------
 * TEXT SELECTION (the pager surfaces)
 * -------------------------------------------------------------------------
 * `view` and `hexview` are monospace text pagers: long-press + drag selects a
 * character range, the highlight stays after the finger lifts, a tap inside
 * copies it and a tap outside clears it. Both go through the shared selection
 * core, so the text lands in the shell clipboard (`clip`/`paste`). The verbs
 * below are the serial entry points for those same two actions (`ui selection
 * copy|clear`), so a suite can drive the core without a pointer. LVGL task.
 */
bool modal_surf_selection_copy(void);
bool modal_surf_selection_clear(void);

#ifdef __cplusplus
}
#endif



/**
 * @brief Run a hex viewer modal surface for binary files.
 *
 * @param title       Dialog title (may be NULL).
 * @param file_path   Path to binary file to view (may be NULL for stdin).
 * @param timeout_ms  Auto-cancel after this many ms (0 = wait forever).
 * @return 0 on exit, -1 on error.
 */
int modal_hexview_run(const char *title, const char *file_path, uint32_t timeout_ms);

/**
 * @brief Run a hex viewer modal surface over an in-memory buffer.
 *
 * Same surface and row format as modal_hexview_run, but renders @p bytes
 * ( @p len bytes) instead of opening a file; @p base_addr is printed for
 * bytes[0] (a RAM address or disk LBA base). Used by the `debug mem` /
 * `debug disk` verbs. Batch/headless callers use the plain-text dump
 * instead — a modal blocks the command worker.
 *
 * @param title       Dialog title (may be NULL).
 * @param bytes       Buffer to render (must outlive the call only).
 * @param len         Bytes to render (capped like file content).
 * @param base_addr   Address printed for bytes[0].
 * @param timeout_ms  Auto-cancel after this many ms (0 = wait forever).
 * @return 0 on exit, -1 on error.
 */
int modal_hexview_run_buf(const char *title, const uint8_t *bytes, size_t len,
                          uint32_t base_addr, uint32_t timeout_ms);

/* -------------------------------------------------------------------------
 * FORM: a multi-field modal editor for batch apps (the Palm-style form).
 * ---------------------------------------------------------------------- */

#define MODAL_FORM_MAX_FIELDS     12
#define MODAL_FORM_OPTIONS_BYTES  256

typedef enum {
    MODAL_FORM_TEXT = 0,
    MODAL_FORM_PASSWORD,
    MODAL_FORM_CHECK,
    MODAL_FORM_SELECT,
    MODAL_FORM_RANGE,
} modal_form_field_type_t;

typedef struct {
    modal_form_field_type_t type;
    const char *label;       /**< Field label. */
    char *value;             /**< In: prefill; out: the accepted value. */
    size_t value_size;       /**< Capacity of @p value. */
    const char *options;     /**< SELECT only: `a|b|c`. */
    int min;                 /**< RANGE only. */
    int max;                 /**< RANGE only. */
} modal_form_field_t;

/**
 * @brief Show a multi-field form and return the accepted values.
 *
 * @param title       Form title (may be NULL).
 * @param fields      Field descriptors (mutated with accepted values).
 * @param count       1..MODAL_FORM_MAX_FIELDS.
 * @param timeout_ms  Auto-cancel after this many ms (0 = wait forever).
 * @return 0 when the user accepted, -1 on cancel/Esc/timeout.
 */
int modal_form_run(const char *title, modal_form_field_t *fields, int count,
                   uint32_t timeout_ms);

/**
 * @brief Parse one `/t:secs` option argument (shared by dialog/list/ask/
 * browse/view/hexview).
 *
 * Accepts exactly the historical spelling: a `/t:` prefix (case-insensitive)
 * with at least one trailing character. `secs` parses via atoi(); zero,
 * negative, and non-numeric values all yield timeout 0 (wait forever),
 * matching the inline parsers this replaces.
 *
 * @param arg            Candidate argv element (may be NULL).
 * @param timeout_ms_out Receives secs*1000 on true, untouched on false.
 * @return true when @p arg is a `/t:` option (even a zero one).
 */
bool modal_parse_timeout_arg(const char *arg, uint32_t *timeout_ms_out);

/**
 * @brief Parse one `/v:NAME` option argument (shared by list/ask/browse).
 *
 * @param arg      Candidate argv element (may be NULL).
 * @param name_out Receives the pointer past `/v:` on true, untouched on false.
 * @return true when @p arg is a `/v:` option with a non-empty name.
 */
bool modal_parse_var_arg(const char *arg, const char **name_out);

/* -------------------------------------------------------------------------
 * PURE SELECTORS / STATE (hardware-free; implemented in modal_args.c)
 * -------------------------------------------------------------------------
 * These helpers contain no LVGL, display, storage or FreeRTOS calls. The
 * surfaces call them from their existing handle_usb_key / handle_serial_line
 * callbacks, which the shared runtime reaches through the registered
 * shell_command_ops_t.modal_* hooks - there is no second input path. The host
 * modal gate compiles this very translation unit to verify the transitions.
 */

/* Raw HID key codes used for focus navigation (same values the list surface
 * already interprets: Up 0x52, Down 0x51, Enter 0x28, Esc 0x29). */
#define MODAL_KEY_UP        0x52u
#define MODAL_KEY_DOWN      0x51u
#define MODAL_KEY_LEFT      0x50u
#define MODAL_KEY_RIGHT     0x4Fu
#define MODAL_KEY_HOME      0x4Au
#define MODAL_KEY_END       0x4Du
#define MODAL_KEY_PAGE_UP   0x4Bu
#define MODAL_KEY_PAGE_DOWN 0x4Eu
#define MODAL_KEY_ENTER     0x28u
#define MODAL_KEY_ESC       0x29u

/** Clamp an index into [0, count). Returns 0 when count <= 0. */
int modal_focus_clamp(int index, int count);

/**
 * @brief Move a focus index by one navigation key.
 *
 * @param key_code   Raw HID code (MODAL_KEY_* above).
 * @param count      Number of focusable items (<= 0 -> no movement).
 * @param current    Current focused index.
 * @param page_step  Rows per PageUp/PageDown (<= 0 defaults to 5).
 * @param out        Receives the new index on true, untouched on false.
 * @return true only when @p out was written (the caller repaints).
 */
bool modal_focus_move(uint8_t key_code, int count, int current,
                      int page_step, int *out);

/** Progress percentage, clamped to 0..100. total == 0 returns 0. */
uint8_t modal_progress_percent(uint32_t done, uint32_t total);

/** Maximum number of suspended modal sessions. */
#define MODAL_SESSION_MAX 8

typedef enum {
    MODAL_SESSION_FORE = 0,  /**< foreground / running */
    MODAL_SESSION_BACK = 1,  /**< suspended / background */
} modal_session_state_t;

typedef struct {
    const char *name;                /**< borrowed label, never copied */
    int focus;                       /**< focused row/widget index */
    modal_session_state_t state;
} modal_session_slot_t;

typedef struct {
    modal_session_slot_t slot[MODAL_SESSION_MAX];
    int depth;                       /**< 0..MODAL_SESSION_MAX; top is depth-1 */
} modal_session_stack_t;

void modal_session_stack_init(modal_session_stack_t *st);
int  modal_session_stack_depth(const modal_session_stack_t *st);
bool modal_session_stack_empty(const modal_session_stack_t *st);
bool modal_session_stack_full(const modal_session_stack_t *st);

/** @param from_top 0 = top of stack. Returns NULL when out of range. */
const char *modal_session_stack_name(const modal_session_stack_t *st, int from_top);

/** Find a name by exact match, searching from the top. @return from_top index
 *  or -1 when absent. */
int modal_session_stack_find(const modal_session_stack_t *st, const char *name);

/** Push a foreground slot, marking the previous top suspended. */
bool modal_session_stack_push(modal_session_stack_t *st, const char *name);

/** Pop the top slot and make the new top foreground again. */
bool modal_session_stack_pop(modal_session_stack_t *st);

bool modal_session_stack_suspend(modal_session_stack_t *st);
bool modal_session_stack_resume(modal_session_stack_t *st);

/** Bring a named slot to the top (rotate), making it the only foreground one. */
bool modal_session_stack_switch(modal_session_stack_t *st, const char *name);

/** Focus accessor by from_top index (0 = top). */
int  modal_session_stack_focus(const modal_session_stack_t *st, int from_top);
bool modal_session_stack_set_focus(modal_session_stack_t *st, int from_top, int focus);

/* -------------------------------------------------------------------------
 * NEW READY-MADE SURFACES (modal_surface_t descriptors in modal_surf.c)
 * -------------------------------------------------------------------------
 * All three accept a `timeout_ms` produced by modal_parse_timeout_arg() and
 * all three descriptors set abort_cancels = true. The /v:NAME variable write
 * belongs to the batch verb layer, which reuses modal_parse_var_arg().
 */

/** Scrollable picker with focus navigation. @return selected 0-based index,
 *  or -1 on cancel/Esc/timeout. @p initial is clamped to [0,count). */
int modal_picker_run(const char *title, const char **items, int count,
                     int initial, uint32_t timeout_ms, int *out_selected);

/**
 * @brief Progress surface driven by async event bits.
 *
 * @p tick_ms > 0 with @p step > 0 auto-advances done by @p step on each tick
 * and closes cleanly at 100%. With tick_ms == 0 the surface waits for
 * modal_progress_publish() (manual mode). /t: still bounds both modes.
 * @return 0 on completion, -1 on cancel/Esc/timeout.
 */
int modal_progress_run(const char *title, const char *label, uint32_t total,
                       uint32_t tick_ms, uint32_t step, uint32_t timeout_ms);

/** Publish progress from a native producer. Returns false when no progress
 *  surface is active. Values are clamped; reaching total sets DONE. */
bool modal_progress_publish(uint32_t done, uint32_t total);

/**
 * @brief Extended form: configurable button labels + keyboard focus walking.
 *
 * Up/Down/Tab move the focused field (rendered with LV_STATE_FOCUSED); Enter
 * accepts, Esc cancels. @p out_focus, when non-NULL, receives the last focus.
 */
int modal_form_run_ex(const char *title, modal_form_field_t *fields, int count,
                      const char *accept_label, const char *cancel_label,
                      uint32_t timeout_ms, int *out_focus);

#endif /* P4MINISHELL_MODAL_SURF_H */
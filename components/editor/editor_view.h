/**
 * @file editor_view.h
 * @brief LVGL surface for the P4MiniShell editor.
 *
 * Renders an editor_doc_t as syntax-coloured rows inside a scrollable
 * container, draws a block cursor and a selection overlay, and routes touch
 * (tap to move the caret, drag to select) and OSK/USB keys into the document.
 * The view is modal: while open it owns the transcript region and the input
 * row becomes a status bar.
 */

#ifndef P4MINISHELL_EDITOR_VIEW_H
#define P4MINISHELL_EDITOR_VIEW_H

#include "editor.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open the modal editor view for @p doc. Must run on the LVGL task.
 * @return true on success.
 */
bool editor_view_open(editor_doc_t *doc, editor_control_t *control);

/** Close the modal editor view and release its widgets. LVGL task. */
void editor_view_close(void);

/** Whether the editor view is currently open. */
bool editor_view_is_open(void);

/** Snapshot of the open editor's document state (for `ui state` and tests). */
typedef struct {
    bool open;
    bool modified;
    bool preview;
    bool wrap;
    bool readonly;
    size_t cursor_row;
    size_t cursor_col;
    size_t line_count;
    char path[P4_CONFIG_SD_PATH_BYTES];
} editor_view_state_t;

/** Fill @p out with the current editor state (zeroed when closed). */
void editor_view_get_state(editor_view_state_t *out);

/** Whether the rendered Markdown preview is showing (read-only). */
bool editor_view_is_preview(void);

/** Live-update the cursor blink period of an open editor (0 = steady).
 * No-op unless open. Takes the port lock. */
void editor_view_set_blink_ms(uint32_t blink_ms);

/** Rebuild open editor spans with current fonts after a font switch.
 * No-op unless open (spans re-resolve per rebuild). */
void editor_view_refresh_fonts(void);

/**
 * Handle a USB key press routed to the editor. Runs on the LVGL task.
 * @return true when the key was consumed by the editor.
 */
bool editor_view_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii);

/**
 * Handle an on-screen keyboard button label routed to the editor.
 * Runs on the LVGL task. @p label is the button text (e.g. "a", "1#",
 * LV_SYMBOL_BACKSPACE, ...).
 * @return true when the key was consumed by the editor.
 */
bool editor_view_handle_osk(const char *label);

/**
 * Map an editor OSK command label ("Save", "WdL", "Open", ...) to its
 * editor_key_t. Pure string matching (no LVGL side effects), so unit tests
 * and the surface share one table. Symbols, mode buttons, and single
 * characters are handled elsewhere and return false here.
 * @return true with @p out set when @p label names an editor command.
 */
bool editor_osk_key_from_label(const char *label, editor_key_t *out);

/** Notify the editor that the document was saved (update the status bar). */
void editor_view_notify_saved(bool ok);

/** lv_async_call trampoline for editor_view_notify_saved(). */
void editor_view_notify_saved_cb(void *user_data);

/** Notify the editor that the document was reloaded (rebuild + status). */
void editor_view_notify_reloaded(bool ok);

/** lv_async_call trampoline for editor_view_notify_reloaded(). */
void editor_view_notify_reloaded_cb(void *user_data);

/** Notify the editor that another file was opened (rebuild + status). */
void editor_view_notify_opened(bool ok);

/** lv_async_call trampoline for editor_view_notify_opened(). */
void editor_view_notify_opened_cb(void *user_data);

/** Scroll the editor surface vertically by @p pixels (mouse wheel). */
void editor_view_scroll_by(int32_t pixels);

/** Request a quit even while the view is still opening (LVGL task). */
void editor_view_set_quit_requested(void);

/** Request a save even while the view is still opening (LVGL task). */
void editor_view_set_save_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EDITOR_VIEW_H */

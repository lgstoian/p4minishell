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

#endif /* P4MINISHELL_MODAL_SURF_H */
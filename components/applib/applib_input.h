/**
 * @file applib_input.h
 * @brief Input-with-timeout group of the applib.
 *
 * Native apps that need a bounded keypress or a typed line (the primitive
 * behind the shell's `pause` / `choice /T` / `set /p` timeouts) use these
 * helpers instead of reaching into the shell key queue. Every wait is bounded
 * by the caller's timeout, and on a headless board (no UART console, no USB
 * keyboard) both helpers return false immediately rather than stalling.
 */

#ifndef P4MINISHELL_APPLIB_INPUT_H
#define P4MINISHELL_APPLIB_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wait up to @p timeout_ms for a single keypress.
 *
 * @param timeout_ms  Wait bound in milliseconds (0 still waits at least one
 *                    scheduler tick and returns false when nothing arrives).
 * @param key_out     Receives the pressed key on success (may be NULL to
 *                    discard, like `pause`).
 * @return true when a key arrived, false on timeout or when no interactive
 *         input source is attached.
 */
bool app_wait_key(uint32_t timeout_ms, char *key_out);

/**
 * Read one line (up to @p size bytes, newline stripped) within @p timeout_ms.
 * Echoes as the user types; an empty line is a successful read of `""`.
 *
 * @param buf         Destination buffer (must not be NULL when size > 0).
 * @param size        Destination capacity in bytes.
 * @param timeout_ms  Wait bound in milliseconds.
 * @return true when a line was submitted, false on cancel / timeout / no
 *         interactive source.
 */
bool app_read_line(char *buf, size_t size, uint32_t timeout_ms);

/**
 * Read a line within @p timeout_ms WITHOUT echoing the typed keys (password
 * mode: Backspace erases silently, ESC cancels, Enter completes and prints a
 * newline). The app-side equivalent of `set /p NAME=<prompt> /P`.
 *
 * @return true when a line was submitted, false on cancel / timeout / no
 *         interactive source.
 */
bool app_read_password(char *buf, size_t size, uint32_t timeout_ms);

/**
 * Render a numbered menu in the transcript display and read a numeric choice
 * within @p timeout_ms — the app menu/form primitive (the batch `menu`
 * command is its batch-side equivalent).
 *
 * @param title       Optional heading (may be NULL or "").
 * @param items       NULL-terminated item strings (at least one).
 * @param timeout_ms  Read bound in milliseconds.
 * @return The 1-based index of the chosen item, or 0 on cancel / timeout /
 *         an invalid entry.
 */
int app_menu(const char *title, const char **items, int count, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_INPUT_H */

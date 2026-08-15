/**
 * @file applib_ui.h
 * @brief App-mode group of the applib (take over the shell screen).
 *
 * A clean way for a native app to take over the shell, exactly like the batch
 * `appmode` command: the current transcript is saved, the shell input widgets
 * can be hidden for a full-screen app surface, and on exit the saved screen is
 * restored. The mechanics live once in the shell core (`shell_screen_*` /
 * `shell_app_mode_*`) and are shared with the batch `appmode` command, so
 * nothing is duplicated.
 */

#ifndef P4MINISHELL_APPLIB_UI_H
#define P4MINISHELL_APPLIB_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Enter app mode: the current transcript is saved (colours preserved) and,
 * when @p full_screen is true, the shell input widgets are hidden so the
 * transcript becomes a clean full-screen app surface. While in app mode the
 * app prints its own content; call `app_mode_exit` when done to restore the
 * saved screen.
 *
 * @return true on success.
 */
bool app_mode_enter(bool full_screen);

/**
 * Leave app mode: restore the shell input widgets (if full-screen) and
 * restore the saved transcript, discarding the app's output.
 *
 * @return true on success.
 */
bool app_mode_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_UI_H */

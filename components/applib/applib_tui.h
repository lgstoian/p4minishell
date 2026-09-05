/**
 * @file applib_tui.h
 * @brief Native TUI SDK — re-exports the shared TUI engine for native apps.
 *
 * Native apps use the same TUI cell buffer as batch apps (components/tui).
 * This header is a thin re-export so `applib.h` remains the single umbrella
 * for native apps. The implementation lives in components/tui (tui.c); this
 * component's applib_tui.c is intentionally empty (stub) until native TUI
 * helpers are needed.
 */

#ifndef P4MINISHELL_APPLIB_TUI_H
#define P4MINISHELL_APPLIB_TUI_H

#include "tui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* No extra declarations — native apps include <tui.h> directly or via this
 * header and call tui_init/deinit/draw_box/print_at/flush etc. */

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_TUI_H */

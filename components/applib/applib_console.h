/**
 * @file applib_console.h
 * @brief Console output group of the native-app runtime library.
 *
 * An app's stdout is the on-screen transcript (and the UART console); when the
 * app runs inside a command dispatch the text is also captured by the active
 * `>` / `>>` redirection, so `myapp > out.txt` works like a built-in command.
 * Data arguments containing a literal `@` or `%` are passed as a `%s`
 * argument so they stay data.
 */

#ifndef P4MINISHELL_APPLIB_CONSOLE_H
#define P4MINISHELL_APPLIB_CONSOLE_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Print to the app's stdout (transcript + redirection layer).
 *  @return characters written, or -1 on failure. */
int app_printf(const char *format, ...);

/** `app_printf` with an explicit `va_list`. */
int app_vprintf(const char *format, va_list args);

/** Print with ANSI `@`-specifier colours (the shell palette).
 *  @return characters written, or -1 on failure. */
int app_printf_ansi(const char *format, ...);

/** `app_printf_ansi` with an explicit `va_list`. */
int app_vprintf_ansi(const char *format, va_list args);

/**
 * Print text wrapped in the given ANSI SGR codes (`ESC[<codes>m text
 * ESC[0m`) — the menu/form primitive for reverse video, bold, and colour
 * (`"7"` reverse, `"1;7"` bold reverse, `"31"` red, `"90"` muted, ...).
 * Rendered in the transcript display like any app output.
 *
 * @return characters written, or -1 on failure.
 */
int app_print_styled(const char *sgr_codes, const char *format, ...);

/** Section heading line (bright green). */
void app_print_heading(const char *format, ...);

/** "label: value" line (value in the important-value colour). */
void app_print_field(const char *label, const char *format, ...);

/** Success line. */
void app_print_ok(const char *format, ...);

/** Error line (does not touch errorlevel or the debug log). */
void app_print_error(const char *format, ...);

/** Warning line. */
void app_print_warning(const char *format, ...);

/** Muted / secondary line. */
void app_print_muted(const char *format, ...);

/** Usage line for an incorrectly invoked app. */
void app_print_usage(const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_CONSOLE_H */

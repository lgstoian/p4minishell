/**
 * @file config_cmd.h
 * @brief `config` command: read/write the CONFIG.SYS settings file.
 *
 * Lives in the command component (alongside command_ui.c) because it bridges
 * the display, audio, shell, networking, keyboard, and header modules through
 * their public APIs and needs command.h's power-idle accessors. The pure
 * CONFIG.SYS line-editing helpers are exposed here so the unit test project
 * can drive them without any hardware.
 */

#ifndef P4MINISHELL_CONFIG_CMD_H
#define P4MINISHELL_CONFIG_CMD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Handle the `config` command (dispatched by shell_execute_command_core). */
void shell_command_config(int argc, char **argv);

/**
 * Persist a single `KEY=value` directive into CONFIG.SYS, applying nothing.
 *
 * This is the ONE writer every owning command uses for `/save` (theme, font,
 * header mode, cursor, keyboard mode, timezone, security, owner, ...), so the
 * scalar-preference store stays single-sourced. Opens the file, upserts the
 * directive preserving comments/other lines, and writes atomically.
 *
 * @return true when CONFIG.SYS now holds the directive.
 */
bool config_persist_set(const char *key, const char *value);

/**
 * Read a directive value from CONFIG.SYS (case-insensitive key).
 *
 * @return the value length (>= 0) when found, or -1 when the key/file is
 *         absent or the SD card is unavailable.
 */
int config_get_saved(const char *key, char *out, size_t out_size);

/**
 * Read the value of a `KEY=value` directive from a CONFIG.SYS text buffer.
 * The key is matched case-insensitively; comment lines, blank lines, and
 * lines without '=' are skipped.
 *
 * @param text     NUL-terminated file text (may be "").
 * @param key      Directive name, e.g. "BRIGHTNESS".
 * @param out      Receives the trimmed value (optional, may be NULL).
 * @param out_size Capacity of @p out.
 * @return The value length (>= 0) when found, or -1 when the key is absent.
 */
int config_directive_get(const char *text, const char *key,
                         char *out, size_t out_size);

/**
 * Set a `KEY=value` directive in a CONFIG.SYS text buffer. Every existing
 * line for the key is removed and a single `KEY=value\n` line is appended,
 * so the value the boot parser applies is always this one. Other lines,
 * comments, and blank lines are preserved.
 *
 * @param text   NUL-terminated buffer, modified in place.
 * @param cap    Total capacity of the buffer (including the NUL).
 * @param key    Directive name, e.g. "BRIGHTNESS".
 * @param value  New value, e.g. "40".
 * @return true when the buffer now holds the updated text.
 */
bool config_directive_upsert(char *text, size_t cap,
                             const char *key, const char *value);

/**
 * Remove every `KEY=value` directive line from a CONFIG.SYS text buffer.
 *
 * @param text NUL-terminated buffer, modified in place.
 * @param cap  Total capacity of the buffer (including the NUL).
 * @param key  Directive name, e.g. "BRIGHTNESS".
 * @return true when at least one matching line was removed.
 */
bool config_directive_remove(char *text, size_t cap, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_CONFIG_CMD_H */

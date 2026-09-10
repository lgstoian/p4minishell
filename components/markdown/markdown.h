/**
 * @file markdown.h
 * @brief Markdown rendering for P4MiniShell (CommonMark-ish subset).
 *
 * Line mode (`markdown_render_line`) renders single blocks (headings,
 * lists, quotes, hr) plus flanking-safe inline spans on any line, so
 * `echo`/`type` output can be styled without ever corrupting plain text
 * (`2 * 3`, `foo_bar`, `*ptr` need no closer and pass through untouched).
 * Document mode (`markdown_render_doc`) adds fences and aligned tables.
 * Everything emits ANSI SGR consumed by the existing pipeline (spans track
 * bold/italic/underline/strike; serial terminals render natively).
 */

#ifndef P4MINISHELL_MARKDOWN_H
#define P4MINISHELL_MARKDOWN_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Session auto-render for echo/type (default on). `markdown on|off`. */
void markdown_set_auto(bool on);
bool markdown_get_auto(void);

/** Display width in cells (ASCII 1, CJK 2, combining 0, control 0). */
size_t markdown_display_width(const char *s);

/** Copy src to dst minus SGR escape sequences (NUL-terminated, bounded).
 * Used by plain-text surfaces (viewer) to show rendered Markdown without
 * markup noise. Returns bytes written excluding NUL. */
size_t markdown_strip_ansi(const char *src, char *dst, size_t dst_size);

/**
 * Render one line to ANSI. Always NUL-terminates (truncates safely, closing
 * any open SGR). Returns true when markup transformed the line.
 */
bool markdown_render_line(const char *line, char *out, size_t out_size);

/**
 * Render a full document (fences, tables, lists). Always NUL-terminates.
 * Returns bytes written excluding NUL.
 */
size_t markdown_render_doc(const char *md, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_MARKDOWN_H */

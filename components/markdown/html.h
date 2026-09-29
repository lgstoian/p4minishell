/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file html.h
 * @brief HTML reading: render an HTML document to ANSI SGR.
 *
 * The reader counterpart of the HTML serializer in markdown_html.c. It is a
 * bounded, single-pass subset reader for on-device display of `.html`/`.htm`
 * files (the viewer, `type`, and the editor preview) and is written for the
 * constructs the serializer emits plus the common hand-written ones:
 * headings, paragraphs, lists, blockquotes, pre/code, tables, emphasis,
 * links, images, and the usual named/numeric entities. `head`, `script`,
 * `style`, and comments are skipped.
 *
 * Output is ANSI SGR, so plain surfaces reuse markdown_strip_ansi() and no
 * second stripper is needed. Self-contained (no shell/batch deps; its own
 * bounded writer and UTF-8 encoder), matching the other document renderers.
 */

#ifndef P4MINISHELL_HTML_H
#define P4MINISHELL_HTML_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Render an HTML document to ANSI SGR.
 *
 * Block tags map to line structure (headings, paragraphs, nested
 * `ul`/`ol` items, blockquotes, `pre`, table rows, `hr`), inline tags to
 * SGR (bold/italic/underline/strike, dim code, cyan-underlined links), and
 * entities are decoded to UTF-8. `head`/`title`/`script`/`style` and
 * comments are skipped. Always NUL-terminates @p out when it is non-NULL.
 *
 * @param html      NUL-terminated source (may be NULL -> returns 0).
 * @param out       Destination, or NULL for a measure-only call.
 * @param out_size  Destination size including the NUL.
 * @return the number of bytes that should be written (excluding the NUL);
 *         a value >= out_size means the destination was truncated.
 */
size_t html_render_ansi(const char *html, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_HTML_H */

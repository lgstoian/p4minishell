/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub_export.h
 * @brief Writerdeck: package a Markdown document as a valid EPUB 3 book.
 *
 * The reader side lives in `epub.c`; this is the write side and it exists so a
 * document written on the device (`edit`) can be exported to an e-book and
 * opened by the same reader (`epub open`). Structure: `mimetype` first (stored,
 * no extra field), `META-INF/container.xml`, `OEBPS/content.opf`,
 * `OEBPS/nav.xhtml`, `OEBPS/toc.ncx` and one XHTML chapter rendered by the
 * single Markdown-to-HTML writer.
 *
 * @module epub
 * @layer leaf
 * @domain app
 * @provides Markdown -> EPUB 3 packaging
 * @status stable
 * @test test/main/test_epub.c
 * @hard-rule ai-context.md#module-layering-rules
 */
#ifndef P4MINISHELL_EPUB_EXPORT_H
#define P4MINISHELL_EPUB_EXPORT_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write @p markdown to @p path as a one-chapter EPUB 3 book.
 *
 * @param path      Destination `.epub` path (truncated).
 * @param title     Book title (also the chapter heading); NULL/empty falls
 *                  back to the destination file name.
 * @param markdown  Markdown source (UTF-8, NUL-terminated).
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for a bad path; ESP_FAIL when
 *         the archive cannot be written (the partial file is removed).
 */
esp_err_t epub_export_write(const char *path, const char *title,
                            const char *markdown);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EPUB_EXPORT_H */

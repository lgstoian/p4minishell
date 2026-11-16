/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file editor_doc_internal.h
 * @brief Internal seam between editor_doc.c (document model) and editor.c (SD).
 *
 * editor_doc.c owns the byte-preserving line store and every pure document
 * operation. The SD half that stays in editor.c (load, save, reload) needs a
 * few line-store primitives to stream a file into the document and to swap a
 * freshly loaded document in: those are defined exactly once in editor_doc.c
 * and declared here. Nothing is duplicated, and editor_doc.c still needs no
 * storage, display, USB or board BSP.
 *
 * The header is private to components/editor; no other component includes it.
 *
 * @module components/editor
 * @layer leaf
 * @domain app
 * @provides internal declarations for the document model owned by editor_doc.c
 * @status stable
 * @test test/main/test_editor.c
 * @hard-rule declarations only - no storage, display, USB or board BSP here
 */

#ifndef P4MINISHELL_EDITOR_DOC_INTERNAL_H
#define P4MINISHELL_EDITOR_DOC_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "editor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Free one line's heap text and zero its length/capacity (editor_doc.c). */
void editor_line_free(editor_line_t *line);

/** Insert a blank line slot at @p row. Returns the new line or NULL. */
editor_line_t *editor_doc_insert_line(editor_doc_t *doc, size_t row);

/** Remove the line at @p row, freeing it. */
void editor_doc_remove_line(editor_doc_t *doc, size_t row);

/**
 * Append a loaded file chunk to the document, splitting it into lines on '\n'
 * and stripping a CR that precedes a newline (the SD loader's parser).
 * @return false on allocation failure or when the line cap is hit.
 */
bool editor_doc_append_bytes(editor_doc_t *doc, const char *buf, size_t len);

/** Release all undo/redo snapshots and reset both rings (reload, free). */
void editor_doc_undo_reset(editor_doc_t *doc);

/**
 * Replace @p dst's contents with @p src's (File > Open): the document object
 * keeps its identity, the bytes/metadata move over, the cursor returns to the
 * origin, the selection clears and the undo ring resets — the post-load
 * baseline, mirroring editor_doc_reload() plus a path change. @p src is
 * emptied (its line array is stolen) and must still be freed by the caller.
 */
void editor_doc_replace_contents(editor_doc_t *dst, editor_doc_t *src);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EDITOR_DOC_INTERNAL_H */

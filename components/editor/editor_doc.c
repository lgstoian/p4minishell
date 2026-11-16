/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file editor_doc.c
 * @brief Hardware-free editor document model, lexers and key routing.
 *
 * Everything here is pure C: the byte-preserving line store, the cursor and
 * selection state, insert/delete/undo/redo, find and replace, the comment
 * toggle, the paren/% match jump, the per-syntax lexers, the OSK label table,
 * the USB key map and the line-number gutter. No LVGL, display, storage, USB
 * or board BSP is referenced, which is what lets the host unit-test lane
 * (test/host) exercise the model natively.
 *
 * The SD half stays in editor.c: load/save/reload, the DMA bounce buffer, the
 * modal worker session and the LVGL view hand-off. It reaches the handful of
 * line-store helpers it needs through editor_doc_internal.h, so each helper is
 * defined exactly once (here).
 *
 * @module components/editor
 * @layer leaf
 * @domain app
 * @provides editor_doc_t document model, syntax lexers, spell-free key routing
 * @status stable
 * @test test/main/test_editor.c
 * @hard-rule pure C only - no LVGL, display, storage, USB or board BSP in this file
 */

#include "editor.h"
#include "editor_doc_internal.h"
#include "ansi_palette.h"
#include "filetype.h"
#include "shell.h"
#include "p4minishell_config.h"
#include "p4heap.h"
#include "esp_heap_caps.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ========================================================================
 * PSRAM-FIRST ALLOCATION
 * ======================================================================== */

void *editor_mem_alloc(size_t size)
{
    void *ptr;

    if (size == 0) {
        size = 1;
    }
    ptr = p4heap_alloc_psram(size);
    return ptr;
}

void *editor_mem_realloc(void *ptr, size_t size)
{
    void *out;

    if (ptr == NULL) {
        return editor_mem_alloc(size);
    }
    if (size == 0) {
        size = 1;
    }
    /* p4heap_realloc_psram leaves @p ptr valid on failure (NULL propagates
     * to the caller, which keeps using the old block). */
    out = p4heap_realloc_psram(ptr, size);
    return out;
}

void editor_mem_free(void *ptr)
{
    if (ptr != NULL) {
        heap_caps_free(ptr);
    }
}
/* ========================================================================
 * LINE HELPERS
 * ======================================================================== */

static void editor_line_init(editor_line_t *line)
{
    line->text = NULL;
    line->length = 0;
    line->capacity = 0;
}

static bool editor_line_reserve(editor_line_t *line, size_t needed)
{
    size_t new_cap;
    char *new_text;

    if (line->capacity >= needed) {
        return true;
    }

    new_cap = line->capacity == 0 ? 16 : line->capacity;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    new_text = editor_mem_realloc(line->text, new_cap);
    if (new_text == NULL) {
        return false;
    }
    line->text = new_text;
    line->capacity = new_cap;
    return true;
}

void editor_line_free(editor_line_t *line)
{
    editor_mem_free(line->text);
    line->text = NULL;
    line->length = 0;
    line->capacity = 0;
}

/** Insert @p len bytes at @p col within @p line (shifting the tail right). */
static bool editor_line_insert_at(editor_line_t *line, size_t col, const char *bytes, size_t len)
{
    if (col > line->length) {
        col = line->length;
    }
    if (!editor_line_reserve(line, line->length + len + 1)) {
        return false;
    }
    memmove(line->text + col + len, line->text + col, line->length - col);
    memcpy(line->text + col, bytes, len);
    line->length += len;
    line->text[line->length] = '\0';
    return true;
}

/** Delete @p len bytes at @p col within @p line. */
static void editor_line_delete_at(editor_line_t *line, size_t col, size_t len)
{
    if (col >= line->length) {
        return;
    }
    if (col + len > line->length) {
        len = line->length - col;
    }
    memmove(line->text + col, line->text + col + len, line->length - col - len);
    line->length -= len;
    line->text[line->length] = '\0';
}

/** Set a line's exact content. */
static bool editor_line_set(editor_line_t *line, const char *bytes, size_t len)
{
    if (!editor_line_reserve(line, len + 1)) {
        return false;
    }
    if (len > 0) {
        memcpy(line->text, bytes, len);
    }
    line->length = len;
    line->text[len] = '\0';
    return true;
}

static void editor_undo_push(editor_doc_t *doc);
static void editor_redo_clear(editor_doc_t *doc);
void editor_doc_undo_reset(editor_doc_t *doc);

/** One undo/redo snapshot: serialized lines plus cursor/selection state. */
typedef struct {
    char *data;                /**< Serialized lines joined with '\n' */
    size_t len;                /**< Serialized byte length (budget accounting) */
    size_t cursor_row;
    size_t cursor_col;
    size_t sel_row;
    size_t sel_col;
    bool selection_active;
    bool modified;             /**< Dirty flag at snapshot time */
} editor_undo_snapshot_t;

/** Undo/redo ring storage, kept on the document itself (undo_storage). */
typedef struct {
    editor_undo_snapshot_t undo[P4_CONFIG_EDITOR_UNDO_DEPTH];
    size_t undo_count;         /**< Valid entries at the tail of undo[] */
    size_t undo_head;          /**< Ring head (oldest entry) */
    editor_undo_snapshot_t redo[P4_CONFIG_EDITOR_UNDO_DEPTH];
    size_t redo_count;
    size_t redo_head;
    size_t bytes;              /**< Total bytes held by undo[] + redo[] data */
} editor_undo_state_t;

/* ========================================================================
 * DOCUMENT LINE ARRAY
 * ======================================================================== */

static bool editor_doc_reserve_lines(editor_doc_t *doc, size_t needed)
{
    size_t new_cap;
    editor_line_t *new_lines;

    if (doc->line_capacity >= needed) {
        return true;
    }

    new_cap = doc->line_capacity == 0 ? 8 : doc->line_capacity;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    new_lines = editor_mem_realloc(doc->lines, new_cap * sizeof(editor_line_t));
    if (new_lines == NULL) {
        return false;
    }
    doc->lines = new_lines;
    doc->line_capacity = new_cap;
    return true;
}

/** Insert a blank line slot at @p row. Returns the new line or NULL. */
editor_line_t *editor_doc_insert_line(editor_doc_t *doc, size_t row)
{
    if (row > doc->line_count) {
        row = doc->line_count;
    }
    /* Bound the document during editing too (not only at load): pastes and
     * Enter can otherwise grow the line array without limit. */
    if (doc->line_count >= P4_CONFIG_EDITOR_MAX_LINES) {
        return NULL;
    }
    if (!editor_doc_reserve_lines(doc, doc->line_count + 1)) {
        return NULL;
    }
    memmove(&doc->lines[row + 1], &doc->lines[row],
            (doc->line_count - row) * sizeof(editor_line_t));
    doc->line_count++;
    editor_line_init(&doc->lines[row]);
    return &doc->lines[row];
}

/** Remove the line at @p row, freeing it. */
void editor_doc_remove_line(editor_doc_t *doc, size_t row)
{
    if (row >= doc->line_count) {
        return;
    }
    editor_line_free(&doc->lines[row]);
    memmove(&doc->lines[row], &doc->lines[row + 1],
            (doc->line_count - row - 1) * sizeof(editor_line_t));
    doc->line_count--;
}
/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

editor_doc_t *editor_doc_new(const char *path)
{
    editor_doc_t *doc = calloc(1, sizeof(editor_doc_t));
    if (doc == NULL) {
        return NULL;
    }
    if (path != NULL) {
        snprintf(doc->path, sizeof(doc->path), "%s", path);
    }
    doc->crlf = true;
    doc->trailing_newline = false;
    doc->cursor_row = 0;
    doc->cursor_col = 0;
    doc->syntax = EDITOR_SYNTAX_PLAIN;
    editor_doc_insert_line(doc, 0);
    editor_doc_pick_syntax(doc);
    return doc;
}

void editor_doc_free(editor_doc_t *doc)
{
    if (doc == NULL) {
        return;
    }
    editor_doc_undo_reset(doc);
    for (size_t i = 0; i < doc->line_count; i++) {
        editor_line_free(&doc->lines[i]);
    }
    editor_mem_free(doc->lines);
    free(doc);
}

void editor_doc_pick_syntax(editor_doc_t *doc)
{
    if (doc == NULL || doc->path[0] == '\0') {
        doc->syntax = EDITOR_SYNTAX_PLAIN;
        return;
    }
    /* Single source of truth for extension -> syntax (components/filetype). */
    switch (filetype_of(doc->path)) {
    case FILETYPE_BATCH:
        doc->syntax = EDITOR_SYNTAX_BATCH;
        break;
    case FILETYPE_MARKDOWN:
        doc->syntax = EDITOR_SYNTAX_MARKDOWN;
        break;
    case FILETYPE_JSON:
        doc->syntax = EDITOR_SYNTAX_JSON;
        break;
    case FILETYPE_HTML:
        doc->syntax = EDITOR_SYNTAX_HTML;
        break;
    default:
        doc->syntax = EDITOR_SYNTAX_PLAIN;
        break;
    }
}

/* ========================================================================
 * LOAD (format parsing only; the SD read itself stays in editor.c)
 * ======================================================================== */

bool editor_doc_append_bytes(editor_doc_t *doc, const char *buf, size_t len)
{
    size_t i = 0;

    while (i < len) {
        editor_line_t *line;
        size_t seg_start = i;

        while (i < len && buf[i] != '\n') {
            i++;
        }

        line = &doc->lines[doc->line_count - 1];
        if (i - seg_start > 0) {
            /* Strip a trailing CR so it never becomes part of a line. A lone
             * CR at end of file is not a CRLF marker; only a '\r' directly
             * before a '\n' classifies the file as CRLF below. */
            size_t seg_len = (i - seg_start);
            if (buf[i - 1] == '\r') {
                seg_len--;
            }
            if (seg_len > 0) {
                if (!editor_line_insert_at(line, line->length, buf + seg_start, seg_len)) {
                    return false;
                }
            }
        }

        if (i < len) { /* buf[i] == '\n' */
            /* A '\r' directly before the '\n' marks CRLF endings. */
            if (i > 0 && i > seg_start && buf[i - 1] == '\r') {
                doc->crlf = true;
            }
            i++;
            if (doc->line_count < P4_CONFIG_EDITOR_MAX_LINES) {
                if (editor_doc_insert_line(doc, doc->line_count) == NULL) {
                    return false;
                }
            } else {
                /* The line cap was hit: loading any further would silently
                 * truncate the document. Fail the load honestly so a later
                 * save can never corrupt the file. */
                return false;
            }
        }
    }

    if (len > 0 && buf[len - 1] == '\n') {
        doc->trailing_newline = true;
    }

    return true;
}
/* Replace the live document's contents with a freshly loaded one (File >
 * Open). The document object identity is preserved so the view keeps pointing
 * at it; only bytes/metadata change. Cursor returns to the origin, selection
 * clears, the file is unmodified, and the undo ring resets — the post-load
 * baseline, mirroring editor_doc_reload() plus a path change. */
void editor_doc_replace_contents(editor_doc_t *dst, editor_doc_t *src)
{
    size_t i;

    if (dst == NULL || src == NULL) {
        return;
    }
    for (i = 0; i < dst->line_count; i++) {
        editor_line_free(&dst->lines[i]);
    }
    editor_mem_free(dst->lines);
    dst->lines = src->lines;
    dst->line_count = src->line_count;
    dst->line_capacity = src->line_capacity;
    dst->crlf = src->crlf;
    dst->trailing_newline = src->trailing_newline;
    dst->syntax = src->syntax;
    dst->readonly = src->readonly;
    dst->content_bytes = src->content_bytes;
    src->lines = NULL;
    src->line_count = 0;
    src->line_capacity = 0;
    dst->cursor_row = 0;
    dst->cursor_col = 0;
    dst->selection_active = false;
    dst->sel_row = 0;
    dst->sel_col = 0;
    dst->overwrite = false;
    dst->modified = false;
    editor_doc_undo_reset(dst);
}
/* ========================================================================
 * CURSOR
 * ======================================================================== */

size_t editor_doc_line_count(const editor_doc_t *doc)
{
    return doc != NULL ? doc->line_count : 0;
}

size_t editor_doc_line_length(const editor_doc_t *doc, size_t row)
{
    if (doc == NULL || row >= doc->line_count) {
        return 0;
    }
    return doc->lines[row].length;
}

const char *editor_doc_line_text(const editor_doc_t *doc, size_t row)
{
    if (doc == NULL || row >= doc->line_count) {
        return "";
    }
    return doc->lines[row].text != NULL ? doc->lines[row].text : "";
}

size_t editor_doc_cursor_row(const editor_doc_t *doc)
{
    return doc != NULL ? doc->cursor_row : 0;
}

size_t editor_doc_cursor_col(const editor_doc_t *doc)
{
    return doc != NULL ? doc->cursor_col : 0;
}

bool editor_doc_is_modified(const editor_doc_t *doc)
{
    return doc != NULL && doc->modified;
}

bool editor_doc_needs_save(const editor_doc_t *doc)
{
    return editor_doc_is_modified(doc);
}

size_t editor_doc_word_count(const editor_doc_t *doc)
{
    size_t words = 0;
    size_t row;

    if (doc == NULL) {
        return 0;
    }
    for (row = 0; row < doc->line_count; row++) {
        const char *text = doc->lines[row].text;
        size_t len = doc->lines[row].length;
        bool in_word = false;
        size_t i;

        if (text == NULL) {
            continue;
        }
        for (i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)text[i];
            bool space = (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
            if (space) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                words++;
            }
        }
    }
    return words;
}

static void editor_doc_mark_modified(editor_doc_t *doc)
{
    doc->modified = true;
}

/* ========================================================================
 * EDITING OPERATIONS
 * ======================================================================== */

void editor_doc_insert_char(editor_doc_t *doc, char ch)
{
    if (doc == NULL) {
        return;
    }
    if (doc->cursor_row >= doc->line_count) {
        return;
    }

    if (doc->overwrite) {
        editor_line_t *line = &doc->lines[doc->cursor_row];
        if (doc->cursor_col < line->length) {
            line->text[doc->cursor_col] = ch;
            doc->cursor_col++;
            editor_doc_mark_modified(doc);
            return;
        }
    }

    if (editor_line_insert_at(&doc->lines[doc->cursor_row], doc->cursor_col, &ch, 1)) {
        doc->cursor_col++;
        editor_doc_mark_modified(doc);
    }
}

/** Split the line at the cursor, leaving the cursor on the new line. Records
 *  no undo; mutators that want one call editor_doc_undo_mark() first.
 *  @return true when the split succeeded. */
static bool editor_doc_split_line(editor_doc_t *doc)
{
    editor_line_t *line;
    editor_line_t *next;
    size_t row;
    size_t col;
    size_t tail_len;

    if (doc == NULL || doc->cursor_row >= doc->line_count) {
        return false;
    }

    row = doc->cursor_row;
    col = doc->cursor_col;
    line = &doc->lines[row];
    if (col > line->length) {
        col = line->length;
    }
    tail_len = line->length - col;

    next = editor_doc_insert_line(doc, row + 1);
    if (next == NULL) {
        return false;
    }
    /* Re-fetch the line: insert_line may have reallocated doc->lines, which
     * would make the pre-insert `line` pointer stale (use-after-free). The
     * original line stays at index `row` — the insert shifts only from
     * row+1 upwards. */
    line = &doc->lines[row];
    if (!editor_line_set(next, line->text != NULL ? line->text + col : "", tail_len)) {
        editor_doc_remove_line(doc, row + 1);
        return false;
    }
    line->length = col;
    if (line->text != NULL) {
        line->text[col] = '\0';
    }

    doc->cursor_row = row + 1;
    doc->cursor_col = 0;
    editor_doc_mark_modified(doc);
    return true;
}

void editor_doc_newline(editor_doc_t *doc)
{
    if (doc == NULL || doc->cursor_row >= doc->line_count) {
        return;
    }
    editor_undo_push(doc);
    editor_redo_clear(doc);
    {
        /* Auto-indent: copy the leading whitespace of the split line onto
         * the new line below (same undo snapshot). Captured before the
         * split truncates the source line. */
        size_t row = doc->cursor_row;
        char indent_buf[128];
        size_t indent = 0;
        if (row < doc->line_count && doc->lines[row].text != NULL) {
            while (indent < doc->lines[row].length &&
                   (doc->lines[row].text[indent] == ' ' ||
                    doc->lines[row].text[indent] == '\t') &&
                   indent < sizeof(indent_buf)) {
                indent_buf[indent] = doc->lines[row].text[indent];
                indent++;
            }
        }
        if (!editor_doc_split_line(doc)) {
            return;
        }
        if (indent > 0 && doc->cursor_row < doc->line_count) {
            editor_line_t *next = &doc->lines[doc->cursor_row];
            if (editor_line_insert_at(next, 0, indent_buf, indent)) {
                doc->cursor_col = indent;
            }
        }
    }
}

void editor_doc_insert_bytes(editor_doc_t *doc, const char *bytes, size_t len)
{
    size_t i;
    if (doc == NULL || bytes == NULL || doc->cursor_row >= doc->line_count) {
        return;
    }
    for (i = 0; i < len; i++) {
        char ch = bytes[i];
        if (ch == '\n') {
            /* A failed split (line cap / allocation) stops the insert so a
             * paste cannot silently drop its remaining lines. */
            if (!editor_doc_split_line(doc)) {
                return;
            }
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        /* Raw insert (never overwrite) so a multi-line paste keeps every
         * character exactly as copied. */
        if (editor_line_insert_at(&doc->lines[doc->cursor_row],
                                  doc->cursor_col, &ch, 1)) {
            doc->cursor_col++;
            editor_doc_mark_modified(doc);
        }
    }
}

void editor_doc_backspace(editor_doc_t *doc)
{
    if (doc == NULL || doc->cursor_row >= doc->line_count) {
        return;
    }

    if (doc->selection_active) {
        editor_doc_selection_delete(doc);
        return;
    }

    editor_undo_push(doc);
    editor_redo_clear(doc);

    if (doc->cursor_col > 0) {
        editor_line_t *line = &doc->lines[doc->cursor_row];
        editor_line_delete_at(line, doc->cursor_col - 1, 1);
        doc->cursor_col--;
        editor_doc_mark_modified(doc);
        return;
    }

    /* Join with the previous line when at the start of a line. */
    if (doc->cursor_row > 0) {
        size_t prev_row = doc->cursor_row - 1;
        editor_line_t *prev = &doc->lines[prev_row];
        editor_line_t *cur = &doc->lines[doc->cursor_row];
        size_t prev_len = prev->length;

        if (editor_line_insert_at(prev, prev_len, cur->text != NULL ? cur->text : "", cur->length)) {
            doc->cursor_row = prev_row;
            doc->cursor_col = prev_len;
            editor_doc_remove_line(doc, prev_row + 1);
            editor_doc_mark_modified(doc);
        }
    }
}

void editor_doc_delete(editor_doc_t *doc)
{
    editor_line_t *line;
    editor_line_t *next;
    size_t row;

    if (doc == NULL || doc->cursor_row >= doc->line_count) {
        return;
    }

    if (doc->selection_active) {
        editor_doc_selection_delete(doc);
        return;
    }

    editor_undo_push(doc);
    editor_redo_clear(doc);

    row = doc->cursor_row;
    line = &doc->lines[row];

    if (doc->cursor_col < line->length) {
        editor_line_delete_at(line, doc->cursor_col, 1);
        editor_doc_mark_modified(doc);
        return;
    }

    /* Join with the next line when at the end of a line. */
    if (row + 1 < doc->line_count) {
        next = &doc->lines[row + 1];
        if (editor_line_insert_at(line, line->length, next->text != NULL ? next->text : "", next->length)) {
            editor_doc_remove_line(doc, row + 1);
            editor_doc_mark_modified(doc);
        }
    }
}

void editor_doc_tab(editor_doc_t *doc)
{
    size_t spaces;
    size_t i;

    if (doc == NULL) {
        return;
    }
    spaces = P4_CONFIG_EDITOR_TAB_WIDTH - (doc->cursor_col % P4_CONFIG_EDITOR_TAB_WIDTH);
    if (spaces == 0) {
        return;
    }
    editor_undo_push(doc);
    editor_redo_clear(doc);
    for (i = 0; i < spaces; i++) {
        editor_doc_insert_char(doc, ' ');
    }
}

/* ========================================================================
 * CURSOR MOVEMENT
 * ======================================================================== */

void editor_doc_cursor_left(editor_doc_t *doc)
{
    if (doc == NULL) {
        return;
    }
    if (doc->cursor_col > 0) {
        doc->cursor_col--;
    } else if (doc->cursor_row > 0) {
        doc->cursor_row--;
        doc->cursor_col = doc->lines[doc->cursor_row].length;
    }
}

void editor_doc_cursor_right(editor_doc_t *doc)
{
    if (doc == NULL || doc->cursor_row >= doc->line_count) {
        return;
    }
    if (doc->cursor_col < doc->lines[doc->cursor_row].length) {
        doc->cursor_col++;
    } else if (doc->cursor_row + 1 < doc->line_count) {
        doc->cursor_row++;
        doc->cursor_col = 0;
    }
}

static void editor_doc_move_vertical(editor_doc_t *doc, int delta)
{
    size_t target_col;
    if (doc == NULL || doc->line_count == 0) {
        return;
    }
    if ((delta < 0 && doc->cursor_row == 0) ||
        (delta > 0 && doc->cursor_row + 1 >= doc->line_count)) {
        return;
    }
    target_col = doc->cursor_col;
    doc->cursor_row = (size_t)((int)doc->cursor_row + delta);
    if (doc->cursor_col > doc->lines[doc->cursor_row].length) {
        doc->cursor_col = doc->lines[doc->cursor_row].length;
    }
    if (doc->cursor_col != target_col && doc->lines[doc->cursor_row].length >= target_col) {
        doc->cursor_col = target_col;
    }
}

void editor_doc_cursor_up(editor_doc_t *doc)
{
    editor_doc_move_vertical(doc, -1);
}

void editor_doc_cursor_down(editor_doc_t *doc)
{
    editor_doc_move_vertical(doc, 1);
}

void editor_doc_cursor_home(editor_doc_t *doc)
{
    if (doc != NULL) {
        doc->cursor_col = 0;
    }
}

void editor_doc_cursor_end(editor_doc_t *doc)
{
    if (doc != NULL && doc->cursor_row < doc->line_count) {
        doc->cursor_col = doc->lines[doc->cursor_row].length;
    }
}

/** Classify a byte as a word constituent (printable non-whitespace). */
static bool editor_is_word_char(char ch)
{
    return !(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
}

void editor_doc_cursor_word_left(editor_doc_t *doc)
{
    if (doc == NULL || doc->cursor_row >= doc->line_count) {
        return;
    }
    {
        const char *text = doc->lines[doc->cursor_row].text;
        if (text == NULL) {
            doc->cursor_col = 0;
            return;
        }
        /* Step over the whitespace run immediately left of the cursor. */
        while (doc->cursor_col > 0 &&
               !editor_is_word_char(text[doc->cursor_col - 1])) {
            doc->cursor_col--;
        }
        /* Step over the word itself. */
        while (doc->cursor_col > 0 &&
               editor_is_word_char(text[doc->cursor_col - 1])) {
            doc->cursor_col--;
        }
    }
}

void editor_doc_cursor_word_right(editor_doc_t *doc)
{
    if (doc == NULL || doc->cursor_row >= doc->line_count) {
        return;
    }
    {
        editor_line_t *line = &doc->lines[doc->cursor_row];
        if (line->text == NULL) {
            doc->cursor_col = 0;
            return;
        }
        /* Step over the word at/after the cursor. */
        while (doc->cursor_col < line->length &&
               editor_is_word_char(line->text[doc->cursor_col])) {
            doc->cursor_col++;
        }
        /* Step over the whitespace that follows it. */
        while (doc->cursor_col < line->length &&
               !editor_is_word_char(line->text[doc->cursor_col])) {
            doc->cursor_col++;
        }
    }
}

bool editor_doc_cursor_word(editor_doc_t *doc, char *buf, size_t size)
{
    const char *text;
    size_t len;
    size_t start;
    size_t end;

    if (doc == NULL || buf == NULL || size == 0) {
        return false;
    }
    buf[0] = '\0';
    if (doc->cursor_row >= doc->line_count) {
        return false;
    }
    text = doc->lines[doc->cursor_row].text;
    len = doc->lines[doc->cursor_row].length;
    if (text == NULL || len == 0) {
        return false;
    }
    /* Expand over the whitespace-delimited run around the cursor. */
    start = doc->cursor_col;
    if (start > len) {
        start = len;
    }
    if (start < len && !editor_is_word_char(text[start]) && start > 0) {
        start--;
    }
    end = start;
    while (start > 0 && editor_is_word_char(text[start - 1])) {
        start--;
    }
    while (end < len && editor_is_word_char(text[end])) {
        end++;
    }
    /* Trim ASCII punctuation from both ends (interior kept). */
    while (start < end && ispunct((unsigned char)text[start])) {
        start++;
    }
    while (end > start && ispunct((unsigned char)text[end - 1])) {
        end--;
    }
    if (start >= end || end - start + 1 > size) {
        return false;
    }
    memcpy(buf, text + start, end - start);
    buf[end - start] = '\0';
    return true;
}

/** True when @p ch may appear inside a link/path token under the cursor. */
static bool editor_is_link_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' ||
           ch == '/' || ch == '\\' || ch == ':' || ch == '$' || ch == '%' ||
           ch == '#' || ch == '?' || ch == '&' || ch == '=' || ch == '+' ||
           ch == '~';
}

bool editor_link_under_cursor(editor_doc_t *doc, char *buf, size_t size)
{
    const char *text;
    size_t len;
    size_t col;
    size_t start;
    size_t end;
    size_t i;

    if (doc == NULL || buf == NULL || size == 0) {
        return false;
    }
    buf[0] = '\0';
    if (doc->cursor_row >= doc->line_count) {
        return false;
    }
    text = doc->lines[doc->cursor_row].text;
    len = doc->lines[doc->cursor_row].length;
    if (text == NULL || len == 0) {
        return false;
    }
    col = doc->cursor_col;
    if (col > len) {
        col = len;
    }
    /* Markdown `[text](url)`: cursor anywhere inside the construct yields
     * the URL (same shape the lexer highlights cyan+underline). */
    for (i = 0; i < len; i++) {
        if (text[i] == '[') {
            size_t t = i + 1;
            size_t u;

            while (t < len && text[t] != ']') {
                t++;
            }
            if (t >= len || t + 1 >= len || text[t + 1] != '(') {
                continue;
            }
            u = t + 2;
            while (u < len && text[u] != ')') {
                u++;
            }
            if (u >= len || u <= t + 2 || u - (t + 2) >= size) {
                continue;
            }
            if (col >= i && col <= u) {
                memcpy(buf, text + t + 2, u - (t + 2));
                buf[u - (t + 2)] = '\0';
                return true;
            }
        }
    }
    /* Bare token: expand over link characters, accept URLs and paths
     * (something with a scheme, a slash, or a drive-ish colon). */
    start = col;
    if (start > len) {
        start = len;
    }
    if (start < len && !editor_is_link_char(text[start]) && start > 0) {
        start--;
    }
    end = start;
    while (start > 0 && editor_is_link_char(text[start - 1])) {
        start--;
    }
    while (end < len && editor_is_link_char(text[end])) {
        end++;
    }
    if (start >= end || end - start + 1 > size) {
        return false;
    }
    {
        bool pathy = false;
        size_t k;

        for (k = start; k < end; k++) {
            if (text[k] == '/' || text[k] == '\\' || text[k] == ':') {
                pathy = true;
                break;
            }
        }
        if (!pathy) {
            return false;
        }
        memcpy(buf, text + start, end - start);
        buf[end - start] = '\0';
    }
    return true;
}

void editor_doc_cursor_doc_home(editor_doc_t *doc)
{
    if (doc != NULL && doc->line_count > 0) {
        doc->cursor_row = 0;
        doc->cursor_col = 0;
    }
}

void editor_doc_cursor_doc_end(editor_doc_t *doc)
{
    if (doc != NULL && doc->line_count > 0) {
        doc->cursor_row = doc->line_count - 1;
        doc->cursor_col = doc->lines[doc->line_count - 1].length;
    }
}

void editor_doc_toggle_overwrite(editor_doc_t *doc)
{
    if (doc != NULL) {
        doc->overwrite = !doc->overwrite;
    }
}

void editor_doc_delete_line(editor_doc_t *doc)
{
    size_t row;
    if (doc == NULL || doc->line_count == 0) {
        return;
    }
    if (doc->selection_active) {
        editor_doc_selection_delete(doc);
        return;
    }
    editor_undo_push(doc);
    editor_redo_clear(doc);

    row = doc->cursor_row;
    if (doc->line_count == 1) {
        /* Deleting the only line leaves a single empty line. */
        editor_line_t *line = &doc->lines[0];
        line->length = 0;
        if (line->text != NULL) {
            line->text[0] = '\0';
        }
        doc->cursor_row = 0;
        doc->cursor_col = 0;
        editor_doc_mark_modified(doc);
        return;
    }
    editor_doc_remove_line(doc, row);
    if (doc->cursor_row >= doc->line_count) {
        doc->cursor_row = doc->line_count - 1;
    }
    doc->cursor_col = 0;
    editor_doc_mark_modified(doc);
}

void editor_doc_delete_to_eol(editor_doc_t *doc)
{
    editor_line_t *line;
    if (doc == NULL || doc->cursor_row >= doc->line_count) {
        return;
    }
    if (doc->selection_active) {
        editor_doc_selection_delete(doc);
        return;
    }
    line = &doc->lines[doc->cursor_row];
    if (doc->cursor_col >= line->length) {
        return;
    }
    editor_undo_push(doc);
    editor_redo_clear(doc);
    editor_line_delete_at(line, doc->cursor_col, line->length - doc->cursor_col);
    editor_doc_mark_modified(doc);
}

void editor_doc_set_path(editor_doc_t *doc, const char *path)
{
    if (doc == NULL) {
        return;
    }
    if (path != NULL) {
        snprintf(doc->path, sizeof(doc->path), "%s", path);
    } else {
        doc->path[0] = '\0';
    }
    editor_doc_pick_syntax(doc);
}

/* ========================================================================
 * SELECTION
 * ======================================================================== */

void editor_doc_selection_begin(editor_doc_t *doc)
{
    if (doc == NULL) {
        return;
    }
    doc->sel_row = doc->cursor_row;
    doc->sel_col = doc->cursor_col;
    doc->selection_active = true;
}

void editor_doc_selection_extend(editor_doc_t *doc)
{
    if (doc == NULL) {
        return;
    }
    if (!doc->selection_active) {
        editor_doc_selection_begin(doc);
    }
}

bool editor_doc_has_selection(const editor_doc_t *doc)
{
    if (doc == NULL || !doc->selection_active) {
        return false;
    }
    return !(doc->sel_row == doc->cursor_row && doc->sel_col == doc->cursor_col);
}

void editor_doc_selection_clear(editor_doc_t *doc)
{
    if (doc != NULL) {
        doc->selection_active = false;
    }
}

void editor_doc_selection_bounds(const editor_doc_t *doc,
                                 size_t *s_row, size_t *s_col,
                                 size_t *e_row, size_t *e_col)
{
    size_t a_row, a_col, c_row, c_col;
    if (doc == NULL) {
        if (s_row) { *s_row = 0; }
        if (s_col) { *s_col = 0; }
        if (e_row) { *e_row = 0; }
        if (e_col) { *e_col = 0; }
        return;
    }
    a_row = doc->sel_row; a_col = doc->sel_col;
    c_row = doc->cursor_row; c_col = doc->cursor_col;
    if (a_row < c_row || (a_row == c_row && a_col <= c_col)) {
        if (s_row) { *s_row = a_row; }
        if (s_col) { *s_col = a_col; }
        if (e_row) { *e_row = c_row; }
        if (e_col) { *e_col = c_col; }
    } else {
        if (s_row) { *s_row = c_row; }
        if (s_col) { *s_col = c_col; }
        if (e_row) { *e_row = a_row; }
        if (e_col) { *e_col = a_col; }
    }
}

/** Collect the selected text into a heap string (NUL-terminated). */
static char *editor_doc_selection_to_string(const editor_doc_t *doc, size_t *out_len)
{
    size_t s_row, s_col, e_row, e_col;
    size_t total = 0;
    size_t row;
    char *result;

    if (!editor_doc_has_selection(doc)) {
        return NULL;
    }
    editor_doc_selection_bounds(doc, &s_row, &s_col, &e_row, &e_col);

    for (row = s_row; row <= e_row; row++) {
        size_t len = doc->lines[row].length;
        if (row == s_row) {
            if (s_col > len) s_col = len;
            if (row == e_row) {
                len = (e_col > len ? len : e_col) - s_col;
            } else {
                len = len - s_col;
            }
        } else if (row == e_row) {
            len = e_col > len ? len : e_col;
        }
        total += len;
        if (row < e_row) {
            total += doc->crlf ? 2 : 1;
        }
    }

    result = editor_mem_alloc(total + 1);
    if (result == NULL) {
        return NULL;
    }

    total = 0;
    for (row = s_row; row <= e_row; row++) {
        const char *text = doc->lines[row].text != NULL ? doc->lines[row].text : "";
        size_t len = doc->lines[row].length;
        size_t start = 0;
        if (row == s_row) {
            start = s_col > len ? len : s_col;
            if (row == e_row) {
                len = (e_col > len ? len : e_col) - start;
            } else {
                len = len - start;
            }
        } else if (row == e_row) {
            len = e_col > len ? len : e_col;
        }
        memcpy(result + total, text + start, len);
        total += len;
        if (row < e_row) {
            /* Separator matching the file's EOL style: CRLF files join with
             * "\r\n", LF files with a bare "\n". A lone CR would be skipped
             * by the paste path and silently join the lines. */
            if (doc->crlf) {
                result[total++] = '\r';
                result[total++] = '\n';
            } else {
                result[total++] = '\n';
            }
        }
    }
    result[total] = '\0';
    if (out_len) {
        *out_len = total;
    }
    return result;
}

bool editor_doc_selection_copy(const editor_doc_t *doc)
{
    char *text = editor_doc_selection_to_string(doc, NULL);
    if (text == NULL) {
        return false;
    }
    shell_clipboard_set(text);
    editor_mem_free(text);
    return true;
}

void editor_doc_selection_delete(editor_doc_t *doc)
{
    size_t s_row, s_col, e_row, e_col;
    size_t row;

    if (doc == NULL || !editor_doc_has_selection(doc)) {
        return;
    }
    editor_doc_selection_bounds(doc, &s_row, &s_col, &e_row, &e_col);
    editor_undo_push(doc);
    editor_redo_clear(doc);

    /* First row: delete [s_col, e_col) when single-row. */
    if (s_row == e_row) {
        editor_line_delete_at(&doc->lines[s_row], s_col, e_col - s_col);
    } else {
        /* Trim the first line's tail. The line may be empty (text NULL)
         * when the selection starts at column 0 of an empty line. */
        doc->lines[s_row].length = s_col;
        if (doc->lines[s_row].text != NULL) {
            doc->lines[s_row].text[s_col] = '\0';
        }
        /* Delete the full lines between. After these removals the original
         * last line shifts down to index s_row + 1. */
        for (row = s_row + 1; row < e_row; row++) {
            editor_doc_remove_line(doc, s_row + 1);
        }
        /* Merge the last line's UN-selected tail onto the first line: the
         * selection covers [0, e_col) of the last line, so what survives is
         * the tail [e_col, length). */
        if (s_row + 1 < doc->line_count) {
            editor_line_t *last = &doc->lines[s_row + 1];
            size_t last_len = last->length;
            if (e_col > last_len) {
                e_col = last_len;
            }
            if (editor_line_insert_at(&doc->lines[s_row], s_col,
                                      last->text != NULL ? last->text + e_col : "",
                                      last_len - e_col)) {
                editor_doc_remove_line(doc, s_row + 1);
            }
        }
    }

    doc->cursor_row = s_row;
    doc->cursor_col = s_col;
    doc->selection_active = false;
    editor_doc_mark_modified(doc);
}

void editor_doc_select_all(editor_doc_t *doc)
{
    if (doc == NULL || doc->line_count == 0) {
        return;
    }
    doc->sel_row = 0;
    doc->sel_col = 0;
    doc->cursor_row = doc->line_count - 1;
    doc->cursor_col = doc->lines[doc->line_count - 1].length;
    doc->selection_active = true;
}

bool editor_doc_selection_cut(editor_doc_t *doc)
{
    bool copied = editor_doc_selection_copy(doc);
    if (copied) {
        editor_doc_selection_delete(doc);
    }
    return copied;
}

void editor_doc_paste(editor_doc_t *doc)
{
    const char *clip = shell_clipboard_get();
    size_t len;
    if (doc == NULL || clip == NULL || shell_clipboard_is_file()) {
        return;
    }
    len = strlen(clip);
    if (len == 0) {
        return;
    }
    editor_undo_push(doc);
    editor_redo_clear(doc);
    editor_doc_insert_bytes(doc, clip, len);
}

/* ========================================================================
 * FIND / REPLACE
 * ======================================================================== */

/** Case-aware byte compare (ASCII folding only; 8-bit bytes compare exact). */
static bool editor_byte_equal_fold(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
    return a == b;
}

bool editor_doc_find_next(const editor_doc_t *doc,
                          const char *needle, size_t needle_len,
                          size_t start_row, size_t start_col,
                          bool case_sensitive, bool wrap,
                          size_t *out_row, size_t *out_col)
{
    size_t row;
    size_t scan_col;
    size_t start;

    if (doc == NULL || needle == NULL || needle_len == 0 ||
        doc->line_count == 0) {
        return false;
    }
    if (start_row >= doc->line_count) {
        start_row = doc->line_count - 1;
    }
    start = start_row;

    for (row = start; row < doc->line_count; row++) {
        const char *text = doc->lines[row].text != NULL ? doc->lines[row].text : "";
        size_t len = doc->lines[row].length;
        scan_col = (row == start_row) ? start_col : 0;
        if (scan_col > len) {
            scan_col = len;
        }
        while (scan_col + needle_len <= len) {
            size_t k;
            bool match = true;
            for (k = 0; k < needle_len; k++) {
                char a = text[scan_col + k];
                char b = needle[k];
                if (case_sensitive ? (a != b) : (!editor_byte_equal_fold(a, b))) {
                    match = false;
                    break;
                }
            }
            if (match) {
                if (out_row) *out_row = row;
                if (out_col) *out_col = scan_col;
                return true;
            }
            scan_col++;
        }
    }

    if (!wrap) {
        return false;
    }

    /* Wrap: rescan from the top up to the original start. */
    for (row = 0; row <= start; row++) {
        const char *text = doc->lines[row].text != NULL ? doc->lines[row].text : "";
        size_t len = doc->lines[row].length;
        scan_col = 0;
        if (row == start) {
            /* Only rescans before the original start column on the start row. */
            if (start_col > len) {
                break;
            }
            scan_col = 0;
            len = start_col < len ? start_col : len;
        }
        while (scan_col + needle_len <= len) {
            size_t k;
            bool match = true;
            for (k = 0; k < needle_len; k++) {
                char a = text[scan_col + k];
                char b = needle[k];
                if (case_sensitive ? (a != b) : (!editor_byte_equal_fold(a, b))) {
                    match = false;
                    break;
                }
            }
            if (match) {
                if (out_row) *out_row = row;
                if (out_col) *out_col = scan_col;
                return true;
            }
            scan_col++;
        }
    }
    return false;
}

bool editor_doc_replace_next(editor_doc_t *doc,
                             const char *needle, size_t needle_len,
                             const char *replacement, size_t repl_len,
                             bool case_sensitive,
                             size_t *out_row, size_t *out_col)
{
    size_t row;
    size_t col;

    if (doc == NULL || needle == NULL || needle_len == 0) {
        return false;
    }

    if (!editor_doc_find_next(doc, needle, needle_len,
                              doc->cursor_row, doc->cursor_col,
                              case_sensitive, true, &row, &col)) {
        return false;
    }

    /* editor_doc_selection_delete() records the undo snapshot itself. */

    /* Select exactly the needle, delete it, then insert the replacement. */
    doc->sel_row = row;
    doc->sel_col = col;
    doc->cursor_row = row;
    doc->cursor_col = col + needle_len;
    doc->selection_active = true;
    editor_doc_selection_delete(doc);
    if (repl_len > 0 && replacement != NULL) {
        if (editor_line_insert_at(&doc->lines[doc->cursor_row],
                                  doc->cursor_col, replacement, repl_len)) {
            doc->cursor_col += repl_len;
        }
        /* On allocation failure the replacement was not inserted, so leave
         * the cursor where the (deleted) match was. */
    }

    if (out_row) *out_row = row;
    if (out_col) *out_col = col;
    return true;
}

/* Replace-all iteration cap: a pathological needle (e.g. empty) can never
 * loop forever; real documents finish orders of magnitude below this. */
#define EDITOR_REPLACE_ALL_MAX 10000

size_t editor_doc_replace_all(editor_doc_t *doc,
                              const char *needle, size_t needle_len,
                              const char *replacement, size_t repl_len,
                              bool case_sensitive)
{
    size_t count = 0;
    size_t row = 0;
    size_t col = 0;

    if (doc == NULL || needle == NULL || needle_len == 0) {
        return 0;
    }
    if (replacement == NULL) {
        repl_len = 0;
    }

    /* Pre-scan before touching the undo ring: no match means no snapshot,
     * so a fruitless replace-all leaves undo/redo exactly as found. */
    if (!editor_doc_find_next(doc, needle, needle_len, 0, 0,
                              case_sensitive, false, &row, &col)) {
        return 0;
    }

    /* One undo snapshot for the whole operation (not one per hit). */
    editor_doc_undo_mark(doc);

    /* Matches never span lines (find_next scans per line), so each hit is
     * a direct single-line splice with no selection machinery. */
    row = 0;
    col = 0;
    while (count < EDITOR_REPLACE_ALL_MAX &&
           editor_doc_find_next(doc, needle, needle_len, row, col,
                                case_sensitive, false, &row, &col)) {
        editor_line_delete_at(&doc->lines[row], col, needle_len);
        if (repl_len > 0 &&
            !editor_line_insert_at(&doc->lines[row], col, replacement, repl_len)) {
            break; /* OOM mid-operation: keep what replaced so far. */
        }
        count++;
        /* Continue AFTER the inserted replacement so a needle inside the
         * replacement text can never re-match (no infinite loop). */
        col += repl_len;
        /* If the replacement ran past EOL (cannot happen: same-line splice
         * keeps col + repl_len <= length), the next find clamps anyway. */
    }
    if (count > 0) {
        doc->cursor_row = row;
        doc->cursor_col = col;
        doc->selection_active = false;
        editor_doc_mark_modified(doc);
    }
    return count;
}

/* ========================================================================
 * COMMENT TOGGLE
 * ========================================================================
 * Prefix comments per syntax (batch "rem ", json "// ", markdown
 * "<!--...-->"). Operates on the selection rows, or the cursor row when
 * nothing is selected. Blank lines are skipped. All-commented ranges
 * uncomment; otherwise every non-blank line is commented. One undo
 * snapshot; returns lines changed (0 for unsupported syntax or no-op).
 */

size_t editor_doc_comment_toggle(editor_doc_t *doc)
{
    const char *prefix = NULL;
    const char *suffix = NULL;
    const char *alt = NULL; /* batch also accepts "::" when uncommenting */
    size_t first_row;
    size_t last_row;
    size_t row;
    bool all_commented = true;
    size_t changed = 0;

    if (doc == NULL || doc->line_count == 0) {
        return 0;
    }
    switch (doc->syntax) {
    case EDITOR_SYNTAX_BATCH:
        prefix = "rem ";
        alt = "::";
        break;
    case EDITOR_SYNTAX_JSON:
        prefix = "// ";
        break;
    case EDITOR_SYNTAX_MARKDOWN:
    case EDITOR_SYNTAX_HTML:
        prefix = "<!-- ";
        suffix = " -->";
        break;
    default:
        return 0;
    }

    if (doc->selection_active) {
        size_t s_row, s_col, e_row, e_col;
        editor_doc_selection_bounds(doc, &s_row, &s_col, &e_row, &e_col);
        first_row = s_row;
        last_row = e_row;
        /* A selection ending at column 0 excludes its last row (the caret
         * sits at the start of a line the user did not drag into). */
        if (e_col == 0 && last_row > first_row) {
            last_row--;
        }
    } else {
        first_row = doc->cursor_row;
        last_row = doc->cursor_row;
    }
    if (first_row >= doc->line_count) {
        return 0;
    }
    if (last_row >= doc->line_count) {
        last_row = doc->line_count - 1;
    }

    /* Pass 1: is every non-blank line already commented? Batch markers
     * match case-insensitively (REM/Rem/rem). */
    for (row = first_row; row <= last_row; row++) {
        const editor_line_t *line = &doc->lines[row];
        size_t col = 0;
        size_t plen = strlen(prefix);
        bool is_batch = (doc->syntax == EDITOR_SYNTAX_BATCH);
        while (col < line->length &&
               (line->text[col] == ' ' || line->text[col] == '\t')) {
            col++;
        }
        if (col >= line->length) {
            continue; /* blank */
        }
        if (line->length - col >= plen &&
            (is_batch ? (strncasecmp(line->text + col, prefix, plen) == 0)
                      : (strncmp(line->text + col, prefix, plen) == 0))) {
            if (suffix == NULL) {
                continue;
            }
        } else if (alt != NULL) {
            size_t alen = strlen(alt);
            if (line->length - col >= alen &&
                strncmp(line->text + col, alt, alen) == 0) {
                continue;
            }
        }
        if (suffix != NULL) {
            size_t slen = strlen(suffix);
            size_t end = line->length;
            while (end > col && (line->text[end - 1] == ' ' ||
                                 line->text[end - 1] == '\t')) {
                end--;
            }
            if (end - col >= plen + slen &&
                strncmp(line->text + col, prefix, plen) == 0 &&
                strncmp(line->text + end - slen, suffix, slen) == 0) {
                continue;
            }
        }
        all_commented = false;
        break;
    }

    editor_doc_undo_mark(doc);

    /* Pass 2: strip or add. Row-local edits only, so top-down is safe. */
    for (row = first_row; row <= last_row; row++) {
        editor_line_t *line = &doc->lines[row];
        size_t col = 0;
        size_t plen = strlen(prefix);
        bool is_batch = (doc->syntax == EDITOR_SYNTAX_BATCH);
        bool has_prefix;
        while (col < line->length &&
               (line->text[col] == ' ' || line->text[col] == '\t')) {
            col++;
        }
        if (col >= line->length) {
            continue; /* blank */
        }
        has_prefix = line->length - col >= plen &&
            (is_batch ? (strncasecmp(line->text + col, prefix, plen) == 0)
                      : (strncmp(line->text + col, prefix, plen) == 0));
        if (all_commented) {
            if (has_prefix) {
                editor_line_delete_at(line, col, plen);
                if (suffix != NULL) {
                    size_t slen = strlen(suffix);
                    size_t end = line->length;
                    while (end > col && (line->text[end - 1] == ' ' ||
                                         line->text[end - 1] == '\t')) {
                        end--;
                    }
                    if (end >= col + slen &&
                        strncmp(line->text + end - slen, suffix, slen) == 0) {
                        editor_line_delete_at(line, end - slen, slen);
                    }
                }
                changed++;
            } else if (alt != NULL) {
                size_t alen = strlen(alt);
                if (line->length - col >= alen &&
                    strncmp(line->text + col, alt, alen) == 0) {
                    editor_line_delete_at(line, col, alen);
                    changed++;
                }
            }
        } else {
            /* Comment branch: skip lines already carrying the marker (or,
             * for markdown, the full wrap) so toggling never double-marks. */
            bool already = has_prefix;
            if (already && suffix != NULL) {
                size_t slen = strlen(suffix);
                size_t end = line->length;
                while (end > col && (line->text[end - 1] == ' ' ||
                                     line->text[end - 1] == '\t')) {
                    end--;
                }
                already = end >= col + slen &&
                    strncmp(line->text + end - slen, suffix, slen) == 0;
            }
            if (!already) {
                if (!editor_line_insert_at(line, col, prefix, plen)) {
                    break; /* OOM: keep what toggled so far. */
                }
                if (suffix != NULL) {
                    size_t slen = strlen(suffix);
                    if (!editor_line_insert_at(line, line->length, suffix, slen)) {
                        break;
                    }
                }
                changed++;
            }
        }
    }
    if (changed > 0) {
        /* Selection stays active so a repeated toggle hits the same range. */
        editor_doc_mark_modified(doc);
    }
    return changed;
}

/* ========================================================================
 * MATCH JUMP (parens + %var%)
 * ========================================================================
 * From a paren or % sign, jump to its match. Parens nest across the whole
 * document (double-quoted spans and whole-line rem/:: comments don't count);
 * %var% pairs match within one line. Inspects the char under the cursor,
 * else the char just before it. No-op returning false when nothing matches.
 */

static bool editor_doc_line_is_batch_comment(const editor_line_t *line)
{
    size_t i = 0;
    if (line == NULL || line->text == NULL) {
        return false;
    }
    while (i < line->length &&
           (line->text[i] == ' ' || line->text[i] == '\t')) {
        i++;
    }
    if (i + 3 <= line->length &&
        (strncasecmp(line->text + i, "rem", 3) == 0)) {
        return true;
    }
    return i + 2 <= line->length && line->text[i] == ':' && line->text[i + 1] == ':';
}

bool editor_doc_match_jump(editor_doc_t *doc)
{
    size_t row;
    size_t col;
    char ch = '\0';

    if (doc == NULL || doc->line_count == 0) {
        return false;
    }
    row = doc->cursor_row;
    if (row >= doc->line_count) {
        return false;
    }
    col = doc->cursor_col;
    {
        const editor_line_t *line = &doc->lines[row];
        if (col < line->length) {
            ch = line->text[col];
        } else if (col > 0) {
            ch = line->text[col - 1];
            /* Cursor just past an opener/closer still counts: step back
             * onto it so scans start on the right side. */
            if (ch == '(' || ch == ')' || ch == '%') {
                col--;
            } else {
                ch = '\0';
            }
        }
    }

    if (ch == '(' || ch == ')') {
        int depth = 0;
        bool fwd = (ch == '(');
        size_t r = row;
        size_t c = fwd ? col + 1 : col;
        /* Backward scans start before the closer. */
        if (!fwd && c > 0) {
            c--;
        } else if (!fwd) {
            if (r == 0) {
                return false;
            }
            r--;
            c = doc->lines[r].length;
            if (c > 0) {
                c--;
            }
        }
        while (true) {
            const editor_line_t *line = &doc->lines[r];
            bool in_str = false;
            size_t start;
            size_t end;
            size_t k;
            if (editor_doc_line_is_batch_comment(line)) {
                goto next_line;
            }
            /* Rescan the quote state from line start (batch has no escapes;
             * a " toggles string mode for the rest of the line). */
            if (fwd) {
                start = (r == row) ? c : 0;
                /* Recompute in_str at start by scanning the prefix. */
                in_str = false;
                for (k = 0; k < start && k < line->length; k++) {
                    if (line->text[k] == '"') {
                        in_str = !in_str;
                    }
                }
                for (k = start; k < line->length; k++) {
                    if (line->text[k] == '"') {
                        in_str = !in_str;
                        continue;
                    }
                    if (in_str) {
                        continue;
                    }
                    if (line->text[k] == '(') {
                        depth++;
                    } else if (line->text[k] == ')') {
                        if (depth == 0) {
                            doc->cursor_row = r;
                            doc->cursor_col = k;
                            doc->selection_active = false;
                            return true;
                        }
                        depth--;
                    }
                }
            } else {
                end = (r == row) ? c + 1 : line->length;
                in_str = false;
                /* Suffix quote parity: count quotes in [end, length); an odd
                 * count means position `end` sits inside a string. */
                {
                    size_t q = end;
                    size_t quotes = 0;
                    while (q < line->length) {
                        if (line->text[q] == '"') {
                            quotes++;
                        }
                        q++;
                    }
                    in_str = (quotes % 2) == 1;
                }
                k = end;
                while (k > 0) {
                    k--;
                    if (line->text[k] == '"') {
                        in_str = !in_str;
                        continue;
                    }
                    if (in_str) {
                        continue;
                    }
                    if (line->text[k] == ')') {
                        depth++;
                    } else if (line->text[k] == '(') {
                        if (depth == 0) {
                            doc->cursor_row = r;
                            doc->cursor_col = k;
                            doc->selection_active = false;
                            return true;
                        }
                        depth--;
                    }
                }
            }
        next_line:
            if (fwd) {
                if (r + 1 >= doc->line_count) {
                    return false;
                }
                r++;
            } else {
                if (r == 0) {
                    return false;
                }
                r--;
            }
        }
    }

    if (ch == '%') {
        const editor_line_t *line = &doc->lines[row];
        size_t k = col + 1;
        while (k < line->length) {
            if (line->text[k] == '%') {
                doc->cursor_row = row;
                doc->cursor_col = k;
                doc->selection_active = false;
                return true;
            }
            k++;
        }
        /* No closer ahead: fall back to the opener behind. */
        if (col > 0) {
            k = col;
            while (k > 0) {
                k--;
                if (line->text[k] == '%') {
                    doc->cursor_row = row;
                    doc->cursor_col = k;
                    doc->selection_active = false;
                    return true;
                }
            }
        }
    }
    return false;
}

/* ========================================================================
 * UNDO / REDO
 * ========================================================================
 * Undo stores full-document snapshots taken before each mutating edit,
 * capped at P4_CONFIG_EDITOR_UNDO_DEPTH. A snapshot is the serialized lines
 * (joined with '\n') plus the cursor and selection. Files are capped at
 * P4_CONFIG_EDITOR_MAX_BYTES, so each snapshot is at most that large; the
 * fixed depth bounds memory. Snapshots are kept on the document itself, so
 * each editor_doc_t has independent undo state. (The snapshot and state
 * types are declared at the top of this file so editor_doc_free() can
 * release their heap buffers.)
 */

/** Release all undo/redo snapshots and reset both rings (reload, free). */
void editor_doc_undo_reset(editor_doc_t *doc)
{
    editor_undo_state_t *state;
    size_t i;
    size_t idx;

    if (doc == NULL) {
        return;
    }
    /* Free the heap-backed undo/redo snapshots: each snapshot's data is a
     * serialized copy of the whole document. */
    state = (editor_undo_state_t *)&doc->undo_storage;
    for (i = 0; i < state->undo_count; i++) {
        idx = (state->undo_head + i) % P4_CONFIG_EDITOR_UNDO_DEPTH;
        editor_mem_free(state->undo[idx].data);
        state->undo[idx].data = NULL;
    }
    for (i = 0; i < state->redo_count; i++) {
        idx = (state->redo_head + i) % P4_CONFIG_EDITOR_UNDO_DEPTH;
        editor_mem_free(state->redo[idx].data);
        state->redo[idx].data = NULL;
    }
    state->undo_count = 0;
    state->undo_head = 0;
    state->redo_count = 0;
    state->redo_head = 0;
    state->bytes = 0;
}

/** Serialize the whole document into one heap string joined with '\n'.
 *  When @p len_out is non-NULL it receives the serialized byte length. */
static char *editor_doc_serialize(const editor_doc_t *doc, size_t *len_out)
{
    size_t total = 0;
    size_t i;
    size_t pos = 0;
    char *out;

    if (len_out != NULL) {
        *len_out = 0;
    }
    if (doc == NULL) {
        return NULL;
    }
    for (i = 0; i < doc->line_count; i++) {
        total += doc->lines[i].length;
        if (i + 1 < doc->line_count) {
            total++; /* '\n' separator */
        }
    }
    out = editor_mem_alloc(total + 1);
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < doc->line_count; i++) {
        if (doc->lines[i].length > 0) {
            memcpy(out + pos, doc->lines[i].text, doc->lines[i].length);
            pos += doc->lines[i].length;
        }
        if (i + 1 < doc->line_count) {
            out[pos++] = '\n';
        }
    }
    out[pos] = '\0';
    if (len_out != NULL) {
        *len_out = pos;
    }
    return out;
}

/** Rebuild a document from a serialized snapshot (replaces all lines).
 *  Builds the new line set into temporary storage first and only swaps it in
 *  on success, so an allocation failure during undo/redo can never leave the
 *  document half-freed. */
static bool editor_doc_deserialize(editor_doc_t *doc, const char *data)
{
    size_t count = 0;
    size_t i;
    const char *p;
    bool ok = true;
    editor_line_t *new_lines = NULL;
    size_t new_count = 0;

    if (doc == NULL || data == NULL) {
        return false;
    }

    for (p = data; *p != '\0'; p++) {
        if (*p == '\n') {
            count++;
        }
    }
    count++; /* trailing line after the last '\n' */

    new_lines = editor_mem_alloc(count * sizeof(editor_line_t));
    if (new_lines == NULL) {
        return false;
    }
    for (i = 0; i < count; i++) {
        editor_line_init(&new_lines[i]);
    }

    /* Split the serialized text into lines. */
    p = data;
    for (i = 0; i < count; i++) {
        const char *nl = strchr(p, '\n');
        size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);

        if (len > 0) {
            if (!editor_line_set(&new_lines[i], p, len)) {
                ok = false;
                break;
            }
        }
        new_count++;
        p = nl != NULL ? nl + 1 : p + len;
    }

    /* Remove the phantom empty tail line when the snapshot ended with '\n'. */
    if (ok && count > 1 && data[strlen(data) - 1] == '\n' &&
        new_count > 0 && new_lines[new_count - 1].length == 0) {
        editor_line_free(&new_lines[new_count - 1]);
        new_count--;
    }

    if (!ok) {
        for (i = 0; i < new_count; i++) {
            editor_line_free(&new_lines[i]);
        }
        editor_mem_free(new_lines);
        return false;
    }

    /* Swap the fresh line set into the document. */
    for (i = 0; i < doc->line_count; i++) {
        editor_line_free(&doc->lines[i]);
    }
    editor_mem_free(doc->lines);
    doc->lines = new_lines;
    doc->line_count = new_count;
    doc->line_capacity = count;
    return true;
}

static void editor_undo_push(editor_doc_t *doc)
{
    editor_undo_state_t *state;
    editor_undo_snapshot_t *slot;
    char *snap;
    size_t snap_len = 0;

    if (doc == NULL) {
        return;
    }
    state = (editor_undo_state_t *)&doc->undo_storage;

    /* Above the snapshot threshold a full-document copy per edit is too
     * expensive, so undo is disabled for the session and any ring is dropped. */
    if (doc->content_bytes > P4_CONFIG_EDITOR_UNDO_MAX_SNAPSHOT_BYTES) {
        if (state->undo_count > 0 || state->redo_count > 0) {
            editor_doc_undo_reset(doc);
        }
        return;
    }

    snap = editor_doc_serialize(doc, &snap_len);
    if (snap == NULL) {
        return;
    }
    doc->content_bytes = snap_len;

    if (state->undo_count < P4_CONFIG_EDITOR_UNDO_DEPTH) {
        slot = &state->undo[state->undo_count];
        state->undo_count++;
    } else {
        /* Ring full: overwrite the oldest. */
        slot = &state->undo[state->undo_head];
        state->bytes -= slot->len;
        editor_mem_free(slot->data);
        state->undo_head = (state->undo_head + 1) % P4_CONFIG_EDITOR_UNDO_DEPTH;
    }

    slot->data = snap;
    slot->len = snap_len;
    slot->cursor_row = doc->cursor_row;
    slot->cursor_col = doc->cursor_col;
    slot->sel_row = doc->sel_row;
    slot->sel_col = doc->sel_col;
    slot->selection_active = doc->selection_active;
    slot->modified = doc->modified;
    state->bytes += snap_len;

    /* Byte budget: a full-document snapshot per step would be
     * depth x file-size; evict the oldest steps until the ring fits the cap,
     * always keeping the newest so even a single huge document can undo once. */
    while (state->bytes > P4_CONFIG_EDITOR_UNDO_MAX_BYTES && state->undo_count > 1) {
        slot = &state->undo[state->undo_head];
        state->bytes -= slot->len;
        editor_mem_free(slot->data);
        slot->data = NULL;
        slot->len = 0;
        state->undo_count--;
        state->undo_head = (state->undo_head + 1) % P4_CONFIG_EDITOR_UNDO_DEPTH;
    }
}

static void editor_redo_clear(editor_doc_t *doc)
{
    editor_undo_state_t *state = (editor_undo_state_t *)&doc->undo_storage;
    size_t i;
    for (i = 0; i < state->redo_count; i++) {
        state->bytes -= state->redo[i].len;
        editor_mem_free(state->redo[i].data);
        state->redo[i].data = NULL;
    }
    state->redo_count = 0;
    state->redo_head = 0;
}

void editor_doc_undo_mark(editor_doc_t *doc)
{
    if (doc == NULL) {
        return;
    }
    editor_undo_push(doc);
    editor_redo_clear(doc);
}

void editor_doc_undo(editor_doc_t *doc)
{
    editor_undo_state_t *state;
    editor_undo_snapshot_t *slot;
    char *snap;
    size_t snap_len = 0;
    size_t idx;

    if (doc == NULL) {
        return;
    }
    state = (editor_undo_state_t *)&doc->undo_storage;
    if (state->undo_count == 0) {
        return;
    }

    idx = (state->undo_head + state->undo_count - 1) % P4_CONFIG_EDITOR_UNDO_DEPTH;
    slot = &state->undo[idx];

    /* Push the current state onto the redo ring before restoring. */
    if (state->redo_count < P4_CONFIG_EDITOR_UNDO_DEPTH) {
        snap = editor_doc_serialize(doc, &snap_len);
        if (snap != NULL) {
            size_t r_idx = (state->redo_head + state->redo_count) % P4_CONFIG_EDITOR_UNDO_DEPTH;
            state->redo[r_idx].data = snap;
            state->redo[r_idx].len = snap_len;
            state->redo[r_idx].cursor_row = doc->cursor_row;
            state->redo[r_idx].cursor_col = doc->cursor_col;
            state->redo[r_idx].sel_row = doc->sel_row;
            state->redo[r_idx].sel_col = doc->sel_col;
            state->redo[r_idx].selection_active = doc->selection_active;
            state->redo[r_idx].modified = doc->modified;
            state->redo_count++;
            state->bytes += snap_len;
        }
    }

    if (!editor_doc_deserialize(doc, slot->data)) {
        /* OOM: the document is unchanged, so do not pop the history entry. */
        return;
    }
    doc->cursor_row = slot->cursor_row;
    doc->cursor_col = slot->cursor_col;
    doc->sel_row = slot->sel_row;
    doc->sel_col = slot->sel_col;
    doc->selection_active = slot->selection_active;
    /* Restores the dirty flag from snapshot time: undoing back to a saved
     * state clears the mark instead of forcing it dirty. */
    doc->modified = slot->modified;

    /* Pop the undo entry. */
    state->bytes -= slot->len;
    editor_mem_free(slot->data);
    slot->data = NULL;
    slot->len = 0;
    state->undo_count--;
}

void editor_doc_redo(editor_doc_t *doc)
{
    editor_undo_state_t *state;
    editor_undo_snapshot_t *slot;
    char *snap;
    size_t snap_len = 0;
    size_t idx;

    if (doc == NULL) {
        return;
    }
    state = (editor_undo_state_t *)&doc->undo_storage;
    if (state->redo_count == 0) {
        return;
    }

    idx = (state->redo_head + state->redo_count - 1) % P4_CONFIG_EDITOR_UNDO_DEPTH;
    slot = &state->redo[idx];

    /* Push the current state onto the undo ring. */
    snap = editor_doc_serialize(doc, &snap_len);
    if (snap != NULL) {
        size_t u_idx = (state->undo_head + state->undo_count) % P4_CONFIG_EDITOR_UNDO_DEPTH;
        if (state->undo_count < P4_CONFIG_EDITOR_UNDO_DEPTH) {
            state->undo[u_idx].data = snap;
            state->undo[u_idx].len = snap_len;
            state->undo[u_idx].cursor_row = doc->cursor_row;
            state->undo[u_idx].cursor_col = doc->cursor_col;
            state->undo[u_idx].sel_row = doc->sel_row;
            state->undo[u_idx].sel_col = doc->sel_col;
            state->undo[u_idx].selection_active = doc->selection_active;
            state->undo[u_idx].modified = doc->modified;
            state->undo_count++;
            state->bytes += snap_len;
        } else {
            /* Ring full: replace the oldest without growing. */
            size_t old = state->undo_head;
            state->bytes -= state->undo[old].len;
            editor_mem_free(state->undo[old].data);
            state->undo[old].data = snap;
            state->undo[old].len = snap_len;
            state->undo[old].cursor_row = doc->cursor_row;
            state->undo[old].cursor_col = doc->cursor_col;
            state->undo[old].sel_row = doc->sel_row;
            state->undo[old].sel_col = doc->sel_col;
            state->undo[old].selection_active = doc->selection_active;
            state->undo[old].modified = doc->modified;
            state->undo_head = (state->undo_head + 1) % P4_CONFIG_EDITOR_UNDO_DEPTH;
            state->bytes += snap_len;
        }
    }

    if (!editor_doc_deserialize(doc, slot->data)) {
        return;   /* OOM: leave the document and history untouched */
    }
    doc->cursor_row = slot->cursor_row;
    doc->cursor_col = slot->cursor_col;
    doc->sel_row = slot->sel_row;
    doc->sel_col = slot->sel_col;
    doc->selection_active = slot->selection_active;
    doc->modified = slot->modified;

    state->bytes -= slot->len;
    editor_mem_free(slot->data);
    slot->data = NULL;
    slot->len = 0;
    state->redo_count--;
}

/* ========================================================================
 * BATCH SYNTAX LEXER
 * ======================================================================== */

static bool editor_is_batch_command_word(const char *word, size_t len)
{
    /* DOS batch commands. Keep this list intentionally short and DOS-flavoured;
     * the shell command table is the source of truth for interactive use. */
    static const char *const cmds[] = {
        "call", "cd", "chcp", "chdir", "choice", "cls", "cmd", "copy", "date",
        "del", "dir", "echo", "endlocal", "erase", "errorlevel", "exit", "for",
        "goto", "if", "md", "mkdir", "move", "path", "pause", "popd", "prompt",
        "pushd", "rd", "rem", "ren", "rename", "rmdir", "set", "setlocal",
        "shift", "start", "time", "title", "type", "ver", "vol",
    };
    size_t i;

    for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (strlen(cmds[i]) == len && strncasecmp(cmds[i], word, len) == 0) {
            return true;
        }
    }
    return false;
}

size_t editor_lex_batch(const char *text, size_t len,
                        editor_syntax_run_t *runs, size_t capacity)
{
    size_t run_count = 0;
    size_t i = 0;

    if (text == NULL || runs == NULL || capacity == 0) {
        return 0;
    }

    /* Skip leading whitespace. */
    while (i < len && (text[i] == ' ' || text[i] == '\t')) {
        i++;
    }

    /* Whole-line comment: "rem ..." or ":: ...". The run covers the whole
     * line from column 0 so the renderer never sees a gap at the start. */
    if (i + 3 <= len) {
        if ((strncasecmp(text + i, "rem", 3) == 0) ||
            (i + 2 <= len && text[i] == ':' && text[i + 1] == ':')) {
            if (run_count < capacity) {
                runs[run_count].start = 0;
                runs[run_count].length = len;
                runs[run_count].color = ANSI_COLOR_BRIGHT_BLACK;
                run_count++;
            }
            return run_count;
        }
    }

    /* A label is a line starting with ':' (but not "::" which is a comment). */
    if (i < len && text[i] == ':' && text[i + 1] != ':') {
        if (run_count < capacity) {
            runs[run_count].start = 0;
            runs[run_count].length = len;
            runs[run_count].color = ANSI_COLOR_BRIGHT_YELLOW;
            run_count++;
        }
        return run_count;
    }

    while (i < len) {
        size_t start = i;
        char ch = text[i];

        /* Quoted string. */
        if (ch == '"') {
            size_t j = i + 1;
            while (j < len && text[j] != '"') {
                j++;
            }
            if (j < len) {
                j++; /* include closing quote */
            }
            if (run_count < capacity) {
                runs[run_count].start = i;
                runs[run_count].length = j - i;
                runs[run_count].color = ANSI_COLOR_BRIGHT_YELLOW;
                run_count++;
            }
            i = j;
            continue;
        }

        /* %VAR% expansion. */
        if (ch == '%') {
            size_t j = i + 1;
            while (j < len && text[j] != '%' && j < i + 64) {
                j++;
            }
            if (j < len && text[j] == '%') {
                j++;
            }
            if (run_count < capacity) {
                runs[run_count].start = i;
                runs[run_count].length = j - i;
                runs[run_count].color = ANSI_COLOR_BRIGHT_CYAN;
                run_count++;
            }
            i = j;
            continue;
        }

        /* Operators. */
        if (ch == '&' || ch == '|' || ch == '<' || ch == '>' || ch == '^') {
            if (run_count < capacity) {
                runs[run_count].start = i;
                runs[run_count].length = 1;
                runs[run_count].color = ANSI_COLOR_BRIGHT_CYAN;
                run_count++;
            }
            i++;
            continue;
        }

        /* A word: highlight batch commands. */
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
            size_t j = i;
            while (j < len && ((text[j] >= 'a' && text[j] <= 'z') ||
                               (text[j] >= 'A' && text[j] <= 'Z') ||
                               (text[j] >= '0' && text[j] <= '9'))) {
                j++;
            }

            if (editor_is_batch_command_word(text + i, j - i)) {
                if (run_count < capacity) {
                    runs[run_count].start = i;
                    runs[run_count].length = j - i;
                    runs[run_count].color = ANSI_COLOR_BRIGHT_GREEN;
                    run_count++;
                }
            } else if (run_count < capacity) {
                /* Plain word: fold into the default run. */
                runs[run_count].start = i;
                runs[run_count].length = j - i;
                runs[run_count].color = ANSI_COLOR_BRIGHT_WHITE;
                run_count++;
            }
            i = j;
            continue;
        }

        /* Everything else: fold whitespace/punctuation into one run. */
        {
            size_t j = i + 1;
            while (j < len && !(text[j] == '"' || text[j] == '%' ||
                                text[j] == '&' || text[j] == '|' ||
                                text[j] == '<' || text[j] == '>' || text[j] == '^' ||
                                ((text[j] >= 'a' && text[j] <= 'z') ||
                                 (text[j] >= 'A' && text[j] <= 'Z')))) {
                j++;
            }
            if (run_count < capacity) {
                runs[run_count].start = start;
                runs[run_count].length = j - start;
                runs[run_count].color = ANSI_COLOR_BRIGHT_WHITE;
                run_count++;
            }
            i = j;
        }
    }

    return run_count;
}

/* ========================================================================
 * LINE-NUMBER GUTTER (pure)
 * ======================================================================== */

size_t editor_format_line_number(size_t line, unsigned width,
                                 char *out, size_t out_size)
{
    char digits[16];
    size_t n;
    size_t pad;
    size_t pos = 0;
    unsigned i;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    n = (size_t)snprintf(digits, sizeof(digits), "%zu", line + 1);
    if (n >= sizeof(digits)) {
        n = sizeof(digits) - 1;
    }
    pad = (n < width) ? (size_t)(width - n) : 0;

    /* pad + digits + trailing space + NUL must fit. */
    if (pad + n + 2 > out_size) {
        return 0;
    }

    for (i = 0; i < pad; i++) {
        out[pos++] = ' ';
    }
    memcpy(out + pos, digits, n);
    pos += n;
    out[pos++] = ' ';
    out[pos] = '\0';
    return pos;
}
/* ========================================================================
 * KEY ROUTING
 * ======================================================================== */

editor_key_t editor_key_from_usb(uint8_t key_code, uint8_t modifiers, char ascii)
{
    bool ctrl = (modifiers & 0x01) || (modifiers & 0x10); /* L/R Ctrl */
    bool shift = (modifiers & 0x02) || (modifiers & 0x20);

    switch (key_code) {
    case 0x28: return EDITOR_KEY_NEWLINE;          /* Enter */
    case 0x2A: return EDITOR_KEY_BACKSPACE;        /* Backspace */
    case 0x2B: return EDITOR_KEY_TAB;              /* Tab */
    case 0x29: return EDITOR_KEY_QUIT;             /* Esc */
    case 0x4C: return EDITOR_KEY_DELETE;           /* Delete */
    case 0x52: return EDITOR_KEY_UP;               /* Up */
    case 0x51: return EDITOR_KEY_DOWN;             /* Down */
    case 0x50: return ctrl ? EDITOR_KEY_WORD_LEFT : EDITOR_KEY_LEFT;   /* Left / Ctrl+Left */
    case 0x4F: return ctrl ? EDITOR_KEY_WORD_RIGHT : EDITOR_KEY_RIGHT; /* Right / Ctrl+Right */
    case 0x4A: return ctrl ? EDITOR_KEY_DOC_HOME : EDITOR_KEY_HOME;    /* Home / Ctrl+Home */
    case 0x4D:                                                          /* End / Ctrl+End / Ctrl+Shift+End */
        if (ctrl && shift) {
            return EDITOR_KEY_DELETE_EOL;
        }
        return ctrl ? EDITOR_KEY_DOC_END : EDITOR_KEY_END;
    case 0x4B: return EDITOR_KEY_PAGE_UP;          /* PageUp */
    case 0x4E: return EDITOR_KEY_PAGE_DOWN;        /* PageDown */
    case 0x49: return EDITOR_KEY_OVERWRITE;        /* Insert */
    case 0x3B: return EDITOR_KEY_SAVE;             /* F2 */
    case 0x3C: return EDITOR_KEY_FIND_NEXT;        /* F3 */
    case 0x3D: return EDITOR_KEY_OPEN;             /* F4 */
    case 0x38: return ctrl ? EDITOR_KEY_COMMENT : EDITOR_KEY_NONE; /* Ctrl+/ */
    default:
        break;
    }

    if (ctrl) {
        /* Redo is Ctrl+Shift+Z (DOS-style Ctrl+Y is delete line). */
        if (shift) {
            switch (ascii) {
            case 'z': case 'Z': return EDITOR_KEY_REDO;
            case 'f': case 'F': return EDITOR_KEY_FOCUS_TOGGLE;
            case 's': case 'S': return EDITOR_KEY_SPELL_TOGGLE;
            default:
                break;
            }
        }
        switch (ascii) {
        case 'a': case 'A': return EDITOR_KEY_SELECT_ALL;
        case 'c': case 'C': return EDITOR_KEY_COPY;
        case 'd': case 'D': return EDITOR_KEY_SPELL_ADD;
        case 'x': case 'X': return EDITOR_KEY_CUT;
        case 'v': case 'V': return EDITOR_KEY_PASTE;
        case 'z': case 'Z': return EDITOR_KEY_UNDO;
        case 'y': case 'Y': return EDITOR_KEY_DELETE_LINE;
        case 's': case 'S': return EDITOR_KEY_SAVE;
        case 'o': case 'O': return EDITOR_KEY_SAVE_AS;
        case 'f': case 'F': return EDITOR_KEY_FIND;
        case 'h': case 'H': return EDITOR_KEY_REPLACE;
        case 'g': case 'G': return EDITOR_KEY_GOTO_LINE;
        case 'p': case 'P': return EDITOR_KEY_PREVIEW;
        case 'r': case 'R':
            if (!shift) {
                return EDITOR_KEY_REPLACE_ALL;
            }
            return EDITOR_KEY_REDO;
        case 't': case 'T': return EDITOR_KEY_CASE_TOGGLE;
        case 'b': case 'B': return EDITOR_KEY_MATCH_JUMP;
        case 'w': case 'W': return EDITOR_KEY_WRAP_TOGGLE;
        case 'l': case 'L': return EDITOR_KEY_RELOAD;
        case 'q': case 'Q': return EDITOR_KEY_QUIT;
        default:
            break;
        }
    }

    (void)shift;
    return EDITOR_KEY_NONE;
}

/* ========================================================================
 * ON-SCREEN KEYBOARD LABELS
 * ========================================================================
 * The editor's Nav/User OSK buttons are named actions rather than characters.
 * This table is the single source for those labels: the view's keyboard
 * handler, `keyboard_test.py`'s page screenshots and the unit tests all read
 * it. LVGL symbols and the mode-switch buttons are handled by the view.
 */

/** One row of the on-screen keyboard nav/edit command tables. */
typedef struct {
    const char *label;
    editor_key_t key;
} editor_osk_entry_t;

static const editor_osk_entry_t k_editor_osk_actions[] = {
    {"Tab", EDITOR_KEY_TAB},
    {"Del", EDITOR_KEY_DELETE},
    {"Ins", EDITOR_KEY_OVERWRITE},
    {"Home", EDITOR_KEY_HOME},
    {"End", EDITOR_KEY_END},
    {"PgUp", EDITOR_KEY_PAGE_UP},
    {"PgDn", EDITOR_KEY_PAGE_DOWN},
    {"Save", EDITOR_KEY_SAVE},
    {"SaveAs", EDITOR_KEY_SAVE_AS},
    {"Open", EDITOR_KEY_OPEN},
    {"Find", EDITOR_KEY_FIND},
    {"Replace", EDITOR_KEY_REPLACE},
    {"Rep", EDITOR_KEY_REPLACE},
    {"ReplAll", EDITOR_KEY_REPLACE_ALL},
    {"All", EDITOR_KEY_REPLACE_ALL},
    {"Case", EDITOR_KEY_CASE_TOGGLE},
    {"Goto", EDITOR_KEY_GOTO_LINE},
    {"Undo", EDITOR_KEY_UNDO},
    {"Redo", EDITOR_KEY_REDO},
    {"Next", EDITOR_KEY_FIND_NEXT},
    {"Preview", EDITOR_KEY_PREVIEW},
    {"Prev", EDITOR_KEY_PREVIEW},
    {"Copy", EDITOR_KEY_COPY},
    {"Cut", EDITOR_KEY_CUT},
    {"Paste", EDITOR_KEY_PASTE},
    {"SelAll", EDITOR_KEY_SELECT_ALL},
    {"WordL", EDITOR_KEY_WORD_LEFT},
    {"WordR", EDITOR_KEY_WORD_RIGHT},
    {"DocTop", EDITOR_KEY_DOC_HOME},
    {"DocBot", EDITOR_KEY_DOC_END},
    {"DelLine", EDITOR_KEY_DELETE_LINE},
    {"DelEOL", EDITOR_KEY_DELETE_EOL},
    {"Quit", EDITOR_KEY_QUIT},
    {"Exit", EDITOR_KEY_QUIT},
    {"Comment", EDITOR_KEY_COMMENT},
    {"Match", EDITOR_KEY_MATCH_JUMP},
    {"Wrap", EDITOR_KEY_WRAP_TOGGLE},
    {"Focus", EDITOR_KEY_FOCUS_TOGGLE},
    {"Spell", EDITOR_KEY_SPELL_TOGGLE},
    {"AddWord", EDITOR_KEY_SPELL_ADD},
    {"Reload", EDITOR_KEY_RELOAD},
};

bool editor_osk_key_from_label(const char *label, editor_key_t *out)
{
    size_t i;

    if (label == NULL || out == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(k_editor_osk_actions) / sizeof(k_editor_osk_actions[0]); i++) {
        if (strcmp(label, k_editor_osk_actions[i].label) == 0) {
            *out = k_editor_osk_actions[i].key;
            return true;
        }
    }
    return false;
}

/* ========================================================================
 * MARKDOWN LEXER (single line, no multi-line state)
 * ======================================================================== */

/* Emit one run if capacity allows (shared by the markdown and HTML lexers). */
static void editor_lex_emit(editor_syntax_run_t *runs, size_t capacity,
                        size_t *count, size_t start, size_t length,
                        ansi_color_index_t color, unsigned attrs)
{
    if (*count >= capacity || length == 0) {
        return;
    }
    runs[*count].start = start;
    runs[*count].length = length;
    runs[*count].color = color;
    runs[*count].attrs = attrs;
    (*count)++;
}

/* Inline emphasis/code/link scan over [start, len). Plain gaps fold into
 * default runs by the caller loop. */
static void md_lex_inline(const char *text, size_t len, size_t start,
                          size_t ilen, editor_syntax_run_t *runs,
                          size_t capacity, size_t *count)
{
    size_t i = start;
    size_t iend = start + ilen;

    while (i < iend) {
        size_t j;
        /* Code span: `...` (no nesting). Unterminated: skip the tick so
         * later constructs on the line still highlight. */
        if (text[i] == '`') {
            j = i + 1;
            while (j < iend && text[j] != '`') {
                j++;
            }
            if (j < iend) {
                editor_lex_emit(runs, capacity, count, i, j - i + 1,
                            ANSI_COLOR_BRIGHT_YELLOW, 0);
                i = j + 1;
                continue;
            }
            i++;
            continue;
        }
        /* Bold: **...** with non-space adjacency. */
        if (i + 1 < iend && text[i] == '*' && text[i + 1] == '*' &&
            i + 2 < iend && text[i + 2] != ' ') {
            j = i + 2;
            while (j + 1 < iend && !(text[j] == '*' && text[j + 1] == '*')) {
                j++;
            }
            if (j + 1 < iend && j > i + 2 && text[j - 1] != ' ') {
                editor_lex_emit(runs, capacity, count, i, j + 2 - i,
                            ANSI_COLOR_BRIGHT_WHITE, ANSI_ATTR_BOLD);
                i = j + 2;
                continue;
            }
            i++;
            continue;
        }
        /* Italic: *...* with non-space adjacency. */
        if (text[i] == '*' && i + 1 < iend && text[i + 1] != ' ') {
            j = i + 1;
            while (j < iend && text[j] != '*') {
                j++;
            }
            if (j < iend && j > i + 1 && text[j - 1] != ' ') {
                editor_lex_emit(runs, capacity, count, i, j - i + 1,
                            ANSI_COLOR_BRIGHT_WHITE, ANSI_ATTR_ITALIC);
                i = j + 1;
                continue;
            }
            i++;
            continue;
        }
        /* Link: [text](url) — text cyan+underline. */
        if (text[i] == '[') {
            size_t t = i + 1;
            size_t u;
            while (t < iend && text[t] != ']') {
                t++;
            }
            if (t < iend && t + 1 < iend && text[t + 1] == '(') {
                u = t + 2;
                while (u < iend && text[u] != ')') {
                    u++;
                }
                if (u < iend) {
                    editor_lex_emit(runs, capacity, count, i, t - i + 1,
                                ANSI_COLOR_BRIGHT_CYAN, ANSI_ATTR_UNDERLINE);
                    i = u + 1;
                    continue;
                }
            }
            /* Not a link: skip the bracket, keep scanning. */
            i++;
            continue;
        }
        /* Plain character: skip it (the renderer folds gaps to default). */
        i++;
    }
    /* Caller folds the unstyled remainder/gaps into default runs. */
    (void)ilen;
}

size_t editor_lex_markdown(const char *text, size_t len,
                           editor_syntax_run_t *runs, size_t capacity)
{
    size_t run_count = 0;
    size_t i = 0;

    if (text == NULL || runs == NULL || capacity == 0) {
        return 0;
    }

    /* Skip leading whitespace (kept in the default run). */
    while (i < len && (text[i] == ' ' || text[i] == '\t')) {
        i++;
    }

    /* ATX heading: marker green, content bold white. */
    if (i < len && text[i] == '#') {
        size_t h = i;
        while (h < len && text[h] == '#') {
            h++;
        }
        if (h - i <= 6 && h < len && (text[h] == ' ' || text[h] == '\t')) {
            editor_lex_emit(runs, capacity, &run_count, i, h - i,
                        ANSI_COLOR_BRIGHT_GREEN, 0);
            editor_lex_emit(runs, capacity, &run_count, h, len - h,
                        ANSI_COLOR_BRIGHT_WHITE, ANSI_ATTR_BOLD);
            return run_count;
        }
    }

    /* Quote marker. */
    if (i < len && text[i] == '>') {
        editor_lex_emit(runs, capacity, &run_count, i, 1,
                    ANSI_COLOR_BRIGHT_BLACK, 0);
        i += 1;
        if (i < len && text[i] == ' ') {
            i++;
        }
        /* Quoted content gets inline spans too. */
        md_lex_inline(text, len, i, len - i, runs, capacity, &run_count);
        return run_count;
    }

    /* List marker: -, +, *, 1., 1) (+ task [ ]/[x] kept as text). */
    {
        size_t m = i;
        bool is_list = false;
        if (m < len && (text[m] == '-' || text[m] == '+' || text[m] == '*')) {
            if (m + 1 < len && (text[m + 1] == ' ' || text[m + 1] == '\t')) {
                is_list = true;
                m += 1;
            }
        } else if (m < len && text[m] >= '0' && text[m] <= '9') {
            while (m < len && text[m] >= '0' && text[m] <= '9') {
                m++;
            }
            if (m < len && (text[m] == '.' || text[m] == ')') &&
                m + 1 < len && (text[m + 1] == ' ' || text[m + 1] == '\t')) {
                is_list = true;
                m += 1;
            }
        }
        if (is_list) {
            editor_lex_emit(runs, capacity, &run_count, i, m - i + 1,
                        ANSI_COLOR_BRIGHT_GREEN, 0);
            i = m + 1;
            while (i < len && (text[i] == ' ' || text[i] == '\t')) {
                i++;
            }
            md_lex_inline(text, len, i, len - i, runs, capacity, &run_count);
            return run_count;
        }
    }

    /* Fence line: whole line code-yellow. */
    if (i + 2 < len && text[i] == '`' && text[i + 1] == '`' && text[i + 2] == '`') {
        editor_lex_emit(runs, capacity, &run_count, 0, len,
                    ANSI_COLOR_BRIGHT_YELLOW, 0);
        return run_count;
    }

    /* HR line: muted whole line. */
    {
        size_t h = i;
        char mark = '\0';
        int cnt = 0;
        bool ok = true;
        while (h < len) {
            if (text[h] == '-' || text[h] == '*' || text[h] == '_') {
                if (mark == '\0') {
                    mark = text[h];
                } else if (text[h] != mark) {
                    ok = false;
                    break;
                }
                cnt++;
            } else if (text[h] != ' ' && text[h] != '\t') {
                ok = false;
                break;
            }
            h++;
        }
        if (ok && cnt >= 3) {
            editor_lex_emit(runs, capacity, &run_count, 0, len,
                        ANSI_COLOR_BRIGHT_BLACK, 0);
            return run_count;
        }
    }

    /* Body: inline constructs; plain gaps fold to default in the renderer. */
    md_lex_inline(text, len, i, len - i, runs, capacity, &run_count);
    return run_count;
}

/* ========================================================================
 * JSON LEXER (single line, no multi-line state)
 * ========================================================================
 * Keys (string followed by ':') cyan, string values green, numbers yellow,
 * true/false/null magenta, structural punctuation white. Unterminated
 * strings highlight to EOL (error-visible) rather than vanishing. Escapes
 * inside strings are skipped so \" never ends the run early.
 */

size_t editor_lex_json(const char *text, size_t len,
                       editor_syntax_run_t *runs, size_t capacity)
{
    size_t run_count = 0;
    size_t i = 0;

    if (text == NULL || runs == NULL || capacity == 0) {
        return 0;
    }

    while (i < len) {
        /* String: "..." with backslash escapes. */
        if (text[i] == '"') {
            size_t j = i + 1;
            while (j < len && text[j] != '"') {
                if (text[j] == '\\' && j + 1 < len) {
                    j += 2;
                } else {
                    j++;
                }
            }
            if (j < len) {
                j++; /* include closing quote */
            }
            /* Key when followed (past spaces) by ':'. */
            {
                size_t k = j;
                ansi_color_index_t color = ANSI_COLOR_BRIGHT_GREEN;
                while (k < len && (text[k] == ' ' || text[k] == '\t')) {
                    k++;
                }
                if (k < len && text[k] == ':') {
                    color = ANSI_COLOR_BRIGHT_CYAN;
                }
                editor_lex_emit(runs, capacity, &run_count, i, j - i, color, 0);
            }
            i = j;
            continue;
        }
        /* Number: -?digits[.digits][eE+-digits]. */
        if ((text[i] >= '0' && text[i] <= '9') || text[i] == '-') {
            size_t j = i;
            if (text[j] == '-') {
                j++;
            }
            while (j < len && text[j] >= '0' && text[j] <= '9') {
                j++;
            }
            if (j < len && text[j] == '.') {
                size_t k = j + 1;
                while (k < len && text[k] >= '0' && text[k] <= '9') {
                    k++;
                }
                if (k > j + 1) {
                    j = k;
                }
            }
            if (j < len && (text[j] == 'e' || text[j] == 'E')) {
                size_t k = j + 1;
                if (k < len && (text[k] == '+' || text[k] == '-')) {
                    k++;
                }
                {
                    size_t d = k;
                    while (d < len && text[d] >= '0' && text[d] <= '9') {
                        d++;
                    }
                    if (d > k) {
                        j = d;
                    }
                }
            }
            if (j > i + (text[i] == '-' ? 1 : 0)) {
                editor_lex_emit(runs, capacity, &run_count, i, j - i,
                            ANSI_COLOR_BRIGHT_YELLOW, 0);
                i = j;
                continue;
            }
            /* Lone '-': fall through to plain skip. */
            i++;
            continue;
        }
        /* Literals true/false/null. */
        if (i + 4 <= len && strncmp(text + i, "true", 4) == 0) {
            editor_lex_emit(runs, capacity, &run_count, i, 4,
                        ANSI_COLOR_BRIGHT_MAGENTA, 0);
            i += 4;
            continue;
        }
        if (i + 5 <= len && strncmp(text + i, "false", 5) == 0) {
            editor_lex_emit(runs, capacity, &run_count, i, 5,
                        ANSI_COLOR_BRIGHT_MAGENTA, 0);
            i += 5;
            continue;
        }
        if (i + 4 <= len && strncmp(text + i, "null", 4) == 0) {
            editor_lex_emit(runs, capacity, &run_count, i, 4,
                        ANSI_COLOR_BRIGHT_MAGENTA, 0);
            i += 4;
            continue;
        }
        /* Structural punctuation. */
        if (text[i] == '{' || text[i] == '}' || text[i] == '[' ||
            text[i] == ']' || text[i] == ':' || text[i] == ',') {
            editor_lex_emit(runs, capacity, &run_count, i, 1,
                        ANSI_COLOR_BRIGHT_WHITE, 0);
            i++;
            continue;
        }
        /* Plain character: renderer folds gaps to default. */
        i++;
    }
    return run_count;
}

/* ========================================================================
 * HTML LEXER (single line, no multi-line state)
 * ======================================================================== */

/** True for an HTML tag/attribute name character. */
static bool editor_html_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == ':' || c == '_';
}

size_t editor_lex_html(const char *text, size_t len,
                       editor_syntax_run_t *runs, size_t capacity)
{
    size_t run_count = 0;
    size_t i = 0;

    if (text == NULL || runs == NULL || capacity == 0) {
        return 0;
    }

    while (i < len) {
        if (text[i] != '<') {
            i++; /* Plain: renderer folds gaps to the default run. */
            continue;
        }
        /* Comment: <!-- ... --> (to EOL when unterminated on this line). */
        if (i + 3 < len && strncmp(text + i, "<!--", 4) == 0) {
            size_t j = i + 4;
            while (j + 2 < len && !(text[j] == '-' && text[j + 1] == '-')) {
                j++;
            }
            if (j + 3 <= len && text[j] == '-' && text[j + 1] == '-') {
                j += 3;
            } else {
                j = len;
            }
            editor_lex_emit(runs, capacity, &run_count, i, j - i,
                            ANSI_COLOR_BRIGHT_GREEN, 0);
            i = j;
            continue;
        }
        /* Doctype / processing instruction / declaration. */
        if (i + 1 < len && (text[i + 1] == '!' || text[i + 1] == '?')) {
            size_t j = i + 1;
            while (j < len && text[j] != '>') {
                j++;
            }
            if (j < len) {
                j++;
            }
            editor_lex_emit(runs, capacity, &run_count, i, j - i,
                            ANSI_COLOR_BRIGHT_MAGENTA, 0);
            i = j;
            continue;
        }
        /* Tag: '<' '/' name (attrs) '>'. */
        {
            size_t j = i;
            editor_lex_emit(runs, capacity, &run_count, j, 1,
                            ANSI_COLOR_BRIGHT_CYAN, 0); /* '<' */
            j++;
            if (j < len && text[j] == '/') {
                editor_lex_emit(runs, capacity, &run_count, j, 1,
                                ANSI_COLOR_BRIGHT_CYAN, 0);
                j++;
            }
            {
                size_t ns = j;
                while (j < len && editor_html_name_char(text[j])) {
                    j++;
                }
                if (j > ns) {
                    editor_lex_emit(runs, capacity, &run_count, ns, j - ns,
                                    ANSI_COLOR_BRIGHT_YELLOW, ANSI_ATTR_BOLD);
                }
            }
            while (j < len && text[j] != '>') {
                if (text[j] == ' ' || text[j] == '\t') {
                    j++;
                    continue;
                }
                if (text[j] == '/' || text[j] == '=') {
                    editor_lex_emit(runs, capacity, &run_count, j, 1,
                                    ANSI_COLOR_BRIGHT_CYAN, 0);
                    j++;
                    continue;
                }
                if (text[j] == '"' || text[j] == '\'') {
                    char quote = text[j];
                    size_t vs = j;
                    j++;
                    while (j < len && text[j] != quote) {
                        j++;
                    }
                    if (j < len) {
                        j++;
                    }
                    editor_lex_emit(runs, capacity, &run_count, vs, j - vs,
                                    ANSI_COLOR_BRIGHT_GREEN, 0);
                    continue;
                }
                {
                    size_t as = j;
                    while (j < len && text[j] != '>' && text[j] != '=' &&
                           text[j] != ' ' && text[j] != '\t' && text[j] != '/') {
                        j++;
                    }
                    if (j > as) {
                        editor_lex_emit(runs, capacity, &run_count, as, j - as,
                                        ANSI_COLOR_BRIGHT_WHITE, 0);
                    } else {
                        j++; /* stray char, keep scanning */
                    }
                }
            }
            if (j < len && text[j] == '>') {
                editor_lex_emit(runs, capacity, &run_count, j, 1,
                                ANSI_COLOR_BRIGHT_CYAN, 0);
                j++;
            }
            i = j;
        }
    }
    return run_count;
}

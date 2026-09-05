/**
 * @file editor.c
 * @brief DOS-style inline text editor document model for P4MiniShell.
 *
 * Implements a byte-preserving, line-based text document with a cursor,
 * text selection, insert/overwrite modes, undo/redo, and SD save. All
 * mutators run on the LVGL task; only editor_doc_load()/save() touch the
 * filesystem (worker task).
 */

#include "editor.h"
#include "editor_view.h"
#include "p4minishell_config.h"
#include "storage.h"
#include "shell.h"
#include "modal.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

    new_text = realloc(line->text, new_cap);
    if (new_text == NULL) {
        return false;
    }
    line->text = new_text;
    line->capacity = new_cap;
    return true;
}

static void editor_line_free(editor_line_t *line)
{
    free(line->text);
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

/** One undo/redo snapshot: serialized lines plus cursor/selection state. */
typedef struct {
    char *data;                /**< Serialized lines joined with '\n' */
    size_t cursor_row;
    size_t cursor_col;
    size_t sel_row;
    size_t sel_col;
    bool selection_active;
} editor_undo_snapshot_t;

/** Undo/redo ring storage, kept on the document itself (undo_storage). */
typedef struct {
    editor_undo_snapshot_t undo[P4_CONFIG_EDITOR_UNDO_DEPTH];
    size_t undo_count;         /**< Valid entries at the tail of undo[] */
    size_t undo_head;          /**< Ring head (oldest entry) */
    editor_undo_snapshot_t redo[P4_CONFIG_EDITOR_UNDO_DEPTH];
    size_t redo_count;
    size_t redo_head;
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

    new_lines = realloc(doc->lines, new_cap * sizeof(editor_line_t));
    if (new_lines == NULL) {
        return false;
    }
    doc->lines = new_lines;
    doc->line_capacity = new_cap;
    return true;
}

/** Insert a blank line slot at @p row. Returns the new line or NULL. */
static editor_line_t *editor_doc_insert_line(editor_doc_t *doc, size_t row)
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
static void editor_doc_remove_line(editor_doc_t *doc, size_t row)
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
    editor_undo_state_t *state;
    size_t i;
    size_t idx;
    if (doc == NULL) {
        return;
    }
    /* Free the heap-backed undo/redo snapshots before the document itself:
     * each snapshot's data is a serialized copy of the whole document. */
    state = (editor_undo_state_t *)&doc->undo_storage;
    for (i = 0; i < state->undo_count; i++) {
        idx = (state->undo_head + i) % P4_CONFIG_EDITOR_UNDO_DEPTH;
        free(state->undo[idx].data);
        state->undo[idx].data = NULL;
    }
    for (i = 0; i < state->redo_count; i++) {
        idx = (state->redo_head + i) % P4_CONFIG_EDITOR_UNDO_DEPTH;
        free(state->redo[idx].data);
        state->redo[idx].data = NULL;
    }
    for (i = 0; i < doc->line_count; i++) {
        editor_line_free(&doc->lines[i]);
    }
    free(doc->lines);
    free(doc);
}

void editor_doc_pick_syntax(editor_doc_t *doc)
{
    const char *dot;
    if (doc == NULL || doc->path[0] == '\0') {
        doc->syntax = EDITOR_SYNTAX_PLAIN;
        return;
    }
    dot = strrchr(doc->path, '.');
    if (dot == NULL) {
        doc->syntax = EDITOR_SYNTAX_PLAIN;
        return;
    }
    if (strcasecmp(dot, ".bat") == 0 || strcasecmp(dot, ".cmd") == 0) {
        doc->syntax = EDITOR_SYNTAX_BATCH;
    } else {
        doc->syntax = EDITOR_SYNTAX_PLAIN;
    }
}

/* ========================================================================
 * LOAD
 * ======================================================================== */

static bool editor_doc_append_bytes(editor_doc_t *doc, const char *buf, size_t len)
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

editor_doc_t *editor_doc_load(const char *path)
{
    editor_doc_t *doc;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    struct stat st;
    char *buf = NULL;
    size_t file_size;
    size_t read_total = 0;
    esp_err_t err;
    bool ok = false;

    if (path == NULL || path[0] == '\0') {
        return NULL;
    }

    err = shell_fs_resolve_path(path, resolved, sizeof(resolved));
    if (err != ESP_OK) {
        return NULL;
    }

    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return NULL;
    }

    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        shell_sd_end(&session, "edit");
        return NULL;
    }

    file_size = (size_t)st.st_size;
    if (file_size > P4_CONFIG_EDITOR_MAX_BYTES) {
        shell_sd_end(&session, "edit");
        return NULL;
    }

    buf = malloc(file_size > 0 ? file_size : 1);
    if (buf == NULL) {
        shell_sd_end(&session, "edit");
        return NULL;
    }

    file = fopen(resolved, "rb");
    if (file == NULL) {
        free(buf);
        shell_sd_end(&session, "edit");
        return NULL;
    }

    while (read_total < file_size) {
        size_t got = fread(buf + read_total, 1, file_size - read_total, file);
        if (got == 0) {
            break;
        }
        read_total += got;
    }
    fclose(file);

    doc = calloc(1, sizeof(editor_doc_t));
    if (doc != NULL) {
        snprintf(doc->path, sizeof(doc->path), "%s", path);
        doc->crlf = false; /* default; CRLF only when a \r\n is observed */
        doc->trailing_newline = false;
        if (editor_doc_insert_line(doc, 0) != NULL) {
            if (read_total > 0) {
                ok = editor_doc_append_bytes(doc, buf, read_total);
            } else {
                ok = true;
            }
        }
        editor_doc_pick_syntax(doc);
    }
    free(buf);
    shell_sd_end(&session, "edit");

    if (!ok || doc == NULL) {
        if (doc != NULL) {
            editor_doc_free(doc);
        }
        return NULL;
    }

    /* A trailing newline leaves a phantom empty line after the last real
     * line; drop it. The cursor can still land on a fresh empty line by
     * pressing Enter at the end of the final line. */
    if (doc->trailing_newline && doc->line_count > 1 &&
        doc->lines[doc->line_count - 1].length == 0) {
        editor_doc_remove_line(doc, doc->line_count - 1);
    }

    return doc;
}

bool editor_file_missing(const char *resolved_path)
{
    shell_sd_session_t session;
    struct stat st;

    if (resolved_path == NULL || resolved_path[0] == '\0') {
        return true;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return true; /* No SD session: treat as missing (fresh buffer). */
    }
    bool missing = (stat(resolved_path, &st) != 0);
    shell_sd_end(&session, "edit");
    return missing;
}

/* ========================================================================
 * SAVE
 * ======================================================================== */

esp_err_t editor_doc_save(editor_doc_t *doc, const char *path)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    size_t i;
    esp_err_t err;

    if (doc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = shell_fs_resolve_path(path, resolved, sizeof(resolved));
    if (err != ESP_OK) {
        return err;
    }

    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }

    file = fopen(resolved, "wb");
    if (file == NULL) {
        shell_sd_end(&session, "edit");
        return ESP_FAIL;
    }

    /* A failed write removes the partial destination: a truncated file that
     * looks complete is worse than no file (the shell's storage guardrail). */
    for (i = 0; i < doc->line_count; i++) {
        const char *text = doc->lines[i].text != NULL ? doc->lines[i].text : "";
        size_t len = doc->lines[i].length;

        if (len > 0 && fwrite(text, 1, len, file) != len) {
            fclose(file);
            remove(resolved);
            shell_sd_end(&session, "edit");
            return ESP_FAIL;
        }
        if (i + 1 < doc->line_count) {
            /* CRLF when the file uses CRLF, otherwise a bare LF. The EOL is
             * selected explicitly: writing only the first byte of "\r\n" when
             * crlf is false would emit a lone CR. */
            const char crlf[] = "\r\n";
            const char lf[] = "\n";
            const char *eol = doc->crlf ? crlf : lf;
            size_t eol_len = doc->crlf ? 2 : 1;
            if (fwrite(eol, 1, eol_len, file) != eol_len) {
                fclose(file);
                remove(resolved);
                shell_sd_end(&session, "edit");
                return ESP_FAIL;
            }
        }
    }

    /* Preserve a trailing newline when the original file had one. */
    if (doc->trailing_newline && doc->line_count > 0) {
        const char crlf[] = "\r\n";
        const char lf[] = "\n";
        const char *eol = doc->crlf ? crlf : lf;
        size_t eol_len = doc->crlf ? 2 : 1;
        if (fwrite(eol, 1, eol_len, file) != eol_len) {
            fclose(file);
            remove(resolved);
            shell_sd_end(&session, "edit");
            return ESP_FAIL;
        }
    }

    fclose(file);
    shell_sd_end(&session, "edit");
    return ESP_OK;
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
    editor_doc_split_line(doc);
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

    result = malloc(total + 1);
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
    free(text);
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
        editor_line_insert_at(&doc->lines[doc->cursor_row],
                              doc->cursor_col, replacement, repl_len);
        doc->cursor_col += repl_len;
    }

    if (out_row) *out_row = row;
    if (out_col) *out_col = col;
    return true;
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

/** Serialize the whole document into one heap string joined with '\n'. */
static char *editor_doc_serialize(const editor_doc_t *doc)
{
    size_t total = 0;
    size_t i;
    size_t pos = 0;
    char *out;

    if (doc == NULL) {
        return NULL;
    }
    for (i = 0; i < doc->line_count; i++) {
        total += doc->lines[i].length;
        if (i + 1 < doc->line_count) {
            total++; /* '\n' separator */
        }
    }
    out = malloc(total + 1);
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

    new_lines = malloc(count * sizeof(editor_line_t));
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
        free(new_lines);
        return false;
    }

    /* Swap the fresh line set into the document. */
    for (i = 0; i < doc->line_count; i++) {
        editor_line_free(&doc->lines[i]);
    }
    free(doc->lines);
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

    if (doc == NULL) {
        return;
    }
    state = (editor_undo_state_t *)&doc->undo_storage;
    snap = editor_doc_serialize(doc);
    if (snap == NULL) {
        return;
    }

    if (state->undo_count < P4_CONFIG_EDITOR_UNDO_DEPTH) {
        slot = &state->undo[state->undo_count];
        state->undo_count++;
    } else {
        /* Ring full: overwrite the oldest. */
        slot = &state->undo[state->undo_head];
        free(slot->data);
        state->undo_head = (state->undo_head + 1) % P4_CONFIG_EDITOR_UNDO_DEPTH;
    }

    slot->data = snap;
    slot->cursor_row = doc->cursor_row;
    slot->cursor_col = doc->cursor_col;
    slot->sel_row = doc->sel_row;
    slot->sel_col = doc->sel_col;
    slot->selection_active = doc->selection_active;
}

static void editor_redo_clear(editor_doc_t *doc)
{
    editor_undo_state_t *state = (editor_undo_state_t *)&doc->undo_storage;
    size_t i;
    for (i = 0; i < state->redo_count; i++) {
        free(state->redo[i].data);
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
        snap = editor_doc_serialize(doc);
        if (snap != NULL) {
            size_t r_idx = (state->redo_head + state->redo_count) % P4_CONFIG_EDITOR_UNDO_DEPTH;
            state->redo[r_idx].data = snap;
            state->redo[r_idx].cursor_row = doc->cursor_row;
            state->redo[r_idx].cursor_col = doc->cursor_col;
            state->redo[r_idx].sel_row = doc->sel_row;
            state->redo[r_idx].sel_col = doc->sel_col;
            state->redo[r_idx].selection_active = doc->selection_active;
            state->redo_count++;
        }
    }

    editor_doc_deserialize(doc, slot->data);
    doc->cursor_row = slot->cursor_row;
    doc->cursor_col = slot->cursor_col;
    doc->sel_row = slot->sel_row;
    doc->sel_col = slot->sel_col;
    doc->selection_active = slot->selection_active;
    doc->modified = true;

    /* Pop the undo entry. */
    free(slot->data);
    slot->data = NULL;
    state->undo_count--;
}

void editor_doc_redo(editor_doc_t *doc)
{
    editor_undo_state_t *state;
    editor_undo_snapshot_t *slot;
    char *snap;
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
    snap = editor_doc_serialize(doc);
    if (snap != NULL) {
        size_t u_idx = (state->undo_head + state->undo_count) % P4_CONFIG_EDITOR_UNDO_DEPTH;
        if (state->undo_count < P4_CONFIG_EDITOR_UNDO_DEPTH) {
            state->undo[u_idx].data = snap;
            state->undo[u_idx].cursor_row = doc->cursor_row;
            state->undo[u_idx].cursor_col = doc->cursor_col;
            state->undo[u_idx].sel_row = doc->sel_row;
            state->undo[u_idx].sel_col = doc->sel_col;
            state->undo[u_idx].selection_active = doc->selection_active;
            state->undo_count++;
        } else {
            /* Ring full: replace the oldest without growing. */
            size_t old = state->undo_head;
            free(state->undo[old].data);
            state->undo[old].data = snap;
            state->undo[old].cursor_row = doc->cursor_row;
            state->undo[old].cursor_col = doc->cursor_col;
            state->undo[old].sel_row = doc->sel_row;
            state->undo[old].sel_col = doc->sel_col;
            state->undo[old].selection_active = doc->selection_active;
            state->undo_head = (state->undo_head + 1) % P4_CONFIG_EDITOR_UNDO_DEPTH;
        }
    }

    editor_doc_deserialize(doc, slot->data);
    doc->cursor_row = slot->cursor_row;
    doc->cursor_col = slot->cursor_col;
    doc->sel_row = slot->sel_row;
    doc->sel_col = slot->sel_col;
    doc->selection_active = slot->selection_active;
    doc->modified = true;

    free(slot->data);
    slot->data = NULL;
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
 * MODAL SURFACE (worker task)
 * ======================================================================== */

/* Session event bits. MODAL_EVENT_CLOSE_REQUEST / MODAL_EVENT_CLOSED live in modal.h. */
#define EDITOR_EVENT_SAVE   (1 << 2)

struct editor_session {
    editor_control_t control;
    EventGroupHandle_t event_group;
    bool exited;
    bool open_failed;     /**< The LVGL view could not be created */
};

/* Set while a modal editor session is active (from session start until the
 * view is torn down). Lets the console reader route serial input to the
 * editor even during the async view-open window. */
static volatile bool s_session_active;

/** Per-modal-run context handed to the shared modal runtime. */
typedef struct {
    const char *path_arg;
    int *errorlevel_out;
    esp_err_t result;
    editor_session_t *session;
} editor_modal_ctx_t;

/** Report whether a modal editor session is running (view open or opening). */
bool editor_session_is_active(void)
{
    return s_session_active;
}

/* Forward: view open/close must run on the LVGL task. */
static void editor_session_open_cb(void *user_data);
static void editor_session_close_cb(void *user_data);

static void editor_session_open_cb(void *user_data)
{
    editor_session_t *session = (editor_session_t *)user_data;
    if (session == NULL) {
        return;
    }
    if (!editor_view_open(session->control.doc, &session->control)) {
        /* The view could not be created; wake the worker so the session does
         * not block forever waiting for a quit that will never come. */
        session->open_failed = true;
        xEventGroupSetBits(session->event_group, MODAL_EVENT_CLOSE_REQUEST);
    }
}

static void editor_session_close_cb(void *user_data)
{
    editor_session_t *session = (editor_session_t *)user_data;
    if (session != NULL) {
        editor_view_close();
        xEventGroupSetBits(session->event_group, MODAL_EVENT_CLOSED);
    }
}

static bool editor_surface_open(void *ctx, EventGroupHandle_t event_group)
{
    editor_modal_ctx_t *mctx = (editor_modal_ctx_t *)ctx;
    const char *path = mctx->path_arg;
    bool unnamed = (path == NULL || path[0] == '\0');
    char resolved[P4_CONFIG_SD_PATH_BYTES] = "";
    editor_session_t *session = calloc(1, sizeof(*session));

    if (session == NULL) {
        mctx->result = ESP_ERR_NO_MEM;
        return false;
    }

    /* Resolve the save path now so save operations don't re-resolve. */
    if (!unnamed) {
        if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
            free(session);
            mctx->result = ESP_ERR_INVALID_ARG;
            return false;
        }
    }

    if (unnamed) {
        session->control.doc = editor_doc_new(NULL);
    } else if (!editor_file_missing(resolved)) {
        /* The file exists (or SD is present): a load failure now is a
         * real error. Never fall through to a fresh buffer here — the
         * user would save an empty buffer over the original file. */
        session->control.doc = editor_doc_load(resolved);
        if (session->control.doc == NULL) {
            free(session);
            shell_print_error("edit: cannot load '%s' (over the %u KB / %u line "
                              "editor limit, or an I/O error)",
                              path,
                              (unsigned)(P4_CONFIG_EDITOR_MAX_BYTES / 1024),
                              (unsigned)P4_CONFIG_EDITOR_MAX_LINES);
            mctx->result = ESP_ERR_NOT_SUPPORTED;
            return false;
        }
    } else {
        /* Missing file: start a new buffer bound to the path. */
        session->control.doc = editor_doc_new(resolved);
    }

    if (session->control.doc == NULL) {
        free(session);
        mctx->result = ESP_ERR_NO_MEM;
        return false;
    }

    session->event_group = event_group;
    session->control.event_group = event_group;

    /* Mark the session active BEFORE the async view-open so serial input
     * arriving while the view is opening routes to the editor, not the shell. */
    s_session_active = true;
    mctx->session = session;

    /* Open the view on the LVGL task via an async call. */
    if (lv_async_call(editor_session_open_cb, session) != LV_RESULT_OK) {
        s_session_active = false;
        editor_doc_free(session->control.doc);
        free(session);
        mctx->session = NULL;
        mctx->result = ESP_FAIL;
        return false;
    }

    return true;
}

static void editor_surface_service(void *ctx, EventBits_t bits)
{
    editor_modal_ctx_t *mctx = (editor_modal_ctx_t *)ctx;
    editor_session_t *session = mctx->session;
    editor_control_t *control;
    esp_err_t err;

    if (session == NULL || (bits & EDITOR_EVENT_SAVE) == 0) {
        return;
    }

    control = &session->control;
    err = ESP_ERR_INVALID_ARG;

    /* A Save-As request carries an explicit target path; otherwise
     * save to the document's source path, and fall back to a
     * generated name only for an unnamed buffer. */
    if (control->save_as_path[0] != '\0') {
        char save_as_resolved[P4_CONFIG_SD_PATH_BYTES] = "";
        if (shell_fs_resolve_path(control->save_as_path,
                                  save_as_resolved,
                                  sizeof(save_as_resolved)) == ESP_OK) {
            err = editor_doc_save(control->doc, save_as_resolved);
            if (err == ESP_OK) {
                editor_doc_set_path(control->doc, save_as_resolved);
            }
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
        control->save_as_path[0] = '\0';
    } else if (control->doc != NULL && control->doc->path[0] != '\0') {
        err = editor_doc_save(control->doc, control->doc->path);
    } else if (control->doc != NULL) {
        /* Unnamed buffer: fall back to a generated name. */
        err = editor_doc_save(control->doc, "EDIT.NEW");
    }
    control->save_ok = (err == ESP_OK);
    if (err != ESP_OK) {
        mctx->result = err;
    }

    /* Refresh the status bar on the LVGL task. */
    lv_async_call(editor_view_notify_saved_cb, (void *)(intptr_t)(err == ESP_OK));
}

static void editor_surface_close(void *ctx)
{
    editor_modal_ctx_t *mctx = (editor_modal_ctx_t *)ctx;
    editor_session_t *session = mctx->session;

    if (session != NULL) {
        lv_async_call(editor_session_close_cb, session);
    }
}

static bool editor_surface_handle_usb_key(void *ctx, uint8_t key_code, uint8_t modifiers, char ascii)
{
    (void)ctx;
    if (!editor_view_is_open()) {
        return false;
    }
    editor_view_handle_usb_key(key_code, modifiers, ascii);
    return true;
}

/* Serial console control verbs accepted while the editor is open. */
static bool editor_serial_is_verb(const char *line, const char *verb)
{
    return line[0] == '\\' && strcasecmp(line + 1, verb) == 0;
}

/* Per-call context for async serial-line dispatch to the LVGL task. */
typedef struct {
    char *line;
    editor_session_t *session;
} editor_serial_line_ctx_t;

/* Runs on the LVGL task: feeds one serial line's characters into the editor. */
static void editor_serial_line_cb(void *user_data)
{
    editor_serial_line_ctx_t *ctx = (editor_serial_line_ctx_t *)user_data;
    const char *line;
    size_t i;
    size_t len;

    if (ctx == NULL || ctx->line == NULL) {
        free(ctx);
        return;
    }

    line = ctx->line;
    len = strlen(line);

    if (editor_serial_is_verb(line, "q") || editor_serial_is_verb(line, "quit")) {
        if (editor_view_is_open()) {
            editor_view_handle_usb_key(0x29, 0, 0); /* Esc -> quit */
        } else {
            editor_view_set_quit_requested();
        }
    } else if (editor_serial_is_verb(line, "s") || editor_serial_is_verb(line, "save")) {
        if (editor_view_is_open()) {
            editor_view_handle_usb_key(0, 0x01, 's'); /* Ctrl+S */
        } else {
            editor_view_set_save_requested();
        }
    } else if (editor_serial_is_verb(line, "u") || editor_serial_is_verb(line, "undo")) {
        editor_view_handle_usb_key(0, 0x01, 'z');
    } else if (editor_serial_is_verb(line, "f") || editor_serial_is_verb(line, "find")) {
        editor_view_handle_usb_key(0, 0x01, 'f'); /* Ctrl+F */
    } else if (editor_serial_is_verb(line, "g") || editor_serial_is_verb(line, "goto")) {
        editor_view_handle_usb_key(0, 0x01, 'g'); /* Ctrl+G -> Go to line */
    } else if (editor_serial_is_verb(line, "o") || editor_serial_is_verb(line, "saveas")) {
        editor_view_handle_usb_key(0, 0x01, 'o'); /* Ctrl+O -> Save As */
    } else if (editor_serial_is_verb(line, "r") || editor_serial_is_verb(line, "redo")) {
        editor_view_handle_usb_key(0, 0x03, 'z'); /* Ctrl+Shift+Z */
    } else if (editor_serial_is_verb(line, "a") || editor_serial_is_verb(line, "selectall")) {
        editor_view_handle_usb_key(0, 0x01, 'a');
    } else {
        if (!editor_view_is_open()) {
            free(ctx->line);
            free(ctx);
            return;
        }
        /* A line of typed text: insert each character, then a newline. */
        for (i = 0; i < len; i++) {
            char ch = line[i];
            if (ch == '\\' && i == 0 && len > 1) {
                /* "\foo" that was not a known verb inserts a literal backslash
                 * and the rest of the line as text. */
                editor_view_handle_usb_key(0, 0, '\\');
                continue;
            }
            if (ch >= 0x20) {
                editor_view_handle_usb_key(0, 0, ch);
            }
        }
        editor_view_handle_usb_key(0x28, 0, '\n'); /* Enter -> newline */
    }

    free(ctx->line);
    free(ctx);
}

static bool editor_surface_handle_serial_line(void *ctx, const char *line)
{
    editor_modal_ctx_t *mctx = (editor_modal_ctx_t *)ctx;
    editor_serial_line_ctx_t *sl_ctx;

    /* Accept lines whenever a session is active (even while the view is
     * still opening), so a quick '\q' after 'edit' is never misrouted as a
     * shell command and lost behind the blocked worker. */
    if (mctx == NULL || mctx->session == NULL || line == NULL) {
        return false;
    }

    /* Defer the whole line to the LVGL task: the editor's document and widget
     * state live there, and rebuilding rows on the UART console task would
     * block it past the watchdog and race the render cycle. */
    sl_ctx = malloc(sizeof(*sl_ctx));
    if (sl_ctx == NULL) {
        return true; /* Consumed (drop) rather than misrouted. */
    }
    sl_ctx->line = strdup(line);
    if (sl_ctx->line == NULL) {
        free(sl_ctx);
        return true;
    }
    sl_ctx->session = mctx->session;

    if (lv_async_call(editor_serial_line_cb, sl_ctx) != LV_RESULT_OK) {
        free(sl_ctx->line);
        free(sl_ctx);
    }
    return true;
}

static const modal_surface_t editor_surface = {
    .name = "edit",
    .open = editor_surface_open,
    .service = editor_surface_service,
    .close = editor_surface_close,
    .handle_usb_key = editor_surface_handle_usb_key,
    .handle_serial_line = editor_surface_handle_serial_line,
};

esp_err_t editor_session_run(const char *path, int *errorlevel)
{
    editor_modal_ctx_t mctx = {0};
    esp_err_t run_err;

    mctx.path_arg = path;
    mctx.errorlevel_out = errorlevel;
    mctx.result = ESP_OK;

    if (errorlevel != NULL) {
        *errorlevel = 0;
    }

    run_err = modal_surface_run(&editor_surface, &mctx, errorlevel);
    if (run_err != ESP_OK && mctx.result == ESP_OK) {
        mctx.result = run_err;
    }

    /* Tear down the session now that the LVGL view is closed. */
    if (mctx.session != NULL) {
        if (mctx.session->open_failed && mctx.result == ESP_OK) {
            mctx.result = ESP_FAIL;
        }
        if (mctx.session->control.doc != NULL) {
            editor_doc_free(mctx.session->control.doc);
        }
        free(mctx.session);
    }

    s_session_active = false;

    if (errorlevel != NULL && mctx.result != ESP_OK) {
        *errorlevel = 1;
    }

    return mctx.result;
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
    default:
        break;
    }

    if (ctrl) {
        /* Redo is Ctrl+Shift+Z (DOS-style Ctrl+Y is delete line). */
        if (shift) {
            switch (ascii) {
            case 'z': case 'Z': return EDITOR_KEY_REDO;
            default:
                break;
            }
        }
        switch (ascii) {
        case 'a': case 'A': return EDITOR_KEY_SELECT_ALL;
        case 'c': case 'C': return EDITOR_KEY_COPY;
        case 'x': case 'X': return EDITOR_KEY_CUT;
        case 'v': case 'V': return EDITOR_KEY_PASTE;
        case 'z': case 'Z': return EDITOR_KEY_UNDO;
        case 'y': case 'Y': return EDITOR_KEY_DELETE_LINE;
        case 's': case 'S': return EDITOR_KEY_SAVE;
        case 'o': case 'O': return EDITOR_KEY_SAVE_AS;
        case 'f': case 'F': return EDITOR_KEY_FIND;
        case 'h': case 'H': return EDITOR_KEY_REPLACE;
        case 'g': case 'G': return EDITOR_KEY_GOTO_LINE;
        case 'q': case 'Q': return EDITOR_KEY_QUIT;
        default:
            break;
        }
    }

    (void)shift;
    return EDITOR_KEY_NONE;
}

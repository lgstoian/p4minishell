/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_editor.c
 * @brief Unit tests for the byte-preserving editor document model.
 *
 * Covers the pure document operations only (no SD, no LVGL): line editing,
 * cursor movement, selection, undo/redo, delete-line/EOL, word navigation,
 * find/replace, and the batch lexer. The LVGL view and the worker session
 * are hardware/display bound and are exercised on the board instead.
 */

#include "unity.h"
#include "editor.h"
#include "editor_view.h"
#include "shell.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */

/** Return the row-th line content as a heap NUL-terminated string. */
static char *line_str(editor_doc_t *doc, size_t row)
{
    size_t len = editor_doc_line_length(doc, row);
    const char *text = editor_doc_line_text(doc, row);
    char *out = malloc(len + 1);

    TEST_ASSERT_NOT_NULL(out);
    if (len > 0) {
        memcpy(out, text, len);
    }
    out[len] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */

void test_editor_osk_key_from_label(void)
{
    editor_key_t key = EDITOR_KEY_NONE;

    /* Every named button on the Nav/Edit OSK pages maps to its editor key. */
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Save", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_SAVE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("SaveAs", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_SAVE_AS, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Open", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_OPEN, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Comment", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_COMMENT, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Match", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_MATCH_JUMP, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Wrap", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_WRAP_TOGGLE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Focus", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_FOCUS_TOGGLE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Spell", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_SPELL_TOGGLE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Reload", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_RELOAD, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Find", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_FIND, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Next", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_FIND_NEXT, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Rep", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_REPLACE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Replace", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_REPLACE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("All", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_REPLACE_ALL, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("ReplAll", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_REPLACE_ALL, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Case", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_CASE_TOGGLE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Goto", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_GOTO_LINE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Undo", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_UNDO, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Redo", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_REDO, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Prev", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_PREVIEW, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Preview", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_PREVIEW, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Copy", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_COPY, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Cut", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_CUT, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Paste", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_PASTE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("SelAll", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_SELECT_ALL, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("WordL", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_WORD_LEFT, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("WordR", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_WORD_RIGHT, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("DocTop", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_DOC_HOME, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("DocBot", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_DOC_END, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("DelLine", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_DELETE_LINE, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("DelEOL", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_DELETE_EOL, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Quit", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_QUIT, key);
    TEST_ASSERT_TRUE(editor_osk_key_from_label("Exit", &key));
    TEST_ASSERT_EQUAL_INT(EDITOR_KEY_QUIT, key);

    /* LVGL symbols, mode buttons, single chars and garbage are NOT command
     * labels (they are routed elsewhere in editor_view_handle_osk). */
    TEST_ASSERT_FALSE(editor_osk_key_from_label(NULL, &key));
    TEST_ASSERT_FALSE(editor_osk_key_from_label("Save", NULL));
    TEST_ASSERT_FALSE(editor_osk_key_from_label("bogus", &key));
    TEST_ASSERT_FALSE(editor_osk_key_from_label("abc", &key));
    TEST_ASSERT_FALSE(editor_osk_key_from_label("a", &key));
}

void test_editor_new_doc(void)
{
    editor_doc_t *doc = editor_doc_new("TEST.BAT");

    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_row(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_col(doc));
    TEST_ASSERT_FALSE(editor_doc_is_modified(doc));
    TEST_ASSERT_EQUAL(EDITOR_SYNTAX_BATCH, doc->syntax);
    editor_doc_free(doc);
}

void test_editor_insert_and_cursor(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);
    const char *text = "hello world";

    editor_doc_insert_bytes(doc, text, strlen(text));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_row(doc));
    TEST_ASSERT_EQUAL_size_t(strlen(text), editor_doc_cursor_col(doc));
    TEST_ASSERT_TRUE(editor_doc_is_modified(doc));

    {
        char *s = line_str(doc, 0);
        TEST_ASSERT_EQUAL_STRING(text, s);
        free(s);
    }

    /* Word navigation: cursor at end -> word_left -> start of "world" (col 6). */
    editor_doc_cursor_word_left(doc);
    TEST_ASSERT_EQUAL_size_t(6, editor_doc_cursor_col(doc));
    editor_doc_cursor_word_left(doc);
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_col(doc));
    editor_doc_cursor_word_right(doc);
    TEST_ASSERT_EQUAL_size_t(6, editor_doc_cursor_col(doc));

    editor_doc_free(doc);
}

void test_editor_newline_split_and_join(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    editor_doc_insert_bytes(doc, "ab", 2);
    /* cursor after "ab" */
    editor_doc_cursor_home(doc);
    editor_doc_cursor_right(doc); /* between 'a' and 'b' */
    editor_doc_newline(doc);

    TEST_ASSERT_EQUAL_size_t(2, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_cursor_row(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_col(doc));

    {
        char *s0 = line_str(doc, 0);
        char *s1 = line_str(doc, 1);
        TEST_ASSERT_EQUAL_STRING("a", s0);
        TEST_ASSERT_EQUAL_STRING("b", s1);
        free(s0);
        free(s1);
    }

    /* Backspace at start of line joins with the previous line. */
    editor_doc_backspace(doc);
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_cursor_col(doc));
    {
        char *s = line_str(doc, 0);
        TEST_ASSERT_EQUAL_STRING("ab", s);
        free(s);
    }

    editor_doc_free(doc);
}

void test_editor_delete_forward(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    editor_doc_insert_bytes(doc, "abcd", 4);
    editor_doc_cursor_home(doc);
    editor_doc_delete(doc); /* removes 'a' */
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_col(doc));
    {
        char *s = line_str(doc, 0);
        TEST_ASSERT_EQUAL_STRING("bcd", s);
        free(s);
    }

    /* Delete joins with the next line at end of line. */
    editor_doc_cursor_end(doc);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "xy", 2);
    editor_doc_cursor_home(doc);
    editor_doc_cursor_up(doc);
    editor_doc_cursor_end(doc);
    editor_doc_delete(doc);
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    {
        char *s = line_str(doc, 0);
        TEST_ASSERT_EQUAL_STRING("bcdxy", s);
        free(s);
    }

    editor_doc_free(doc);
}

void test_editor_overwrite_toggle(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    editor_doc_insert_bytes(doc, "abc", 3);
    editor_doc_cursor_home(doc);
    TEST_ASSERT_FALSE(doc->overwrite);
    editor_doc_toggle_overwrite(doc);
    TEST_ASSERT_TRUE(doc->overwrite);
    editor_doc_insert_char(doc, 'X'); /* overwrites 'a' */
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_cursor_col(doc));
    {
        char *s = line_str(doc, 0);
        TEST_ASSERT_EQUAL_STRING("Xbc", s);
        free(s);
    }

    editor_doc_free(doc);
}

void test_editor_delete_line_and_eol(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    editor_doc_insert_bytes(doc, "line one", 8);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "line two", 8);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "line three", 10);
    TEST_ASSERT_EQUAL_size_t(3, editor_doc_line_count(doc));

    /* Delete line 1 (row index 1): the cursor is on row 2 after typing, so
     * move up one row first. */
    editor_doc_cursor_home(doc);
    editor_doc_cursor_up(doc);
    editor_doc_delete_line(doc);
    TEST_ASSERT_EQUAL_size_t(2, editor_doc_line_count(doc));
    {
        char *s1 = line_str(doc, 1);
        TEST_ASSERT_EQUAL_STRING("line three", s1);
        free(s1);
    }

    /* Delete to end of line from column 5. After delete_line the cursor is
     * on row 1 ("line three"), so the trim lands on that row. */
    editor_doc_cursor_home(doc);
    editor_doc_cursor_right(doc);
    editor_doc_cursor_right(doc);
    editor_doc_cursor_right(doc);
    editor_doc_cursor_right(doc);
    editor_doc_cursor_right(doc);
    editor_doc_delete_to_eol(doc);
    {
        char *s1 = line_str(doc, 1);
        TEST_ASSERT_EQUAL_STRING("line ", s1);
        free(s1);
    }

    /* Deleting the only line leaves a single empty line. */
    editor_doc_free(doc);
    doc = editor_doc_new(NULL);
    editor_doc_insert_bytes(doc, "solo", 4);
    editor_doc_delete_line(doc);
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_line_length(doc, 0));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_row(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_col(doc));

    editor_doc_free(doc);
}

void test_editor_doc_home_end(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    editor_doc_insert_bytes(doc, "alpha", 5);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "beta", 4);

    editor_doc_cursor_doc_end(doc);
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_cursor_row(doc));
    TEST_ASSERT_EQUAL_size_t(4, editor_doc_cursor_col(doc));

    editor_doc_cursor_doc_home(doc);
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_row(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_col(doc));

    editor_doc_free(doc);
}

void test_editor_find_next(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);
    size_t row, col;

    editor_doc_insert_bytes(doc, "the quick brown Fox", 19);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "jumps over the lazy dog", 23);

    /* Case-sensitive find of "fox" must fail (the text has "Fox"). */
    TEST_ASSERT_FALSE(editor_doc_find_next(doc, "fox", 3, 0, 0, true, true, &row, &col));

    /* Case-folded find of "fox" succeeds. */
    TEST_ASSERT_TRUE(editor_doc_find_next(doc, "fox", 3, 0, 0, false, true, &row, &col));
    TEST_ASSERT_EQUAL_size_t(0, row);
    TEST_ASSERT_EQUAL_size_t(16, col);

    /* Wrap: searching from the end finds the first match on line 1. */
    TEST_ASSERT_TRUE(editor_doc_find_next(doc, "the", 3, 1, 20, false, true, &row, &col));
    TEST_ASSERT_EQUAL_size_t(0, row);
    TEST_ASSERT_EQUAL_size_t(0, col);

    /* No wrap: a needle that only exists before the start is not found. */
    TEST_ASSERT_FALSE(editor_doc_find_next(doc, "quick", 5, 1, 0, false, false, &row, &col));

    /* Null/empty needle is rejected. */
    TEST_ASSERT_FALSE(editor_doc_find_next(doc, NULL, 0, 0, 0, false, true, &row, &col));
    TEST_ASSERT_FALSE(editor_doc_find_next(doc, "", 0, 0, 0, false, true, &row, &col));

    editor_doc_free(doc);
}

void test_editor_replace_next(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);
    size_t row, col;
    char *s;

    editor_doc_insert_bytes(doc, "one one two", 11);

    /* Replace the first "one" with "1". */
    TEST_ASSERT_TRUE(editor_doc_replace_next(doc, "one", 3, "1", 1, false, &row, &col));
    TEST_ASSERT_EQUAL_size_t(0, row);
    s = line_str(doc, 0);
    TEST_ASSERT_EQUAL_STRING("1 one two", s);
    free(s);

    /* The cursor now sits after the replacement; the next call replaces the
     * second "one". */
    TEST_ASSERT_TRUE(editor_doc_replace_next(doc, "one", 3, "1", 1, false, &row, &col));
    s = line_str(doc, 0);
    TEST_ASSERT_EQUAL_STRING("1 1 two", s);
    free(s);

    /* No more matches -> false, document unchanged. */
    TEST_ASSERT_FALSE(editor_doc_replace_next(doc, "one", 3, "1", 1, false, &row, &col));
    s = line_str(doc, 0);
    TEST_ASSERT_EQUAL_STRING("1 1 two", s);
    free(s);

    editor_doc_free(doc);
}

void test_editor_undo_redo(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);
    char *s;

    /* editor_doc_undo_mark() records the pre-edit state; editor_doc_newline()
     * records its own snapshot, so the ring holds [empty, "hello world"]. */
    editor_doc_undo_mark(doc);
    editor_doc_insert_bytes(doc, "hello ", 6);
    editor_doc_insert_bytes(doc, "world", 5);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "again", 5);

    /* Undo: back past the newline. */
    editor_doc_undo(doc);
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    s = line_str(doc, 0);
    TEST_ASSERT_EQUAL_STRING("hello world", s);
    free(s);

    /* Undo: back to the empty document. */
    editor_doc_undo(doc);
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_line_length(doc, 0));

    /* Redo restores the full two-line document. */
    editor_doc_redo(doc);
    editor_doc_redo(doc);
    TEST_ASSERT_EQUAL_size_t(2, editor_doc_line_count(doc));
    s = line_str(doc, 1);
    TEST_ASSERT_EQUAL_STRING("again", s);
    free(s);

    /* Undoing past the start is a no-op. */
    editor_doc_undo(doc);
    editor_doc_undo(doc);
    editor_doc_undo(doc);
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_line_length(doc, 0));

    editor_doc_free(doc);
}

void test_editor_selection(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);
    char *s;

    editor_doc_insert_bytes(doc, "first", 5);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "second", 6);

    /* Select all and delete -> single empty line. */
    editor_doc_select_all(doc);
    TEST_ASSERT_TRUE(editor_doc_has_selection(doc));
    editor_doc_selection_delete(doc);
    TEST_ASSERT_FALSE(editor_doc_has_selection(doc));
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_line_length(doc, 0));
    s = line_str(doc, 0);
    TEST_ASSERT_EQUAL_STRING("", s);
    free(s);

    editor_doc_free(doc);
}

void test_editor_selection_copy_lf(void)
{
    /* The multi-line selection copy must emit LF separators for LF files
     * (a lone CR would be dropped by the paste path). */
    editor_doc_t *doc = editor_doc_new(NULL);

    doc->crlf = false;
    editor_doc_insert_bytes(doc, "alpha", 5);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "beta", 4);

    editor_doc_select_all(doc);
    TEST_ASSERT_TRUE(editor_doc_selection_copy(doc));
    TEST_ASSERT_EQUAL_STRING("alpha\nbeta", shell_clipboard_get());

    editor_doc_free(doc);
}

void test_editor_selection_copy_crlf(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    doc->crlf = true;
    editor_doc_insert_bytes(doc, "alpha", 5);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "beta", 4);

    editor_doc_select_all(doc);
    TEST_ASSERT_TRUE(editor_doc_selection_copy(doc));
    TEST_ASSERT_EQUAL_STRING("alpha\r\nbeta", shell_clipboard_get());

    editor_doc_free(doc);
}

void test_editor_set_path(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    editor_doc_set_path(doc, "sd:/dir/script.bat");
    TEST_ASSERT_EQUAL_STRING("sd:/dir/script.bat", doc->path);
    TEST_ASSERT_EQUAL(EDITOR_SYNTAX_BATCH, doc->syntax);

    editor_doc_set_path(doc, "sd:/notes.txt");
    TEST_ASSERT_EQUAL(EDITOR_SYNTAX_PLAIN, doc->syntax);

    editor_doc_free(doc);
}

void test_editor_paste_multiline(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    shell_clipboard_set("p1\np2");
    editor_doc_paste(doc);
    TEST_ASSERT_EQUAL_size_t(2, editor_doc_line_count(doc));
    {
        char *s0 = line_str(doc, 0);
        char *s1 = line_str(doc, 1);
        TEST_ASSERT_EQUAL_STRING("p1", s0);
        TEST_ASSERT_EQUAL_STRING("p2", s1);
        free(s0);
        free(s1);
    }

    editor_doc_free(doc);
}

void test_editor_newline_on_empty_doc(void)
{
    /* Pressing Enter on a freshly-created buffer (whose only line has a NULL
     * text pointer) must not dereference NULL. */
    editor_doc_t *doc = editor_doc_new(NULL);

    editor_doc_newline(doc);
    TEST_ASSERT_EQUAL_size_t(2, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_cursor_row(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_col(doc));

    /* Typing on the fresh line works. */
    editor_doc_insert_bytes(doc, "x", 1);
    {
        char *s = line_str(doc, 1);
        TEST_ASSERT_EQUAL_STRING("x", s);
        free(s);
    }

    editor_doc_free(doc);
}

void test_editor_selection_delete_empty_start(void)
{
    /* A selection that starts at column 0 of an empty line (NULL text) must
     * delete cleanly instead of writing through a NULL pointer. Selecting the
     * whole document leaves a single empty line. */
    editor_doc_t *doc = editor_doc_new(NULL);

    editor_doc_newline(doc);        /* ["", ""] */
    editor_doc_insert_bytes(doc, "xyz", 3); /* ["", "xyz"] */
    editor_doc_select_all(doc);     /* sel (0,0) .. cursor (1,3) */
    editor_doc_selection_delete(doc);

    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_row(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_col(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_line_length(doc, 0));
    {
        char *s = line_str(doc, 0);
        TEST_ASSERT_EQUAL_STRING("", s);
        free(s);
    }

    editor_doc_free(doc);
}

void test_editor_word_nav_empty_line(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    /* Word movement on an empty (NULL text) line must not crash. */
    editor_doc_cursor_word_left(doc);
    editor_doc_cursor_word_right(doc);
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_cursor_col(doc));

    editor_doc_free(doc);
}

void test_editor_line_cap_bounded(void)
{
    /* Repeated Enter must not grow the document past the configured line
     * cap; the failed split leaves the document intact and usable.
     *
     * The cap is reached with one multi-line insert: `editor_doc_split_line`
     * enforces the limit inside `editor_doc_insert_bytes` (which does not take
     * a full-document undo snapshot per line, unlike a per-Enter loop, so the
     * test stays linear). */
    editor_doc_t *doc = editor_doc_new(NULL);
    size_t target = P4_CONFIG_EDITOR_MAX_LINES + 8;
    char *blob;
    size_t i;
    char *s;

    blob = malloc(target + 1);
    TEST_ASSERT_NOT_NULL(blob);
    for (i = 0; i < target; i++) {
        blob[i] = '\n';
    }
    blob[target] = '\0';

    editor_doc_insert_bytes(doc, blob, target);
    free(blob);
    TEST_ASSERT(editor_doc_line_count(doc) <= P4_CONFIG_EDITOR_MAX_LINES);
    TEST_ASSERT(editor_doc_line_count(doc) > 1);

    /* The document still edits normally after hitting the cap. */
    editor_doc_cursor_doc_home(doc);
    editor_doc_insert_char(doc, 'A');
    s = line_str(doc, 0);
    TEST_ASSERT_EQUAL_STRING("A", s);
    free(s);

    editor_doc_free(doc);
}

void test_editor_undo_redo_empty_last_line(void)
{
    /* Undo/redo across a document that ends in an empty line must restore a
     * usable, consistent document (the empty tail follows the file's
     * trailing-newline convention). */
    editor_doc_t *doc = editor_doc_new(NULL);
    char *s;

    editor_doc_undo_mark(doc);
    editor_doc_insert_bytes(doc, "a", 1);
    editor_doc_newline(doc);   /* ["a", ""] */
    editor_doc_insert_bytes(doc, "b", 1); /* ["a", "b"] */

    editor_doc_undo(doc);      /* back to ["a"] (snapshot "a") */
    editor_doc_undo(doc);      /* back to the empty document */
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_line_length(doc, 0));

    editor_doc_redo(doc);      /* ["a"] */
    editor_doc_redo(doc);      /* ["a", "b"] */
    TEST_ASSERT_EQUAL_size_t(2, editor_doc_line_count(doc));
    s = line_str(doc, 1);
    TEST_ASSERT_EQUAL_STRING("b", s);
    free(s);

    editor_doc_free(doc);
}

void test_editor_find_wrap_boundary(void)
{
    /* Search starting exactly at a match must find it; wrapping from the
     * very end must land on the first match. */
    editor_doc_t *doc = editor_doc_new(NULL);
    size_t row, col;

    editor_doc_insert_bytes(doc, "abc abc", 7);

    /* Start at column 0: finds the first "abc". */
    TEST_ASSERT_TRUE(editor_doc_find_next(doc, "abc", 3, 0, 0, false, true, &row, &col));
    TEST_ASSERT_EQUAL_size_t(0, row);
    TEST_ASSERT_EQUAL_size_t(0, col);

    /* Start at column 7 (end of line): wraps to the first match. */
    TEST_ASSERT_TRUE(editor_doc_find_next(doc, "abc", 3, 0, 7, false, true, &row, &col));
    TEST_ASSERT_EQUAL_size_t(0, row);
    TEST_ASSERT_EQUAL_size_t(0, col);

    /* Start at column 4: finds the second "abc". */
    TEST_ASSERT_TRUE(editor_doc_find_next(doc, "abc", 3, 0, 4, false, true, &row, &col));
    TEST_ASSERT_EQUAL_size_t(0, row);
    TEST_ASSERT_EQUAL_size_t(4, col);

    editor_doc_free(doc);
}

void test_editor_undo_ring_wrap_free(void)
{
    /* Drive the undo ring past its depth so it wraps and overwrites the
     * oldest entries, then free the document: the cleanup must free exactly
     * the live snapshots without double-freeing or leaking. */
    editor_doc_t *doc = editor_doc_new(NULL);
    size_t i;
    size_t total = (size_t)P4_CONFIG_EDITOR_UNDO_DEPTH * 3 + 5;

    for (i = 0; i < total; i++) {
        editor_doc_undo_mark(doc);
        editor_doc_insert_char(doc, (char)('a' + (i % 26)));
    }
    TEST_ASSERT_EQUAL_size_t(total, editor_doc_line_length(doc, 0));

    /* Interleave undo/redo across the wrap boundary. */
    editor_doc_undo(doc);
    editor_doc_undo(doc);
    editor_doc_redo(doc);
    editor_doc_free(doc);

    /* A second doc to confirm the state is not corrupted by the first. */
    doc = editor_doc_new(NULL);
    editor_doc_insert_bytes(doc, "clean", 5);
    editor_doc_free(doc);
}

void test_editor_selection_delete_multirow_tail(void)
{
    /* A selection that ends mid-line across rows must keep the last line's
     * UN-selected tail (a previous bug kept the selected head instead). */
    editor_doc_t *doc = editor_doc_new(NULL);
    char *s;

    editor_doc_insert_bytes(doc, "abcd", 4);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "efgh", 4);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "ijkl", 4);

    /* Select (0,3) .. (2,2): "d\nefgh\ni". Deleting leaves "abc"+"kl". */
    doc->sel_row = 0; doc->sel_col = 3;
    doc->cursor_row = 2; doc->cursor_col = 2;
    doc->selection_active = true;
    editor_doc_selection_delete(doc);

    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    s = line_str(doc, 0);
    TEST_ASSERT_EQUAL_STRING("abckl", s);
    free(s);

    /* Selecting to the very end of the last line deletes everything. */
    editor_doc_free(doc);
    doc = editor_doc_new(NULL);
    editor_doc_insert_bytes(doc, "first", 5);
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "second", 6);
    editor_doc_select_all(doc);
    editor_doc_selection_delete(doc);
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_line_count(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_line_length(doc, 0));
    s = line_str(doc, 0);
    TEST_ASSERT_EQUAL_STRING("", s);
    free(s);

    editor_doc_free(doc);
}

void test_editor_format_line_number(void)
{
    char buf[16];
    size_t n;

    /* Right-aligned 1-based line numbers in a fixed-width gutter. */
    n = editor_format_line_number(0, 4, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(5, n); /* "   1 " */
    TEST_ASSERT_EQUAL_STRING("   1 ", buf);

    n = editor_format_line_number(4, 4, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("   5 ", buf);

    n = editor_format_line_number(9, 4, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("  10 ", buf);

    n = editor_format_line_number(99, 4, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(" 100 ", buf);

    n = editor_format_line_number(2047, 4, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2048 ", buf);

    /* A number wider than the gutter still renders (right-aligned overflows). */
    n = editor_format_line_number(9999, 2, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("10000 ", buf);

    /* Too-small buffer: fail cleanly. */
    n = editor_format_line_number(0, 4, buf, 3);
    TEST_ASSERT_EQUAL_size_t(0, n);
    n = editor_format_line_number(0, 4, NULL, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(0, n);
    n = editor_format_line_number(0, 4, buf, 0);
    TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_editor_lex_batch(void)
{
    editor_syntax_run_t runs[16];
    size_t n;

    /* Whole-line comment covers the whole line from column 0. */
    n = editor_lex_batch("  rem skip this line", 20, runs, 16);
    TEST_ASSERT_EQUAL(1, (int)n);
    TEST_ASSERT_EQUAL_size_t(0, runs[0].start);
    TEST_ASSERT_EQUAL_size_t(20, runs[0].length);
    TEST_ASSERT_EQUAL(ANSI_COLOR_BRIGHT_BLACK, runs[0].color);

    /* Label run. */
    n = editor_lex_batch(":top", 4, runs, 16);
    TEST_ASSERT_EQUAL(1, (int)n);
    TEST_ASSERT_EQUAL(ANSI_COLOR_BRIGHT_YELLOW, runs[0].color);

    /* A command word is recognised at the start of the line. */
    n = editor_lex_batch("echo hello", 10, runs, 16);
    TEST_ASSERT(n >= 1);
    TEST_ASSERT_EQUAL(ANSI_COLOR_BRIGHT_GREEN, runs[0].color);

    /* Capacity bounds the run count without crashing. */
    n = editor_lex_batch("set x=\"a\" & echo %PATH%", 25, runs, 2);
    TEST_ASSERT(n <= 2);

    /* NULL safety. */
    TEST_ASSERT_EQUAL_size_t(0, editor_lex_batch(NULL, 4, runs, 16));
    TEST_ASSERT_EQUAL_size_t(0, editor_lex_batch("abc", 3, NULL, 16));
    TEST_ASSERT_EQUAL_size_t(0, editor_lex_batch("abc", 3, runs, 0));
}

void test_editor_replace_all(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);
    size_t n;

    editor_doc_insert_bytes(doc, "foo bar foo\nsecond foo", 22);
    n = editor_doc_replace_all(doc, "foo", 3, "qux", 3, true);
    TEST_ASSERT_EQUAL_size_t(3, n);
    {
        char *s0 = line_str(doc, 0);
        char *s1 = line_str(doc, 1);
        TEST_ASSERT_EQUAL_STRING("qux bar qux", s0);
        TEST_ASSERT_EQUAL_STRING("second qux", s1);
        free(s0);
        free(s1);
    }
    /* One undo restores everything (single snapshot). */
    editor_doc_undo(doc);
    {
        char *s0 = line_str(doc, 0);
        TEST_ASSERT_EQUAL_STRING("foo bar foo", s0);
        free(s0);
    }

    /* Replacement containing the needle never re-matches. */
    editor_doc_free(doc);
    doc = editor_doc_new(NULL);
    editor_doc_insert_bytes(doc, "aa", 2);
    n = editor_doc_replace_all(doc, "a", 1, "aa", 2, true);
    TEST_ASSERT_EQUAL_size_t(2, n);
    {
        char *s0 = line_str(doc, 0);
        TEST_ASSERT_EQUAL_STRING("aaaa", s0);
        free(s0);
    }

    /* Fruitless call: no snapshot, clean undo state. */
    n = editor_doc_replace_all(doc, "zzz", 3, "q", 1, true);
    TEST_ASSERT_EQUAL_size_t(0, n);

    /* NULL safety. */
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_replace_all(NULL, "a", 1, "b", 1, true));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_replace_all(doc, NULL, 0, "b", 1, true));
    editor_doc_free(doc);
}

void test_editor_comment_toggle(void)
{
    editor_doc_t *doc = editor_doc_new("T.BAT");
    size_t n;

    editor_doc_insert_bytes(doc, "echo hi\nrem old\ndir", 19);
    /* Mixed range: only the two unmarked lines change (no double-mark). */
    doc->sel_row = 0;
    doc->sel_col = 0;
    doc->cursor_row = 2;
    doc->cursor_col = 3;
    doc->selection_active = true;
    n = editor_doc_comment_toggle(doc);
    TEST_ASSERT_EQUAL_size_t(2, n);
    {
        char *s0 = line_str(doc, 0);
        char *s2 = line_str(doc, 2);
        TEST_ASSERT_EQUAL_STRING("rem echo hi", s0);
        TEST_ASSERT_EQUAL_STRING("rem dir", s2);
        free(s0);
        free(s2);
    }
    /* Toggling the same range again uncomments all three. */
    n = editor_doc_comment_toggle(doc);
    TEST_ASSERT_EQUAL_size_t(3, n);
    {
        char *s0 = line_str(doc, 0);
        char *s1 = line_str(doc, 1);
        TEST_ASSERT_EQUAL_STRING("echo hi", s0);
        TEST_ASSERT_EQUAL_STRING("old", s1);
        free(s0);
        free(s1);
    }
    /* Plain syntax is a no-op. */
    editor_doc_free(doc);
    doc = editor_doc_new("T.TXT");
    editor_doc_insert_bytes(doc, "hi", 2);
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_comment_toggle(doc));
    editor_doc_free(doc);
}

void test_editor_match_jump(void)
{
    editor_doc_t *doc = editor_doc_new("T.BAT");
    editor_doc_insert_bytes(doc, "if (a) (b)", 10);

    /* On the opener: jumps forward to its match. */
    doc->cursor_row = 0;
    doc->cursor_col = 3;
    TEST_ASSERT_TRUE(editor_doc_match_jump(doc));
    TEST_ASSERT_EQUAL_size_t(5, editor_doc_cursor_col(doc));
    /* On the closer: jumps back. */
    TEST_ASSERT_TRUE(editor_doc_match_jump(doc));
    TEST_ASSERT_EQUAL_size_t(3, editor_doc_cursor_col(doc));
    /* %var% pair on one line. */
    editor_doc_free(doc);
    doc = editor_doc_new("T.BAT");
    editor_doc_insert_bytes(doc, "echo %HOME%!", 12);
    doc->cursor_row = 0;
    doc->cursor_col = 5;
    TEST_ASSERT_TRUE(editor_doc_match_jump(doc));
    TEST_ASSERT_EQUAL_size_t(10, editor_doc_cursor_col(doc));
    /* No bracket under cursor: no move. */
    doc->cursor_col = 0;
    TEST_ASSERT_FALSE(editor_doc_match_jump(doc));
    editor_doc_free(doc);
}

void test_editor_undo_restores_clean(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    /* Mirror editor_apply_ascii: snapshot first, then mutate. */
    editor_doc_undo_mark(doc);
    editor_doc_insert_bytes(doc, "ab", 2);
    TEST_ASSERT_TRUE(editor_doc_is_modified(doc));
    /* Undo the insert: back to the pristine empty buffer, mark cleared. */
    editor_doc_undo(doc);
    TEST_ASSERT_FALSE(editor_doc_is_modified(doc));
    /* Redo re-dirties. */
    editor_doc_redo(doc);
    TEST_ASSERT_TRUE(editor_doc_is_modified(doc));
    editor_doc_free(doc);
}

void test_editor_newline_auto_indent(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    editor_doc_insert_bytes(doc, "  foo", 5);
    editor_doc_newline(doc);
    /* Split line keeps its text; the new line inherits the indent. */
    TEST_ASSERT_EQUAL_size_t(2, editor_doc_line_count(doc));
    {
        char *s0 = line_str(doc, 0);
        char *s1 = line_str(doc, 1);
        TEST_ASSERT_EQUAL_STRING("  foo", s0);
        TEST_ASSERT_EQUAL_STRING("  ", s1);
        free(s0);
        free(s1);
    }
    TEST_ASSERT_EQUAL_size_t(1, editor_doc_cursor_row(doc));
    TEST_ASSERT_EQUAL_size_t(2, editor_doc_cursor_col(doc));
    /* No indent: no padding. */
    editor_doc_free(doc);
    doc = editor_doc_new(NULL);
    editor_doc_insert_bytes(doc, "x", 1);
    editor_doc_newline(doc);
    {
        char *s1 = line_str(doc, 1);
        TEST_ASSERT_EQUAL_STRING("", s1);
        free(s1);
    }
    editor_doc_free(doc);
}

void test_editor_lex_json(void)
{
    editor_syntax_run_t runs[16];
    size_t n;

    n = editor_lex_json("{\"k\": 12, \"s\": \"v\"}", 19, runs, 16);
    TEST_ASSERT(n >= 3);
    /* The key run is cyan (runs[0] is the "{" punctuation). */
    TEST_ASSERT_EQUAL(ANSI_COLOR_BRIGHT_CYAN, runs[1].color);
    TEST_ASSERT_EQUAL_size_t(1, runs[1].start);
    /* NULL safety. */
    TEST_ASSERT_EQUAL_size_t(0, editor_lex_json(NULL, 4, runs, 16));
    TEST_ASSERT_EQUAL_size_t(0, editor_lex_json("abc", 3, NULL, 16));
    TEST_ASSERT_EQUAL_size_t(0, editor_lex_json("abc", 3, runs, 0));
}

void test_editor_lex_markdown(void)
{
    editor_syntax_run_t runs[16];
    size_t n;

    /* ATX heading: marker green, content bold white. */
    n = editor_lex_markdown("## Hi", 5, runs, 16);
    TEST_ASSERT(n >= 2);
    TEST_ASSERT_EQUAL_size_t(0, runs[0].start);
    TEST_ASSERT_EQUAL_size_t(2, runs[0].length);
    TEST_ASSERT_EQUAL(ANSI_COLOR_BRIGHT_GREEN, runs[0].color);

    /* Bold span carries the bold attr through inline scan. */
    n = editor_lex_markdown("a **b** c", 9, runs, 16);
    TEST_ASSERT(n >= 1);
    {
        bool found_bold = false;
        size_t i;
        for (i = 0; i < n; i++) {
            if ((runs[i].attrs & ANSI_ATTR_BOLD) != 0) {
                found_bold = true;
            }
        }
        TEST_ASSERT_TRUE(found_bold);
    }

    /* Unterminated code span: no run, renderer folds to default. */
    n = editor_lex_markdown("a `oops", 7, runs, 16);
    TEST_ASSERT_EQUAL_size_t(0, n);

    /* Fence line highlights whole as code. */
    n = editor_lex_markdown("```c", 4, runs, 16);
    TEST_ASSERT(n >= 1);
    TEST_ASSERT_EQUAL(ANSI_COLOR_BRIGHT_YELLOW, runs[0].color);

    /* NULL safety. */
    TEST_ASSERT_EQUAL_size_t(0, editor_lex_markdown(NULL, 4, runs, 16));
    TEST_ASSERT_EQUAL_size_t(0, editor_lex_markdown("abc", 3, NULL, 16));
    TEST_ASSERT_EQUAL_size_t(0, editor_lex_markdown("abc", 3, runs, 0));
}

void test_editor_word_count(void)
{
    editor_doc_t *doc = editor_doc_new(NULL);

    TEST_ASSERT_NOT_NULL(doc);
    editor_doc_insert_bytes(doc, "three words here", 16);
    TEST_ASSERT_EQUAL_size_t(3, editor_doc_word_count(doc));
    editor_doc_newline(doc);
    editor_doc_insert_bytes(doc, "  two   more ", 12);
    TEST_ASSERT_EQUAL_size_t(5, editor_doc_word_count(doc));
    TEST_ASSERT_EQUAL_size_t(0, editor_doc_word_count(NULL));
    editor_doc_free(doc);
}

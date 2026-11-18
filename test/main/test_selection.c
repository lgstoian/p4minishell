/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_selection.c
 * @brief Unit tests for the shared selection core (components/selection).
 *
 * Exercises the pure half: the drag-range model, the cell provider assembly
 * (per-row trailing-blank trim, '\n' join, no trailing newline), the single
 * clipboard commit sink, and the character/row helpers the modal pagers use to
 * turn a label character index into a cell and back. The LVGL highlight paints
 * are covered on target by the hardware suites (s10_input_ui).
 */

#include "unity.h"
#include "selection.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Captured commit output: the core must hand the exact text to the one sink. */
static char s_committed[512];
static int s_commit_calls;

static void test_commit_sink(const char *text)
{
    s_commit_calls++;
    snprintf(s_committed, sizeof(s_committed), "%s", (text != NULL) ? text : "");
}

/* A two-row grid provider; columns past the end of a row answer "no glyph". */
static const char *const s_grid_rows[] = { "P4SEL ALPHA", "P4SEL BETA" };

static bool test_grid_cell(void *ctx, int row, int col, char *out, size_t out_size)
{
    const char *line;
    size_t len;

    (void)ctx;
    if (row < 1 || row > 2 || col < 1 || out_size < 2) {
        return false;
    }
    line = s_grid_rows[row - 1];
    len = strlen(line);
    if ((size_t)col > len) {
        return false;
    }
    out[0] = line[col - 1];
    out[1] = '\0';
    return true;
}

/* ========================================================================
 * RANGE MODEL
 * ======================================================================== */

void test_selection_range_normalize(void)
{
    sel_range_t range;
    sel_bounds_t bounds;

    /* A backward drag (bottom-right to top-left) normalises to r0/c0 <= r1/c1. */
    sel_range_init(&range, 4, 9);
    sel_range_focus(&range, 2, 3);
    sel_range_bounds(&range, &bounds);
    TEST_ASSERT_EQUAL_INT(2, bounds.r0);
    TEST_ASSERT_EQUAL_INT(3, bounds.c0);
    TEST_ASSERT_EQUAL_INT(4, bounds.r1);
    TEST_ASSERT_EQUAL_INT(9, bounds.c1);

    /* Every cell inside is contained; outside is not. */
    TEST_ASSERT_TRUE(sel_bounds_contains(&bounds, 2, 3));
    TEST_ASSERT_TRUE(sel_bounds_contains(&bounds, 3, 6));
    TEST_ASSERT_TRUE(sel_bounds_contains(&bounds, 4, 9));
    TEST_ASSERT_FALSE(sel_bounds_contains(&bounds, 1, 5));
    TEST_ASSERT_FALSE(sel_bounds_contains(&bounds, 3, 2));
    TEST_ASSERT_FALSE(sel_bounds_contains(&bounds, 3, 10));

    /* A forward drag normalises the other way round. */
    sel_range_init(&range, 1, 1);
    sel_range_focus(&range, 3, 2);
    sel_range_bounds(&range, &bounds);
    TEST_ASSERT_EQUAL_INT(1, bounds.r0);
    TEST_ASSERT_EQUAL_INT(3, bounds.r1);
    TEST_ASSERT_EQUAL_INT(2, bounds.c1);

    /* A press without a drag is a single anchor cell. */
    sel_range_init(&range, 2, 2);
    sel_range_bounds(&range, &bounds);
    TEST_ASSERT_TRUE(sel_bounds_is_anchor(&bounds));
}

/* ========================================================================
 * CELL ASSEMBLY + COMMIT
 * ======================================================================== */

void test_selection_assembly_joins_rows(void)
{
    char out[128];
    sel_bounds_t bounds = { 1, 1, 2, 20 };
    size_t len;

    len = sel_assembly_from_cells(test_grid_cell, NULL, &bounds, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("P4SEL ALPHA\nP4SEL BETA", out);
    TEST_ASSERT_EQUAL_INT((int)strlen(out), (int)len);
}

void test_selection_assembly_trims_trailing_blanks(void)
{
    char out[128];
    sel_bounds_t bounds = { 1, 1, 1, 16 };

    /* The grid pads row 1 to 16 columns with blanks: the copy trims them, so a
     * table-style surface does not paste a wall of spaces. */
    TEST_ASSERT_EQUAL_INT(11, (int)strlen(s_grid_rows[0]));
    TEST_ASSERT_EQUAL_INT(11, (int)sel_assembly_from_cells(test_grid_cell, NULL,
                                                           &bounds, out,
                                                           sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("P4SEL ALPHA", out);
}

void test_selection_assembly_keeps_inner_blanks(void)
{
    char out[64];
    sel_bounds_t bounds = { 1, 1, 1, 8 };

    /* Columns 1..8 of "P4SEL ALPHA": the space inside the selection stays. */
    sel_assembly_from_cells(test_grid_cell, NULL, &bounds, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("P4SEL AL", out);
}

void test_selection_copy_commits_once(void)
{
    sel_bounds_t bounds = { 1, 1, 2, 20 };

    sel_set_commit(test_commit_sink);
    s_committed[0] = '\0';
    s_commit_calls = 0;

    TEST_ASSERT_TRUE(sel_copy_cells(test_grid_cell, NULL, &bounds));
    TEST_ASSERT_EQUAL_INT(1, s_commit_calls);
    TEST_ASSERT_EQUAL_STRING("P4SEL ALPHA\nP4SEL BETA", s_committed);
}

void test_selection_copy_empty_is_not_committed(void)
{
    sel_bounds_t bounds = { 1, 1, 2, 20 };
    sel_range_t range;
    sel_bounds_t empty;

    sel_set_commit(test_commit_sink);
    s_commit_calls = 0;

    /* A cell past every row assembles to nothing: no clipboard write, no
     * "copied" notice, and the caller keeps its selection. */
    sel_range_init(&range, 3, 40);
    sel_range_focus(&range, 3, 40);
    sel_range_bounds(&range, &empty);
    TEST_ASSERT_FALSE(sel_copy_cells(test_grid_cell, NULL, &empty));
    TEST_ASSERT_EQUAL_INT(0, s_commit_calls);

    /* No sink installed at all: the copy still refuses rather than pretending. */
    sel_set_commit(NULL);
    TEST_ASSERT_FALSE(sel_copy_cells(test_grid_cell, NULL, &bounds));
    TEST_ASSERT_EQUAL_INT(0, s_commit_calls);
}

/* ========================================================================
 * CONTIGUOUS TEXT SPAN (the modal pagers)
 * ======================================================================== */

void test_selection_text_span_crosses_lines(void)
{
    static const char text[] = "P4SEL-ALPHA\nP4SEL-BETA\nP4SEL-GAMMA";

    sel_set_commit(test_commit_sink);
    s_committed[0] = '\0';

    /* Characters 6..21 = "ALPHA\nP4SEL-BETA" (half-open end). */
    TEST_ASSERT_TRUE(sel_copy_text_span(text, 6, 22));
    TEST_ASSERT_EQUAL_STRING("ALPHA\nP4SEL-BETA", s_committed);
}

void test_selection_text_span_trims_line_blanks(void)
{
    static const char text[] = "alpha   \nbeta";

    sel_set_commit(test_commit_sink);

    /* A whole-line drag includes the padded columns; they are trimmed. */
    TEST_ASSERT_TRUE(sel_copy_text_span(text, 0, 13));
    TEST_ASSERT_EQUAL_STRING("alpha\nbeta", s_committed);
}

void test_selection_text_span_rejects_empty(void)
{
    static const char text[] = "abc";

    sel_set_commit(test_commit_sink);
    s_commit_calls = 0;

    TEST_ASSERT_FALSE(sel_copy_text_span(text, 2, 2));
    TEST_ASSERT_FALSE(sel_copy_text_span(NULL, 0, 1));
    TEST_ASSERT_EQUAL_INT(0, s_commit_calls);
}

/* ========================================================================
 * CHARACTER / ROW HELPERS
 * ======================================================================== */

void test_selection_row_col_roundtrip(void)
{
    static const char text[] = "alpha\nbe\n\ngamma";

    TEST_ASSERT_EQUAL_INT(4, sel_line_count(text));
    TEST_ASSERT_EQUAL_INT(0, sel_row_start_index(text, 1));
    TEST_ASSERT_EQUAL_INT(6, sel_row_start_index(text, 2));
    TEST_ASSERT_EQUAL_INT(9, sel_row_start_index(text, 3));
    TEST_ASSERT_EQUAL_INT(10, sel_row_start_index(text, 4));
    TEST_ASSERT_EQUAL_INT(-1, sel_row_start_index(text, 5));

    /* Row 2 column 2 is 'e'; a column past the row clamps to its end. */
    TEST_ASSERT_EQUAL_INT(7, sel_row_col_to_index(text, 2, 2));
    TEST_ASSERT_EQUAL_INT(8, sel_row_col_to_index(text, 2, 99));

    {
        int row = 0;
        int col = 0;

        TEST_ASSERT_TRUE(sel_index_to_row_col(text, 7, &row, &col));
        TEST_ASSERT_EQUAL_INT(2, row);
        TEST_ASSERT_EQUAL_INT(2, col);

        /* The newline at index 5 belongs to row 1 (end of line). */
        TEST_ASSERT_TRUE(sel_index_to_row_col(text, 5, &row, &col));
        TEST_ASSERT_EQUAL_INT(1, row);
        TEST_ASSERT_EQUAL_INT(6, col);

        TEST_ASSERT_FALSE(sel_index_to_row_col(text, 999, &row, &col));
    }
}

void test_selection_text_span_from_range(void)
{
    static const char text[] = "HEAD\nALPHA\nBETA\nGAMMA";
    sel_range_t range;
    int first = 0;
    int last = 0;

    TEST_ASSERT_EQUAL_INT(-1, sel_cell_order(1, 1, 2, 1));
    TEST_ASSERT_EQUAL_INT(1, sel_cell_order(3, 2, 3, 1));
    TEST_ASSERT_EQUAL_INT(0, sel_cell_order(2, 4, 2, 4));

    /* Drag from mid line 2 (col 3) down to line 4 col 2: the span is linear,
     * so it starts inside line 2 and ends inside line 4 - NOT a column
     * rectangle that would drop line 2's head. */
    sel_range_init(&range, 2, 3);
    sel_range_focus(&range, 4, 2);
    TEST_ASSERT_TRUE(sel_text_span_from_range(text, &range, &first, &last));
    TEST_ASSERT_EQUAL_INT(7, first);   /* "HEAD\nAL" behind us */
    TEST_ASSERT_EQUAL_INT(18, last);   /* through "GA" of GAMMA */
    TEST_ASSERT_TRUE(sel_text_range_contains(text, &range, 7));
    TEST_ASSERT_TRUE(sel_text_range_contains(text, &range, 17));
    TEST_ASSERT_FALSE(sel_text_range_contains(text, &range, 6));
    TEST_ASSERT_FALSE(sel_text_range_contains(text, &range, 18));

    /* A backward drag selects the same span in reading order. */
    sel_range_init(&range, 4, 2);
    sel_range_focus(&range, 2, 3);
    {
        int first2 = 0;
        int last2 = 0;

        TEST_ASSERT_TRUE(sel_text_span_from_range(text, &range, &first2,
                                                  &last2));
        TEST_ASSERT_EQUAL_INT(first, first2);
        TEST_ASSERT_EQUAL_INT(last, last2);
    }
}

void test_selection_utf8_indices(void)
{
    /* "aé" + newline + "Ωb": 2 glyphs, 3 bytes on row 1; 2 glyphs on row 2. */
    static const char text[] = "a\xC3\xA9\n\xCE\xA9z";
    int row = 0;
    int col = 0;
    char glyph[8];

    TEST_ASSERT_EQUAL_INT(5, sel_char_count(text));
    TEST_ASSERT_EQUAL_INT(3, sel_row_start_index(text, 2));

    TEST_ASSERT_TRUE(sel_index_to_row_col(text, 3, &row, &col));
    TEST_ASSERT_EQUAL_INT(2, row);
    TEST_ASSERT_EQUAL_INT(1, col);

    /* Byte offsets follow the glyph widths, not the character index. */
    TEST_ASSERT_EQUAL_INT(1, (int)sel_char_byte_offset(text, 1));
    TEST_ASSERT_EQUAL_INT(4, (int)sel_char_byte_offset(text, 3));

    /* Glyph extraction returns whole sequences. */
    TEST_ASSERT_EQUAL_INT(2, (int)sel_utf8_glyph(text + 1, glyph, sizeof(glyph)));
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9", glyph);
    TEST_ASSERT_EQUAL_INT(2, (int)sel_utf8_glyph(text + 4, glyph, sizeof(glyph)));
    TEST_ASSERT_EQUAL_STRING("\xCE\xA9", glyph);

    /* A UTF-8 span copy keeps the bytes intact. */
    sel_set_commit(test_commit_sink);
    TEST_ASSERT_TRUE(sel_copy_text_span(text, 0, 5));
    TEST_ASSERT_EQUAL_STRING("a\xC3\xA9\n\xCE\xA9z", s_committed);
}

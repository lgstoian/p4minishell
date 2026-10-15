/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tui_commands.c
 * @brief TUI and modal-surface verbs for P4MiniShell.
 *
 * `draw`/`anchor`/`browse` moved out of command.c; `dialog`/`list`/`ask`/
 * `browse_batch`/`view`/`hexview`/`color`/`locate`/`tui` moved out of the
 * batch language engine (batch.c). All render through components/tui and
 * components/modal; the single dispatcher in command.c calls these, it
 * never implements them. `appmode` stays in batch.c (batch-frame cleanup).
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "shell.h"
#include "batch.h"
#include "editor.h"
#include "filetype.h"
#include "modal_surf.h"
#include "tui.h"
#include "tui_pane.h"
#include "tui_fonts.h"
#include "windows.h"
#include "storage.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "command.h"
#include "p4minishell_config.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

/* ========================================================================
 * TUI PRIMITIVES: draw, anchor
 * ================================================================ */

/** Parse a draw color argument: 0..16 passes through as a DOS index
 * (16 = default), anything larger is 24-bit RGB quantized to DOS 0-15.
 * Keeps bare `7`-style DOS numbers working while hex like 0xFFAA00 maps
 * to the nearest cell color. */
/** Map a parsed color value to DOS: 0..16 passes through (16 = default),
 * anything larger is 24-bit RGB quantized to DOS 0-15. */
static uint8_t draw_dos_value(unsigned long v)
{
    if (v <= 16) return (uint8_t)v;
    if (v > 0xFFFFFF) v = 0xFFFFFF;
    return tui_rgb_to_dos((uint32_t)v);
}

/* Frame coalescing for batch animation loops: `draw hold on` suppresses the
 * per-verb tui_flush() so N verbs cost one LVGL label rebuild, closed by an
 * explicit `draw refresh` (which always flushes). `draw hold off` resumes
 * and flushes; `draw close` resets. Foreground only (bg jobs are refused
 * below before they can reach this). */
static bool s_draw_hold = false;

void draw_maybe_flush(void)
{
    if (!s_draw_hold) {
        tui_flush();
    }
}

/** Headless-safe accessor for unit tests (hold flag round-trip). */
bool draw_hold_active(void)
{
    return s_draw_hold;
}

/** Refuse shared-display verbs in `start` background jobs (BOUNCE-style
 * loud message + ERRORLEVEL 1, same shape as the `gfx` refusal). Modal
 * verbs already refuse in modal.c; this covers draw/tui/color/locate.
 * Shared with the `plot` coordinate layer via command.h. */
bool draw_require_foreground(const char *verb)
{
    if (batch_bg_is_background()) {
        shell_transcript_appendf_ansi(SH_ERR "%s: not available in background jobs (shared display)\n" SH_RST,
                                      verb);
        batch_set_errorlevel(1);
        return false;
    }
    return true;
}

uint8_t draw_color_arg(const char *s, uint8_t fallback)
{
    unsigned long v;
    char *end = NULL;

    if (s == NULL || *s == '\0') return fallback;
    v = strtoul(s, &end, 0);
    if (end == s) return fallback;
    return draw_dos_value(v);
}

bool shell_command_draw(int argc, char **argv)
{
    if (argc < 2) {
        shell_transcript_appendf_ansi(SH_ERR "draw: missing subcommand\n" SH_RST);
        shell_transcript_appendf_ansi("Usage: draw box|text|line|fill|clear|save|restore|cursor|bar|table|list|hold|alt-screen <args>\n");
        batch_set_errorlevel(2);
        return false;
    }

    if (!draw_require_foreground("draw")) return false;

    bool use_tui = tui_is_active();
    // Auto-enter TUI for drawing commands that benefit from it (keep header visible)
    if (!use_tui && (shell_text_equals_ignore_case(argv[1], "box") ||
                      shell_text_equals_ignore_case(argv[1], "text") ||
                      shell_text_equals_ignore_case(argv[1], "line") ||
                      shell_text_equals_ignore_case(argv[1], "fill") ||
                      shell_text_equals_ignore_case(argv[1], "bar") ||
                      shell_text_equals_ignore_case(argv[1], "table") ||
                      shell_text_equals_ignore_case(argv[1], "list") ||
                      shell_text_equals_ignore_case(argv[1], "image") ||
                      shell_text_equals_ignore_case(argv[1], "clear"))) {
        if (tui_init()) use_tui = true;
    }

    if (shell_text_equals_ignore_case(argv[1], "box")) {
        /* draw box <x> <y> <w> <h> [single|double|rounded] [fg] [bg] [title] */
        if (argc < 6) {
            shell_transcript_appendf_ansi(SH_ERR "draw box: usage: draw box <x> <y> <w> <h> [single|double|rounded] [fg] [bg] [title]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        int w = atoi(argv[4]);
        int h = atoi(argv[5]);
        const char *style = (argc > 6) ? argv[6] : "single";
        uint32_t fg = 0xFFFFFF;
        uint32_t bg = 0x000000;
        const char *title = NULL;
        if (argc > 7) {
            /* Detect if argv[7] is style or fg; if style already consumed, check numeric */
            char *end = NULL;
            strtoul(argv[7], &end, 0);
            if (end && *end == '\0' && strlen(argv[7]) > 0 && (argv[7][0] == '0' || isdigit((unsigned char)argv[7][0]))) {
                fg = strtoul(argv[7], NULL, 0);
                if (argc > 8) bg = strtoul(argv[8], NULL, 0);
                if (argc > 9) title = argv[9];
            } else if (argc > 7 && !shell_text_equals_ignore_case(argv[7], "single") && !shell_text_equals_ignore_case(argv[7], "double") && !shell_text_equals_ignore_case(argv[7], "rounded")) {
                /* argv[7] might be title if no fg/bg */
                title = argv[7];
            }
        }
        if (use_tui) {
            /* TUI cells are DOS 0-15: quantize explicit hex, keep the
             * historical 7/16 look when no colors were given. */
            uint8_t tfg = 7;
            uint8_t tbg = 16;

            if (fg != 0xFFFFFF || bg != 0x000000 || argc > 8) {
                tfg = draw_dos_value(fg);
                tbg = draw_dos_value(bg);
            }
            tui_draw_box(x, y, w, h, style, tfg, tbg, title);
            draw_maybe_flush();
            batch_set_errorlevel(0);
            return true;
        }

        const char *c_tl, *c_tr, *c_bl, *c_br, *c_h, *c_v;
        if (shell_text_equals_ignore_case(style, "double")) {
            c_tl = SH_BOX_TL2; c_tr = SH_BOX_TR2; c_bl = SH_BOX_BL2; c_br = SH_BOX_BR2;
            c_h = SH_BOX_H2; c_v = SH_BOX_V2;
        } else if (shell_text_equals_ignore_case(style, "rounded")) {
            c_tl = SH_BOX_TLR; c_tr = SH_BOX_TRR; c_bl = SH_BOX_BLR; c_br = SH_BOX_BRR;
            c_h = SH_BOX_H; c_v = SH_BOX_V;
        } else {
            c_tl = SH_BOX_TL; c_tr = SH_BOX_TR; c_bl = SH_BOX_BL; c_br = SH_BOX_BR;
            c_h = SH_BOX_H; c_v = SH_BOX_V;
        }

        /* Save cursor: \033[s */
        shell_transcript_appendf_ansi("\033[s");
        /* Set cursor: \033[%d;%dH */
        shell_transcript_appendf_ansi("\033[%d;%dH#%06X#%06X%s", y, x, fg, bg, c_tl);
        for (int i = 1; i < w - 1; i++) shell_transcript_appendf_ansi("%s", c_h);
        shell_transcript_appendf_ansi("%s\n", c_tr);
        for (int row = 1; row < h - 1; row++) {
            shell_transcript_appendf_ansi("\033[%d;%dH#%06X#%06X%s", y + row, x, fg, bg, c_v);
            shell_transcript_appendf_ansi("\033[%d;%dH%s\n", y + row, x + w - 1, c_v);
        }
        shell_transcript_appendf_ansi("\033[%d;%dH#%06X#%06X%s", y + h - 1, x, fg, bg, c_bl);
        for (int i = 1; i < w - 1; i++) shell_transcript_appendf_ansi("%s", c_h);
        shell_transcript_appendf_ansi("%s\n", c_br);
        /* Restore cursor: \033[u */
        shell_transcript_appendf_ansi("\033[u");

        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "text")) {
        /* draw text <x> <y> "text" [fg] [bg] */
        if (argc < 5) {
            shell_transcript_appendf_ansi(SH_ERR "draw text: usage: draw text <x> <y> \"text\" [fg] [bg]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        const char *text = argv[4];
        uint32_t fg = 0xFFFFFF;
        uint32_t bg = 0x000000;
        if (argc > 5) fg = strtoul(argv[5], NULL, 0);
        if (argc > 6) bg = strtoul(argv[6], NULL, 0);
        if (use_tui) {
            tui_print_at(x, y, text,
                         (argc > 5) ? draw_color_arg(argv[5], 7) : 7,
                         (argc > 6) ? draw_color_arg(argv[6], 16) : 16);
            draw_maybe_flush();
            batch_set_errorlevel(0);
            return true;
        }

        shell_transcript_appendf_ansi("\033[s");
        shell_transcript_appendf_ansi("\033[%d;%dH#%06X#%06X%s", y, x, fg, bg, text);
        shell_transcript_appendf_ansi("\033[u");

        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "line")) {
        /* draw line <x1> <y1> <x2> <y2> [fg] [bg] */
        if (argc < 6) {
            shell_transcript_appendf_ansi(SH_ERR "draw line: usage: draw line <x1> <y1> <x2> <y2> [fg] [bg]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        int x1 = atoi(argv[2]);
        int y1 = atoi(argv[3]);
        int x2 = atoi(argv[4]);
        int y2 = atoi(argv[5]);
        uint32_t fg = 0xFFFFFF;
        uint32_t bg = 0x000000;
        if (argc > 6) fg = strtoul(argv[6], NULL, 0);
        if (argc > 7) bg = strtoul(argv[7], NULL, 0);
        if (use_tui) {
            tui_draw_line(x1, y1, x2, y2, "single",
                          (argc > 6) ? draw_color_arg(argv[6], 7) : 7,
                          (argc > 7) ? draw_color_arg(argv[7], 16) : 16);
            draw_maybe_flush();
            batch_set_errorlevel(0);
            return true;
        }

        /* Simple horizontal/vertical lines only for now */
        if (y1 == y2) {
            shell_transcript_appendf_ansi("\033[s");
            shell_transcript_appendf_ansi("\033[%d;%dH#%06X#%06X", y1, x1, fg, bg);
            for (int i = x1; i <= x2; i++) shell_transcript_appendf_ansi("%s", SH_BOX_H);
            shell_transcript_appendf_ansi("\n");
            shell_transcript_appendf_ansi("\033[u");
        } else if (x1 == x2) {
            shell_transcript_appendf_ansi("\033[s");
            for (int row = y1; row <= y2; row++) {
                shell_transcript_appendf_ansi("\033[%d;%dH#%06X#%06X%s\n", row, x1, fg, bg, SH_BOX_V);
            }
            shell_transcript_appendf_ansi("\033[u");
        } else {
            shell_transcript_appendf_ansi(SH_ERR "draw line: only horizontal/vertical supported\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }

        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "fill")) {
        /* draw fill <x> <y> <w> <h> <char> [fg] [bg] */
        if (argc < 7) {
            shell_transcript_appendf_ansi(SH_ERR "draw fill: usage: draw fill <x> <y> <w> <h> <char> [fg] [bg]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        int w = atoi(argv[4]);
        int h = atoi(argv[5]);
        char fill_char = argv[6][0];
        uint32_t fg = 0xFFFFFF;
        uint32_t bg = 0x000000;
        if (argc > 7) fg = strtoul(argv[7], NULL, 0);
        if (argc > 8) bg = strtoul(argv[8], NULL, 0);
        if (use_tui) {
            tui_fill(x, y, w, h, fill_char,
                     (argc > 7) ? draw_color_arg(argv[7], 7) : 7,
                     (argc > 8) ? draw_color_arg(argv[8], 16) : 16);
            draw_maybe_flush();
            batch_set_errorlevel(0);
            return true;
        }

        shell_transcript_appendf_ansi("\033[s");
        for (int row = 0; row < h; row++) {
            shell_transcript_appendf_ansi("\033[%d;%dH#%06X#%06X", y + row, x, fg, bg);
            for (int col = 0; col < w; col++) shell_transcript_appendf_ansi("%c", fill_char);
            shell_transcript_appendf_ansi("\n");
        }
        shell_transcript_appendf_ansi("\033[u");

        batch_set_errorlevel(0);
        return true;
    }

    /* Max table columns (widths[] stack budget on the worker). */
#define DRAW_TABLE_MAX_COLS 16
#define DRAW_TABLE_MAX_ROWS 32
#define DRAW_TABLE_ROW_BYTES 256
#define DRAW_TABLE_COL_MAX 40

    if (shell_text_equals_ignore_case(argv[1], "bar")) {
        /* draw bar <x> <y> <w> <pct> [fillch] [emptych] [fg] [bg] */
        if (argc < 6) {
            shell_transcript_appendf_ansi(SH_ERR "draw bar: usage: draw bar <x> <y> <w> <pct 0..100> [fillch] [emptych] [fg] [bg]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        int w = atoi(argv[4]);
        int pct = atoi(argv[5]);
        char fillch = (argc > 6 && argv[6][0] != '\0') ? argv[6][0] : '#';
        char emptych = (argc > 7 && argv[7][0] != '\0') ? argv[7][0] : '-';
        uint8_t tfg = (argc > 8) ? draw_color_arg(argv[8], 7) : 7;
        uint8_t tbg = (argc > 9) ? draw_color_arg(argv[9], 16) : 16;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (w < 3) w = 3;
        if (use_tui) {
            tui_draw_bar(x, y, w, pct, fillch, emptych, tfg, tbg);
            draw_maybe_flush();
            batch_set_errorlevel(0);
            return true;
        }

        {
            uint32_t fg = 0xFFFFFF;
            uint32_t bg = 0x000000;
            int inner = w - 2;
            int filled = (inner * pct + 50) / 100;
            int i;

            if (argc > 8) fg = strtoul(argv[8], NULL, 0);
            if (argc > 9) bg = strtoul(argv[9], NULL, 0);
            shell_transcript_appendf_ansi("\033[s");
            shell_transcript_appendf_ansi("\033[%d;%dH#%06X#%06X[", y, x,
                                          (unsigned int)fg, (unsigned int)bg);
            for (i = 0; i < inner; i++) {
                shell_transcript_appendf_ansi("%c", (i < filled) ? fillch : emptych);
            }
            shell_transcript_appendf_ansi("]\n");
            shell_transcript_appendf_ansi("\033[u");
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "table")) {
        /* draw table <x> <y> <fg> <bg> <"h1|h2|..."> [row "c1|c2|..." ...]
         *   [/cursor:N] [/sel:a,b,...]
         * First quoted arg is the header (bold on TUI). Column widths are
         * the max cell length + padding, capped so the table fits 80 cols.
         * Trailing /cursor:N (1-based data row, header excluded) renders a
         * bright-white bold cursor row; /sel:a,b renders bold selected
         * rows. A row that looks like a flag is consumed as a flag. */
        if (argc < 6) {
            shell_transcript_appendf_ansi(SH_ERR "draw table: usage: draw table <x> <y> <fg> <bg> \"h1|h2|...\" [row \"c1|c2|...\" ...] [/cursor:N] [/sel:a,b,...]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        uint8_t tfg = draw_color_arg(argv[4], 7);
        uint8_t tbg = draw_color_arg(argv[5], 16);
        /* Row storage is heap (PSRAM): a bg job and the main worker can
         * draw tables concurrently, so neither static nor worker stack. */
        char (*rows)[DRAW_TABLE_ROW_BYTES] = NULL;
        const char **cells = NULL;
        int widths[DRAW_TABLE_MAX_COLS];
        int ncols = 0;
        int nrows = 0;
        int r;
        int c;
        /* Trailing flags partitioned out before row processing. */
        const char *row_args[DRAW_TABLE_MAX_ROWS + 4];
        const char *cursor_arg = NULL;
        const char *sel_arg = NULL;
        int cursor = 0;
        uint32_t sel = 0u;
        int ai;

        for (ai = 6; ai < argc; ai++) {
            if (strncasecmp(argv[ai], "/cursor:", 8) == 0) {
                cursor_arg = argv[ai] + 8;
            } else if (strncasecmp(argv[ai], "/sel:", 5) == 0) {
                sel_arg = argv[ai] + 5;
            } else if (nrows < (int)(sizeof(row_args) / sizeof(row_args[0]))) {
                row_args[nrows++] = argv[ai];
            }
        }
        if (nrows < 1) {
            shell_transcript_appendf_ansi(SH_ERR "draw table: usage: draw table <x> <y> <fg> <bg> \"h1|h2|...\" [row \"c1|c2|...\" ...] [/cursor:N] [/sel:a,b,...]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (cursor_arg != NULL) {
            cursor = tui_table_parse_cursor(cursor_arg, nrows - 1);
        }
        if (sel_arg != NULL) {
            sel = tui_table_parse_sel(sel_arg, nrows - 1);
        }

        /* Count header columns first (no mutation yet). */
        {
            const char *p = row_args[0];

            ncols = 1;
            for (; *p != '\0'; p++) {
                if (*p == '|') ncols++;
            }
        }
        if (ncols > DRAW_TABLE_MAX_COLS) {
            shell_transcript_appendf_ansi(SH_ERR "draw table: too many columns (max %d)\n" SH_RST,
                                          DRAW_TABLE_MAX_COLS);
            batch_set_errorlevel(2);
            return false;
        }
        if (nrows > DRAW_TABLE_MAX_ROWS) nrows = DRAW_TABLE_MAX_ROWS;
        rows = malloc((size_t)nrows * DRAW_TABLE_ROW_BYTES);
        cells = malloc((size_t)nrows * (size_t)ncols * sizeof(*cells));
        if (rows == NULL || cells == NULL) {
            free(rows);
            free(cells);
            shell_transcript_appendf_ansi(SH_ERR "draw table: out of memory\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        for (c = 0; c < ncols; c++) widths[c] = 1;
        for (r = 0; r < nrows; r++) {
            char *dst = rows[r];
            const char *src = row_args[r];
            size_t n = strlen(src);

            if (n > DRAW_TABLE_ROW_BYTES - 1) n = DRAW_TABLE_ROW_BYTES - 1;
            memcpy(dst, src, n);
            dst[n] = '\0';
            /* Split on '|' but keep the tail glued to the last column when
             * a row has more cells than the header. */
            c = 0;
            cells[r * ncols + c] = dst;
            for (char *p = dst; *p != '\0'; p++) {
                if (*p == '|' && c < ncols - 1) {
                    *p = '\0';
                    c++;
                    cells[r * ncols + c] = p + 1;
                }
            }
            /* Pad short rows with empty cells; measure widths. c is the
             * last filled index, so padding starts one past it (a full
             * row pads nothing). */
            for (c++; c < ncols; c++) {
                cells[r * ncols + c] = "";
            }
            for (c = 0; c < ncols; c++) {
                int len = (int)strlen(cells[r * ncols + c]);

                if (len > widths[c]) widths[c] = len;
            }
        }
        for (c = 0; c < ncols; c++) {
            if (widths[c] > DRAW_TABLE_COL_MAX) widths[c] = DRAW_TABLE_COL_MAX;
        }
        if (tui_table_total_width(ncols, widths) > P4_CONFIG_TUI_COLS) {
            free(rows);
            free(cells);
            shell_transcript_appendf_ansi(SH_ERR "draw table: too wide (max %d columns of grid)\n" SH_RST,
                                          P4_CONFIG_TUI_COLS);
            batch_set_errorlevel(2);
            return false;
        }
        if (use_tui) {
            tui_draw_table_ex(x, y, ncols, widths, nrows, cells, true, tfg, tbg,
                              cursor, sel);
            draw_maybe_flush();
            free(rows);
            free(cells);
            batch_set_errorlevel(0);
            return true;
        }

        /* Off-TUI: plain bordered block, x-space indent (y ignored). */
        {
            uint32_t fg = strtoul(argv[4], NULL, 0);
            uint32_t bg = strtoul(argv[5], NULL, 0);
            char line[300];
            int li;
            int i;
            int k;
/* Append guarded: never let li run past the buffer. */
#define TAPP(...) do { \
        if (li < (int)sizeof(line) - 12) { \
            li += snprintf(line + li, sizeof(line) - (size_t)li, __VA_ARGS__); \
        } \
    } while (0)

            /* Top border */
            li = 0;
            for (i = 0; i < x - 1 && li < (int)sizeof(line) - 12; i++) line[li++] = ' ';
            TAPP("#%06X#%06X%s", (unsigned int)fg, (unsigned int)bg, SH_BOX_TL);
            for (c = 0; c < ncols; c++) {
                for (k = 0; k < widths[c] + 2; k++) TAPP("%s", SH_BOX_H);
                TAPP("%s", (c == ncols - 1) ? SH_BOX_TR : SH_BOX_T);
            }
            shell_transcript_appendf_ansi("%s\n", line);
            for (r = 0; r < nrows; r++) {
                bool last = (r == nrows - 1);
                /* Off-TUI cursor/selection markers (TUI renders these as
                 * bright/bold rows instead). */
                char mark = ' ';

                if (cursor >= 1 && r == cursor) {
                    mark = '>';
                } else if (r >= 1 && r <= 32 && (sel & (1u << (r - 1))) != 0u) {
                    mark = '*';
                }

                li = 0;
                for (i = 0; i < x - 1 && li < (int)sizeof(line) - 12; i++) line[li++] = ' ';
                if (li < (int)sizeof(line) - 12) line[li++] = mark;
                TAPP("#%06X#%06X%s", (unsigned int)fg, (unsigned int)bg, SH_BOX_V);
                for (c = 0; c < ncols; c++) {
                    TAPP(" %-*.*s %s", widths[c], widths[c],
                         cells[r * ncols + c], SH_BOX_V);
                }
                shell_transcript_appendf_ansi("%s\n", line);
                li = 0;
                for (i = 0; i < x - 1 && li < (int)sizeof(line) - 12; i++) line[li++] = ' ';
                TAPP("#%06X#%06X%s", (unsigned int)fg, (unsigned int)bg,
                     last ? SH_BOX_BL : SH_BOX_L);
                for (c = 0; c < ncols; c++) {
                    for (k = 0; k < widths[c] + 2; k++) TAPP("%s", SH_BOX_H);
                    TAPP("%s", last ? ((c == ncols - 1) ? SH_BOX_BR : SH_BOX_B)
                                    : ((c == ncols - 1) ? SH_BOX_R : SH_BOX_X));
                }
                shell_transcript_appendf_ansi("%s\n", line);
            }
#undef TAPP
        }
        free(rows);
        free(cells);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        /* draw list <x> <y> <w> <h> <file> [fg] [bg] [/top:N] [/cursor:N]
         *   [/sel:a,b,...]
         * Renders a file's lines as a bordered, selectable panel â€” the file
         * manager primitive (the batch `dir /b > file` + `draw list` pair
         * gives a real column of names). /top is the 1-based first line
         * shown (scroll), /cursor the 1-based highlighted line (bright), and
         * /sel marks lines (1-based) with a `*`. Batch has no arrays or
         * delayed expansion, so this keeps the list state in the verb. */
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        FILE *f = NULL;
        uint8_t lfg = 7;
        uint8_t lbg = 16;
        int x;
        int y;
        int w;
        int h;
        int top = 1;
        int cursor = 0;
        uint32_t sel = 0u;
        const char *title = NULL;
        const char *count_var = NULL;
        bool count_only = false;
        char (*lines)[96] = NULL;
        int nlines = 0;
        int start;
        int shown;
        int i;
        int ai;

        if (argc < 7) {
            shell_transcript_appendf_ansi(SH_ERR "draw list: usage: draw list <x> <y> <w> <h> <file> [fg] [bg] [/top:N] [/cursor:N] [/sel:a,b,...] [/title:T] [/count:NAME] [/countonly]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        x = atoi(argv[2]);
        y = atoi(argv[3]);
        w = atoi(argv[4]);
        h = atoi(argv[5]);
        /* Optional positional fg/bg right after <file> (non-flag args). */
        ai = 7;
        if (ai < argc && argv[ai][0] != '/') {
            lfg = draw_color_arg(argv[ai], 7);
            ai++;
        }
        if (ai < argc && argv[ai][0] != '/') {
            lbg = draw_color_arg(argv[ai], 16);
            ai++;
        }
        for (; ai < argc; ai++) {
            if (strncasecmp(argv[ai], "/top:", 5) == 0) {
                top = atoi(argv[ai] + 5);
            } else if (strncasecmp(argv[ai], "/cursor:", 8) == 0) {
                cursor = atoi(argv[ai] + 8);
            } else if (strncasecmp(argv[ai], "/sel:", 5) == 0) {
                sel = tui_table_parse_sel(argv[ai] + 5, 256);
            } else if (strncasecmp(argv[ai], "/title:", 7) == 0) {
                title = argv[ai] + 7;
            } else if (strncasecmp(argv[ai], "/count:", 7) == 0) {
                count_var = argv[ai] + 7;
            } else if (strcasecmp(argv[ai], "/countonly") == 0) {
                count_only = true;
            }
        }
        if (w < 4) w = 4;
        if (h < 3) h = 3;
        if (top < 1) top = 1;

        if (shell_fs_resolve_path(argv[6], resolved, sizeof(resolved)) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "draw list: invalid path %s\n" SH_RST, argv[6]);
            batch_set_errorlevel(2);
            return false;
        }
        f = fopen(resolved, "rb");
        if (f == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "draw list: cannot open %s\n" SH_RST, resolved);
            batch_set_errorlevel(1);
            return false;
        }
        lines = malloc((size_t)256 * sizeof(*lines));
        if (lines == NULL) {
            fclose(f);
            shell_transcript_appendf_ansi(SH_ERR "draw list: out of memory\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        while (nlines < 256 && fgets(lines[nlines], sizeof(lines[nlines]), f) != NULL) {
            size_t len = strlen(lines[nlines]);

            while (len > 0 && (lines[nlines][len - 1] == '\n' ||
                               lines[nlines][len - 1] == '\r')) {
                lines[nlines][--len] = '\0';
            }
            nlines++;
        }
        fclose(f);

        /* `/count:NAME` publishes the line count to the batch environment;
         * batch has no non-flooding counter loop, so the verb reports it
         * (the file-manager clamp/page math reads it back). */
        if (count_var != NULL && count_var[0] != '\0') {
            char nbuf[16];

            snprintf(nbuf, sizeof(nbuf), "%d", nlines);
            (void)shell_env_set(count_var, nbuf);
        }
        if (count_only) {
            free(lines);
            batch_set_errorlevel(0);
            return true;
        }

        if (!use_tui) {
            /* Off-TUI fallback: plain lines with `>`/`*` markers. */
            for (i = top - 1; i < nlines && i < top - 1 + (h - 2); i++) {
                char mark = (cursor == i + 1) ? '>' :
                            ((i < 32 && (sel & (1u << i))) ? '*' : ' ');
                shell_transcript_appendf("%c%s\n", mark, lines[i]);
            }
            free(lines);
            batch_set_errorlevel(0);
            return true;
        }

        tui_draw_box(x, y, w, h, "single", lfg, lbg, title);
        start = top - 1;
        shown = h - 2;
        for (i = 0; i < shown; i++) {
            int li = start + i;
            char txt[128];
            uint8_t rfg = (cursor == li + 1) ? 15 : lfg;
            const char *name;

            if (li >= nlines) break;
            name = lines[li];
            if (li < 32 && (sel & (1u << li)) != 0u) {
                snprintf(txt, sizeof(txt), "*%s", name);
            } else if (cursor == li + 1) {
                snprintf(txt, sizeof(txt), ">%s", name);
            } else {
                snprintf(txt, sizeof(txt), " %s", name);
            }
            txt[sizeof(txt) - 1] = '\0';
            /* Hard clamp to the inner width so the panel border survives. */
            if ((int)strlen(txt) > w - 3) {
                txt[w - 3] = '\0';
            }
            tui_print_at(x + 1, y + 1 + i, txt, rfg, lbg);
        }
        free(lines);
        draw_maybe_flush();
        batch_set_errorlevel(nlines > 0 ? 0 : 1);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        /* draw clear [screen|line|eol|eos] */
        const char *what = (argc > 2) ? argv[2] : "screen";
        if (use_tui) {
            if (shell_text_equals_ignore_case(what, "screen")) tui_clear();
            else if (shell_text_equals_ignore_case(what, "line")) tui_clear_line(2);
            else if (shell_text_equals_ignore_case(what, "eol")) tui_clear_line(0);
            else if (shell_text_equals_ignore_case(what, "eos")) tui_clear();
            else {
                shell_transcript_appendf_ansi(SH_ERR "draw clear: unknown target %s\n" SH_RST, what);
                batch_set_errorlevel(2);
                return false;
            }
            draw_maybe_flush();
            batch_set_errorlevel(0);
            return true;
        }
        if (shell_text_equals_ignore_case(what, "screen")) {
            shell_transcript_appendf_ansi("\033[2J"); /* entire screen */
        } else if (shell_text_equals_ignore_case(what, "line")) {
            shell_transcript_appendf_ansi("\033[2K"); /* entire line */
        } else if (shell_text_equals_ignore_case(what, "eol")) {
            shell_transcript_appendf_ansi("\033[0K"); /* cursor to end of line */
        } else if (shell_text_equals_ignore_case(what, "eos")) {
            shell_transcript_appendf_ansi("\033[0J"); /* cursor to end of screen */
        } else {
            shell_transcript_appendf_ansi(SH_ERR "draw clear: unknown target %s\n" SH_RST, what);
            batch_set_errorlevel(2);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "save")) {
        if (use_tui) { tui_save_cursor(); batch_set_errorlevel(0); return true; }
        shell_transcript_appendf_ansi("\033[s");
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "restore")) {
        if (use_tui) { tui_restore_cursor(); batch_set_errorlevel(0); return true; }
        shell_transcript_appendf_ansi("\033[u");
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "cursor")) {
        /* draw cursor <on|off> */
        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "draw cursor: usage: draw cursor <on|off>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (use_tui) {
            if (shell_text_equals_ignore_case(argv[2], "on")) tui_set_cursor_visible(true);
            else if (shell_text_equals_ignore_case(argv[2], "off")) tui_set_cursor_visible(false);
            else {
                shell_transcript_appendf_ansi(SH_ERR "draw cursor: unknown state %s\n" SH_RST, argv[2]);
                batch_set_errorlevel(2);
                return false;
            }
            batch_set_errorlevel(0);
            return true;
        }
        if (shell_text_equals_ignore_case(argv[2], "on")) {
            shell_transcript_appendf_ansi("\033[?25h");
        } else if (shell_text_equals_ignore_case(argv[2], "off")) {
            shell_transcript_appendf_ansi("\033[?25l");
        } else {
            shell_transcript_appendf_ansi(SH_ERR "draw cursor: unknown state %s\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "alt-screen")) {
        /* draw alt-screen <on|off> */
        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "draw alt-screen: usage: draw alt-screen <on|off>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (use_tui) {
            if (shell_text_equals_ignore_case(argv[2], "on")) tui_alt_enter();
            else if (shell_text_equals_ignore_case(argv[2], "off")) tui_alt_leave();
            else {
                shell_transcript_appendf_ansi(SH_ERR "draw alt-screen: unknown state %s\n" SH_RST, argv[2]);
                batch_set_errorlevel(2);
                return false;
            }
            batch_set_errorlevel(0);
            return true;
        }
        if (shell_text_equals_ignore_case(argv[2], "on")) {
            shell_transcript_appendf_ansi("\033[?1049h");
        } else if (shell_text_equals_ignore_case(argv[2], "off")) {
            shell_transcript_appendf_ansi("\033[?1049l");
        } else {
            shell_transcript_appendf_ansi(SH_ERR "draw alt-screen: unknown state %s\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "window")) {
        /* draw window <id> <x> <y> <w> <h> [title] â€” alias to box with id ignored */
        if (argc < 7) {
            shell_transcript_appendf_ansi(SH_ERR "draw window: usage: draw window <id> <x> <y> <w> <h> [title]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        int x = atoi(argv[3]);
        int y = atoi(argv[4]);
        int w = atoi(argv[5]);
        int h = atoi(argv[6]);
        const char *title = (argc > 7) ? argv[7] : NULL;
        if (use_tui) {
            if (!tui_is_active() && !tui_init()) {
                shell_transcript_appendf_ansi(SH_ERR "draw window: tui init failed\n" SH_RST);
                batch_set_errorlevel(1);
                return false;
            }
            tui_draw_box(x, y, w, h, "single", 7, 16, title);
            draw_maybe_flush();
            batch_set_errorlevel(0);
            return true;
        }
        shell_transcript_appendf_ansi(SH_ERR "draw window: tui not active\n" SH_RST);
        batch_set_errorlevel(1);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "close")) {
        s_draw_hold = false; /* never leak a held frame into interactive use */
        if (use_tui) { tui_deinit(); batch_set_errorlevel(0); return true; }
        shell_transcript_appendf_ansi(SH_ERR "draw close: tui not active\n" SH_RST);
        batch_set_errorlevel(1);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "hold")) {
        /* draw hold <on|off>: coalesce a frame's verbs into one flush. */
        if (argc != 3 ||
            (!shell_text_equals_ignore_case(argv[2], "on") &&
             !shell_text_equals_ignore_case(argv[2], "off"))) {
            shell_transcript_appendf_ansi(SH_ERR "draw hold: usage: draw hold <on|off>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        s_draw_hold = shell_text_equals_ignore_case(argv[2], "on");
        if (use_tui && !s_draw_hold) {
            tui_flush();
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "fullscreen")) {
        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "draw fullscreen: usage: draw fullscreen <on|off>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (shell_text_equals_ignore_case(argv[2], "on")) {
            tui_enter_fullscreen();
            batch_set_errorlevel(0);
            return true;
        } else if (shell_text_equals_ignore_case(argv[2], "off")) {
            tui_exit_fullscreen();
            batch_set_errorlevel(0);
            return true;
        } else {
            shell_transcript_appendf_ansi(SH_ERR "draw fullscreen: unknown state %s\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return false;
        }
    }

    if (shell_text_equals_ignore_case(argv[1], "refresh")) {
        if (use_tui) { tui_flush(); batch_set_errorlevel(0); return true; }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "flip")) {
        /* Back-buffer swap: flush the current frame and re-render the
         * workspace layer (panes + widgets + HUD) on top. This is the
         * compositing entry point for a retained-mode frame. */
        if (use_tui) {
            tui_flush();
            tui_workspace_render();
            tui_flush();
            batch_set_errorlevel(0);
            return true;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "image")) {
        /* draw image <file.bmp> <x> <y> <w> <h>  (cells, 1-based): render a BMP
         * into the cell grid as nearest-DOS-color blocks. Reuses the single BMP
         * decoder in components/gfx. */
        uint8_t *buf = NULL;
        size_t got = 0;
        gfx_bmp_info_t info;
        gfx_surface_t img = {NULL, 0, 0};
        int x;
        int y;
        int w;
        int h;
        int rc;

        if (argc != 7) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: usage: draw image <file.bmp> <x> <y> <w> <h>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (!use_tui) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: tui init failed\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        x = atoi(argv[3]);
        y = atoi(argv[4]);
        w = atoi(argv[5]);
        h = atoi(argv[6]);
        if (w < 1 || h < 1) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: bad cell size %dx%d\n" SH_RST, w, h);
            batch_set_errorlevel(2);
            return false;
        }
        rc = command_load_file_psram(argv[2], "draw image",
                                     P4_CONFIG_IMAGE_MAX_BYTES, &buf, &got);
        if (rc != 0) {
            batch_set_errorlevel(rc);
            return false;
        }
        if (!gfx_bmp_parse_header_ex(buf, got, &info, GFX_IMAGE_MAX_W, GFX_IMAGE_MAX_H)) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: not a 24/32-bit BI_RGB BMP (%s)\n" SH_RST, argv[2]);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        if (!gfx_bmp_decode_scaled_565(buf, got, &info, w, h, &img)) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: decode failed (%s)\n" SH_RST, argv[2]);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        heap_caps_free(buf);
        tui_draw_image(x, y, w, h, &img);
        gfx_surface_free(&img);
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    shell_transcript_appendf_ansi(SH_ERR "draw: unknown subcommand %s\n" SH_RST, argv[1]);
    batch_set_errorlevel(2);
    return false;
}

bool shell_command_anchor(int argc, char **argv)
{
    /* anchor <label> <command> [continue_line] */
    if (argc < 3) {
        shell_transcript_appendf_ansi(SH_ERR "anchor: usage: anchor <label> <command> [continue_line]\n" SH_RST);
        batch_set_errorlevel(2);
        return false;
    }

    if (!draw_require_foreground("anchor")) return false;

    const char *label = argv[1];
    const char *command = argv[2];
    bool continue_line = false;
    if (argc > 3) {
        continue_line = shell_text_equals_ignore_case(argv[3], "true") || atoi(argv[3]) != 0;
    }

    shell_transcript_add_anchor_region(label, command, continue_line);

    shell_transcript_appendf_ansi(SH_OK "anchor registered: %s -> %s\n" SH_RST, label, command);
    batch_set_errorlevel(0);
    return true;
}

bool shell_command_link(int argc, char **argv)
{
    /* link <label> <command...> â€” emit a tap-confirm hyperlink (batch apps
     * use this to publish clickable actions); link list; link open <n>. */
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "list")) {
        int count = shell_link_count();
        int i;

        for (i = 0; i < count; i++) {
            const char *label = NULL;
            const char *command = NULL;

            if (shell_link_get(i, &label, &command) && label != NULL &&
                command != NULL) {
                shell_transcript_appendf_ansi(SH_NUM "%d" SH_RST ": " SH_LINK "%s" SH_RST
                                              SH_MUTE " -> %s\n" SH_RST,
                                              i, label, command);
            }
        }
        if (count == 0) {
            shell_transcript_appendf_ansi(SH_MUTE "link: no links (try `help`)\n" SH_RST);
        }
        batch_set_errorlevel(0);
        return true;
    }
    if (argc >= 3 && shell_text_equals_ignore_case(argv[1], "open")) {
        char *end = NULL;
        long n = strtol(argv[2], &end, 10);
        const char *command = NULL;

        if (end == NULL || *end != '\0' || n < 0 ||
            !shell_link_get((int)n, NULL, &command) || command == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "link: no link %s (see `link list`)\n" SH_RST,
                                          argv[2]);
            batch_set_errorlevel(1);
            return true;
        }
        {
            char *copy = malloc(P4_CONFIG_COMMAND_BYTES);

            if (copy == NULL) {
                shell_transcript_appendf_ansi(SH_ERR "link: out of memory\n" SH_RST);
                batch_set_errorlevel(1);
                return true;
            }
            snprintf(copy, P4_CONFIG_COMMAND_BYTES, "%s", command);
            shell_link_clear_pending();
            shell_transcript_appendf("%s%s\n", shell_prompt_render_plain(), copy);
            shell_execute_command(copy);
            free(copy);
        }
        return true;
    }
    if (argc >= 3) {
        /* Emit form: join the tail so `link Docs help /all` works unquoted. */
        char *command = malloc(P4_CONFIG_COMMAND_BYTES);
        size_t used = 0;
        int i;

        if (!draw_require_foreground("link")) {
            free(command);
            return false;
        }
        if (command == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "link: out of memory\n" SH_RST);
            batch_set_errorlevel(1);
            return true;
        }
        command[0] = '\0';
        for (i = 2; i < argc; i++) {
            size_t piece = strlen(argv[i]);
            size_t need = used + piece + ((i > 2) ? 1u : 0u) + 1u;

            if (need > (size_t)P4_CONFIG_COMMAND_BYTES) {
                break;
            }
            if (i > 2) {
                command[used++] = ' ';
            }
            memcpy(command + used, argv[i], piece);
            used += piece;
            command[used] = '\0';
        }
        if (command[0] == '\0') {
            shell_transcript_appendf_ansi(SH_ERR "link: empty command\n" SH_RST);
            batch_set_errorlevel(2);
            free(command);
            return true;
        }
        shell_transcript_append_link(argv[1], command);
        shell_transcript_appendf_ansi("\n");
        free(command);
        batch_set_errorlevel(0);
        return true;
    }
    shell_transcript_appendf_ansi(SH_ERR "link: usage: link <label> <command...> | link list | link open <n>\n" SH_RST);
    batch_set_errorlevel(2);
    return true;
}

void shell_command_browse(int argc, char **argv)
{
    const char *varname = "BROWSE_RESULT";
    const char *start_path = NULL;
    uint32_t timeout_ms = 0;
    char selected_path[P4_CONFIG_TUI_BROWSE_PATH_BYTES];
    int result;
    int i;

    /* Parse the shared modal options here too: the dispatcher previously sent
     * only `/v:` commands to the batch variant, so `browse /t:30 sd:/APPS`
     * consumed `/t:30` as the start path (empty listing, no timeout). */
    for (i = 1; i < argc; i++) {
        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        } else if (modal_parse_var_arg(argv[i], &varname)) {
            continue;
        } else if (start_path == NULL) {
            start_path = argv[i];
        } else {
            shell_print_usage("Usage: browse [/t:secs] [/v:NAME] [path]");
            batch_set_errorlevel(2);
            return;
        }
    }

    result = modal_filebrowser_run("Browse", start_path, selected_path,
                                   sizeof(selected_path), timeout_ms);

    if (result != 0) {
        shell_env_set(varname, "");
        shell_print_muted("browse: cancelled");
        batch_set_errorlevel(1);
        return;
    }
    if (shell_env_set(varname, selected_path) != ESP_OK) {
        shell_print_error("browse: cannot set %s", varname);
        batch_set_errorlevel(1);
        return;
    }
    shell_print_ok("selected: %s", selected_path);
    batch_set_errorlevel(0);
}

/* ========================================================================
 * NATIVE MODAL SURFACES (dialog, list, ask)
 * ========================================================================
 * Batch apps can launch small native modal surfaces when the transcript UI
 * is not polished enough. These commands block the batch worker until the
 * user dismisses the surface and return the choice through ERRORLEVEL (and
 * ASK_RESULT for text input).
 */

void shell_command_dialog(int argc, char **argv)
{
    const char *title = NULL;
    const char *message = NULL;
    const char *button1 = NULL;
    const char *button2 = NULL;
    uint32_t timeout_ms = 0;
    int result;
    int i;

    for (i = 1; i < argc; i++) {
        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        } else if (title == NULL) {
            title = argv[i];
        } else if (message == NULL) {
            message = argv[i];
        } else if (button1 == NULL) {
            button1 = argv[i];
        } else if (button2 == NULL) {
            button2 = argv[i];
        }
    }

    if (title == NULL || message == NULL) {
        shell_print_usage("Usage: dialog [/t:secs] \"title\" \"message\" [button1] [button2]");
        batch_set_errorlevel(2);
        return;
    }

    result = modal_dialog_run(title, message, button1, button2, timeout_ms);
    batch_set_errorlevel(result < 0 ? 255 : result);
}

void shell_command_list(int argc, char **argv)
{
    const char *varname = NULL;
    const char *title = NULL;
    const char **items = NULL;
    uint32_t timeout_ms = 0;
    int count = 0;
    int result;
    int i;

    for (i = 1; i < argc; i++) {
        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        } else if (modal_parse_var_arg(argv[i], &varname)) {
            continue;
        } else if (title == NULL) {
            title = argv[i];
        } else if (items == NULL) {
            items = (const char **)(argv + i);
            count = argc - i;
            break;
        }
    }

    if (title == NULL || count < 1) {
        shell_print_usage("Usage: list [/t:secs] [/v:NAME] \"title\" item1 [item2...]");
        batch_set_errorlevel(2);
        return;
    }

    result = modal_list_run(title, items, count, timeout_ms);
    if (result < 0) {
        batch_set_errorlevel(255);
        return;
    }

    if (varname != NULL) {
        shell_env_set(varname, items[result]);
    }
    batch_set_errorlevel(result);
}

void shell_command_ask(int argc, char **argv)
{
    const char *varname = "ASK_RESULT";
    const char *prompt = NULL;
    const char *default_text = NULL;
    bool password = false;
    uint32_t timeout_ms = 0;
    char result[P4_CONFIG_ENV_VALUE_BYTES];
    int rc;
    int i;

    for (i = 1; i < argc; i++) {
        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        } else if (modal_parse_var_arg(argv[i], &varname)) {
            continue;
        } else if (strcasecmp(argv[i], "/p") == 0) {
            password = true;
        } else if (prompt == NULL) {
            prompt = argv[i];
        } else if (default_text == NULL) {
            default_text = argv[i];
        }
    }

    if (prompt == NULL) {
        shell_print_usage("Usage: ask [/t:secs] [/v:NAME] [/p] \"prompt\" [default]");
        batch_set_errorlevel(2);
        return;
    }

    rc = modal_ask_run(prompt, default_text, password, timeout_ms,
                       result, sizeof(result));
    if (rc != 0) {
        batch_set_errorlevel(1);
        return;
    }

    if (shell_env_set(varname, result) != ESP_OK) {
        shell_print_error("ask: cannot set %s", varname);
        batch_set_errorlevel(1);
        return;
    }
    batch_set_errorlevel(0);
}

/* `form` - a multi-field modal form for batch apps. Each field spec is
 *   "Label=<type>[:<arg>]:VAR"
 * with type in text|password|check|select|range. select's arg is `a|b|c`;
 * range's arg is `min-max`. VAR is prefilled from the environment when set and
 * receives the accepted value. OK sets ERRORLEVEL 0; cancel/skip/timeout 255. */
void shell_command_form(int argc, char **argv)
{
    modal_form_field_t fields[MODAL_FORM_MAX_FIELDS];
    char labels[MODAL_FORM_MAX_FIELDS][80];
    char *values[MODAL_FORM_MAX_FIELDS];
    const char *vars[MODAL_FORM_MAX_FIELDS];
    const char *title = NULL;
    uint32_t timeout_ms = 0;
    int nf = 0;
    int rc;
    int i;

    memset(fields, 0, sizeof(fields));
    memset(labels, 0, sizeof(labels));
    memset(values, 0, sizeof(values));
    memset(vars, 0, sizeof(vars));

    for (i = 1; i < argc; i++) {
        char *spec, *eq, *colon, *arg, *var, *sep;
        char type[24];
        const char *argp = NULL;
        modal_form_field_t *f;
        size_t n;

        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        }
        if (title == NULL) {
            title = argv[i];
            continue;
        }
        if (nf >= MODAL_FORM_MAX_FIELDS) {
            break;
        }

        spec = argv[i];
        eq = strchr(spec, '=');
        if (eq == NULL) {
            shell_print_muted("form: skipping '%s' (want Label=type[:arg]:VAR)", spec);
            continue;
        }
        n = (size_t)(eq - spec);
        if (n >= sizeof(labels[0])) n = sizeof(labels[0]) - 1;
        memcpy(labels[nf], spec, n);
        labels[nf][n] = '\0';

        colon = strchr(eq + 1, ':');
        if (colon == NULL) {
            shell_print_muted("form: skipping '%s' (want Label=type[:arg]:VAR)", spec);
            continue;
        }
        n = (size_t)(colon - (eq + 1));
        if (n >= sizeof(type)) n = sizeof(type) - 1;
        memcpy(type, eq + 1, n);
        type[n] = '\0';

        /* remainder: [arg:]VAR */
        arg = colon + 1;
        sep = strchr(arg, ':');
        if (sep != NULL) {
            argp = arg;
            *sep = '\0';
            var = sep + 1;
        } else {
            var = arg;
        }
        if (var == NULL || var[0] == '\0') {
            shell_print_muted("form: skipping '%s' (missing :VAR)", spec);
            continue;
        }

        f = &fields[nf];
        f->label = labels[nf];
        f->value = values[nf] = malloc(P4_CONFIG_ENV_VALUE_BYTES);
        if (f->value == NULL) {
            break;
        }
        f->value[0] = '\0';
        f->value_size = P4_CONFIG_ENV_VALUE_BYTES;
        vars[nf] = var;

        if (strcasecmp(type, "password") == 0) {
            f->type = MODAL_FORM_PASSWORD;
        } else if (strcasecmp(type, "check") == 0) {
            f->type = MODAL_FORM_CHECK;
        } else if (strcasecmp(type, "select") == 0) {
            f->type = MODAL_FORM_SELECT;
            f->options = argp ? argp : "";
        } else if (strcasecmp(type, "range") == 0) {
            int lo = 0, hi = 100;
            if (argp != NULL) {
                sscanf(argp, "%d-%d", &lo, &hi);
            }
            f->type = MODAL_FORM_RANGE;
            f->min = lo;
            f->max = hi;
        } else {
            f->type = MODAL_FORM_TEXT;
        }

        {
            const char *cur = shell_env_get(var);
            if (cur != NULL && cur[0] != '\0') {
                snprintf(f->value, f->value_size, "%s", cur);
            }
        }
        /* The label/argp pointers are freed with argv after dispatch; the
         * modal copies the label text when it builds the widget, but the
         * surface runs synchronously here, so they stay valid. */
        nf++;
    }

    if (nf == 0) {
        shell_print_usage("Usage: form [/t:secs] \"title\" \"Label=type[:arg]:VAR\" ...");
        for (i = 0; i < MODAL_FORM_MAX_FIELDS; i++) free(values[i]);
        batch_set_errorlevel(2);
        return;
    }

    rc = modal_form_run(title, fields, nf, timeout_ms);
    if (rc != 0) {
        for (i = 0; i < nf; i++) free(values[i]);
        batch_set_errorlevel(255);
        return;
    }

    for (i = 0; i < nf; i++) {
        if (shell_env_set(vars[i], values[i]) != ESP_OK) {
            shell_print_error("form: cannot set %s", vars[i]);
        }
        free(values[i]);
    }
    batch_set_errorlevel(0);
}

/** Shared image-view entry (used by `view` and `open`): resolve, show in the
 * image viewer, map to ERRORLEVEL. One implementation, no duplicated routing.
 * @p verb names the caller in messages (`open x.bmp` used to blame `view`). */
static void view_image_file(const char *verb, const char *path, uint32_t timeout_ms)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];

    if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("%s: invalid path %s", verb, path);
        batch_set_errorlevel(2);
        return;
    }
    if (modal_image_run("Image", resolved, timeout_ms, P4_CONFIG_IMAGE_VIEWER_FIT != 0) != 0) {
        shell_print_error("%s: cannot open image %s", verb, path);
        batch_set_errorlevel(1);
        return;
    }
    batch_set_errorlevel(0);
}

/** Shared `view`/`open` option parse: [/t:secs] [--raw] <file>. @return true
 *  with @p path_out set, false after printing usage (ERRORLEVEL 2). */
bool view_parse_args(const char *verb, int argc, char **argv,
                     const char **path_out, uint32_t *timeout_ms_out,
                     bool *raw_out)
{
    const char *path = NULL;
    uint32_t timeout_ms = 0;
    bool raw = false;

    for (int i = 1; i < argc; i++) {
        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        } else if (shell_text_equals_ignore_case(argv[i], "--raw")) {
            raw = true;
        } else if (path == NULL) {
            path = argv[i];
        }
    }

    if (path == NULL) {
        shell_print_usage("Usage: %s [/t:secs] [--raw] <file>", verb);
        batch_set_errorlevel(2);
        return false;
    }
    *path_out = path;
    *timeout_ms_out = timeout_ms;
    *raw_out = raw;
    return true;
}

/** Shared text-or-image dispatch behind `view` and `open`: .bmp/.dib goes
 *  to the image viewer, everything else to the text pager (or rendered
 *  Markdown). The type test is the central registry. */
static void view_show_path(const char *verb, const char *path, uint32_t timeout_ms, bool raw)
{
    if (filetype_is_image(filetype_of(path))) {
        if (raw) {
            shell_print_muted("%s: --raw ignored for images", verb);
        }
        view_image_file(verb, path, timeout_ms);
        return;
    }

    int rc = modal_viewer_run_raw("View", path, timeout_ms, raw);
    batch_set_errorlevel(rc == 0 ? 0 : 1);
}

void shell_command_view(int argc, char **argv)
{
    const char *path = NULL;
    uint32_t timeout_ms = 0;
    bool raw = false;

    if (!view_parse_args("view", argc, argv, &path, &timeout_ms, &raw)) {
        return;
    }
    view_show_path("view", path, timeout_ms, raw);
}

void shell_command_open(int argc, char **argv)
{
    const char *path = NULL;
    uint32_t timeout_ms = 0;
    bool raw = false;

    if (!view_parse_args("open", argc, argv, &path, &timeout_ms, &raw)) {
        return;
    }

    /* Dispatch by registry type. Scripts NEVER execute here: typing a
     * .bat/.cmd name runs it, while `open` shows its source in the editor.
     * Markdown renders (viewer branch); everything else views as text. */
    if (filetype_is_executable(filetype_of(path))) {
        int errorlevel = 0;
        esp_err_t err = editor_session_run(path, &errorlevel);
        if (err != ESP_OK && errorlevel == 0) {
            errorlevel = 1;
        }
        batch_set_errorlevel(errorlevel);
        return;
    }

    /* Association dispatch: a mapped extension launches its app with the
     * file as %1 (`assoc`/`ftype`). A mapping to a missing app warns and
     * falls through to the viewer â€” forgiving, like DOS. Unmapped files
     * keep the historical viewer behavior exactly. */
    {
        char app[64];
        char *quoted = NULL;

        if (assoc_lookup_app(path, app, sizeof(app))) {
            quoted = malloc(strlen(path) + 3u);
            if (quoted == NULL) {
                shell_print_error("open: out of memory");
                batch_set_errorlevel(1);
                return;
            }
            snprintf(quoted, strlen(path) + 3u, "\"%s\"", path);
            if (shell_launch_app_with_args(app, quoted)) {
                free(quoted);
                return;
            }
            free(quoted);
            shell_print_warning("open: '%s' is mapped but missing; showing as text", app);
        }
    }

    view_show_path("open", path, timeout_ms, raw);
}

void shell_command_hexview(int argc, char **argv)
{
    const char *path = NULL;
    uint32_t timeout_ms = 0;

    for (int i = 1; i < argc; i++) {
        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        } else if (path == NULL) {
            path = argv[i];
        }
    }

    if (path == NULL) {
        shell_print_usage("Usage: hexview [/t:secs] <file>");
        batch_set_errorlevel(2);
        return;
    }

    int rc = modal_hexview_run("Hexview", path, timeout_ms);
    batch_set_errorlevel(rc == 0 ? 0 : 1);
}

void shell_command_color(int argc, char **argv)
{
    if (argc == 1) {
        uint8_t fg, bg;
        tui_get_default_color(&fg, &bg);
        if (tui_is_active()) {
            shell_print_field("color", "fg=%u bg=%u", fg, bg);
        } else {
            shell_print_field("color", "default");
        }
        batch_set_errorlevel(0);
        return;
    }
    if (argc < 2 || argc > 3) {
        shell_print_usage("Usage: color [fg] [bg]  (0-15 or palette name)");
        batch_set_errorlevel(2);
        return;
    }
    if (!draw_require_foreground("color")) return;
    int fg = atoi(argv[1]);
    int bg = (argc > 2) ? atoi(argv[2]) : 16;
    if (fg < 0) fg = 0;
    if (fg > 15) fg = 15;
    if (bg < 0) bg = 0;
    if (bg > 15 && bg != 16) bg = 16;
    if (tui_is_active()) {
        tui_set_default_color((uint8_t)fg, (uint8_t)bg);
        draw_maybe_flush();
    }
    shell_transcript_appendf_ansi(SH_MUTE "color: fg=%s bg=%s" SH_RST "\n",
                                   argv[1], argc > 2 ? argv[2] : "default");
    batch_set_errorlevel(0);
}

void shell_command_locate(int argc, char **argv)
{
    if (argc != 3) {
        shell_print_usage("Usage: locate <row> <col>  (1-based, 1..25 1..80)");
        batch_set_errorlevel(2);
        return;
    }
    if (!draw_require_foreground("locate")) return;
    int row = atoi(argv[1]);
    int col = atoi(argv[2]);
    if (row < 1) row = 1;
    if (row > P4_CONFIG_TUI_ROWS) row = P4_CONFIG_TUI_ROWS;
    if (col < 1) col = 1;
    if (col > P4_CONFIG_TUI_COLS) col = P4_CONFIG_TUI_COLS;
    if (tui_is_active()) {
        tui_set_cursor(row, col);
        draw_maybe_flush();
    }
    /* Emit ANSI CUP via raw escape so transcript and UART agree; TUI mode handles it via CSI if active */
    shell_transcript_appendf_ansi("\x1b[%d;%dH", row, col);
    batch_set_errorlevel(0);
}

void shell_command_tui(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: tui fullscreen <on|off> | tui status | tui stats | tui clear | tui selftest");
        batch_set_errorlevel(2);
        return;
    }
    /* `tui status` / `tui stats` / `tui selftest` are read-only queries: safe
     * (and useful) in bg jobs. */
    if (!shell_text_equals_ignore_case(argv[1], "status") &&
        !shell_text_equals_ignore_case(argv[1], "stats") &&
        !shell_text_equals_ignore_case(argv[1], "selftest") &&
        !draw_require_foreground("tui")) {
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "fullscreen")) {
        if (argc < 3) {
            shell_print_usage("Usage: tui fullscreen <on|off>");
            batch_set_errorlevel(2);
            return;
        }
        if (shell_text_equals_ignore_case(argv[2], "on")) {
            tui_enter_fullscreen();
            batch_set_errorlevel(0);
            return;
        } else if (shell_text_equals_ignore_case(argv[2], "off")) {
            tui_exit_fullscreen();
            batch_set_errorlevel(0);
            return;
        }
    } else if (shell_text_equals_ignore_case(argv[1], "status")) {
        int cell_w = 0;
        int cell_h = 0;
        int cell_index = -1;

        shell_print_field("tui", "%s", tui_is_active() ? "active" : "inactive");
        shell_print_field("fullscreen", "%s", tui_is_fullscreen() ? "on" : "off");
        window_rect_t r = windows_get_rect(WINDOW_REGION_TRANSCRIPT);
        shell_print_field("transcript rect", "%dx%d at %d,%d", r.width, r.height, r.x, r.y);
        shell_print_field("cols/rows", "%d x %d", P4_CONFIG_TUI_COLS, P4_CONFIG_TUI_ROWS);
        tui_bound_cell_info(&cell_w, &cell_h, &cell_index);
        if (cell_index >= 0) {
            shell_print_field("cell font", "%dx%d (slot %d of %d)",
                              cell_w, cell_h, cell_index + 1, tui_font_cell_count);
        } else {
            shell_print_field("cell font", "%s", "(unbound)");
        }
        batch_set_errorlevel(0);
        return;
    } else if (shell_text_equals_ignore_case(argv[1], "selftest")) {
        const char *fail_name = NULL;
        int palette_fails;
        int coverage_fails;
        int metrics_fails;

        /* Pure checks (no display needed), so a broken TUI still reports
         * instead of hanging. Each group runs even if an earlier one fails. */
        palette_fails = tui_selftest_palette(&fail_name);
        if (palette_fails == 0) {
            shell_print_field("tui.selftest.palette", "%s", "ok");
        } else {
            shell_print_field("tui.selftest.palette", "FAIL at %s",
                              fail_name != NULL ? fail_name : "?");
        }
        fail_name = NULL;
        coverage_fails = tui_selftest_coverage(&fail_name);
        if (coverage_fails == 0) {
            shell_print_field("tui.selftest.coverage", "%s", "ok");
        } else {
            shell_print_field("tui.selftest.coverage", "FAIL at %s",
                              fail_name != NULL ? fail_name : "?");
        }
        fail_name = NULL;
        metrics_fails = tui_selftest_metrics(&fail_name);
        if (metrics_fails == 0) {
            shell_print_field("tui.selftest.metrics", "%s", "ok");
        } else {
            shell_print_field("tui.selftest.metrics", "FAIL at %s",
                              fail_name != NULL ? fail_name : "?");
        }
        if (palette_fails + coverage_fails + metrics_fails == 0) {
            shell_print_ok("tui selftest: clean");
            batch_set_errorlevel(0);
        } else {
            shell_print_error("tui selftest: %d failure(s)",
                              palette_fails + coverage_fails + metrics_fails);
            batch_set_errorlevel(1);
        }
        return;
    } else if (shell_text_equals_ignore_case(argv[1], "stats")) {
        /* TUI frame pacing: frames/min/avg/max/jitter/dropped (per present). */
        if (argc >= 3 && shell_text_equals_ignore_case(argv[2], "reset")) {
            tui_frame_stats_reset();
            shell_print_ok("tui stats: reset");
            batch_set_errorlevel(0);
            return;
        }
        if (argc >= 4 && shell_text_equals_ignore_case(argv[2], "target")) {
            uint32_t fps = (shell_text_equals_ignore_case(argv[3], "off") ||
                            shell_text_equals_ignore_case(argv[3], "0"))
                           ? 0u : (uint32_t)atoi(argv[3]);
            tui_frame_stats_set_target_fps(fps);
            shell_transcript_appendf("tui stats: target %u fps\n", (unsigned)fps);
            batch_set_errorlevel(0);
            return;
        }
        {
            char fields[160];
            gfx_frame_stats_format(tui_frame_stats_get(), fields, sizeof(fields));
            shell_transcript_appendf("tui stats: %s\n", fields);
        }
        batch_set_errorlevel(0);
        return;
    } else if (shell_text_equals_ignore_case(argv[1], "clear")) {
        if (tui_is_active()) tui_clear();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: tui fullscreen <on|off> | tui status | tui stats | tui clear | tui selftest");
    batch_set_errorlevel(2);
}

/* ========================================================================
 * WORKSPACE VERBS: ui, pane, hud, key, tap, prompt, event, trace
 * ================================================================ */

/** `ui` â€” declare and mutate declarative widgets. */
bool shell_command_wig(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: ui /panel NAME X Y W H [/border single] [/title T] | ui /text NAME X Y TEXT | ui /gauge NAME X Y W /max N | ui /button NAME X Y W LABEL /on LABEL | ui /list NAME X Y W /sel VAR | ui /clear NAME | ui /render");
        batch_set_errorlevel(2);
        return false;
    }
    if (!draw_require_foreground("wig")) return false;

    if (shell_text_equals_ignore_case(argv[1], "panel")) {
        if (argc < 7) {
            shell_print_usage("Usage: ui /panel NAME X Y W H [/title T]");
            batch_set_errorlevel(2);
            return false;
        }
        const char *name = argv[2];
        int x = atoi(argv[3]);
        int y = atoi(argv[4]);
        int w = atoi(argv[5]);
        int h = atoi(argv[6]);
        const char *title = NULL;
        for (int i = 7; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/title") && i + 1 < argc) {
                title = argv[i + 1];
                i++;
            }
        }
        if (!tui_widget_add(name, "panel", x, y, w, h, title)) {
            shell_print_error("ui /panel: cannot create widget '%s'", name);
            batch_set_errorlevel(1);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "text")) {
        if (argc < 6) {
            shell_print_usage("Usage: ui /text NAME X Y TEXT");
            batch_set_errorlevel(2);
            return false;
        }
        const char *name = argv[2];
        int x = atoi(argv[3]);
        int y = atoi(argv[4]);
        const char *text = argv[5];
        if (!tui_widget_add(name, "text", x, y, 0, 0, NULL)) {
            shell_print_error("ui /text: cannot create widget '%s'", name);
            batch_set_errorlevel(1);
            return false;
        }
        tui_widget_set_text(name, text);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "gauge")) {
        if (argc < 6) {
            shell_print_usage("Usage: ui /gauge NAME X Y W /max N");
            batch_set_errorlevel(2);
            return false;
        }
        const char *name = argv[2];
        int x = atoi(argv[3]);
        int y = atoi(argv[4]);
        int w = atoi(argv[5]);
        int max_val = 100;
        for (int i = 6; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/max") && i + 1 < argc) {
                max_val = atoi(argv[i + 1]);
                i++;
            }
        }
        if (!tui_widget_add(name, "gauge", x, y, w, 1, NULL)) {
            shell_print_error("ui /gauge: cannot create widget '%s'", name);
            batch_set_errorlevel(1);
            return false;
        }
        tui_widget_set_max(name, max_val);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "button")) {
        if (argc < 7) {
            shell_print_usage("Usage: ui /button NAME X Y W LABEL /on LABEL");
            batch_set_errorlevel(2);
            return false;
        }
        const char *name = argv[2];
        int x = atoi(argv[3]);
        int y = atoi(argv[4]);
        int w = atoi(argv[5]);
        const char *label = argv[6];
        const char *on_label = NULL;
        for (int i = 7; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/on") && i + 1 < argc) {
                on_label = argv[i + 1];
                i++;
            }
        }
        if (!tui_widget_add(name, "button", x, y, w, 1, label)) {
            shell_print_error("ui /button: cannot create widget '%s'", name);
            batch_set_errorlevel(1);
            return false;
        }
        if (on_label != NULL) {
            tui_widget_bind_label(name, on_label);
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        if (argc < 6) {
            shell_print_usage("Usage: ui /list NAME X Y W /sel VAR");
            batch_set_errorlevel(2);
            return false;
        }
        const char *name = argv[2];
        int x = atoi(argv[3]);
        int y = atoi(argv[4]);
        int w = atoi(argv[5]);
        for (int i = 6; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/sel") && i + 1 < argc) {
                const char *var = argv[i + 1];
                const char *val = shell_env_get(var);
                tui_widget_add(name, "list", x, y, w, 2, NULL);
                tui_widget_set_value(name, val != NULL ? atoi(val) : 0);
                batch_set_errorlevel(0);
                return true;
            }
        }
        tui_widget_add(name, "list", x, y, w, 2, NULL);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        if (argc < 3) {
            shell_print_usage("Usage: ui /clear NAME");
            batch_set_errorlevel(2);
            return false;
        }
        if (!tui_widget_remove(argv[2])) {
            shell_print_error("ui /clear: widget '%s' not found", argv[2]);
            batch_set_errorlevel(1);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "render")) {
        if (!tui_is_active()) {
            if (!tui_init()) {
                shell_print_error("ui /render: tui init failed");
                batch_set_errorlevel(1);
                return false;
            }
        }
        tui_workspace_render();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    shell_print_usage("Usage: ui /panel|/text|/gauge|/button|/list|/clear|/render");
    batch_set_errorlevel(2);
    return false;
}

/** `pane` â€” manage the pane stack. */
bool shell_command_pane(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: pane /add NAME X Y W H [/title T] | pane /split NAME /h | pane /focus NAME | pane /cycle | pane /hide NAME | pane /close NAME | pane /list");
        batch_set_errorlevel(2);
        return false;
    }
    if (!draw_require_foreground("pane")) return false;

    if (shell_text_equals_ignore_case(argv[1], "add")) {
        if (argc < 6) {
            shell_print_usage("Usage: pane /add NAME X Y W H [/title T]");
            batch_set_errorlevel(2);
            return false;
        }
        const char *name = argv[2];
        int x = atoi(argv[3]);
        int y = atoi(argv[4]);
        int w = atoi(argv[5]);
        int h = atoi(argv[6]);
        const char *title = NULL;
        for (int i = 7; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/title") && i + 1 < argc) {
                title = argv[i + 1];
                i++;
            }
        }
        if (!tui_pane_add(name, x, y, w, h, title)) {
            shell_print_error("pane /add: cannot create pane '%s'", name);
            batch_set_errorlevel(1);
            return false;
        }
        if (!tui_is_active()) {
            if (!tui_init()) {
                shell_print_error("pane /add: tui init failed");
                batch_set_errorlevel(1);
                return false;
            }
        }
        tui_workspace_render();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "split")) {
        if (argc < 4) {
            shell_print_usage("Usage: pane /split NAME /h|/v");
            batch_set_errorlevel(2);
            return false;
        }
        const char *name = argv[2];
        bool horizontal = shell_text_equals_ignore_case(argv[3], "h");
        if (!tui_pane_split(name, horizontal)) {
            shell_print_error("pane /split: cannot split pane '%s'", name);
            batch_set_errorlevel(1);
            return false;
        }
        tui_workspace_render();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "focus")) {
        if (argc < 3) {
            shell_print_usage("Usage: pane /focus NAME");
            batch_set_errorlevel(2);
            return false;
        }
        if (!tui_pane_focus(argv[2])) {
            shell_print_error("pane /focus: pane '%s' not found", argv[2]);
            batch_set_errorlevel(1);
            return false;
        }
        tui_workspace_render();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "cycle")) {
        if (!tui_pane_cycle()) {
            shell_print_error("pane /cycle: no panes");
            batch_set_errorlevel(1);
            return false;
        }
        tui_workspace_render();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "hide")) {
        if (argc < 3) {
            shell_print_usage("Usage: pane /hide NAME");
            batch_set_errorlevel(2);
            return false;
        }
        if (!tui_pane_hide(argv[2], true)) {
            shell_print_error("pane /hide: pane '%s' not found", argv[2]);
            batch_set_errorlevel(1);
            return false;
        }
        tui_workspace_render();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "close")) {
        if (argc < 3) {
            shell_print_usage("Usage: pane /close NAME");
            batch_set_errorlevel(2);
            return false;
        }
        if (!tui_pane_close(argv[2])) {
            shell_print_error("pane /close: pane '%s' not found", argv[2]);
            batch_set_errorlevel(1);
            return false;
        }
        tui_workspace_render();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        int count = tui_pane_count();
        char buf[256];
        int offset = 0;
        offset += snprintf(buf + offset, sizeof(buf) - offset, "panes=%d", count);
        for (int i = 0; i < count && offset < (int)sizeof(buf) - 2; i++) {
            char name[P4_CONFIG_WS_PANE_NAME_BYTES];
            if (tui_pane_get_name(i, name, sizeof(name))) {
                offset += snprintf(buf + offset, sizeof(buf) - offset, " %s", name);
            }
        }
        shell_transcript_appendf("%s\n", buf);
        batch_set_errorlevel(0);
        return true;
    }

    shell_print_usage("Usage: pane /add|/split|/focus|/cycle|/hide|/close|/list");
    batch_set_errorlevel(2);
    return false;
}

/** `hud` â€” bind live status cells. */
bool shell_command_hud(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: hud /add NAME TEXT [/right] | hud /refresh MS | hud /flash NAME N | hud /del NAME");
        batch_set_errorlevel(2);
        return false;
    }
    if (!draw_require_foreground("hud")) return false;

    if (shell_text_equals_ignore_case(argv[1], "add")) {
        if (argc < 4) {
            shell_print_usage("Usage: hud /add NAME TEXT [/right]");
            batch_set_errorlevel(2);
            return false;
        }
        const char *name = argv[2];
        const char *text = argv[3];
        int position = 0;
        for (int i = 4; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/right")) {
                position = 1;
            }
        }
        if (!tui_hud_add(name, position)) {
            shell_print_error("hud /add: cannot add HUD cell '%s'", name);
            batch_set_errorlevel(1);
            return false;
        }
        tui_hud_set_text(name, text);
        if (!tui_is_active()) {
            if (!tui_init()) {
                shell_print_error("hud /add: tui init failed");
                batch_set_errorlevel(1);
                return false;
            }
        }
        tui_workspace_render();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "refresh")) {
        if (argc < 3) {
            shell_print_usage("Usage: hud /refresh MS");
            batch_set_errorlevel(2);
            return false;
        }
        tui_hud_set_refresh((uint32_t)atoi(argv[2]));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "flash")) {
        if (argc < 4) {
            shell_print_usage("Usage: hud /flash NAME N");
            batch_set_errorlevel(2);
            return false;
        }
        if (!tui_hud_flash(argv[2], atoi(argv[3]))) {
            shell_print_error("hud /flash: cell '%s' not found", argv[2]);
            batch_set_errorlevel(1);
            return false;
        }
        tui_workspace_render();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "del")) {
        if (argc < 3) {
            shell_print_usage("Usage: hud /del NAME");
            batch_set_errorlevel(2);
            return false;
        }
        if (!tui_hud_remove(argv[2])) {
            shell_print_error("hud /del: cell '%s' not found", argv[2]);
            batch_set_errorlevel(1);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    shell_print_usage("Usage: hud /add|/refresh|/flash|/del");
    batch_set_errorlevel(2);
    return false;
}

/** `key` â€” key bindings and synthetic input. */
bool shell_command_key(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: key /bind KEY LABEL | key /unbind KEY | key /inject TEXT | key /mode raw|line|passthrough | key /list");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "bind")) {
        if (argc < 4) {
            shell_print_usage("Usage: key /bind KEY LABEL");
            batch_set_errorlevel(2);
            return false;
        }
        if (!tui_key_bind(argv[2], argv[3])) {
            shell_print_error("key /bind: cannot bind '%s'", argv[2]);
            batch_set_errorlevel(1);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "unbind")) {
        if (argc < 3) {
            shell_print_usage("Usage: key /unbind KEY");
            batch_set_errorlevel(2);
            return false;
        }
        if (!tui_key_unbind(argv[2])) {
            shell_print_error("key /unbind: key '%s' not found", argv[2]);
            batch_set_errorlevel(1);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "inject")) {
        if (argc < 3) {
            shell_print_usage("Usage: key /inject TEXT");
            batch_set_errorlevel(2);
            return false;
        }
        if (!tui_key_inject(argv[2])) {
            shell_print_error("key /inject: failed");
            batch_set_errorlevel(1);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "mode")) {
        if (argc < 3) {
            shell_print_usage("Usage: key /mode raw|line|passthrough");
            batch_set_errorlevel(2);
            return false;
        }
        int mode = 0;
        if (shell_text_equals_ignore_case(argv[2], "raw")) mode = 1;
        else if (shell_text_equals_ignore_case(argv[2], "passthrough")) mode = 2;
        tui_key_set_mode(mode);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        int count = tui_key_count();
        char buf[256];
        int offset = 0;
        offset += snprintf(buf + offset, sizeof(buf) - offset, "keys=%d", count);
        for (int i = 0; i < count && offset < (int)sizeof(buf) - 2; i++) {
            char key[P4_CONFIG_WS_KEY_NAME_BYTES];
            if (tui_key_get_name(i, key, sizeof(key))) {
                offset += snprintf(buf + offset, sizeof(buf) - offset, " %s", key);
            }
        }
        shell_transcript_appendf("%s\n", buf);
        batch_set_errorlevel(0);
        return true;
    }

    shell_print_usage("Usage: key /bind|/unbind|/inject|/mode|/list");
    batch_set_errorlevel(2);
    return false;
}

/** `tap` â€” touch hotspot binding. */
bool shell_command_tap(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: tap /on X Y W H LABEL | tap /off X Y | tap /clear | tap /list");
        batch_set_errorlevel(2);
        return false;
    }
    if (!draw_require_foreground("tap")) return false;

    if (shell_text_equals_ignore_case(argv[1], "on")) {
        if (argc < 7) {
            shell_print_usage("Usage: tap /on X Y W H LABEL");
            batch_set_errorlevel(2);
            return false;
        }
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        int w = atoi(argv[4]);
        int h = atoi(argv[5]);
        const char *label = argv[6];
        if (!tui_tap_add(x, y, w, h, label)) {
            shell_print_error("tap /on: too many hotspots");
            batch_set_errorlevel(1);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "off")) {
        if (argc < 4) {
            shell_print_usage("Usage: tap /off X Y");
            batch_set_errorlevel(2);
            return false;
        }
        int removed = tui_tap_remove(atoi(argv[2]), atoi(argv[3]));
        shell_transcript_appendf("tap /off: removed %d hotspot(s)\n", removed);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        tui_tap_clear();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        int count = tui_tap_count();
        shell_transcript_appendf("tap: %d hotspot(s)\n", count);
        batch_set_errorlevel(0);
        return true;
    }

    shell_print_usage("Usage: tap /on|/off|/clear|/list");
    batch_set_errorlevel(2);
    return false;
}

/** `prompt` â€” enhanced line input. */
bool shell_command_prompt_ws(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: prompt /text TEXT [/default D] [/var NAME] | prompt /mask /var NAME | prompt /timeout N /var NAME");
        batch_set_errorlevel(2);
        return false;
    }

    const char *var = NULL;
    const char *default_val = NULL;
    const char *text = NULL;
    bool mask = false;
    int timeout_ms = P4_CONFIG_KEY_WAIT_TIMEOUT_MS;

    for (int i = 2; i < argc; i++) {
        if (shell_text_equals_ignore_case(argv[i], "/var") && i + 1 < argc) {
            var = argv[i + 1]; i++;
        } else if (shell_text_equals_ignore_case(argv[i], "/default") && i + 1 < argc) {
            default_val = argv[i + 1]; i++;
        } else if (shell_text_equals_ignore_case(argv[i], "/text") && i + 1 < argc) {
            text = argv[i + 1]; i++;
        } else if (shell_text_equals_ignore_case(argv[i], "/mask")) {
            mask = true;
        } else if (shell_text_equals_ignore_case(argv[i], "/timeout") && i + 1 < argc) {
            timeout_ms = atoi(argv[i + 1]); i++;
        }
    }

    if (var == NULL) {
        shell_print_usage("Usage: prompt /var NAME ...");
        batch_set_errorlevel(2);
        return false;
    }

    if (text != NULL) {
        shell_transcript_appendf("%s", text);
    }
    if (default_val != NULL) {
        shell_transcript_appendf(" [%s]", default_val);
    }
    shell_transcript_appendf(" ");

    char result[P4_CONFIG_SET_PROMPT_INPUT_BYTES];
    bool ok;
    if (mask) {
        ok = shell_read_line_hidden(result, sizeof(result), (uint32_t)timeout_ms);
    } else {
        ok = shell_read_line(result, sizeof(result), (uint32_t)timeout_ms);
    }

    if (!ok) {
        shell_env_set(var, "");
        batch_set_errorlevel(1);
        return false;
    }

    shell_env_set(var, result);
    batch_set_errorlevel(0);
    return true;
}

/** `event` â€” wait, inspect, dispatch. */
bool shell_command_event(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: event /wait MS | event /type IDX | event /field IDX | event /dispatch | event /post TYPE PAYLOAD | event /clear | event /count");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "wait")) {
        if (argc < 3) {
            shell_print_usage("Usage: event /wait MS");
            batch_set_errorlevel(2);
            return false;
        }
        int idx = tui_event_wait(atoi(argv[2]));
        if (idx < 0) {
            batch_set_errorlevel(1);
            return false;
        }
        tui_event_type_t type;
        char payload[P4_CONFIG_WS_EVENT_PAYLOAD_BYTES];
        tui_event_get(idx, &type, payload, sizeof(payload));
        char type_buf[16];
        const char *type_str = "unknown";
        switch (type) {
        case TUI_EVENT_KEY: type_str = "key"; break;
        case TUI_EVENT_TAP: type_str = "tap"; break;
        case TUI_EVENT_TIMER: type_str = "timer"; break;
        case TUI_EVENT_SERIAL: type_str = "serial"; break;
        case TUI_EVENT_RESIZE: type_str = "resize"; break;
        default: break;
        }
        snprintf(type_buf, sizeof(type_buf), "EV_%s", type_str);
        shell_env_set("EV_TYPE", type_buf);
        shell_env_set("EV", payload);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "type")) {
        if (argc < 3) {
            shell_print_usage("Usage: event /type IDX");
            batch_set_errorlevel(2);
            return false;
        }
        tui_event_type_t type;
        tui_event_get(atoi(argv[2]), &type, NULL, 0);
        const char *type_str = "unknown";
        switch (type) {
        case TUI_EVENT_KEY: type_str = "key"; break;
        case TUI_EVENT_TAP: type_str = "tap"; break;
        case TUI_EVENT_TIMER: type_str = "timer"; break;
        case TUI_EVENT_SERIAL: type_str = "serial"; break;
        case TUI_EVENT_RESIZE: type_str = "resize"; break;
        default: break;
        }
        shell_transcript_appendf("%s\n", type_str);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "field")) {
        if (argc < 4) {
            shell_print_usage("Usage: event /field IDX FIELD");
            batch_set_errorlevel(2);
            return false;
        }
        char payload[P4_CONFIG_WS_EVENT_PAYLOAD_BYTES];
        tui_event_get(atoi(argv[2]), NULL, payload, sizeof(payload));
        shell_env_set(argv[3], payload);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "dispatch")) {
        int dispatched = 0;
        while (tui_event_count() > 0) {
            tui_event_type_t type;
            char payload[P4_CONFIG_WS_EVENT_PAYLOAD_BYTES];
            tui_event_get(0, &type, payload, sizeof(payload));
            tui_event_clear();
            dispatched++;
            if (type == TUI_EVENT_TAP || type == TUI_EVENT_KEY) {
                if (payload[0] != '\0') {
                    shell_execute_command(payload);
                }
            }
        }
        shell_transcript_appendf("event /dispatch: dispatched %d event(s)\n", dispatched);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "post")) {
        if (argc < 4) {
            shell_print_usage("Usage: event /post TYPE PAYLOAD");
            batch_set_errorlevel(2);
            return false;
        }
        tui_event_type_t type = TUI_EVENT_TIMER;
        if (shell_text_equals_ignore_case(argv[2], "key")) type = TUI_EVENT_KEY;
        else if (shell_text_equals_ignore_case(argv[2], "tap")) type = TUI_EVENT_TAP;
        else if (shell_text_equals_ignore_case(argv[2], "serial")) type = TUI_EVENT_SERIAL;
        else if (shell_text_equals_ignore_case(argv[2], "resize")) type = TUI_EVENT_RESIZE;
        if (!tui_event_post(type, argv[3])) {
            shell_print_error("event /post: queue full");
            batch_set_errorlevel(1);
            return false;
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        tui_event_clear();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "count")) {
        shell_transcript_appendf("%d\n", tui_event_count());
        batch_set_errorlevel(0);
        return true;
    }

    shell_print_usage("Usage: event /wait|/type|/field|/dispatch|/post|/clear|/count");
    batch_set_errorlevel(2);
    return false;
}

/** `trace` â€” on-screen diagnostics. */
bool shell_command_trace(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: trace /on | trace /off | trace /level LEVEL | trace /status");
        batch_set_errorlevel(2);
        return false;
    }

    static bool s_trace_on = false;
    static int s_trace_level = 1; /* 0=none, 1=warn, 2=info, 3=debug */

    if (shell_text_equals_ignore_case(argv[1], "on")) {
        s_trace_on = true;
        shell_transcript_appendf("trace: on\n");
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "off")) {
        s_trace_on = false;
        shell_transcript_appendf("trace: off\n");
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "level")) {
        if (argc < 3) {
            shell_print_usage("Usage: trace /level none|warn|info|debug");
            batch_set_errorlevel(2);
            return false;
        }
        if (shell_text_equals_ignore_case(argv[2], "none")) s_trace_level = 0;
        else if (shell_text_equals_ignore_case(argv[2], "warn")) s_trace_level = 1;
        else if (shell_text_equals_ignore_case(argv[2], "info")) s_trace_level = 2;
        else if (shell_text_equals_ignore_case(argv[2], "debug")) s_trace_level = 3;
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "status")) {
        const char *level_str = "none";
        switch (s_trace_level) {
        case 1: level_str = "warn"; break;
        case 2: level_str = "info"; break;
        case 3: level_str = "debug"; break;
        default: break;
        }
        shell_transcript_appendf("trace: %s level=%s\n", s_trace_on ? "on" : "off", level_str);
        batch_set_errorlevel(0);
        return true;
    }

    shell_print_usage("Usage: trace /on|/off|/level|/status");
    batch_set_errorlevel(2);
    return false;
}

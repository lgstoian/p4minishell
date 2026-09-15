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
         * Renders a file's lines as a bordered, selectable panel — the file
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
        /* draw window <id> <x> <y> <w> <h> [title] — alias to box with id ignored */
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

    if (shell_text_equals_ignore_case(argv[1], "image")) {
        /* draw image <file.bmp> <x> <y> <w> <h>  (cells, 1-based): render a BMP
         * into the cell grid as nearest-DOS-color blocks. Reuses the single BMP
         * decoder in components/gfx. */
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;
        FILE *file = NULL;
        long size = 0;
        uint8_t *buf = NULL;
        size_t got = 0;
        gfx_bmp_info_t info;
        gfx_surface_t img = {NULL, 0, 0};
        int x;
        int y;
        int w;
        int h;

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
        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: invalid path %s\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return false;
        }
        if (shell_sd_begin(&session) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: SD card not present\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        file = fopen(resolved, "rb");
        if (file == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: cannot open %s\n" SH_RST, resolved);
            shell_sd_end(&session, "draw image");
            batch_set_errorlevel(1);
            return false;
        }
        if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: cannot size %s\n" SH_RST, resolved);
            fclose(file);
            shell_sd_end(&session, "draw image");
            batch_set_errorlevel(1);
            return false;
        }
        if (size < 54 || (uint64_t)size > P4_CONFIG_IMAGE_MAX_BYTES) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: bad size %ld (max %u bytes)\n" SH_RST,
                                          size, (unsigned)P4_CONFIG_IMAGE_MAX_BYTES);
            fclose(file);
            shell_sd_end(&session, "draw image");
            batch_set_errorlevel(1);
            return false;
        }
        buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) buf = malloc((size_t)size);
        if (buf == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: out of memory\n" SH_RST);
            fclose(file);
            shell_sd_end(&session, "draw image");
            batch_set_errorlevel(1);
            return false;
        }
        got = fread(buf, 1, (size_t)size, file);
        fclose(file);
        shell_sd_end(&session, "draw image");
        if (got != (size_t)size) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: short read %s\n" SH_RST, resolved);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        if (!gfx_bmp_parse_header_ex(buf, got, &info, GFX_IMAGE_MAX_W, GFX_IMAGE_MAX_H)) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: not a 24/32-bit BI_RGB BMP (%s)\n" SH_RST, resolved);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return false;
        }
        if (!gfx_bmp_decode_scaled_565(buf, got, &info, w, h, &img)) {
            shell_transcript_appendf_ansi(SH_ERR "draw image: decode failed (%s)\n" SH_RST, resolved);
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

void shell_command_browse(int argc, char **argv)
{
    char selected_path[P4_CONFIG_TUI_BROWSE_PATH_BYTES];
    const char *start_path = NULL;

    if (argc > 1) {
        start_path = argv[1];
    }

    int result = modal_filebrowser_run("Browse", start_path, selected_path, sizeof(selected_path), 0);

    if (result == 0) {
        shell_transcript_appendf_ansi(SH_OK "selected: %s" SH_RST "\n", selected_path);
    } else {
        shell_transcript_appendf_ansi(SH_MUTE "browse: cancelled" SH_RST "\n");
    }
    batch_set_errorlevel(result == 0 ? 0 : 1);
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
    char *values[MODAL_FORM_MAX_FIELDS];
    const char *vars[MODAL_FORM_MAX_FIELDS];
    const char *title = NULL;
    uint32_t timeout_ms = 0;
    int nf = 0;
    int rc;
    int i;

    memset(fields, 0, sizeof(fields));
    memset(values, 0, sizeof(values));
    memset(vars, 0, sizeof(vars));

    for (i = 1; i < argc; i++) {
        char *spec, *eq, *colon, *arg, *var, *sep;
        char label[80];
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
            continue;
        }
        n = (size_t)(eq - spec);
        if (n >= sizeof(label)) n = sizeof(label) - 1;
        memcpy(label, spec, n);
        label[n] = '\0';

        colon = strchr(eq + 1, ':');
        if (colon == NULL) {
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
            continue;
        }

        f = &fields[nf];
        f->label = label;
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

void shell_command_browse_batch(int argc, char **argv)
{
    const char *varname = "BROWSE_RESULT";
    const char *start_path = NULL;
    uint32_t timeout_ms = 0;
    char selected[P4_CONFIG_TUI_BROWSE_PATH_BYTES];
    int rc;

    for (int i = 1; i < argc; i++) {
        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        } else if (modal_parse_var_arg(argv[i], &varname)) {
            continue;
        } else if (start_path == NULL) {
            start_path = argv[i];
        }
    }

    rc = modal_filebrowser_run("Browse", start_path, selected, sizeof(selected), timeout_ms);
    if (rc != 0) {
        batch_set_errorlevel(1);
        shell_env_set(varname, "");
        return;
    }
    if (shell_env_set(varname, selected) != ESP_OK) {
        shell_print_error("browse: cannot set %s", varname);
        batch_set_errorlevel(1);
        return;
    }
    shell_print_ok("selected: %s", selected);
    batch_set_errorlevel(0);
}

/** Shared image-view entry (used by `view` and `open`): resolve, show in the
 * image viewer, map to ERRORLEVEL. One implementation, no duplicated routing. */
static void view_image_file(const char *path, uint32_t timeout_ms)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];

    if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("view: invalid path %s", path);
        batch_set_errorlevel(2);
        return;
    }
    if (modal_image_run("Image", resolved, timeout_ms, P4_CONFIG_IMAGE_VIEWER_FIT != 0) != 0) {
        shell_print_error("view: cannot open image %s", path);
        batch_set_errorlevel(1);
        return;
    }
    batch_set_errorlevel(0);
}

void shell_command_view(int argc, char **argv)
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
        shell_print_usage("Usage: view [/t:secs] [--raw] <file>");
        batch_set_errorlevel(2);
        return;
    }

    /* .bmp/.dib route to the image viewer; everything else is text (or
     * rendered Markdown). The type test is the central registry. */
    if (filetype_is_image(filetype_of(path))) {
        view_image_file(path, timeout_ms);
        return;
    }

    int rc = modal_viewer_run_raw("View", path, timeout_ms, raw);
    batch_set_errorlevel(rc == 0 ? 0 : 1);
}

void shell_command_open(int argc, char **argv)
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
        shell_print_usage("Usage: open [/t:secs] [--raw] <file>");
        batch_set_errorlevel(2);
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

    if (filetype_is_image(filetype_of(path))) {
        view_image_file(path, timeout_ms);
        return;
    }

    {
        int rc = modal_viewer_run_raw("View", path, timeout_ms, raw);
        batch_set_errorlevel(rc == 0 ? 0 : 1);
    }
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
        shell_print_usage("Usage: tui fullscreen <on|off> | tui status | tui clear");
        batch_set_errorlevel(2);
        return;
    }
    /* `tui status` is a read-only query: safe (and useful) in bg jobs. */
    if (!shell_text_equals_ignore_case(argv[1], "status") &&
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
        shell_print_field("tui", "%s", tui_is_active() ? "active" : "inactive");
        shell_print_field("fullscreen", "%s", tui_is_fullscreen() ? "on" : "off");
        window_rect_t r = windows_get_rect(WINDOW_REGION_TRANSCRIPT);
        shell_print_field("transcript rect", "%dx%d at %d,%d", r.width, r.height, r.x, r.y);
        shell_print_field("cols/rows", "%d x %d", P4_CONFIG_TUI_COLS, P4_CONFIG_TUI_ROWS);
        batch_set_errorlevel(0);
        return;
    } else if (shell_text_equals_ignore_case(argv[1], "clear")) {
        if (tui_is_active()) tui_clear();
        draw_maybe_flush();
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: tui fullscreen <on|off> | tui status | tui clear");
    batch_set_errorlevel(2);
}

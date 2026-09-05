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

#include "shell.h"
#include "batch.h"
#include "modal_surf.h"
#include "tui.h"
#include "windows.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "command.h"
#include "p4minishell_config.h"
#include "esp_err.h"

/* ========================================================================
 * TUI PRIMITIVES: draw, anchor
 * ================================================================ */

bool shell_command_draw(int argc, char **argv)
{
    if (argc < 2) {
        shell_transcript_appendf_ansi(SH_ERR "draw: missing subcommand\n" SH_RST);
        shell_transcript_appendf_ansi("Usage: draw box|text|line|fill|clear|save|restore|cursor|alt-screen <args>\n");
        batch_set_errorlevel(2);
        return false;
    }

    bool use_tui = tui_is_active();
    // Auto-enter TUI for drawing commands that benefit from it (keep header visible)
    if (!use_tui && (shell_text_equals_ignore_case(argv[1], "box") ||
                     shell_text_equals_ignore_case(argv[1], "text") ||
                     shell_text_equals_ignore_case(argv[1], "line") ||
                     shell_text_equals_ignore_case(argv[1], "fill") ||
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
            tui_draw_box(x, y, w, h, style, 7, 16, title);
            tui_flush();
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
            tui_print_at(x, y, text, 7, 16);
            tui_flush();
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
            tui_draw_line(x1, y1, x2, y2, "single", 7, 16);
            tui_flush();
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
            tui_fill(x, y, w, h, fill_char, 7, 16);
            tui_flush();
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
            tui_flush();
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
            tui_flush();
            batch_set_errorlevel(0);
            return true;
        }
        shell_transcript_appendf_ansi(SH_ERR "draw window: tui not active\n" SH_RST);
        batch_set_errorlevel(1);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "close")) {
        if (use_tui) { tui_deinit(); batch_set_errorlevel(0); return true; }
        shell_transcript_appendf_ansi(SH_ERR "draw close: tui not active\n" SH_RST);
        batch_set_errorlevel(1);
        return false;
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

void shell_command_view(int argc, char **argv)
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
        shell_print_usage("Usage: view [/t:secs] <file>");
        batch_set_errorlevel(2);
        return;
    }

    int rc = modal_viewer_run("View", path, timeout_ms);
    batch_set_errorlevel(rc == 0 ? 0 : 1);
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
    int fg = atoi(argv[1]);
    int bg = (argc > 2) ? atoi(argv[2]) : 16;
    if (fg < 0) fg = 0;
    if (fg > 15) fg = 15;
    if (bg < 0) bg = 0;
    if (bg > 15 && bg != 16) bg = 16;
    if (tui_is_active()) {
        tui_set_default_color((uint8_t)fg, (uint8_t)bg);
        tui_flush();
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
    int row = atoi(argv[1]);
    int col = atoi(argv[2]);
    if (row < 1) row = 1;
    if (row > P4_CONFIG_TUI_ROWS) row = P4_CONFIG_TUI_ROWS;
    if (col < 1) col = 1;
    if (col > P4_CONFIG_TUI_COLS) col = P4_CONFIG_TUI_COLS;
    if (tui_is_active()) {
        tui_set_cursor(row, col);
        tui_flush();
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
        tui_flush();
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: tui fullscreen <on|off> | tui status | tui clear");
    batch_set_errorlevel(2);
}

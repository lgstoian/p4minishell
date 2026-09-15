/**
 * @file font_commands.c
 * @brief Font verbs for P4MiniShell (`font info|coverage|list|set|size`).
 *
 * `font coverage` prints labeled glyph rows whose serial bytes are exact, so
 * each phase's expectations are verifiable without a screenshot. `font set`
 * switches roles live (terminal choices are clamp-checked against the 80x25
 * grid); `font size` resizes TTF-backed roles. All choices persist to
 * sd:/APPS/SHELL.INI with /save and restore at first SD mount.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "windows.h"
#include "keyboard.h"
#include "header.h"
#include "tui.h"
#include "editor_view.h"
#include "font.h"
#include "theme.h"
#include "storage.h"
#include "ansi_palette.h"
#include "command.h"
#include "config_cmd.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"

/* SHELL.INI keys for the saved font choice (sd:/APPS/SHELL.INI). */
#define FONT_INI_KEY_TERMINAL      "font_terminal"
#define FONT_INI_KEY_UI            "font_ui"
#define FONT_INI_KEY_TERMINAL_SIZE "font_terminal_size"
#define FONT_INI_KEY_UI_SIZE       "font_ui_size"
#define THEME_INI_KEY              "theme"

static void font_shell_ini_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/APPS/SHELL.INI", BSP_SD_MOUNT_POINT);
}

/** Push the new chains onto every live widget after a successful switch.
 * Each callee no-ops when its surface is absent/closed and locks itself. */
static void font_refresh_live(void)
{
    windows_refresh_fonts();
    keyboard_refresh_fonts();
    header_refresh_fonts();
    tui_refresh_fonts();
    editor_view_refresh_fonts();
}

/** Re-apply the active theme to every live chrome surface. Modal surfaces
 * read the table when they open, so an open modal keeps its colors. */
static void theme_refresh_live(void)
{
    windows_refresh_theme();
    keyboard_refresh_theme();
    header_refresh_theme();
}

/** Transcript rect with keyboard-hidden height (width is keyboard-invariant;
 * rows are clamped against the space TUI apps actually get). */
static window_rect_t font_clamp_rect(void)
{
    window_rect_t rect = windows_get_rect(WINDOW_REGION_TRANSCRIPT);

    rect.height = windows_get_display_height()
        - windows_get_rect(WINDOW_REGION_HEADER).height
        - windows_get_rect(WINDOW_REGION_INPUT_ROW).height;
    return rect;
}

/** Persist both roles (names + sizes) to CONFIG.SYS. The settings store is
 * single-sourced there; SHELL.INI is only read as a legacy fallback. */
static bool font_save_current(void)
{
    char size_buf[16];

    if (!config_persist_set("FONT_TERMINAL", font_current_name(FONT_ROLE_TERMINAL)) ||
        !config_persist_set("FONT_UI", font_current_name(FONT_ROLE_UI))) {
        return false;
    }
    /* Sizes are best-effort (names already saved); keep going on failure. */
    snprintf(size_buf, sizeof(size_buf), "%d", font_current_size(FONT_ROLE_TERMINAL));
    config_persist_set("FONT_TERMINAL_SIZE", size_buf);
    snprintf(size_buf, sizeof(size_buf), "%d", font_current_size(FONT_ROLE_UI));
    config_persist_set("FONT_UI_SIZE", size_buf);
    return true;
}

/** Persist the active theme name to CONFIG.SYS. */
static bool theme_save_current(void)
{
    return config_persist_set("THEME", theme_current()->name);
}

/** Read a saved preference: CONFIG.SYS first, then the legacy SHELL.INI file.
 * @return true when @p out holds a value. */
static bool font_pref_get(const char *path, const char *key,
                          char *out, size_t out_size)
{
    if (out_size > 0) {
        out[0] = '\0';
    }
    if (config_get_saved(key, out, out_size) >= 0) {
        return true;
    }
    return storage_ini_file_get(path, key, out, out_size) == ESP_OK;
}

/** Best-effort boot restore of the saved choice. Silent when nothing is saved
 * or the SD is unavailable — defaults stand. Called by boot_on_sd_first_mount(),
 * where the mount is guaranteed.
 * @return true when the restore completed (or there was nothing saved);
 *         false when the SD was not readable, so the caller retries on the
 *         next mount instead of skipping the restore for the whole boot. */
bool font_restore_saved(void)
{
    char path[P4_CONFIG_SD_PATH_BYTES];
    char terminal[P4_CONFIG_FONT_NAME_BYTES];
    char ui[P4_CONFIG_FONT_NAME_BYTES];
    char size_buf[16];
    char theme_name[32];
    bool have_terminal = false;
    bool have_ui = false;

    font_shell_ini_path(path, sizeof(path));

    /* Theme restores first (color-only); the live refresh no-ops until the
     * chrome exists. */
    if (font_pref_get(path, THEME_INI_KEY, theme_name, sizeof(theme_name))) {
        if (theme_set(theme_name)) {
            theme_refresh_live();
        }
    }

    /* Header layout mode (auto/full/compact) restores alongside the theme. */
    header_restore_saved();

    if (font_pref_get(path, FONT_INI_KEY_TERMINAL, terminal, sizeof(terminal))) {
        have_terminal = true;
    }
    if (font_pref_get(path, FONT_INI_KEY_UI, ui, sizeof(ui))) {
        have_ui = true;
    }
    if (have_terminal || have_ui) {
        font_restore(have_terminal ? terminal : NULL, have_ui ? ui : NULL);
    }
    /* Sizes restore after names (a size needs its TTF selected first).
     * Terminal sizes are clamp-checked (a saved size may postdate a
     * rotation); invalid values are ignored silently — defaults stand. */
    if (font_pref_get(path, FONT_INI_KEY_TERMINAL_SIZE, size_buf, sizeof(size_buf))) {
        int px = atoi(size_buf);
        if (px > 0) {
            window_rect_t rect = font_clamp_rect();
            int max_px = font_terminal_max_px(font_current_name(FONT_ROLE_TERMINAL),
                                              rect.width, rect.height);
            if (max_px < 0 || px <= max_px) {
                font_set_size(FONT_ROLE_TERMINAL, px);
            }
        }
    }
    if (font_pref_get(path, FONT_INI_KEY_UI_SIZE, size_buf, sizeof(size_buf))) {
        int px = atoi(size_buf);
        if (px > 0) {
            font_set_size(FONT_ROLE_UI, px);
        }
    }
    /* Report the mount state (the original contract): an unreadable card makes
     * the caller re-arm the one-shot so the next mount retries. */
    return storage_sd_is_mounted();
}

static void shell_command_font_usage(void)
{
    shell_transcript_appendf_ansi("Usage: font info | font coverage | font list | font set <terminal|ui> <name> [/save] | font size <terminal|ui> <px> [/save]\n");
    shell_transcript_appendf_ansi("  info      - font roles, line heights, fallback state\n");
    shell_transcript_appendf_ansi("  coverage  - labeled glyph rows ([have] renders, [want:P2] tofu until SD TTFs)\n");
    shell_transcript_appendf_ansi("  list      - built-ins + sd:/FONTS/*.ttf with per-role sizes\n");
    shell_transcript_appendf_ansi("  set       - switch a role live; terminal TTFs are clamp-checked to 80x25\n");
    shell_transcript_appendf_ansi("  size      - resize a TTF role 10..28px (bitmaps refuse); terminal clamped\n");
}

static void shell_command_font_info(void)
{
    const lv_font_t *term = windows_get_terminal_font();
    const lv_font_t *ui = windows_get_ui_font();

    shell_transcript_appendf("font roles:\n");
    shell_transcript_appendf("  transcript : %s (pure; TUI cell metrics exact)\n",
                             font_current_name(FONT_ROLE_TERMINAL));
    shell_transcript_appendf("  tui        : %s (monospace-only rule)\n",
                             font_current_name(FONT_ROLE_TERMINAL));
    shell_transcript_appendf("  input      : %s chain\n", font_current_name(FONT_ROLE_UI));
    shell_transcript_appendf("  header     : chain\n");
    shell_transcript_appendf("  keyboard   : chain\n");
    shell_transcript_appendf("  buttons    : chain\n");
    shell_transcript_appendf("line heights: terminal=%ld ui=%ld\n",
                             (long)lv_font_get_line_height(term),
                             (long)lv_font_get_line_height(ui));
    shell_transcript_appendf("fallback: montserrat_14 (FontAwesome PUA subset); TTFs load from sd:/FONTS/ on demand\n");
    batch_set_errorlevel(0);
}

static void shell_command_font_coverage(void)
{
    /* Literal rows go through append_text (no printf-format risk from %
     * signs or multibyte sequences). Serial bytes are exact; screen tofu on
     * [want:P2] rows is expected until Phase 2. */
    shell_transcript_append_text("font coverage - [have]=renders [want:P2]=tofu until SD TTFs\n");
    shell_transcript_append_text("[have] ascii: ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefg 0123456789 !@#$%^&*()\n");
    shell_transcript_append_text("[have] box: \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90\xe2\x94\x82\xe2\x94\x94\xe2\x94\x98 \xe2\x95\x94\xe2\x95\x90\xe2\x95\x97\xe2\x95\x91\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d \xe2\x94\x80\xe2\x94\x82\xe2\x94\xbc\n");
    shell_transcript_append_text("[have] symbols: \xe2\x98\x80\xe2\x98\x81\xe2\x98\x85\xe2\x98\x86\xe2\x97\x8b\xe2\x97\x8f\xe2\x97\x86\xe2\x96\xa0\xe2\x96\xb2\xe2\x96\xb6\xe2\x97\x86\n");
    shell_transcript_append_text("[want:P2] currency/math: \xe2\x82\xac\xc2\xa3\xc2\xa5\xc3\x97\xc3\xb7\xc2\xb1\xc2\xb0\xc2\xb2\xc2\xb3\n");
    shell_transcript_append_text("[want:P2] arrows: \xe2\x86\x90\xe2\x86\x91\xe2\x86\x92\xe2\x86\x93\xe2\x86\x94\n");
    shell_transcript_append_text("[have:P3] cjk-a (NotoSansSC auto-attaches; tofu without it): \xe4\xb8\xad\xe6\x96\x87\xe6\x97\xa5\xe6\x9c\xac\n");
    shell_transcript_append_text("[have:P3] cjk-b: \xe8\xaa\x9e\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4\n");
    shell_transcript_append_text("icons: keyboard/header FA icons via Montserrat fallback - screenshot-verify\n");
    batch_set_errorlevel(0);
}

void shell_command_font(int argc, char **argv)
{
    if (argc < 2) {
        shell_command_font_usage();
        batch_set_errorlevel(2);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "info")) {
        shell_command_font_info();
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "coverage")) {
        shell_command_font_coverage();
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "list")) {
        char stems[16][P4_CONFIG_FONT_NAME_BYTES];
        int ntts;
        int i;

        shell_transcript_appendf("fonts (built-in bitmaps + sd:/FONTS/):\n");
        for (i = 0; i < font_builtin_count(); i++) {
            shell_transcript_appendf("  %s%s\n",
                                     font_builtin_name(i),
                                     font_builtin_monospace(i) ? " (monospace)" : " (proportional)");
        }
        ntts = font_scan_ttf(stems, 16);
        for (i = 0; i < ntts; i++) {
            shell_transcript_appendf("  %s (ttf)\n", stems[i]);
        }
        if (ntts == 0) {
            shell_transcript_appendf("  (no sd:/FONTS/*.ttf found - push with push_fonts.py)\n");
        }
        shell_transcript_appendf("roles: terminal=%s@%d ui=%s@%d\n",
                                 font_current_name(FONT_ROLE_TERMINAL),
                                 font_current_size(FONT_ROLE_TERMINAL),
                                 font_current_name(FONT_ROLE_UI),
                                 font_current_size(FONT_ROLE_UI));
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "set")) {
        font_role_t role;
        bool save = false;
        int i;

        if (argc < 4) {
            shell_transcript_appendf_ansi(SH_ERR "font set: usage: font set <terminal|ui> <name> [/save]\n" SH_RST);
            batch_set_errorlevel(2);
            return;
        }
        if (shell_text_equals_ignore_case(argv[2], "terminal")) {
            role = FONT_ROLE_TERMINAL;
        } else if (shell_text_equals_ignore_case(argv[2], "ui")) {
            role = FONT_ROLE_UI;
        } else {
            shell_transcript_appendf_ansi(SH_ERR "font set: unknown role '%s' (terminal|ui)\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        for (i = 4; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/save")) {
                save = true;
            }
        }
        /* Terminal gates, in refusal-priority order: known name, monospace,
         * then grid fit. The checks run before switching so a refusal leaves
         * the live font untouched. */
        if (role == FONT_ROLE_TERMINAL) {
            window_rect_t rect = font_clamp_rect();
            int max_px;
            int want_px = font_current_size(role);
            if (!font_is_monospace(argv[3])) {
                shell_transcript_appendf_ansi(SH_ERR "font set: '%s' is unknown or not monospace (terminal needs monospace)\n" SH_RST, argv[3]);
                batch_set_errorlevel(1);
                return;
            }
            max_px = font_terminal_max_px(argv[3], rect.width, rect.height);
            if (max_px < 0) {
                shell_transcript_appendf_ansi(SH_ERR "font set: cannot load '%s'\n" SH_RST, argv[3]);
                batch_set_errorlevel(1);
                return;
            }
            if (want_px > max_px) {
                shell_transcript_appendf_ansi(SH_ERR "font set: '%s' at %dpx breaks 80x25 (max %dpx here)\n" SH_RST,
                                              argv[3], want_px, max_px);
                batch_set_errorlevel(1);
                return;
            }
        }
        if (!font_set(role, argv[3])) {
            shell_transcript_appendf_ansi(SH_ERR "font set: unknown font '%s' (or not monospace for terminal)\n" SH_RST, argv[3]);
            batch_set_errorlevel(1);
            return;
        }
        if (save && !font_save_current()) {
            shell_transcript_appendf_ansi(SH_ERR "font set: applied, but could not save SHELL.INI\n" SH_RST);
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf("font: %s -> %s@%d%s\n", font_role_name(role),
                                 font_current_name(role), font_current_size(role),
                                 save ? " (saved)" : "");
        font_refresh_live();
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "size")) {
        font_role_t role;
        bool save = false;
        int i;
        int px;

        if (argc < 4) {
            shell_transcript_appendf_ansi(SH_ERR "font size: usage: font size <terminal|ui> <px 10..28> [/save]\n" SH_RST);
            batch_set_errorlevel(2);
            return;
        }
        if (shell_text_equals_ignore_case(argv[2], "terminal")) {
            role = FONT_ROLE_TERMINAL;
        } else if (shell_text_equals_ignore_case(argv[2], "ui")) {
            role = FONT_ROLE_UI;
        } else {
            shell_transcript_appendf_ansi(SH_ERR "font size: unknown role '%s' (terminal|ui)\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        px = atoi(argv[3]);
        for (i = 4; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/save")) {
                save = true;
            }
        }
        if (px < P4_CONFIG_FONT_SIZE_MIN || px > P4_CONFIG_FONT_SIZE_MAX) {
            shell_transcript_appendf_ansi(SH_ERR "font size: %d out of range %d..%d\n" SH_RST, px,
                                          P4_CONFIG_FONT_SIZE_MIN, P4_CONFIG_FONT_SIZE_MAX);
            batch_set_errorlevel(2);
            return;
        }
        if (role == FONT_ROLE_TERMINAL) {
            window_rect_t rect = font_clamp_rect();
            int max_px = font_terminal_max_px(font_current_name(role),
                                              rect.width, rect.height);
            if (max_px >= 0 && px > max_px) {
                shell_transcript_appendf_ansi(SH_ERR "font size: %dpx breaks 80x25 (max %dpx here)\n" SH_RST,
                                              px, max_px);
                batch_set_errorlevel(1);
                return;
            }
        }
        if (!font_set_size(role, px)) {
            shell_transcript_appendf_ansi(SH_ERR "font size: '%s' is a fixed-size bitmap (sizes need an SD TTF)\n" SH_RST,
                                          font_current_name(role));
            batch_set_errorlevel(1);
            return;
        }
        if (save && !font_save_current()) {
            shell_transcript_appendf_ansi(SH_ERR "font size: applied, but could not save SHELL.INI\n" SH_RST);
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf("font: %s size -> %d%s\n", font_role_name(role),
                                 px, save ? " (saved)" : "");
        font_refresh_live();
        batch_set_errorlevel(0);
        return;
    }
    shell_transcript_appendf_ansi(SH_ERR "font: unknown subcommand '%s'\n" SH_RST, argv[1]);
    shell_command_font_usage();
    batch_set_errorlevel(2);
}

static void shell_command_theme_usage(void)
{
    shell_transcript_appendf_ansi("Usage: theme list | theme show [name] | theme set <name> [/save]\n");
}

static void theme_print(const theme_t *theme, bool active)
{
    shell_transcript_appendf("theme: %s%s\n", theme->name, active ? " (active)" : "");
    if (theme->description != NULL) {
        shell_transcript_appendf("  %s\n", theme->description);
    }
    shell_transcript_appendf("  bg: screen=%06X transcript=%06X input_row=%06X keyboard=%06X\n",
                             (unsigned)theme->bg_screen, (unsigned)theme->bg_transcript,
                             (unsigned)theme->bg_input_row, (unsigned)theme->bg_keyboard);
    shell_transcript_appendf("  header: panel=%06X sys=%06X\n",
                             (unsigned)theme->header_panel_bg, (unsigned)theme->header_sys_bg);
    shell_transcript_appendf("  text: accent=%06X body=%06X muted=%06X warn=%06X\n",
                             (unsigned)theme->text, (unsigned)theme->text_body,
                             (unsigned)theme->text_muted, (unsigned)theme->warn);
    shell_transcript_appendf("  modal: border=%06X title=%06X message=%06X\n",
                             (unsigned)theme->modal_panel_border,
                             (unsigned)theme->modal_title, (unsigned)theme->modal_message);
    shell_transcript_appendf("  fonts: terminal=%s@%d ui=%s@%d\n",
                             theme->terminal_font, theme->terminal_px,
                             theme->ui_font, theme->ui_px);
}

void shell_command_theme(int argc, char **argv)
{
    const theme_t *cur = theme_current();

    if (argc < 2 || shell_text_equals_ignore_case(argv[1], "show")) {
        const theme_t *theme = cur;

        if (argc >= 3) {
            theme = theme_get(argv[2]);
            if (theme == NULL) {
                shell_transcript_appendf_ansi(SH_ERR "theme show: unknown theme '%s'\n" SH_RST, argv[2]);
                batch_set_errorlevel(2);
                return;
            }
        }
        theme_print(theme, theme == cur);
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "list")) {
        int i;

        for (i = 0; i < theme_builtin_count(); i++) {
            const theme_t *theme = theme_builtin_at(i);

            shell_transcript_appendf("  %s %-8s %s\n",
                                     (i == theme_active_index()) ? "*" : " ",
                                     theme->name,
                                     (theme->description != NULL) ? theme->description : "");
        }
        shell_transcript_appendf("active: %s (theme set <name> [/save] to switch)\n", cur->name);
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "set")) {
        bool save = false;
        bool saved_ok = true;
        int i;

        if (argc < 3) {
            shell_command_theme_usage();
            batch_set_errorlevel(2);
            return;
        }
        for (i = 3; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/save")) {
                save = true;
            }
        }
        if (!theme_set(argv[2])) {
            shell_transcript_appendf_ansi(SH_ERR "theme set: unknown theme '%s' (theme list)\n" SH_RST, argv[2]);
            batch_set_errorlevel(1);
            return;
        }
        if (save) {
            saved_ok = theme_save_current();
        }
        theme_refresh_live();
        shell_transcript_appendf("theme: -> %s%s\n", theme_current()->name,
                                 save ? (saved_ok ? " (saved)" : " (save failed)") : "");
        batch_set_errorlevel(saved_ok ? 0 : 1);
        return;
    }
    shell_command_theme_usage();
    batch_set_errorlevel(2);
}

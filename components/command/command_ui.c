/**
 * @file command_ui.c
 * @brief UI query command handlers for display, keyboard, and windows.
 *
 * These handlers live in their own translation unit so that the main
 * command.c does not pull in display.h, keyboard.h, and windows.h
 * transitively. Each handler delegates to the owning module's public API.
 */

#include "command_ui.h"
#include "shell.h"
#include "ansi_palette.h"
#include "display.h"
#include "keyboard.h"
#include "windows.h"
#include "editor_view.h"
#include "config_cmd.h"
#include "p4minishell_config.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================================================================
 * DISPLAY COMMANDS
 * ======================================================================== */

bool shell_command_display(int argc, char **argv)
{
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "info")) {
        display_print_info(shell_transcript_appendf);
        return true;
    }
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "resolution")) {
        display_resolution_t res = display_get_resolution();
        shell_transcript_appendf_ansi(SH_LBL "display.resolution:" SH_RST " %" PRId32 " x %" PRId32
                                 " (native %" PRId32 " x %" PRId32 ")\n",
                                 res.current_width, res.current_height,
                                 res.native_width, res.native_height);
        return true;
    }
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "refresh")) {
        display_refresh_config_t ref = display_get_refresh_config();
        shell_transcript_appendf_ansi(SH_LBL "display.refresh:" SH_RST " target=%" PRIu32 "Hz current=%" PRIu32 "Hz "
                                 "pclk=%" PRIu32 "MHz dsi_bitrate=%" PRIu32 "Mbps\n",
                                 ref.target_hz, ref.current_hz,
                                 ref.pixel_clock_mhz, ref.dsi_lane_bitrate_mbps);
        return true;
    }
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "power")) {
        if (argc >= 3) {
            if (shell_text_equals_ignore_case(argv[2], "on")) {
                display_set_power_state(DISPLAY_POWER_ON);
                shell_transcript_appendf_ansi(SH_OK "display power on" SH_RST "\n");
            } else if (shell_text_equals_ignore_case(argv[2], "sleep")) {
                display_set_power_state(DISPLAY_POWER_SLEEP);
                shell_transcript_appendf_ansi(SH_WARN "display sleep" SH_RST "\n");
            } else if (shell_text_equals_ignore_case(argv[2], "off")) {
                display_set_power_state(DISPLAY_POWER_OFF);
                shell_transcript_appendf_ansi(SH_ERR "display power off" SH_RST "\n");
            } else {
                shell_transcript_appendf_ansi(SH_WARN "Usage: display power <on|sleep|off>" SH_RST "\n");
            }
        } else {
            display_power_state_t ps = display_get_power_state();
            if (ps == DISPLAY_POWER_ON) {
                shell_transcript_appendf_ansi(SH_LBL "display.power:" SH_RST " " SH_OK "on" SH_RST "\n");
            } else if (ps == DISPLAY_POWER_SLEEP) {
                shell_transcript_appendf_ansi(SH_LBL "display.power:" SH_RST " " SH_WARN "sleep" SH_RST "\n");
            } else {
                shell_transcript_appendf_ansi(SH_LBL "display.power:" SH_RST " " SH_ERR "off" SH_RST "\n");
            }
        }
        return true;
    }
    shell_transcript_appendf_ansi(SH_WARN "Usage: display <info|resolution|refresh|power>" SH_RST "\n");
    return true;
}

/* ========================================================================
 * KEYBOARD COMMANDS
 * ======================================================================== */

bool shell_command_keyboard(int argc, char **argv)
{
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "hide")) {
        keyboard_hide();
        shell_transcript_appendf_ansi(SH_OK "keyboard hidden" SH_RST "\n");
    } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "show")) {
        keyboard_show();
        shell_transcript_appendf_ansi(SH_OK "keyboard shown" SH_RST "\n");
    } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "toggle")) {
        keyboard_toggle();
        shell_transcript_appendf_ansi(SH_OK "keyboard %s" SH_RST "\n", keyboard_is_visible() ? "shown" : "hidden");
    } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "status")) {
        /* Colours must be literal in the format string for ansi_vformat to
         * convert them — @-specifiers in a %s argument are not converted. */
        if (keyboard_is_visible()) {
            shell_transcript_appendf_ansi(SH_LBL "keyboard:" SH_RST " " SH_OK "visible" SH_RST
                                     ", mode=%d, height=%" PRId32 ", external=%s\n",
                                     (int)keyboard_get_mode(),
                                     (int32_t)keyboard_get_height(),
                                     keyboard_is_external_input_enabled() ? "on" : "off");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "keyboard:" SH_RST " " SH_MUTE "hidden" SH_RST
                                     ", mode=%d, height=%" PRId32 ", external=%s\n",
                                     (int)keyboard_get_mode(),
                                     (int32_t)keyboard_get_height(),
                                     keyboard_is_external_input_enabled() ? "on" : "off");
        }
    } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "nav")) {
        /* Batch apps request the symbols page's Nav key: `keyboard nav on`
         * before using the editor navigation page, `keyboard nav off` after.
         * The request is reference-counted and ORed with the shell context. */
        if (argc == 2) {
            uint32_t caps = keyboard_effective_capabilities();

            shell_transcript_appendf("keyboard.nav=%s\n",
                                     (caps & KEYBOARD_CAP_NAV) ? "on" : "off");
        } else if (argc == 3 &&
                   (shell_text_equals_ignore_case(argv[2], "on") ||
                    shell_text_equals_ignore_case(argv[2], "off"))) {
            bool on = shell_text_equals_ignore_case(argv[2], "on");

            keyboard_request_capability(KEYBOARD_CAP_NAV, on);
            shell_transcript_appendf("keyboard.nav=%s\n", on ? "on" : "off");
        } else {
            shell_transcript_appendf_ansi(SH_WARN "Usage: keyboard nav [on|off]" SH_RST "\n");
        }
    } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "mode")) {
        if (argc == 2) {
            shell_transcript_appendf("keyboard.page=%s\n",
                                     keyboard_mode_name(keyboard_get_mode()));
        } else if (argc == 3) {
            keyboard_mode_t mode;

            if (!keyboard_mode_parse(argv[2], &mode)) {
                shell_transcript_appendf_ansi(SH_ERR "keyboard mode: unknown page '%s' (text_lower|text_upper|number|symbols|nav|nav2)\n" SH_RST,
                                              argv[2]);
            } else {
                keyboard_set_mode(mode);
                shell_transcript_appendf("keyboard.page=%s\n", keyboard_mode_name(mode));
            }
        } else {
            shell_transcript_appendf_ansi(SH_WARN "Usage: keyboard <show|hide|toggle|status|mode [page]|nav [on|off]>" SH_RST "\n");
        }
    } else {
        shell_transcript_appendf_ansi(SH_WARN "Usage: keyboard <show|hide|toggle|status|mode [page]|nav [on|off]>" SH_RST "\n");
    }
    return true;
}

/* ========================================================================
 * WINDOW MANAGER COMMANDS
 * ======================================================================== */

bool shell_command_windows(int argc, char **argv)
{
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "info")) {
        lv_coord_t dw = windows_get_display_width();
        lv_coord_t dh = windows_get_display_height();
        shell_transcript_appendf_ansi(SH_LBL "windows.display:" SH_RST " %" PRId32 " x %" PRId32 "\n", (int32_t)dw, (int32_t)dh);
        for (int r = 0; r < WINDOW_REGION_COUNT; r++) {
            window_rect_t rect = windows_get_rect((window_region_t)r);
            const char *names[] = {"header", "transcript", "input_row", "keyboard"};
            shell_transcript_appendf_ansi("  " SH_LBL "windows.%s:" SH_RST " x=%" PRId32 " y=%" PRId32 " w=%" PRId32 " h=%" PRId32 "\n",
                                     names[r], (int32_t)rect.x, (int32_t)rect.y,
                                     (int32_t)rect.width, (int32_t)rect.height);
        }
        return true;
    }
    shell_transcript_appendf_ansi(SH_WARN "Usage: windows <info>" SH_RST "\n");
    return true;
}

/* ========================================================================
 * CURSOR COMMAND
 * ======================================================================== */

/* Live input-cursor state (session-only; defaults from P4_CONFIG). */
static bool s_cursor_block = true;
static uint32_t s_cursor_blink_ms = P4_CONFIG_CURSOR_BLINK_MS;

bool shell_command_cursor(int argc, char **argv)
{
    /* Session-only live state (defaults from P4_CONFIG). */
    int i = 1;
    bool changed = false;

    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "/b")) {
        shell_transcript_appendf("CURSOR=%s\n", s_cursor_block ? "block" : "bar");
        shell_transcript_appendf("CURSOR_BLINK=%lu\n",
                                 s_cursor_blink_ms == 0 ? (unsigned long)0 : (unsigned long)s_cursor_blink_ms);
        return true;
    }

    if (argc < 2) {
        shell_transcript_appendf_ansi(SH_LBL "cursor:" SH_RST " %s, blink %s\n",
                                      s_cursor_block ? "block" : "bar",
                                      s_cursor_blink_ms == 0 ? "off" : "on");
        shell_transcript_appendf_ansi(SH_WARN "Usage: cursor [block|bar] [blink <ms 0..2000|off>] [status /b]" SH_RST "\n");
        return true;
    }
    while (i < argc) {
        if (shell_text_equals_ignore_case(argv[i], "block") ||
            shell_text_equals_ignore_case(argv[i], "bar")) {
            s_cursor_block = shell_text_equals_ignore_case(argv[i], "block");
            windows_input_cursor_style(s_cursor_block);
            config_persist_set("CURSOR", s_cursor_block ? "block" : "bar");
            shell_transcript_appendf_ansi(SH_OK "cursor %s" SH_RST "\n",
                                          s_cursor_block ? "block" : "bar");
            changed = true;
            i++;
        } else if (shell_text_equals_ignore_case(argv[i], "blink") && i + 1 < argc) {
            if (shell_text_equals_ignore_case(argv[i + 1], "off")) {
                s_cursor_blink_ms = 0;
            } else {
                char *end = NULL;
                long ms = strtol(argv[i + 1], &end, 10);
                if (end == NULL || *end != '\0' || ms < 0 || ms > 2000) {
                    shell_transcript_appendf_ansi(SH_WARN "Usage: cursor blink <ms 0..2000|off>" SH_RST "\n");
                    return true;
                }
                s_cursor_blink_ms = (uint32_t)ms;
            }
            windows_input_cursor_blink(s_cursor_blink_ms);
            editor_view_set_blink_ms(s_cursor_blink_ms);
            config_persist_set("CURSOR_BLINK", s_cursor_blink_ms == 0 ? "off" : argv[i + 1]);
            shell_transcript_appendf_ansi(SH_OK "cursor blink %s" SH_RST "\n",
                                          s_cursor_blink_ms == 0 ? "off" : argv[i + 1]);
            changed = true;
            i += 2;
        } else if (shell_text_equals_ignore_case(argv[i], "status")) {
            shell_transcript_appendf_ansi(SH_LBL "cursor:" SH_RST " %s, blink %s\n",
                                          s_cursor_block ? "block" : "bar",
                                          s_cursor_blink_ms == 0 ? "off" : "on");
            i++;
        } else {
            shell_transcript_appendf_ansi(SH_WARN "Usage: cursor [block|bar] [blink <ms 0..2000|off>] [status /b]" SH_RST "\n");
            return true;
        }
    }
    (void)changed;
    return true;
}

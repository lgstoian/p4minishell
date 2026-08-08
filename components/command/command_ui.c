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
#include "p4minishell_config.h"
#include <inttypes.h>
#include <stdio.h>

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
            shell_transcript_appendf_ansi(SH_LBL "display.power:" SH_RST " %s\n",
                                     ps == DISPLAY_POWER_ON ? SH_OK "on" SH_RST :
                                     ps == DISPLAY_POWER_SLEEP ? SH_WARN "sleep" SH_RST : SH_ERR "off" SH_RST);
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
        shell_transcript_appendf_ansi(SH_LBL "keyboard:" SH_RST " %s, mode=%d, height=%" PRId32 "\n",
                                 keyboard_is_visible() ? SH_OK "visible" SH_RST : SH_MUTE "hidden" SH_RST,
                                 (int)keyboard_get_mode(),
                                 (int32_t)keyboard_get_height());
    } else {
        shell_transcript_appendf_ansi(SH_WARN "Usage: keyboard <show|hide|toggle|status>" SH_RST "\n");
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

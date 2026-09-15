/**
 * @file theme.c
 * @brief Built-in UI themes + active selection. Pure data, no LVGL/IO.
 */

#include "theme.h"
#include "p4minishell_config.h"

#include <string.h>
#include <strings.h>

static const theme_t s_themes[] = {
    {
        .name = "default",
        .description = "Forest green CRT (the original palette)",
        .bg_screen = 0x0B0F10,
        .bg_transcript = 0x050806,
        .bg_input_row = 0x111816,
        .bg_keyboard = 0x1D2625,
        .text = P4_CONFIG_HEADER_ACCENT_COLOR,
        .text_body = P4_CONFIG_HEADER_TEXT_COLOR,
        .text_muted = P4_CONFIG_HEADER_MUTED_COLOR,
        .warn = P4_CONFIG_HEADER_WARN_COLOR,
        .err = 0xFF5A5A,
        .header_panel_bg = 0x1A2A22,
        .header_sys_bg = 0x16241E,
        .modal_panel_border = 0x335577,
        .modal_title = 0x8DFF96,
        .modal_message = 0xC7FFD0,
        .terminal_font = P4_CONFIG_FONT_TERMINAL_DEFAULT,
        .ui_font = P4_CONFIG_FONT_UI_DEFAULT,
        .terminal_px = P4_CONFIG_FONT_DEFAULT_PX,
        .ui_px = P4_CONFIG_FONT_DEFAULT_PX,
    },
    {
        .name = "amber",
        .description = "Amber phosphor terminal",
        .bg_screen = 0x120C05,
        .bg_transcript = 0x0A0703,
        .bg_input_row = 0x1A1008,
        .bg_keyboard = 0x2A1B0C,
        .text = 0xFFB000,
        .text_body = 0xFFD98A,
        .text_muted = 0x7A5A2E,
        .warn = 0xFF6A00,
        .err = 0xFF4D4D,
        .header_panel_bg = 0x2A1B0C,
        .header_sys_bg = 0x22160A,
        .modal_panel_border = 0x7A5A2E,
        .modal_title = 0xFFC24D,
        .modal_message = 0xFFE3B0,
        .terminal_font = P4_CONFIG_FONT_TERMINAL_DEFAULT,
        .ui_font = P4_CONFIG_FONT_UI_DEFAULT,
        .terminal_px = P4_CONFIG_FONT_DEFAULT_PX,
        .ui_px = P4_CONFIG_FONT_DEFAULT_PX,
    },
    {
        .name = "ice",
        .description = "Cool blue terminal",
        .bg_screen = 0x050A12,
        .bg_transcript = 0x02060C,
        .bg_input_row = 0x0A1420,
        .bg_keyboard = 0x122334,
        .text = 0x7FE9FF,
        .text_body = 0xD6F2FF,
        .text_muted = 0x4E6B85,
        .warn = 0xFFCC66,
        .err = 0xFF6B6B,
        .header_panel_bg = 0x122334,
        .header_sys_bg = 0x0E1C2A,
        .modal_panel_border = 0x2E5A7A,
        .modal_title = 0x9BE8FF,
        .modal_message = 0xCDEAFF,
        .terminal_font = P4_CONFIG_FONT_TERMINAL_DEFAULT,
        .ui_font = P4_CONFIG_FONT_UI_DEFAULT,
        .terminal_px = P4_CONFIG_FONT_DEFAULT_PX,
        .ui_px = P4_CONFIG_FONT_DEFAULT_PX,
    },
    {
        .name = "mono",
        .description = "Neutral grey",
        .bg_screen = 0x0A0A0A,
        .bg_transcript = 0x050505,
        .bg_input_row = 0x141414,
        .bg_keyboard = 0x1E1E1E,
        .text = 0xE0E0E0,
        .text_body = 0xF0F0F0,
        .text_muted = 0x808080,
        .warn = 0xFFB000,
        .err = 0xE05555,
        .header_panel_bg = 0x1E1E1E,
        .header_sys_bg = 0x181818,
        .modal_panel_border = 0x555555,
        .modal_title = 0xFFFFFF,
        .modal_message = 0xDDDDDD,
        .terminal_font = P4_CONFIG_FONT_TERMINAL_DEFAULT,
        .ui_font = P4_CONFIG_FONT_UI_DEFAULT,
        .terminal_px = P4_CONFIG_FONT_DEFAULT_PX,
        .ui_px = P4_CONFIG_FONT_DEFAULT_PX,
    },
};

#define THEME_COUNT ((int)(sizeof(s_themes) / sizeof(s_themes[0])))

static int s_active = 0;

int theme_builtin_count(void)
{
    return THEME_COUNT;
}

const theme_t *theme_builtin_at(int index)
{
    if (index < 0 || index >= THEME_COUNT) return NULL;
    return &s_themes[index];
}

const theme_t *theme_current(void)
{
    return &s_themes[s_active];
}

int theme_active_index(void)
{
    return s_active;
}

const theme_t *theme_get(const char *name)
{
    int i;

    if (name == NULL) return NULL;
    for (i = 0; i < THEME_COUNT; i++) {
        if (strcasecmp(name, s_themes[i].name) == 0) {
            return &s_themes[i];
        }
    }
    return NULL;
}

bool theme_set(const char *name)
{
    int i;

    if (name == NULL) return false;
    for (i = 0; i < THEME_COUNT; i++) {
        if (strcasecmp(name, s_themes[i].name) == 0) {
            s_active = i;
            return true;
        }
    }
    return false;
}

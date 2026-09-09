/**
 * @file theme.c
 * @brief Theme table prep (Phase 3 light): one struct, zero visual change.
 */

#include "theme.h"
#include "p4minishell_config.h"

#include <string.h>

static const theme_t s_theme_default = {
    .name = "default",
    .bg_screen = 0x0B0F10,
    .bg_transcript = 0x050806,
    .bg_input_row = 0x111816,
    .bg_keyboard = 0x1D2625,
    .text = P4_CONFIG_HEADER_ACCENT_COLOR,
    .text_muted = P4_CONFIG_HEADER_MUTED_COLOR,
    .modal_panel_border = 0x335577,
    .modal_title = 0x8DFF96,
    .modal_message = 0xC7FFD0,
    .terminal_font = P4_CONFIG_FONT_TERMINAL_DEFAULT,
    .ui_font = P4_CONFIG_FONT_UI_DEFAULT,
    .terminal_px = P4_CONFIG_FONT_DEFAULT_PX,
    .ui_px = P4_CONFIG_FONT_DEFAULT_PX,
};

const theme_t *theme_current(void)
{
    return &s_theme_default;
}

const theme_t *theme_get(const char *name)
{
    if (name != NULL && strcmp(name, s_theme_default.name) == 0) {
        return &s_theme_default;
    }
    return NULL;
}

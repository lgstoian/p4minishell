/**
 * @file theme.h
 * @brief Theme table prep (Phase 3 light).
 *
 * A single `theme_t` gathers every chrome color + font role that used to be
 * scattered literals, so a future switcher only swaps this table. Values
 * below are pixel-identical to the pre-theme literals (zero visual change).
 * Switching arrives later; `theme show` prints the active table and the
 * future sd:/APPS/THEME.INI format is documented in command.md.
 */

#ifndef P4MINISHELL_THEME_H
#define P4MINISHELL_THEME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One complete theme. Colors are 0xRRGGBB. */
typedef struct {
    const char *name;
    uint32_t bg_screen;
    uint32_t bg_transcript;
    uint32_t bg_input_row;
    uint32_t bg_keyboard;
    uint32_t text;
    uint32_t text_muted;
    uint32_t modal_panel_border;
    uint32_t modal_title;
    uint32_t modal_message;
    const char *terminal_font;
    const char *ui_font;
    int terminal_px;
    int ui_px;
} theme_t;

/** Active theme (always "default" until switching lands). Never NULL. */
const theme_t *theme_current(void);

/** Look up a theme by name (NULL when unknown; only "default" exists). */
const theme_t *theme_get(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_THEME_H */

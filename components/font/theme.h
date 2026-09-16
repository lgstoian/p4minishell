/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file theme.h
 * @brief UI theme registry: chrome colors + font roles, switchable at runtime.
 *
 * A single `theme_t` gathers every chrome color + font role that used to be
 * scattered literals. `theme set <name>` swaps the active table and the
 * command layer re-applies it live to the screen/transcript/input row
 * (windows), the keyboard, and the header; modal surfaces read the table at
 * creation. `theme show`/`theme list` print the tables; the sd:/APPS/SHELL.INI
 * `theme` key persists the choice. The original "default" table is
 * pixel-identical to the pre-theme literals (zero visual change).
 */

#ifndef P4MINISHELL_THEME_H
#define P4MINISHELL_THEME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One complete theme. Colors are 0xRRGGBB. */
typedef struct {
    const char *name;
    const char *description;
    uint32_t bg_screen;
    uint32_t bg_transcript;
    uint32_t bg_input_row;
    uint32_t bg_keyboard;
    uint32_t text;         /**< accent (headings, CPU/battery, highlights) */
    uint32_t text_body;    /**< primary body text (header/modal message) */
    uint32_t text_muted;   /**< secondary/dim text */
    uint32_t warn;         /**< warnings, low battery, hot CPU */
    uint32_t err;          /**< off/failed status (header error color) */
    uint32_t header_panel_bg; /**< header status-panel tint */
    uint32_t header_sys_bg;   /**< header system-panel tint */
    uint32_t modal_panel_border;
    uint32_t modal_title;
    uint32_t modal_message;
    const char *terminal_font;
    const char *ui_font;
    int terminal_px;
    int ui_px;
} theme_t;

/** Active theme. Never NULL (falls back to "default"). */
const theme_t *theme_current(void);

/** Look up a built-in theme by name (case-insensitive); NULL when unknown. */
const theme_t *theme_get(const char *name);

/** Number of built-in themes / the i-th theme (NULL when out of range). */
int theme_builtin_count(void);
const theme_t *theme_builtin_at(int index);

/** Select a built-in theme by name (case-insensitive). @return false when
 * unknown (the active theme is unchanged). */
bool theme_set(const char *name);

/** Index of the active theme within the built-in list (>= 0). */
int theme_active_index(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_THEME_H */

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file css_color.h
 * @brief The one CSS colour parser: RGBA + DOS-palette quantisation.
 *
 * HTML/CSS, the Canvas 2D context, emoji fallbacks and the TUI all need to
 * understand colour strings. Keeping one parser is a hard project rule: a
 * second `#rgb`/`rgba()`/named-colour implementation is how a page renders
 * differently in the browser and in a modal.
 *
 * @module markdown
 * @layer leaf
 * @domain ui
 * @provides CSS colour parsing (RGBA) and DOS palette quantisation
 * @status stable
 * @test test/main/test_css_color.c
 * @hard-rule ai-context.md#module-layering-rules
 */
#ifndef P4MINISHELL_CSS_COLOR_H
#define P4MINISHELL_CSS_COLOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;   /**< 0 = transparent, 255 = opaque */
} css_rgba_t;

/** Parse the supported CSS Color 3 subset: #rgb/#rgba/#rrggbb/#rrggbbaa,
 *  rgb()/rgba() (0-255 or %), `transparent`, and the basic named colours.
 *  Returns false for an unsupported value (the caller keeps the old colour). */
bool css_color_parse(const char *text, css_rgba_t *out);

/** Nearest DOS 16-colour index, or 16 for "transparent/unset". */
uint8_t css_color_to_dos(const css_rgba_t *color);

/** RGB for a DOS palette index (16 returns black). */
css_rgba_t css_color_from_dos(uint8_t dos);

/** Blend @p fg over @p bg with @p alpha (0..255), returning an opaque colour.
 *  Used where a surface can render only one foreground colour per cell. */
css_rgba_t css_color_blend(css_rgba_t fg, css_rgba_t bg, uint8_t alpha);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_CSS_COLOR_H */

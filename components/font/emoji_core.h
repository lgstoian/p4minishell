/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file emoji_core.h
 * @brief The one emoji decision point: is this codepoint emoji, and which
 *        retro DOS/CP437 glyph stands in for it.
 *
 * The firmware ships no colour emoji font. Instead every text surface asks
 * this core what an emoji codepoint is:
 *
 *   - a **retro fallback** (the built-in, MIT, CP437-style map below) so text
 *     always renders with a DOS look and never shows tofu;
 *   - optional **third-party packs** (`EMOJI/EMOJI.INI` on SD/USB) that the
 *     emoji adapter draws as cell images - see components/emoji/emoji_pack.h.
 *
 * One core, two adapters: the TUI/HTML text path substitutes or blits cells,
 * and the LVGL path installs a glyph-remapping fallback font that uses the
 * same map. There is deliberately no second emoji table anywhere.
 *
 * Only single codepoints are mapped. ZWJ sequences (family/profession),
 * regional-indicator flags and skin-tone modifiers are documented as falling
 * back to their components (v1.4.3).
 *
 * @module font
 * @layer leaf
 * @domain ui
 * @provides emoji codepoint detection and the retro DOS fallback map
 * @status stable
 * @test test/main/test_emoji.c
 * @hard-rule ai-context.md#module-layering-rules
 */
#ifndef P4MINISHELL_EMOJI_CORE_H
#define P4MINISHELL_EMOJI_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True for a codepoint that carries emoji presentation (the ranges browsers
 *  and terminals treat as emoji, including flags and modifiers). */
bool emoji_is_codepoint(uint32_t cp);

/** True for codepoints that only modify the previous emoji (ZWJ, variation
 *  selectors, skin tones, keycap). Callers skip them after a fallback. */
bool emoji_is_modifier(uint32_t cp);

/** The retro DOS/CP437 substitute codepoint for @p cp, or 0 when the core has
 *  no fallback (the caller then leaves the codepoint alone). Deterministic:
 *  range defaults plus a curated table for the common faces and symbols. */
uint32_t emoji_dos_glyph(uint32_t cp);

/** Write the UTF-8 fallback for @p cp into @p out (at least 5 bytes) and
 *  return its length. Returns 0 when there is no fallback. */
size_t emoji_fallback_utf8(uint32_t cp, char out[5]);

/** Encode one codepoint as UTF-8; returns the byte count (1-4). */
size_t emoji_utf8_encode(uint32_t cp, char out[5]);

/** Decode one UTF-8 codepoint from @p text; returns the byte count (>=1) and
 *  stores the codepoint in @p cp_out (U+FFFD on malformed input). */
size_t emoji_utf8_decode(const char *text, uint32_t *cp_out);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EMOJI_CORE_H */

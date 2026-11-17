/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file emoji.h
 * @brief Third-party emoji packs from SD/USB, drawn through the shared image
 *        ramp; the built-in retro map (font/emoji_core.h) is always the base.
 *
 * A pack is a directory `EMOJI/` at the root of a mounted volume:
 *
 * @code
 * ; sd:/EMOJI/EMOJI.INI
 * 1F600=GRIN.BMP
 * 1F600.cells=2
 * 1F60D=HEART.PNG
 * @endcode
 *
 * Keys are uppercase hex codepoints (a `.cells` suffix sets the width in
 * cells, 1-2, default 2). Values are file names relative to the pack directory
 * (BMP or PNG). The parser is the shared INI core (`storage_ini_file_foreach`)
 * and the decoder is the shared `imgdec`/`gfx` path - there is no second INI
 * or image implementation.
 *
 * SD wins over USB when both carry a pack. A malformed or missing pack is not
 * an error: the retro fallback renders instead.
 *
 * @module emoji
 * @layer leaf
 * @domain ui
 * @provides emoji pack discovery, lookup and cell-image decode
 * @status stable
 * @test test/main/test_emoji.c
 * @hard-rule ai-context.md#module-layering-rules
 */
#ifndef P4MINISHELL_EMOJI_H
#define P4MINISHELL_EMOJI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Largest pack entry count (bounded PSRAM, documented cap). */
#define EMOJI_PACK_MAX_ENTRIES 128
/** Decoded-glyph cache slots (a page shows a handful of distinct emoji). */
#define EMOJI_PACK_CACHE_SLOTS 4
/** Widest a pack glyph may be, in cells. */
#define EMOJI_PACK_MAX_CELLS 2

/** Load the pack once (SD then USB). Safe to call repeatedly; a missing pack
 *  leaves the built-in map in charge. */
void emoji_pack_init(void);

/** Forget the loaded pack and the image cache (e.g. after a volume change or
 *  an explicit `emoji reload`). */
void emoji_pack_free(void);

/** Rescan the volumes (free + init). */
void emoji_pack_reload(void);

/** Number of entries in the loaded pack (0 = built-in map only). */
int emoji_pack_count(void);

/** Volume the pack came from ("sd", "usb", or "" when none). */
const char *emoji_pack_source(void);

/** Look up @p cp in the loaded pack.
 *  @param path      receives the resolved pack image path (may be NULL)
 *  @param path_size size of @p path
 *  @param cells     receives the width in cells 1-2 (may be NULL)
 *  @return true when the pack has this codepoint. */
bool emoji_pack_lookup(uint32_t cp, char *path, size_t path_size, int *cells);

/** Decode @p cp to a cell-grid surface: @p cells*8 wide, 8 tall, RGB565.
 *  The result is owned by the pack's small cache and must not be freed.
 *  @return NULL when the codepoint is not in the pack or decoding failed. */
const gfx_surface_t *emoji_pack_image(uint32_t cp, int *cells);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EMOJI_H */

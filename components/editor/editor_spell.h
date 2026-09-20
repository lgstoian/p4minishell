/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file editor_spell.h
 * @brief Offline spellchecker for the `edit` editor (writerdeck).
 *
 * A wordlist lives on the SD card at sd:/DICTS/<name>.words (one lower-case
 * word per line; a curated `en` sample ships in apps/dicts/ and is pushed
 * with `python apps/push_dicts.py <COM_PORT>`). It is loaded once per
 * session into PSRAM (a single pool + a sorted pointer index) and consulted
 * per word during span rendering. When no list is loaded every word reads as
 * correct, so the feature degrades silently on a card without dictionaries.
 *
 * Tokenization is UTF-8 aware: ASCII, Latin-1/Extended, Greek, and Cyrillic
 * letters form words (in-word `'`/`-`/U+2019 joiners keep `don't` and
 * `well-known` whole) without ever splitting a multi-byte sequence. Tokens
 * containing CJK, digits, or `_` are never flagged (the wordlist cannot
 * cover them); tokens with non-ASCII Latin letters are likewise skipped
 * rather than split. Single-character and overlong tokens are checked like
 * any other word (lookups are length-unlimited).
 */

#ifndef P4MINISHELL_EDITOR_SPELL_H
#define P4MINISHELL_EDITOR_SPELL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Load sd:/DICTS/<name>.words into PSRAM (replacing any current list).
 *  @return true on success; false when the file is missing/too large or the
 *  allocation failed (the current list, if any, is left untouched). */
bool editor_spell_load(const char *name);

/** Free the loaded wordlist. Safe to call when nothing is loaded. */
void editor_spell_unload(void);

/** True when a wordlist is loaded and checks are meaningful. */
bool editor_spell_ready(void);

/** Number of words in the loaded list (0 when none). */
size_t editor_spell_word_count(void);

/** True when @p word (length @p len, any length including 1) appears in
 *  the list (ASCII case-insensitive). Always true when no list is loaded. */
bool editor_spell_ok(const char *word, size_t len);

/** Decode one UTF-8 codepoint at @p s (up to @p avail bytes).
 *  @return bytes consumed, or 0 on NUL/invalid/truncated input. */
size_t editor_spell_utf8(const char *s, size_t avail, unsigned long *cp_out);

/** True when @p cp is a spellcheck word letter (ASCII, Latin-1/Extended,
 *  Greek, Cyrillic, combining marks, or CJK). */
bool editor_spell_is_letter(unsigned long cp);

/** True when @p cp is CJK (tokens containing CJK are never flagged). */
bool editor_spell_is_cjk(unsigned long cp);

/** True when @p cp may join a word (`'`/`-`/U+2019 between letters). */
bool editor_spell_is_joiner(unsigned long cp);

/** Check one tokenizer token: whole-word hit, else every letter part of
 *  length >= 2 hits (shorter parts ignored, so `don't` needs `don` and
 *  `well-known` needs `well` + `known`). Tokens with CJK, digits, `_`, or
 *  non-ASCII Latin letters are never flagged. Always true when no list is
 *  loaded. */
bool editor_spell_token_ok(const char *word, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EDITOR_SPELL_H */

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file editor_spell.h
 * @brief Offline spellchecker for the `edit` editor (writerdeck).
 *
 * A wordlist lives on the SD card at sd:/DICTS/<name>.words (one lower-case
 * word per line, ASCII). It is loaded once per session into PSRAM (a single
 * pool + a sorted pointer index) and consulted per word during span
 * rendering. When no list is loaded every word reads as correct, so the
 * feature degrades silently on a card without dictionaries.
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

/** True when @p word (ASCII, length @p len) appears in the list
 *  (case-insensitive). Always true when no list is loaded. */
bool editor_spell_ok(const char *word, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EDITOR_SPELL_H */

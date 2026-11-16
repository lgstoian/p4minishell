/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file editor_spell_internal.h
 * @brief Internal seam between editor_spell.c (pure core) and its SD half.
 *
 * editor_spell.c owns the wordlist sets, the lookups and the tokenizer - all
 * pure C, so the host unit-test lane can link it. The two operations that must
 * write the user overlay file (`spell learn` / `spell forget`) stay in
 * editor_spell_sd.c behind the two calls declared here, and the wordlists
 * themselves are *read* by that TU and installed into the sets below.
 *
 * The sets are defined exactly once in editor_spell.c and declared here so the
 * loader can install a parsed wordlist without duplicating state. The header is
 * private to components/editor; no other component includes it.
 *
 * @module components/editor
 * @layer leaf
 * @domain app
 * @provides internal declarations for the spellchecker core and its SD half
 * @status stable
 * @test test/main/test_editor.c
 * @hard-rule declarations only - no storage or board BSP here
 */

#ifndef P4MINISHELL_EDITOR_SPELL_INTERNAL_H
#define P4MINISHELL_EDITOR_SPELL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * WORDLIST SETS (owned by editor_spell.c)
 * ======================================================================== */

/** Base dictionary: the pool, its sorted pointer index and the word count. */
extern char *editor_spell_base_pool;
extern char **editor_spell_base_index;
extern size_t editor_spell_base_count;
extern bool editor_spell_base_ready;

/** User overlay: same shape as the base set (consulted first). */
extern char *editor_spell_user_pool;
extern char **editor_spell_user_index;
extern size_t editor_spell_user_count;
extern bool editor_spell_user_ready;

/**
 * Split @p pool in place (NUL-terminate and lower-case each word) and build the
 * sorted pointer index, capped at @p cap_words. The caller owns @p pool and the
 * returned index. Shared by the base and overlay loaders.
 * @return true when at least one word was indexed.
 */
bool editor_spell_build_index(char *pool, size_t got, size_t cap_words,
                              char ***index_out, size_t *count_out);

/* ========================================================================
 * SD HALF (editor_spell_sd.c)
 * ======================================================================== */

/**
 * Append the normalized word @p norm to the user overlay on the SD card (the
 * caller has validated it and ruled out the words already known) and reload
 * the overlay. @return true on success.
 */
bool editor_spell_user_append(const char *norm);

/**
 * Rewrite the user overlay on the SD card without @p norm (the caller has
 * validated it and confirmed it is present) and reload the overlay.
 * @return true when a user word was removed.
 */
bool editor_spell_user_remove(const char *norm);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EDITOR_SPELL_INTERNAL_H */
